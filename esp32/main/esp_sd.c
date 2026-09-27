// SOTN ESP32-S3 — F7: microSD mount for the disc image (XA music source).
//
// The card shares SPI2 with the LCD (board design: both are CS-managed on
// the same bus; lcd_init parks SD_CS high before the first LCD byte). The
// bus is already initialized by lcd_init, so only the sdspi device is added
// here. Mount is best-effort: no card simply means no XA music.

#include <stdio.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "board_pins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Every card command goes through card->host.do_transaction; wrapping it is
// the one place the bus mutex can be taken for ALL card traffic (FATFS reads,
// writes, status) without touching the file layer. See lcd_esp32.c.
extern SemaphoreHandle_t sotn_spi_mux;
static esp_err_t (*sRawTransaction)(int slot, sdmmc_command_t* cmd);
static esp_err_t lockedTransaction(int slot, sdmmc_command_t* cmd) {
    if (sotn_spi_mux) xSemaphoreTake(sotn_spi_mux, portMAX_DELAY);
    esp_err_t err = sRawTransaction(slot, cmd);
    if (sotn_spi_mux) xSemaphoreGive(sotn_spi_mux);
    return err;
}

int sd_init(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_SPI_HOST;
    // XA streaming reads sequential 2352-byte sectors; 20MHz is plenty and
    // keeps the shared bus friendly with the LCD's 80MHz bursts
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = LCD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        // Three is enough when the card only carries the XA disc image. On
        // the handheld it carries the whole game, so it needs the same
        // allowance the flash partition got on the other board — a stage load
        // opens several files while the audio stream stays open.
#ifdef SOTN_BOARD_XIAO
        .max_files = 16,
#else
        .max_files = 3,
#endif
    };
    sdmmc_card_t* card = NULL;
    esp_err_t err =
        esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        printf("sotn: no microSD (%s)\n", esp_err_to_name(err));
        return -1;
    }
    sRawTransaction = card->host.do_transaction;
    card->host.do_transaction = lockedTransaction;
    printf("sotn: microSD mounted (%llu MB)\n",
           ((unsigned long long)card->csd.capacity * card->csd.sector_size) /
               (1024 * 1024));
    return 0;
}
