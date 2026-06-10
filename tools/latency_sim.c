/* Host-side latency model for the ambilight capture/smoothing pipeline.
 *
 * Compares the two side-edge capture schedulers and the two smoothing
 * policies in firmware terms, without hardware:
 *
 *   schedulers:  BANDS    - TOP, RIGHT(one 1280x16 band), BOTTOM, repeat;
 *                           each band updates one right zone and the
 *                           mirrored left zone (legacy production path)
 *                VERTICAL - TOP, RIGHT(28x720), BOTTOM, LEFT(28x720);
 *                           every zone refreshes each 4-capture cycle
 *                           (DCMI_SIDE_VERTICAL_CROPS)
 *
 *   smoothing:   OLD - weak 1/4, med 1/2, strong 3/4; scene cuts ease at 3/4
 *                NEW - same ladder, but per-zone cuts (delta >= 96) snap to
 *                      the raw color, and a capture-wide cut (>= 1/3 of
 *                      sampled zones past delta 32, >= 8 zones) snaps all
 *
 * Capture timing model: one capture per source frame slot (16.7 ms at 60 Hz
 * link). Analysis/restart overhead is ignored (sub-millisecond after the
 * LUT fast path and zero restart delay), which slightly flatters BOTH
 * configurations equally.
 *
 * Scenarios:
 *   saber-sweep - a 3-zone bright saber blob sweeps the left edge bottom to
 *                 top in 600 ms; reports mean/max tracking error of the
 *                 left-edge LEDs vs the screen, and how much of the saber's
 *                 brightness the LEDs ever reproduced.
 *   scene-cut   - the whole perimeter goes dark to orange; reports how long
 *                 until 50% / 90% / 100% of zones are within delta 16.
 *
 * Build and run:  cc -O2 -Wall -o latency_sim latency_sim.c && ./latency_sim
 */
#include <stdio.h>

#define ZONE_COUNT 136
#define RIGHT_OFF 0
#define RIGHT_CNT 25
#define TOP_OFF 25
#define TOP_CNT 43
#define LEFT_OFF 68
#define LEFT_CNT 25
#define BOTTOM_OFF 93
#define BOTTOM_CNT 43

#define CAPTURE_MS 16.7

/* Firmware smoothing constants (dcmi_capture.c). */
#define DELTA_STRONG_MAX 8
#define DELTA_MED_MAX 32
#define SCENE_CUT_DELTA 96
#define GLOBAL_CUT_MIN_ZONES 8

typedef struct { int r, g, b; } rgb_t;

typedef enum { SCHED_BANDS, SCHED_VERTICAL } sched_t;
typedef enum { SMOOTH_OLD, SMOOTH_NEW } smooth_t;

static rgb_t screen[ZONE_COUNT];   /* what the TV edge shows right now */
static rgb_t led[ZONE_COUNT];      /* smoothed zone state (the LEDs)    */

static int imax3(int a, int b, int c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
static int iabs(int v) { return v < 0 ? -v : v; }

static int zone_delta(rgb_t a, rgb_t b)
{
    return imax3(iabs(a.r - b.r), iabs(a.g - b.g), iabs(a.b - b.b));
}

static int blend(int prev, int raw, int num, int den)
{
    return (prev * (den - num) + raw * num) / den;
}

/* Apply one capture: zones[] lists the zone indices this capture samples. */
static void apply_capture(const int *zones, int count, smooth_t mode)
{
    int big = 0, candidates = 0, force_snap = 0;

    if (mode == SMOOTH_NEW) {
        for (int i = 0; i < count; i++) {
            if (zone_delta(screen[zones[i]], led[zones[i]]) > DELTA_MED_MAX) {
                big++;
            }
            candidates++;
        }
        if (candidates >= GLOBAL_CUT_MIN_ZONES && big * 3 >= candidates) {
            force_snap = 1;
        }
    }

    for (int i = 0; i < count; i++) {
        int z = zones[i];
        int delta = zone_delta(screen[z], led[z]);
        int num = 1, den = 4; /* weak */

        if (mode == SMOOTH_NEW && (force_snap || delta >= SCENE_CUT_DELTA)) {
            num = 1; den = 1; /* snap */
        } else if (delta >= SCENE_CUT_DELTA) {
            num = 3; den = 4; /* OLD: cuts ease at strong */
        } else if (delta > DELTA_MED_MAX) {
            num = 3; den = 4; /* strong */
        } else if (delta > DELTA_STRONG_MAX) {
            num = 1; den = 2; /* medium */
        }

        led[z].r = blend(led[z].r, screen[z].r, num, den);
        led[z].g = blend(led[z].g, screen[z].g, num, den);
        led[z].b = blend(led[z].b, screen[z].b, num, den);
    }
}

/* One scheduler step; returns number of zones captured into zones[]. */
static int scheduler_step(sched_t sched, int step, int *zones)
{
    int n = 0;

    if (sched == SCHED_BANDS) {
        int phase = step % 3;       /* TOP, RIGHT band, BOTTOM */
        int band = (step / 3) % RIGHT_CNT;

        if (phase == 0) {
            for (int i = 0; i < TOP_CNT; i++) zones[n++] = TOP_OFF + i;
        } else if (phase == 1) {
            zones[n++] = RIGHT_OFF + band;
            zones[n++] = LEFT_OFF + (LEFT_CNT - 1 - band); /* mirrored */
        } else {
            for (int i = 0; i < BOTTOM_CNT; i++) zones[n++] = BOTTOM_OFF + i;
        }
    } else {
        int phase = step % 4;       /* TOP, RIGHT, BOTTOM, LEFT */

        if (phase == 0) {
            for (int i = 0; i < TOP_CNT; i++) zones[n++] = TOP_OFF + i;
        } else if (phase == 1) {
            for (int i = 0; i < RIGHT_CNT; i++) zones[n++] = RIGHT_OFF + i;
        } else if (phase == 2) {
            for (int i = 0; i < BOTTOM_CNT; i++) zones[n++] = BOTTOM_OFF + i;
        } else {
            for (int i = 0; i < LEFT_CNT; i++) zones[n++] = LEFT_OFF + i;
        }
    }
    return n;
}

static void fill_screen(rgb_t c)
{
    for (int z = 0; z < ZONE_COUNT; z++) screen[z] = c;
}

/* --- scenario 1: saber sweep along the left edge --------------------- */
static void run_saber(sched_t sched, smooth_t mode, const char *label)
{
    const rgb_t bg = {8, 8, 16};
    const rgb_t saber = {40, 200, 255};
    const double sweep_ms = 600.0;
    double err_sum = 0.0;
    int err_n = 0, err_max = 0;
    int peak_led_g = 0; /* brightest green the left LEDs ever reached */

    fill_screen(bg);
    for (int z = 0; z < ZONE_COUNT; z++) led[z] = bg;

    for (int step = 0; step * CAPTURE_MS < sweep_ms + 400.0; step++) {
        double t = step * CAPTURE_MS;
        int zones[ZONE_COUNT];
        int n;

        /* saber blob: 3 zones wide, sweeping bottom (index 24) to top (0) */
        fill_screen(bg);
        if (t < sweep_ms) {
            int center = LEFT_CNT - 1 - (int)((t / sweep_ms) * (LEFT_CNT - 1));
            for (int k = -1; k <= 1; k++) {
                int zi = center + k;
                if (zi >= 0 && zi < LEFT_CNT) screen[LEFT_OFF + zi] = saber;
            }
        }

        n = scheduler_step(sched, step, zones);
        apply_capture(zones, n, mode);

        /* tracking error across the left edge while the saber is on screen */
        if (t < sweep_ms) {
            for (int i = 0; i < LEFT_CNT; i++) {
                int d = zone_delta(screen[LEFT_OFF + i], led[LEFT_OFF + i]);
                err_sum += d;
                err_n++;
                if (d > err_max) err_max = d;
            }
            for (int i = 0; i < LEFT_CNT; i++) {
                if (led[LEFT_OFF + i].g > peak_led_g) peak_led_g = led[LEFT_OFF + i].g;
            }
        }
    }

    printf("  %-22s mean err %5.1f   max err %3d   saber peak on LEDs %3d/%d\n",
           label, err_sum / err_n, err_max, peak_led_g, saber.g);
}

/* --- scenario 2: full-perimeter scene cut ----------------------------- */
static void run_cut(sched_t sched, smooth_t mode, const char *label)
{
    const rgb_t before = {10, 10, 20};
    const rgb_t after = {220, 120, 30};
    double t50 = -1, t90 = -1, t100 = -1;

    fill_screen(before);
    for (int z = 0; z < ZONE_COUNT; z++) led[z] = before;

    /* settle, then cut at t=0 */
    fill_screen(after);

    for (int step = 0; step * CAPTURE_MS < 3000.0; step++) {
        int zones[ZONE_COUNT];
        int n = scheduler_step(sched, step, zones);
        int settled = 0;
        double t = (step + 1) * CAPTURE_MS;

        apply_capture(zones, n, mode);

        for (int z = 0; z < ZONE_COUNT; z++) {
            if (zone_delta(screen[z], led[z]) <= 16) settled++;
        }
        if (t50 < 0 && settled * 2 >= ZONE_COUNT) t50 = t;
        if (t90 < 0 && settled * 10 >= ZONE_COUNT * 9) t90 = t;
        if (t100 < 0 && settled == ZONE_COUNT) { t100 = t; break; }
    }

    printf("  %-22s 50%% in %6.0f ms   90%% in %6.0f ms   100%% in %6.0f ms\n",
           label, t50, t90, t100);
}

int main(void)
{
    printf("Ambilight latency model (capture slot %.1f ms)\n\n", CAPTURE_MS);

    printf("Saber sweep, left edge, 600 ms (lower err = LEDs track the saber):\n");
    run_saber(SCHED_BANDS, SMOOTH_OLD, "bands + old smoothing");
    run_saber(SCHED_BANDS, SMOOTH_NEW, "bands + new smoothing");
    run_saber(SCHED_VERTICAL, SMOOTH_OLD, "vertical + old");
    run_saber(SCHED_VERTICAL, SMOOTH_NEW, "vertical + new");

    printf("\nFull-perimeter scene cut (time until zones within delta 16):\n");
    run_cut(SCHED_BANDS, SMOOTH_OLD, "bands + old smoothing");
    run_cut(SCHED_BANDS, SMOOTH_NEW, "bands + new smoothing");
    run_cut(SCHED_VERTICAL, SMOOTH_OLD, "vertical + old");
    run_cut(SCHED_VERTICAL, SMOOTH_NEW, "vertical + new");

    return 0;
}
