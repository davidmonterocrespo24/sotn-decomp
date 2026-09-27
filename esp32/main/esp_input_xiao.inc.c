/* Controls for the XIAO ESP32S3 Sense handheld.
 *
 * Eleven pins total, and the controls get four: two buttons with a pin each,
 * six more sharing one ADC pin through a resistor ladder, and an analog stick
 * on two. Included by esp_shim.c when SOTN_BOARD_XIAO is set; the Waveshare
 * board's ten direct GPIOs live there instead.
 *
 * Every constant lives in board_pins_xiao.h and every one of them was measured
 * on the assembled unit rather than assumed. The two that look arbitrary --
 * the dead zone and the averaging count -- are the ones that took longest to
 * get right.
 */

#include "esp_adc/adc_oneshot.h"
#include "freertos/task.h"

static adc_oneshot_unit_handle_t sAdc;

/* Stick calibration, learned at boot and refined as the stick is used.
 *
 * A drone gimbal is not a console stick: it rests wherever its springs leave
 * it (2317 on this unit, not the 2048 a mid-scale assumption gives) and its
 * throw covers only part of the range. Mapping the raw reading straight to a
 * direction therefore leaves Alucard walking at rest and unable to reach full
 * speed. Centre is whatever the stick reads at boot, and each half is scaled
 * by the travel actually seen in that direction, so it self-corrects as the
 * player moves. */
static struct {
    int cx, cy;     /* rest position, sampled once at boot */
    int lo_x, hi_x; /* travel seen so far, per axis        */
    int lo_y, hi_y;
    int ready;
} sCal;

/* The ADC is sampled on its own task, never from the frame path.
 *
 * adc_oneshot_read takes the driver's mutex and waits out the conversion.
 * Twenty-four conversions per frame is a millisecond of the frame budget
 * spent waiting on an ADC, and in the MGS port on this same hardware the
 * equivalent call froze the cooperative scheduler outright. A plain task
 * samples at its own pace and publishes three integers; the poll only reads
 * them, which cannot block. */
static volatile int sRawX, sRawY, sRawLadder;

/* raw x, raw y, centre x, centre y, peak deflection x, peak deflection y,
 * ladder reading, any pad bits seen -- for the diagnostic line below */
int sotn_stick_dbg[8];
/* Counted every loop so a reading of zero can be told apart from a sampler
 * that never started. Without it those two look identical, and they need
 * opposite fixes: one is wiring, the other is a failed task creation. */
int sotn_adc_loops;

static void xiaoAdcTask(void* arg) {
    (void)arg;
    for (;;) {
        sotn_adc_loops++;
        int sx = 0, sy = 0, sl = 0;
        for (int i = 0; i < STICK_SAMPLES; i++) {
            int a = 0, b = 0, c = 0;
            adc_oneshot_read(sAdc, PIN_STICK_X - 1, &a);
            adc_oneshot_read(sAdc, PIN_STICK_Y - 1, &b);
            adc_oneshot_read(sAdc, PIN_BTN_LADDER - 1, &c);
            sx += a;
            sy += b;
            sl += c;
        }
        sRawX = sx / STICK_SAMPLES;
        sRawY = sy / STICK_SAMPLES;
        sRawLadder = sl / STICK_SAMPLES;
        /* 10 ms: faster than the game reads the pad at any frame rate it
         * reaches here, and slow enough to cost nothing. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void xiaoInputInit(void) {
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &sAdc));
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12,
                                   .bitwidth = ADC_BITWIDTH_12};
    /* ADC1 channel N is GPIO N+1 on the S3, which is why these had to be
     * D0/D1/D2: D6 and D7 have no ADC at all, and putting the ladder there
     * was an early mistake that cost a rewire. */
    adc_oneshot_config_channel(sAdc, PIN_STICK_X - 1, &ccfg);
    adc_oneshot_config_channel(sAdc, PIN_STICK_Y - 1, &ccfg);
    adc_oneshot_config_channel(sAdc, PIN_BTN_LADDER - 1, &ccfg);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_ATTACK) | (1ULL << PIN_BTN_JUMP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* Pinned to core 1: the game loop owns core 0 and this has no business
     * competing with it.
     *
     * 4 KB of stack, and the result is CHECKED. Both matter: adc_oneshot_read
     * goes through the driver's locking and a short stack fails creation
     * outright, and a silent failure here surfaces much later as every control
     * reading zero -- indistinguishable from a wiring fault. This very line
     * went missing once in the MGS port and cost a round trip diagnosing "the
     * stick does not work" when the sampler simply did not exist. */
    BaseType_t ok =
        xTaskCreatePinnedToCore(xiaoAdcTask, "xiao_adc", 4096, NULL, 4, NULL, 1);
    printf("sotn: XIAO pad -- ladder on D2, attack D5, jump D7, stick "
           "D0/D1 -- sampler %s\n",
           ok == pdPASS ? "running" : "FAILED TO START");
}

/* One axis, raw counts -> -256..+256, dead zone applied.
 *
 * Subtracting the dead zone from both sides of the fraction rather than just
 * clamping changes how the stick feels: motion starts from zero as the player
 * leaves the centre instead of jumping to a step. */
static int xiaoAxis(int raw, int centre, int lo, int hi, int invert) {
    int d = raw - centre;
    int span = (d >= 0) ? (hi - centre) : (centre - lo);
    if (span < 500) span = 500; /* before the stick has been swept */
    int a = d < 0 ? -d : d;
    if (a <= STICK_DEADZONE) return 0;
    int v = ((a - STICK_DEADZONE) * 256) / (span - STICK_DEADZONE);
    if (v > 256) v = 256;
    if (d < 0) v = -v;
    return invert ? -v : v;
}

/* Build the psyz pad word from this board's controls.
 * Bit order: 0 L2, 1 R2, 2 L1, 3 R1, 4 TRIANGLE, 5 CIRCLE, 6 CROSS,
 *            7 SQUARE, 8 SELECT, 9 L3, 10 R3, 11 START, 12 UP, 13 RIGHT,
 *            14 DOWN, 15 LEFT. */
static unsigned xiaoReadPad(void) {
    unsigned pressed = 0;

    /* --- six buttons on one ADC pin -----------------------------------
     * A ladder reports only the lowest-resistance button held, so whatever
     * lands here is single-press by construction. That is why attack and jump
     * are NOT here. */
    int lad = sRawLadder;
    if      (lad < LADDER_W1_MAX) pressed |= 1u << 5;   /* CIRCLE   */
    else if (lad < LADDER_W2_MAX) pressed |= 1u << 3;   /* R1       */
    else if (lad < LADDER_W3_MAX) pressed |= 1u << 4;   /* TRIANGLE */
    else if (lad < LADDER_W4_MAX) pressed |= 1u << 2;   /* L1       */
    else if (lad < LADDER_W5_MAX) pressed |= 1u << 11;  /* START    */
    else if (lad < LADDER_W6_MAX) pressed |= 1u << 8;   /* SELECT   */

    /* --- the two with a pin to themselves -----------------------------
     * Alucard attacks out of a jump constantly, so these two must be able to
     * register together; on the ladder one of them would silently vanish. */
    if (!gpio_get_level(PIN_BTN_ATTACK)) pressed |= 1u << 7; /* SQUARE */
    if (!gpio_get_level(PIN_BTN_JUMP))   pressed |= 1u << 6; /* CROSS  */

    /* --- stick -> the d-pad the game reads ---------------------------- */
    int rx = sRawX;
    int ry = sRawY;
    if (!rx && !ry) return pressed; /* the sampler has not run yet */
    if (!sCal.ready) {
        sCal.cx = sCal.lo_x = sCal.hi_x = rx;
        sCal.cy = sCal.lo_y = sCal.hi_y = ry;
        sCal.ready = 1;
        printf("sotn: stick centre X %d Y %d\n", rx, ry);
    }
    if (rx < sCal.lo_x) sCal.lo_x = rx;
    if (rx > sCal.hi_x) sCal.hi_x = rx;
    if (ry < sCal.lo_y) sCal.lo_y = ry;
    if (ry > sCal.hi_y) sCal.hi_y = ry;

    int ax = xiaoAxis(rx, sCal.cx, sCal.lo_x, sCal.hi_x, STICK_INVERT_X);
    int ay = xiaoAxis(ry, sCal.cy, sCal.lo_y, sCal.hi_y, STICK_INVERT_Y);

    /* The game reads a digital pad, so the analog reading becomes direction
     * bits. Half deflection is the threshold: low enough to turn without
     * fighting the stick, high enough that a diagonal needs real intent.
     *
     * Down matters more here than it did in MGS: crouch, the down+attack
     * slide, and every descend-through-platform input go through it, so the
     * threshold must be reachable without bottoming the gimbal out. Half
     * deflection on a calibrated span is. */
    if (ax >  128) pressed |= 1u << 13; /* RIGHT */
    if (ax < -128) pressed |= 1u << 15; /* LEFT  */
    if (ay >  128) pressed |= 1u << 12; /* UP    */
    if (ay < -128) pressed |= 1u << 14; /* DOWN  */

    /* Published for the diagnostic line. Raw counts, the calibrated centre and
     * the resulting deflection together say which stage is wrong: a raw value
     * that never moves is wiring, a centre far from the raw is a bad
     * calibration, and a deflection that never crosses the threshold is
     * scaling.
     *
     * Deflection is the LARGEST seen since the last report, not the value at
     * the instant of printing: a stick is pushed and released in well under
     * the reporting interval, so sampling instantaneously caught it at rest
     * every single time and looked like a dead axis. The reader clears these. */
    sotn_stick_dbg[0] = rx;
    sotn_stick_dbg[1] = ry;
    sotn_stick_dbg[2] = sCal.cx;
    sotn_stick_dbg[3] = sCal.cy;
    if ((ax < 0 ? -ax : ax) > sotn_stick_dbg[4])
        sotn_stick_dbg[4] = ax < 0 ? -ax : ax;
    if ((ay < 0 ? -ay : ay) > sotn_stick_dbg[5])
        sotn_stick_dbg[5] = ay < 0 ? -ay : ay;
    sotn_stick_dbg[6] = lad;
    sotn_stick_dbg[7] |= (int)pressed;

    return pressed;
}
