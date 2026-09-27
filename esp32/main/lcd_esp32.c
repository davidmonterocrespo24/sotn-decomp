// SOTN ESP32-S3 — F3 scanout: g_RawVram display area -> ST7789 over SPI DMA.
//
// Same proven pipeline as the Descent/OpenLara ports on this board: 16-line
// ping-pong chunks, conversion overlapped against the in-flight transfer.
// Source is PSX RGB5551 (R in bits 0-4, G 5-9, B 10-14) with a 1024-px row
// stride; output is byteswapped RGB565. The PSX 256-px-wide frame sits
// pillarboxed in the 320-px panel (black bars pushed once at init).

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#ifdef SOTN_BOARD_XIAO
#include "esp_lcd_ili9341.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"

#include "board_pins.h"

#define PSX_W 256
#define PSX_H 240
#define PILLAR_X ((320 - PSX_W) / 2)
#define VRAM_STRIDE 1024

// Lines per SPI burst. 8 measured FASTER than 16 on the handheld — the send
// went 19.2 -> 25 ms when the bursts were doubled, so the fixed per-burst cost
// this was meant to amortise is not what dominates; larger bursts just hold
// the bus longer and give the pipeline coarser granularity to overlap with.
// Kept at 8 on both boards, now for a measured reason rather than only to
// leave internal DRAM for the SEL stage.
#define CHUNK_LINES 8   // 5 KB per band; leaves DRAM for the SEL stage
#define CHUNK_BYTES (PSX_W * CHUNK_LINES * 2)

static const char* TAG = "lcd";

// F5-flicker: the scan task DMAs from a SNAPSHOT, never from live VRAM, so
// nothing the game writes mid-transfer can reach the panel. sFreeze holds
// the last snapshot (diagnostic: if a frozen image still flickers, the
// artifact is panel-side, not content-side).
static EXT_RAM_BSS_ATTR uint16_t sSnap[2][PSX_H][PSX_W];
static QueueHandle_t sScanQ; // snapshot index handed to the DMA stage
volatile int sotn_lcd_freeze;
SemaphoreHandle_t sotn_spi_mux; // SPI2 shared with the card: see presentSync
// frames the game offered that the panel was too busy to accept, and the
// running total offered — the pair says whether the panel keeps up
unsigned sotn_lcd_drops, sNotifies;
// lost DMA completions, and submissions the driver refused — the pair names
// which half of the stall happened without needing a debugger attached
unsigned sotn_lcd_timeouts, sotn_lcd_sendfails;
// 0 = DMA straight from the finished VRAM frame (no copy); 1 = safety copy
//
// The Waveshare board can take the no-copy path because its panel finishes a
// frame well inside the game's 16.7 ms: the game is always drawing into the
// OPPOSITE half of VRAM from the one being scanned out, so the two never meet.
//
// The handheld cannot. Its scanout takes ~24 ms against the same 16.7 ms
// frame, so the game laps the panel — it swaps buffers and starts redrawing
// the half still being transmitted, and the DMA sends pixels that are being
// rewritten underneath it. THAT is the flicker, and it is a coherence problem,
// not a speed one: it would persist at any frame rate the panel could manage.
// Copying the finished frame first costs ~2.6 ms on core 1 and makes what the
// panel sends immutable, which is the whole point.
volatile int sotn_lcd_snapshot =
#ifdef SOTN_BOARD_XIAO
    1;
#else
    0;
#endif
unsigned sotn_snap_cycles;

static esp_lcd_panel_handle_t sPanel;
static esp_lcd_panel_io_handle_t sIO;
static SemaphoreHandle_t sTransDone;
// signalled by the scan task once the frame snapshot is safely copied
static SemaphoreHandle_t sSnapDone;
static uint16_t* sChunk[2];

static bool IRAM_ATTR onTransDone(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t* ev,
                                  void* user) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sTransDone, &woken);
    return woken == pdTRUE;
}

int lcd_init(void) {
    // The handheld strapped its backlight to 3V3, so PIN_LCD_BL is -1 there
    // and `1ULL << PIN_LCD_BL` would be a shift by a negative count — the
    // compiler folds that to garbage and the resulting mask configures pins
    // nobody asked for. Both uses are guarded rather than the pin faked,
    // because a fake pin number would quietly drive a real GPIO.
    uint64_t out_mask = 1ULL << PIN_SD_CS;
#if PIN_LCD_BL >= 0
    out_mask |= 1ULL << PIN_LCD_BL;
#endif
    gpio_config_t bl = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
#if PIN_LCD_BL >= 0
    gpio_set_level(PIN_LCD_BL, !LCD_BL_ON_LEVEL);
#endif
    gpio_set_level(PIN_SD_CS, 1); // keep the microSD off the shared bus

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * CHUNK_LINES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,
        .on_color_trans_done = onTransDone,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &sIO));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
#ifdef SOTN_BOARD_XIAO
    // The handheld's panel is an ILI9341, which ESP-IDF does not ship a driver
    // for — only ST7789 and NT35510 are built in — so it comes from the
    // component registry (see idf_component.yml). The two controllers share
    // most of the MIPI command set and it is tempting to drive one with the
    // other's driver; they differ in the init sequence and in whether display
    // inversion is on, which is exactly the sort of "nearly works" that costs
    // an evening.
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(sIO, &panel_cfg, &sPanel));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(sIO, &panel_cfg, &sPanel));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(sPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(sPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(sPanel, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(sPanel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(sPanel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(sPanel, LCD_GAP_X, LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(sPanel, true));

    sTransDone = xSemaphoreCreateCounting(2, 2);
    sotn_spi_mux = xSemaphoreCreateMutex();
    sSnapDone = xSemaphoreCreateBinary();
    sScanQ = xQueueCreate(1, sizeof(int));
    for (int i = 0; i < 2; i++) {
        sChunk[i] = heap_caps_malloc(320 * CHUNK_LINES * 2,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!sChunk[i]) {
            ESP_LOGE(TAG, "no DMA memory for chunk buffers");
            return -1;
        }
    }

    // full-width black frame: paints the pillarbox bars, then backlight on
    memset(sChunk[0], 0, 320 * CHUNK_LINES * 2);
    for (int y = 0; y < 240; y += CHUNK_LINES) {
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, 320, y + CHUNK_LINES,
                                  sChunk[0]);
    }
    for (int i = 0; i < 2; i++) xSemaphoreTake(sTransDone, portMAX_DELAY);
    for (int i = 0; i < 2; i++) xSemaphoreGive(sTransDone);
#if PIN_LCD_BL >= 0
    gpio_set_level(PIN_LCD_BL, LCD_BL_ON_LEVEL);
#endif

    ESP_LOGI(TAG, "%s up: %d Hz SPI, %d-line chunks",
#ifdef SOTN_BOARD_XIAO
             "ILI9341",
#else
             "ST7789",
#endif
             LCD_SPI_HZ, CHUNK_LINES);
    return 0;
}

// same conversion but reading VRAM directly (1024-px stride)
static inline void convertChunkStride(const uint16_t* restrict src,
                                      uint16_t* restrict dst, int rows);

// PSX 5551 -> byteswapped RGB565, two pixels per 32-bit store. Returns a
// cheap fingerprint of the band it produced (see sBandHash below): folding
// each output word in costs a rotate and a xor per two pixels, on data that
// is already in a register.
static inline uint32_t convertChunk(const uint16_t* restrict src,
                                    uint16_t* restrict dst, int rows) {
    uint32_t* restrict d = (uint32_t*)dst;
    uint32_t h = 0x811C9DC5u;
    for (int y = 0; y < rows; y++) {
        const uint16_t* row = src + y * PSX_W;
        for (int x = 0; x < PSX_W; x += 2) {
            uint16_t c0 = row[x], c1 = row[x + 1];
            uint16_t p0 = ((c0 & 0x1F) << 11) | (((c0 >> 5) & 0x1F) << 6) |
                          ((c0 >> 10) & 0x1F);
            uint16_t p1 = ((c1 & 0x1F) << 11) | (((c1 >> 5) & 0x1F) << 6) |
                          ((c1 >> 10) & 0x1F);
            p0 = (p0 >> 8) | (p0 << 8);
            p1 = (p1 >> 8) | (p1 << 8);
            uint32_t w = (uint32_t)p0 | ((uint32_t)p1 << 16);
            *d++ = w;
            h = ((h << 5) | (h >> 27)) ^ w;
        }
    }
    return h;
}

// src points at the top-left of the finished frame inside g_RawVram.
// The rows outside the stage clip are NOT overscan: the HUD lives there,
// so everything gets sent as the game drew it.
static const uint16_t* sDirectSrc;
// Where a frame's scanout time actually goes, split so the two candidate
// causes cannot be confused: `wait` is time blocked on the panel's previous
// DMA (the SPI link is the limit), `conv` is time turning PSX 5551 into
// RGB565 (the CPU is). Guessing between them is how a clock bump gets
// credited with fixing something it never touched.
// ...plus the total, which is the only figure that cannot mislead: the first
// version of this split omitted esp_lcd_panel_draw_bitmap, and that call
// BLOCKS once the SPI driver's transaction queue fills. It reported a 5 ms
// scanout for a frame that was really taking five times that, and the missing
// time was sitting in the one call nobody was watching.
unsigned sotn_lcd_wait_us, sotn_lcd_conv_us, sotn_lcd_total_us;
/* SPI2 is shared with the microSD, and on the handheld the card carries the
 * whole game, so the two DO overlap: a stage load reads the card while this
 * task streams a frame. The IDF driver then starts the card's polling command
 * while a queued DMA transfer of the panel is still on the wire, and asserts
 * (spi_hal_setup_trans: running_cmd == 0) — a panic and a reboot during every
 * room/stage load. This mutex gives the bus to one side at a time; the panel
 * holds it for a frame and DRAINS its in-flight DMA before letting go, which
 * is exactly the condition the assertion demands. The card side takes it
 * per command (esp_sd.c). */
static uint32_t sBandHash[PSX_H / CHUNK_LINES];
static int sForceFull = 1;
unsigned sotn_lcd_skipped; // bands not sent because the panel already had them
static void presentSync(int snapIdx) {
    static unsigned frames;
    sForceFull = (frames++ % 30) == 0;
    int buf = 0;
    unsigned t0, t1, t2, tA, tB, wait = 0, conv = 0;
    if (xSemaphoreTake(sotn_spi_mux, pdMS_TO_TICKS(300)) != pdTRUE) {
        sotn_lcd_timeouts++;
        return;
    }
    __asm__ volatile("rsr.ccount %0" : "=a"(tA));
    for (int y = 0; y < PSX_H; y += CHUNK_LINES) {
        __asm__ volatile("rsr.ccount %0" : "=a"(t0));
        /* Never portMAX_DELAY here.
         *
         * This semaphore counts free chunk buffers and is replenished by the
         * DMA completion callback. If one completion never arrives the count
         * drops for good and this wait blocks FOREVER: the game keeps running
         * perfectly, the telemetry keeps printing, and the panel simply stops
         * — which reads to the player as the whole console freezing, and cost
         * a long diagnosis because every game-side number looked healthy. The
         * giveaway was this function's own timing figure repeating byte for
         * byte across reports.
         *
         * 300 ms is ~6x the worst send measured on this board, so a genuine
         * transfer never trips it; only a lost one does. Giving the count back
         * before bailing restores the balance, and because the semaphore is
         * capped at 2 an over-give (the late completion arriving after all)
         * fails harmlessly rather than corrupting the count. */
        if (xSemaphoreTake(sTransDone, pdMS_TO_TICKS(300)) != pdTRUE) {
            sotn_lcd_timeouts++;
            xSemaphoreGive(sTransDone);
            break; /* abandon this frame; the next one repaints everything */
        }
        __asm__ volatile("rsr.ccount %0" : "=a"(t1));
        if (sotn_lcd_snapshot) {
            uint32_t h =
                convertChunk(&sSnap[snapIdx][y][0], sChunk[buf], CHUNK_LINES);
            /* Skip bands identical to what the panel already shows. At 40 MHz
             * the wire is the bottleneck (24.6 ms per full frame and the S3's
             * SPI has no clock between 40 and 80), so the only way to put
             * more frames on screen is to send fewer bytes. The HUD and any
             * part of the scene that did not move cost nothing; a scrolling
             * scene changes every band and gains nothing, but loses nothing
             * either. Every 30th frame is sent whole regardless, so a hash
             * collision or a panel glitch can never stick for long. */
            int band = y / CHUNK_LINES;
            if (!sForceFull && h == sBandHash[band]) {
                xSemaphoreGive(sTransDone); // took a buffer, sending nothing
                sotn_lcd_skipped++;
                __asm__ volatile("rsr.ccount %0" : "=a"(t2));
                wait += t1 - t0;
                conv += t2 - t1;
                continue;
            }
            sBandHash[band] = h;
        } else {
            convertChunkStride(sDirectSrc + y * VRAM_STRIDE, sChunk[buf],
                               CHUNK_LINES);
        }
        __asm__ volatile("rsr.ccount %0" : "=a"(t2));
        wait += t1 - t0;
        conv += t2 - t1;
        /* The return value is CHECKED, and that is the other half of the same
         * bug: a failed submission produces no transfer, so no completion
         * callback ever runs, so the buffer this loop just took is never
         * handed back. Ignoring the error therefore leaks one count per
         * failure until the wait above blocks permanently. Give it back by
         * hand when the submission did not happen. */
        if (esp_lcd_panel_draw_bitmap(sPanel, PILLAR_X, y, PILLAR_X + PSX_W,
                                      y + CHUNK_LINES, sChunk[buf]) != ESP_OK) {
            sotn_lcd_sendfails++;
            xSemaphoreGive(sTransDone);
        }
        buf ^= 1;
    }
    __asm__ volatile("rsr.ccount %0" : "=a"(tB));
    sotn_lcd_wait_us = wait / 240; // 240 MHz
    sotn_lcd_conv_us = conv / 240;
    sotn_lcd_total_us = (tB - tA) / 240;
    // drain: both chunk buffers free <=> no panel DMA left on the bus
    int got = 0;
    for (int i = 0; i < 2; i++) {
        if (xSemaphoreTake(sTransDone, pdMS_TO_TICKS(300)) == pdTRUE) got++;
    }
    for (int i = 0; i < got; i++) xSemaphoreGive(sTransDone);
    xSemaphoreGive(sotn_spi_mux);
}

// F4: scanout runs on core 1 so the game loop on core 0 never waits for SPI.
// The game double-buffers in VRAM (display area != draw area), so converting
// the displayed buffer while the other one is being drawn is tear-free by
// construction — same contract as the real PSX CRT scanout.
static TaskHandle_t sScanTask;
static const uint16_t* volatile sScanSrc;

static inline void convertChunkStride(const uint16_t* restrict src,
                                      uint16_t* restrict dst, int rows) {
    uint32_t* restrict d = (uint32_t*)dst;
    for (int y = 0; y < rows; y++) {
        const uint16_t* row = src + y * VRAM_STRIDE;
        for (int x = 0; x < PSX_W; x += 2) {
            uint16_t c0 = row[x], c1 = row[x + 1];
            uint16_t p0 = ((c0 & 0x1F) << 11) | (((c0 >> 5) & 0x1F) << 6) |
                          ((c0 >> 10) & 0x1F);
            uint16_t p1 = ((c1 & 0x1F) << 11) | (((c1 >> 5) & 0x1F) << 6) |
                          ((c1 >> 10) & 0x1F);
            p0 = (p0 >> 8) | (p0 << 8);
            p1 = (p1 >> 8) | (p1 << 8);
            *d++ = (uint32_t)p0 | ((uint32_t)p1 << 16);
        }
    }
}

static void dmaTask(void* arg) {
    (void)arg;
    for (;;) {
        int idx;
        if (xQueueReceive(sScanQ, &idx, portMAX_DELAY) == pdTRUE) {
            presentSync(idx);
        }
    }
}

// Copy stage: takes the coherent snapshot and releases the game immediately,
// then hands the buffer to the DMA stage. Two snapshots mean the copy never
// waits for the panel transfer of the previous frame (that serialisation
// cost 10ms/frame when both lived in one task).
static int sLastSent = 1; // buffer most recently handed to the DMA stage
static void copyTask(void* arg) {
    (void)arg;
    int cur = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint16_t* src = sScanSrc;
        if (!src) {
            continue;
        }
        /* Copy only when the queue is EMPTY, into the buffer that was NOT sent
         * last. With a one-deep queue, an empty queue means the DMA stage is
         * scanning at most the last buffer sent, so its sibling is free. The
         * old rule (copy into `cur` whatever happened, flip on accept) let a
         * copy overwrite the buffer still being transmitted whenever a frame
         * was dropped: the panel received bands from two frames, which on a
         * horizontally scrolling scene read as rectangles sliding sideways.
         * A frame that would be dropped is now dropped BEFORE the copy —
         * which also saves the ~12 ms copy on every dropped frame. */
        if (sotn_lcd_snapshot && uxQueueMessagesWaiting(sScanQ) > 0) {
            sotn_lcd_drops++;
            xSemaphoreGive(sSnapDone);
            goto report;
        }
        cur = sLastSent ^ 1;
        if (!sotn_lcd_freeze && sotn_lcd_snapshot) {
            /* The WHOLE frame, every time. This used to copy only the rows
             * Soft_VisibleRows reported — but that is the rasterizer's
             * CURRENT drawing clip, in absolute VRAM coordinates, sampled at
             * whatever moment this task happens to run. When an effect narrows
             * the clip to a small rectangle, or the next frame's draw area sits
             * off the displayed rows, the copy grabbed a few rows or none and
             * the panel kept showing a stale snapshot while the game ran on
             * underneath: the handheld "froze" during effects, with every game
             * number healthy. The Waveshare board never took this path (no
             * snapshot there), which is why it hid for so long. */
            int vy0 = 0, vy1 = PSX_H - 1;
            unsigned t0, t1;
            __asm__ volatile("rsr.ccount %0" : "=a"(t0));
            for (int y = vy0; y <= vy1; y++) {
                memcpy(&sSnap[cur][y][0], src + y * VRAM_STRIDE, PSX_W * 2);
            }
            __asm__ volatile("rsr.ccount %0" : "=a"(t1));
            sotn_snap_cycles += t1 - t0;
        }
        sDirectSrc = src;
        xSemaphoreGive(sSnapDone); // the game may draw again
        int idx = cur;
        if (xQueueSend(sScanQ, &idx, 0) == pdTRUE) {
            sLastSent = idx;
        } else {
            // The DMA stage is still busy with the previous frame, so this one
            // is dropped. Counted rather than ignored because it is the direct
            // measure of the panel being slower than the game: with the direct
            // (non-snapshot) path the game is simultaneously redrawing the
            // buffer still being transmitted, which is what flicker IS. A
            // handful of drops is normal jitter; a steady rate near the frame
            // rate means the SPI clock is too low for the resolution.
            sotn_lcd_drops++;
        }
    report:
        if (++sNotifies % 120 == 0) {
            printf("sotn: lcd %u offered, %u dropped (%u%%) | "
                   "total %u us (wait %u + convert %u + send %u) | "
                   "lost %u failed %u | bands skipped %u\n",
                   sNotifies, sotn_lcd_drops, sotn_lcd_drops * 100 / sNotifies,
                   sotn_lcd_total_us, sotn_lcd_wait_us, sotn_lcd_conv_us,
                   sotn_lcd_total_us - sotn_lcd_wait_us - sotn_lcd_conv_us,
                   sotn_lcd_timeouts, sotn_lcd_sendfails, sotn_lcd_skipped);
        }
    }
}

void lcd_present(const uint16_t* src) {
    if (!sChunk[0]) {
        return; // init failed or not run: stay headless
    }
    if (!sScanTask) {
        xTaskCreatePinnedToCore(dmaTask, "lcd_dma", 4096, NULL, 4, NULL, 1);
        xTaskCreatePinnedToCore(copyTask, "lcd_copy", 4096, NULL, 6,
                                &sScanTask, 1);
    }
    sScanSrc = src;
    // pending notifications coalesce: if core 1 is mid-frame we simply drop
    // to its pace instead of queueing stale frames
    xTaskNotifyGive(sScanTask);
}

// Called by the game thread after its frame-pacing delay: the copy on core 1
// normally finished long before (it overlaps the wait), so this is free on
// light frames and only costs the leftover on heavy ones - but it guarantees
// the game never draws into VRAM while the snapshot is still being taken.
void lcd_wait_snapshot(void) {
    if (sSnapDone && sScanTask) {
        xSemaphoreTake(sSnapDone, pdMS_TO_TICKS(50));
    }
}
