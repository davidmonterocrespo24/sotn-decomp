// SOTN ESP32-S3 — entry point: bring up the data source, boot the game,
// count frames over serial.
//
// Where the disc-derived files come from is the one thing that differs
// between the two boards. The Waveshare board has 16 MB of flash and carries
// them in a FAT partition flashed from the build, so the user handles no card
// at all. The handheld has 8 MB total, which the app and a 3.7 MB data image
// cannot share, so there they live on the microSD that the board already has
// for the XA audio. io.c's relative "disks/us/..." paths are rebased onto
// SOTN_DATA_ROOT by esp_fileapi.c either way.

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "board_pins.h"

int sotn_main(int argc, char* argv[]); // src/pc/main.c, renamed at compile
int lcd_init(void);                    // lcd_esp32.c
int sd_init(void);                     // esp_sd.c

void app_main(void) {
#ifdef SOTN_BOARD_XIAO
    // First thing, before anything prints: the handheld's only console is the
    // chip's native USB port, and every write below goes through the
    // best-effort path in sotn_printf.h, which needs this driver to exist.
    // Routing the IDF log through the same path matters as much as the game's
    // own printf — the panel and card drivers both log at init, and a blocked
    // ESP_LOGI would stop the boot just as dead.
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&usb_cfg);
    esp_log_set_vprintf(Sotn_VPrintf);
    // Why the LAST run ended. A reset that prints nothing — brownout, a
    // watchdog, a USB drop that takes the host listener with it — leaves no
    // trace in a capture, so the new boot has to say it itself.
    printf("sotn: previous reset reason = %d "
           "(1 power-on, 3 software, 4 panic, 5-7 watchdog, 9 brownout)\n",
           (int)esp_reset_reason());
#else
    static wl_handle_t wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = 16,
        .format_if_mount_failed = false,
    };
    esp_err_t err =
        esp_vfs_fat_spiflash_mount_rw_wl("/flash", "storage", &cfg, &wl);
    if (err != ESP_OK) {
        printf("sotn: data partition mount failed (%s) - flash the data "
               "image first\n",
               esp_err_to_name(err));
        return;
    }
    chdir("/flash");
#endif

    if (lcd_init() != 0) {
        printf("sotn: LCD init failed\n");
    }

    // The card shares SPI2 with the panel, so it can only be added after
    // lcd_init has created the bus. On the handheld that makes this the point
    // where the game's data becomes reachable at all — a missing card is
    // fatal there, while on the Waveshare board it only costs the music.
    int have_sd = (sd_init() == 0);
#ifdef SOTN_BOARD_XIAO
    if (!have_sd) {
        printf("sotn: no microSD — the handheld keeps ALL game data on the "
               "card, so there is nothing to run. Copy esp32/fatfs_root/* to "
               "the card root.\n");
        return;
    }
    chdir(SOTN_DATA_ROOT);

    // Check one known asset before handing over. Without this the missing
    // data surfaces as a failure deep inside the stage loader, far from the
    // cause — and "I copied the files into a folder on the card instead of
    // its root" is by far the likeliest way to get here.
    {
        FILE* probe = fopen(SOTN_DATA_ROOT "/disks/us/BIN/F_GAME.BIN", "r");
        if (!probe) {
            printf("sotn: the card has no game data.\n"
                   "      Copy the CONTENTS of esp32/fatfs_root/ to the ROOT "
                   "of the microSD,\n"
                   "      so that this file exists: " SOTN_DATA_ROOT
                   "/disks/us/BIN/F_GAME.BIN\n");
            return;
        }
        fclose(probe);
    }
#endif

    {
        // trap the wild writer that stomps DRAM statics on the nz0 path:
        // debug exception fires at the exact store instruction
        extern int psyz_queue_canary_lo, psyz_queue_canary_hi;
        esp_cpu_set_watchpoint(0, &psyz_queue_canary_lo, 4,
                               ESP_CPU_WATCHPOINT_STORE);
        esp_cpu_set_watchpoint(1, &psyz_queue_canary_hi, 4,
                               ESP_CPU_WATCHPOINT_STORE);
        printf("sotn: watchpoints on %p / %p\n", (void*)&psyz_queue_canary_lo,
               (void*)&psyz_queue_canary_hi);
    }

    // boot stage comes from the data root so switching stage is a file edit,
    // not an app rebuild. The word "boot" (or an empty file) means the normal
    // sequence (logos -> title -> save menu), which needs the SEL overlay
    // statically linked.
    static char stage[8] = "";
    FILE* f = fopen(SOTN_DATA_ROOT "/stage.txt", "r");
    if (f) {
        if (fgets(stage, sizeof(stage), f) == NULL) {
            stage[0] = '\0';
        }
        for (char* p = stage; *p; p++) {
            if (*p == '\r' || *p == '\n') *p = '\0';
        }
        fclose(f);
    }
    if (!strcmp(stage, "boot")) {
        stage[0] = 0;
    }
    printf("sotn: stage.txt = [%s]\n", stage);

    // XA music streams from the full disc image, which is 583 MB and only
    // ever lives on the card. Optional on both boards: without it the game
    // runs with music absent.
    int have_disk = 0;
    if (have_sd) {
        FILE* c = fopen("/sd/sotn.us.cue", "r");
        if (c) {
            fclose(c);
            have_disk = 1;
        } else {
            printf("sotn: SD mounted but no /sd/sotn.us.cue - no XA\n");
        }
    }

    // An EMPTY stage means "run the normal sequence", and that has to be
    // expressed by OMITTING --stage, not by passing an empty one: the
    // argument parser rejects "" outright, so `--stage ""` turns the
    // normal-boot path into a usage dump and an immediate exit. The bug never
    // showed while stage.txt said "nz0"; it surfaced the first time the file
    // was missing, which reads identically to an empty one.
    static char* argv[6];
    int argc = 0;
    argv[argc++] = "sotn";
    if (stage[0]) {
        argv[argc++] = "--stage";
        argv[argc++] = stage;
    }
    if (have_disk) {
        argv[argc++] = "--disk";
        argv[argc++] = "/sd/sotn.us.cue";
    }
    argv[argc] = NULL;
    printf("sotn: booting stage [%s]%s\n", stage[0] ? stage : "normal sequence",
           have_disk ? " with XA" : "");
    sotn_main(argc, argv);
    printf("sotn: game returned\n");
}
