// SOTN ESP32-S3 — platform shims: everything psyz's SDL platform files
// provided that the game core actually calls, minus a display.
//
// The one subtle contract here is timing (blocker R4): the game is
// fixed-timestep, one logic tick per VSync, and psyz's root counter (RCnt)
// drives the 240Hz sound sequencer. On PC both hang off the SDL frame loop;
// here VSync paces with esp_timer at NTSC rate and feeds RCnt 4 ticks per
// frame, so the sequencer state machines stay in sync even with audio output
// absent.

#include <psyz.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include <fcntl.h>
#include <psyz/input.h>
#include "board_pins.h"
long psyz_read(long fd, void* buf, long n); // esp_fileapi.c

// lcd_esp32.c
int lcd_init(void);
void lcd_present(const unsigned short* src);
const unsigned short* Soft_DisplayPtr(void);
void lcd_wait_snapshot(void);

#ifdef SOTN_BOARD_XIAO
// ---- non-blocking console for the handheld ---------------------------------
//
// See sotn_printf.h for why this exists. Both entry points do the same thing:
// format into a stack buffer and offer it to the USB driver with a zero
// timeout, so a console nobody is reading costs dropped characters instead of
// a stopped game. Neither may call printf — the force-include has renamed it
// to the first of these.
int Sotn_VPrintf(const char* fmt, va_list ap) {
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return n;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    usb_serial_jtag_write_bytes(line, (size_t)n, 0);
    return n;
}

int Sotn_Printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = Sotn_VPrintf(fmt, ap);
    va_end(ap);
    return n;
}
#endif

// ---- logging (psyz/log.h contract) -----------------------------------------

LOG_LEVEL psyz_logLevel = LOG_LEVEL_I; // DEBUG spam would drown the UART

void psyz_log(unsigned int level, const char* file, unsigned int line,
              const char* func, const char* fmt, ...) {
    static const char lv[] = {'D', 'I', 'W', 'E'};
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)file;
    printf("[%c][%s:%d] %s\n", lv[level & 3], func, line, buf);
}

// ---- frame pacing + RCnt (the game clock) ----------------------------------

void Psyz_RcntAdd(int n); // provided by psyz's libetc.c

static int64_t next_frame_us;
#define FRAME_US 16683 // 59.94Hz NTSC

void PlatformBackend_Present(void) {
    // F3 replaces this with the LCD scanout; headless F2 only paces
}

void PollEvents(void) {}

int InitPlatform(void) { return 1; }

int sotn_vsync_count;
int Psyz_VideoVSync(int mode) {
#define vsync_count sotn_vsync_count
    (void)mode;
    // scanout first: the DMA transfer then overlaps the pacing wait below.
    // (the 30Hz A/B gate is gone - the flicker was a content race, fixed by
    // the PSRAM snapshot, so the panel gets every frame again)
    lcd_present(Soft_DisplayPtr());
    int64_t now = esp_timer_get_time();
    if (next_frame_us == 0) {
        next_frame_us = now;
    }
    next_frame_us += FRAME_US;
    int64_t wait = next_frame_us - now;
    if (wait > 0 && wait < 2 * FRAME_US) {
        esp_rom_delay_us((uint32_t)wait);
    } else if (wait < -4 * FRAME_US) {
        next_frame_us = now; // fell far behind: resync rather than spiral
    }
    // barrier: the snapshot copy on core 1 must be complete before the game
    // starts drawing the next frame into VRAM (it overlaps the wait above)
    lcd_wait_snapshot();
    // the core-1 pull only ACCUMULATES sample counts; the RCnt handlers
    // (game code!) must run here on the game thread to avoid cross-core races
    {
        extern volatile int psyz_pending_rcnt;
        int n = psyz_pending_rcnt;
        if (n > 0) {
            psyz_pending_rcnt -= n;
            Psyz_RcntAdd(n);
        }
    }
    // Trampa del pisador de DRAM. Valores LEGITIMOS del spinlock del kernel
    // (IDF v5.5): SPINLOCK_FREE 0xB33FFFFF, or the owning core's id
    // (0x0000CDCD core0 / 0x0000ABAB core1). El congelon en partida real
    // mostro 0x600080B4 ahi dentro - un puntero a espacio de perifericos,
    // basura pura. Cualquier otro valor = alguien lo piso.
    {
        // xKernelLock es estatico: se ancla via el global adyacente
        // esp_ipc_isr_end_fl (+0xFC en este build, verificado con nm)
        extern unsigned int esp_ipc_isr_end_fl;
        volatile unsigned* kernel_lock =
            (volatile unsigned*)((char*)&esp_ipc_isr_end_fl + 0xFC);
        unsigned v = *kernel_lock;
        static int tripped;
        if (!tripped && v != 0xB33FFFFFu && v != 0x0000CDCDu &&
            v != 0x0000ABABu && v != 0u) {
            tripped = 1;
            printf("sotn: KERNEL LOCK STOMPED = %08x en frame %d\n", v,
                   sotn_vsync_count);
            unsigned char* p = (unsigned char*)kernel_lock - 96;
            for (int i = 0; i < 192; i += 16) {
                printf("  %p:", (void*)(p + i));
                for (int j = 0; j < 16; j++) {
                    printf(" %02x", p[i + j]);
                }
                printf("\n");
            }
            printf("sotn: (the game keeps running; copy these lines)\n");
        }
    }
    ++vsync_count;
    if (vsync_count == 120) {
        // well past the one legitimate write of these links at init
        extern void Sotn_ArmBufferWatch(void);
        Sotn_ArmBufferWatch();
    }
    if (vsync_count <= 5) {
        printf("sotn: vsync %d\n", vsync_count);
    }
    if (vsync_count % 60 == 0) {
        extern int g_GameState, g_GameStep, g_StageId;
        extern unsigned sotn_prim_cycles;
        static int64_t last_us;
        int64_t t = esp_timer_get_time();
        unsigned frame_us = last_us ? (unsigned)((t - last_us) / 60) : 0;
        last_us = t;
        unsigned prim_us = sotn_prim_cycles / 240 / 60; // 240MHz, 60 frames
        sotn_prim_cycles = 0;
        extern unsigned sotn_prim_census[8], sotn_prim_cycles_by[8];
        extern unsigned sotn_poly_kind[4];
        extern unsigned sotn_audio_batches, sotn_audio_active;
        extern int g_GameState, g_GameStep, g_StageId;
        printf("sotn: frame %d st %d/%d/%d | %u us/f prim %u | poly %u (%u us: "
               "flat %u tex %u sflat %u stex %u) rect %u (%u us) | "
               "spu %u/%u | heap %u\n",
               vsync_count, g_GameState, g_GameStep, g_StageId, frame_us,
               prim_us, sotn_prim_census[1] / 60,
               sotn_prim_cycles_by[1] / 240 / 60, sotn_poly_kind[0] / 60,
               sotn_poly_kind[1] / 60, sotn_poly_kind[2] / 60,
               sotn_poly_kind[3] / 60, sotn_prim_census[3] / 60,
               sotn_prim_cycles_by[3] / 240 / 60, sotn_audio_active,
               sotn_audio_batches, ({extern unsigned sotn_snap_cycles; unsigned q = sotn_snap_cycles / 240 / 60; sotn_snap_cycles = 0; q;}),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        memset(sotn_prim_census, 0, sizeof(sotn_prim_census));
        memset(sotn_prim_cycles_by, 0, sizeof(sotn_prim_cycles_by));
        memset(sotn_poly_kind, 0, sizeof(sotn_poly_kind));
    }
    if (vsync_count % 300 == 0) {
        // where in VRAM is the game drawing? nonzero census + bounding box
        extern u16 g_RawVram[512 * 1024];
        int minx = 1024, miny = 512, maxx = -1, maxy = -1;
        unsigned n = 0, h = 2166136261u;
        for (int y = 0; y < 512; y++) {
            const u16* row = &g_RawVram[y * 1024];
            for (int x = 0; x < 1024; x++) {
                u16 p = row[x];
                h = (h ^ p) * 16777619u;
                if (p) {
                    n++;
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                    if (y < miny) miny = y;
                    if (y > maxy) maxy = y;
                }
            }
        }
        printf("sotn: vram frame %d crc %08x nonzero %u bbox (%d,%d)-(%d,%d)\n",
               vsync_count, h, n, minx, miny, maxx, maxy);
    }
    return vsync_count;
}

// pads: the linker names the exact platform entry point in the next round

// ---- pads + audio locks (single-threaded in F2) ----------------------------

#ifdef SOTN_BOARD_XIAO
// The handheld has four pins for controls, not ten: a resistor ladder, an
// analog stick, and two dedicated buttons. Same psyz bit order, different
// hardware entirely.
#include "esp_input_xiao.inc.c"
#else
// board buttons (header P1, active low) -> PSX pad, psyz bit order:
// 0 L2, 1 R2, 2 L1, 3 R1, 4 TRIANGLE, 5 CIRCLE, 6 CROSS, 7 SQUARE,
// 8 SELECT, 9 L3, 10 R3, 11 START, 12 UP, 13 RIGHT, 14 DOWN, 15 LEFT
static const struct {
    int gpio;
    int bit;
} sButtons[] = {
    {PIN_BTN_UP, 12},    {PIN_BTN_RIGHT, 13}, {PIN_BTN_DOWN, 14},
    {PIN_BTN_LEFT, 15},  {PIN_BTN_A, 6},      {PIN_BTN_B, 7},
    {PIN_BTN_L, 4},      {PIN_BTN_R, 5},      {PIN_BTN_START, 11},
    {PIN_BTN_SELECT, 8}, {0 /* BOOT */, 11},
};
static unsigned sButtonUnwired; // pins with no physical button (read as pressed)
#endif

// load-time visibility: the game can be busy for long stretches before the
// first VSync; this fires regardless and shows which counters move
static void ProgressTick(void* arg) {
    (void)arg;
    extern unsigned sotn_move_count, sotn_exeque_count, sotn_push_count;
    printf("sotn: tick exeque %u push %u move %u\n", sotn_exeque_count,
           sotn_push_count, sotn_move_count);
}

void MyPadInit(int mode) {
    (void)mode;
#ifdef SOTN_BOARD_XIAO
    xiaoInputInit();

    // Nothing else to do: the USB-Serial-JTAG driver is already up (app_main
    // installs it before the first line of output), and it is the ONLY
    // console this board may use. Installing the UART0 driver would hand
    // GPIO43 back to the UART peripheral, and on the handheld that pin is the
    // panel's data/command line — the screen would stop framing commands and
    // go blank, with nothing in the log to say why.
    return;
#else
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        mask |= 1ULL << sButtons[i].gpio;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    // Buttons are active-low with pull-ups; a pin that already reads LOW at
    // boot has nothing wired to it (or is held by another function) and would
    // look like a button jammed down forever - CROSS was stuck like that,
    // which is why jumping did nothing. Mask those pins out.
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        if (!gpio_get_level(sButtons[i].gpio)) {
            sButtonUnwired |= 1u << i;
        }
    }
    printf("sotn: unwired buttons: %04x\n", sButtonUnwired);

    // Input can arrive on EITHER interface: the console is UART0 (the
    // USB-UART bridge the user's COM port maps to) while the native
    // USB-Serial-JTAG is the secondary console. Listen on both.
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&usb_cfg);
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        uart_vfs_dev_use_driver(UART_NUM_0);
    }
    fcntl(0 /* stdin */, F_SETFL, O_NONBLOCK); // follows the real console
#endif
}

// PC-keyboard play over USB serial (same scheme as the OpenLara/Descent
// ports): each received char presses its PSX button for a few frames; the
// keyboard's auto-repeat sustains held keys.
#define SERIAL_HOLD_FRAMES 8
static unsigned char sHold[16];

static int keyToPsxBit(char c) {
    switch (c) {
    case 'w': return 12; // UP
    case 'd': return 13; // RIGHT
    case 's': return 14; // DOWN
    case 'a': return 15; // LEFT
    case 'x': return 6;  // CROSS (jump/confirm)
    case 'z': return 7;  // SQUARE (attack)
    case 'q': return 4;  // TRIANGLE
    case 'e': return 5;  // CIRCLE
    case '1': return 2;  // L1
    case '2': return 3;  // R1
    case '3': return 0;  // L2
    case '4': return 1;  // R2
    case '\r':
    case '\n': return 11; // START
    case ' ': return 8;   // SELECT
    case 'f': { // diagnostic: freeze the image sent to the LCD
        extern volatile int sotn_lcd_freeze;
        sotn_lcd_freeze = !sotn_lcd_freeze;
        printf("sotn: LCD freeze %s\n", sotn_lcd_freeze ? "ON" : "OFF");
        return -1;
    }
    }
    return -1;
}

void Psyz_PadsPoll(void) {
    unsigned char rx[16];
    int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), 0);
#ifndef SOTN_BOARD_XIAO
    // The handheld never installs these (see MyPadInit): its console is the
    // native USB port above, and touching UART0 would steal the panel's
    // data/command pin.
    if (n <= 0) {
        n = uart_read_bytes(UART_NUM_0, rx, sizeof(rx), 0);
    }
    if (n <= 0) {
        // psyz macro-renames read(); psyz_read maps straight onto newlib's
        n = (int)psyz_read(0 /* stdin */, rx, sizeof(rx));
    }
#endif
    if (n < 0) {
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        int bit = keyToPsxBit((char)rx[i]);
        if (bit >= 0) {
            sHold[bit] = SERIAL_HOLD_FRAMES;
        }
    }

    unsigned pressed = 0;
    for (int i = 0; i < 16; i++) {
        if (sHold[i]) {
            sHold[i]--;
            pressed |= 1u << i;
        }
    }
    // NOTE: an earlier auto-START synth here keyed on state 2/step 3
    // believing it was the stage-select screen - that state IS normal
    // gameplay (Game_Play==2), so it held the pause menu open (black
    // screen). Direct-boot enters gameplay on its own; no synthesis.
#ifdef SOTN_BOARD_XIAO
    pressed |= xiaoReadPad();
#else
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        if ((sButtonUnwired & (1u << i)) == 0 &&
            !gpio_get_level(sButtons[i].gpio)) {
            pressed |= 1u << sButtons[i].bit;
        }
    }
#endif
    char frame[PSYZ_PAD_BUF_LEN];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00;
    frame[1] = (char)PSYZ_CTRL_DIGITAL_PAD;
    frame[2] = (char)(~(pressed >> 8) & 0xFF);
    frame[3] = (char)(~pressed & 0xFF);
    Psyz_PadsSet(0, frame, sizeof(frame));
    memset(frame, 0xFF, sizeof(frame)); // port 1: disconnected
    Psyz_PadsSet(1, frame, sizeof(frame));
}
// Psyz_AudioInit/Lock/Unlock live in esp_audio.c (F6 SPU pull task)

// room foreground layouts are const flash tables; the game mutates tiles
// (red doors, breakables) so each room load serves a PSRAM copy
u16* Sotn_TilemapWritableCopy(const u16* src, int count) {
    static EXT_RAM_BSS_ATTR u16 pool[0x10000]; // 128KB: one room's fg
    if (count < 0) {
        count = 0;
    }
    if (count > (int)(sizeof(pool) / sizeof(pool[0]))) {
        printf("sotn: tilemap copy clamped (%d tiles)\n", count);
        count = sizeof(pool) / sizeof(pool[0]);
    }
    memcpy(pool, src, (size_t)count * 2);
    return pool;
}

// ---- video misc the game calls at boot -------------------------------------

int Psyz_VideoSetInternalResolution(unsigned multiplier) {
    (void)multiplier;
    return 1; // the soft rasterizer renders at native PSX resolution
}
#include <psyz/video.h>
int Psyz_VideoSetVsyncMode(PsyzVsyncMode mode) {
    (void)mode;
    return 0;
}
void Psyz_SetTitle(const char* title) { (void)title; }
void ResetPlatform(void) {}
