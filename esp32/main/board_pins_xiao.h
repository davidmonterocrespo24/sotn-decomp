// XIAO ESP32S3 Sense + ILI9341 320x240 SPI panel — the handheld console.
//
// This is the SAME physical unit the MGS port runs on, so every pin number
// here was already verified on hardware with the shakedown firmware in that
// project (mgs_reversing/port/consola/): the panel's controller, its rotation,
// the button ladder's steps, the stick's rest point and travel, and the
// microSD's chip select. Nothing below is copied from a datasheet.
//
// The XIAO exposes eleven pins, D0..D10, and all eleven are used. The
// silkscreen D-numbers are NOT the GPIO numbers; both are given because
// mixing them up is the easiest way to lose an afternoon here.
#pragma once

#include "driver/spi_master.h"
#include "hal/lcd_types.h"

// ---- LCD: ILI9341, 320x240 landscape, SPI2 (shared with the microSD) ----
//
// 320x240 matches the Waveshare panel exactly, so the pillarboxed 256-px PSX
// frame and the whole scanout path in lcd_esp32.c carry over unchanged.
#define LCD_SPI_HOST    SPI2_HOST
// 40 MHz — measured, not guessed. It went to 80 once and back:
//
// At 80 MHz the frame goes out faster, but on these flying jumper wires the
// signal does not hold: bytes slip, a whole 8-line band lands shifted
// sideways, and the screen shows rectangles sliding left and right that move
// every frame. Back at 40 the user confirmed the image clean ("se ve mucho
// mejor y no parpadea").
//
// 40 MHz is too slow for the panel to keep pace with a 60 fps game (a frame
// takes ~25 ms to send), which is why the scanout copies each finished frame
// first (sotn_lcd_snapshot=1 in lcd_esp32.c): the game can then lap the panel
// without it ever transmitting a half-redrawn buffer. Frames the panel cannot
// take are dropped, so the screen runs at roughly 30 fps while the game logic
// stays at full speed.
//
// Untried middle ground: 53 MHz (160 MHz / 3). Anything above 40 needs a
// soldered harness, not jumpers, to be trusted.
#define LCD_SPI_HZ      (40 * 1000 * 1000)

#define PIN_LCD_SCLK    7     // D8
#define PIN_LCD_MOSI    9     // D10
#define PIN_LCD_CS      4     // D3
#define PIN_LCD_DC      43    // D6  — also the chip's UART0 TX, see the note
#define PIN_LCD_RST     5     // D4  — a real reset line, not strapped
#define PIN_LCD_BL      (-1)  // backlight strapped to 3V3; no GPIO control
#define LCD_BL_ON_LEVEL 1

// DC sits on GPIO43, which is UART0 TX. That is fine as a plain GPIO but it
// means NOTHING may install the UART0 driver on this board: doing so hands the
// pin back to the UART peripheral and the panel stops receiving command/data
// framing. The console is USB-Serial-JTAG here for exactly this reason.

// RST is a dedicated pin and that is load-bearing: strapping it high and
// relying on the software reset left this panel dead, which is why the user's
// own proven ILI9341 project reserves a whole GPIO for it.

// Landscape, rotated 180: MADCTL 0xE8 = MY|MX|MV|BGR. Determined by looking at
// the assembled unit — the un-rotated setting read upside down in the case.
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_INVERT_COLOR false     // ILI9341 is not an IPS panel; no inversion
#define LCD_MIRROR_X     true
#define LCD_MIRROR_Y     true
#define LCD_GAP_X        0
#define LCD_GAP_Y        0

// ---- microSD: the Sense expansion board's own slot ----
//
// Shares the LCD's SPI bus (Seeed gives SCK/MISO/MOSI as D8/D9/D10) with its
// chip select on GPIO21, which is internal to the expansion board and never
// appears on the header. Verified mounting on this unit at 1910 MB.
#define PIN_SD_MISO     8     // D9
#define PIN_SD_CS       21    // internal to the Sense board

// The card holds EVERYTHING on this board, not just the XA audio: 8 MB of
// flash has no room for a 3.7 MB data partition beside the app, so the disc
// assets live on the card and the game chdir's to "/sd" instead of "/flash".
// That string is SOTN_DATA_ROOT, set in main/CMakeLists.txt rather than here
// because esp_fileapi.c needs it and deliberately includes no headers.

// ---- Controls ----
//
// Two buttons have a pin each; the other six share one ADC pin through a
// resistor ladder, which can only ever report the lowest-resistance press.
// So the pair that must work SIMULTANEOUSLY gets the dedicated pins, and for
// this game that pair is attack and jump — Alucard attacks out of a jump
// constantly, and a ladder would silently drop one of the two.
//
// This differs from the MGS build on the same hardware, where the dedicated
// pair is square and R1. Physically it is the same two buttons; only which
// PSX button they report changes, because the games hold different pairs.
#define PIN_BTN_ATTACK  6     // D5 — SQUARE, psyz bit 7
#define PIN_BTN_JUMP    44    // D7 — CROSS,  psyz bit 6
#define PIN_BTN_LADDER  3     // D2 — six buttons, ADC1_CH2

// Ladder windows, measured on the assembled unit with a 10k pull-up. Named by
// POSITION rather than by button because the resistor in each slot is fixed
// hardware while the PSX button it reports is a per-game choice (see above).
//   W1 0R -> 0      W2 2k2 -> 738   W3 4k7 -> 1310
//   W4 10k -> 2048  W5 22k -> 2816  W6 47k -> 3376   idle -> 4095
// Half a volt between steps, so 5% resistors are comfortable.
#define LADDER_W1_MAX    350
#define LADDER_W2_MAX   1000
#define LADDER_W3_MAX   1700
#define LADDER_W4_MAX   2450
#define LADDER_W5_MAX   3100
#define LADDER_W6_MAX   3700

// ---- Analog stick: a drone gimbal, not a console stick ----
//
// It rests wherever its springs leave it (2317 on this unit, not 2048) and its
// throw covers only part of the scale. Both are handled by calibrating at boot
// rather than assuming a range. The dead zone is four times the noise measured
// at rest; without it the character walked on its own.
#define PIN_STICK_X     1     // D0 — ADC1_CH0
#define PIN_STICK_Y     2     // D1 — ADC1_CH1
#define STICK_DEADZONE  60
// X reads backwards because of which end terminal went to 3V3 — a sign flip,
// not a resolder. Y does not: the screen-space negation that the shakedown
// firmware needed to draw its crosshair does NOT belong here, because the game
// is handed direction bits where up is simply up. Both confirmed on the unit.
#define STICK_INVERT_X  1
#define STICK_INVERT_Y  0
#define STICK_SAMPLES   8     // averaged per read; the ADC is noisy bare

// ---- No touch ----
// The panel has an XPT2046 but all eleven pins are spoken for. Its T_CS is
// strapped to 3V3, which is not optional: left floating it selects itself and
// drives the shared MISO line, corrupting card reads.
