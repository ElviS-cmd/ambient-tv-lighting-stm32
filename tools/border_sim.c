/* Host-side scenario test for the black-border detector in dcmi_capture.c.
 *
 * The state machine (border_process_capture) and the probe scheduling are
 * transcribed 1:1 from the firmware; the capture model assumes the vertical
 * side-crop scheduler (TOP, RIGHT, BOTTOM, LEFT, one capture per 16.7 ms
 * slot) and perfect transport. Each scenario asserts the behavior the
 * firmware must show:
 *
 *   1 fullscreen      - insets never move
 *   2 letterbox 90    - top/bottom lock onto the content boundary in < 10 s
 *   3 dark scene      - a long all-black scene does not disturb the insets
 *   4 fullscreen back - probes snap the insets out in < 6 s
 *   5 pillarbox 160   - left/right lock onto the content columns
 *
 * Build and run:  cc -O2 -Wall -o border_sim border_sim.c && ./border_sim
 */
#include <stdio.h>

#define EDGE_TOP 0
#define EDGE_RIGHT 1
#define EDGE_BOTTOM 2
#define EDGE_LEFT 3
#define EDGE_COUNT 4

#define VIDEO_W 1280
#define VIDEO_H 720
#define EDGE_BORDER 16
#define TOP_INSET 32
#define BOTTOM_INSET 32
#define SIDE_CROP_W 28
#define CAPTURE_MS 16.7

/* Firmware detector constants (dcmi_capture.c). */
#define BLACK_LEVEL 12
#define CONTENT_LEVEL 40
#define GROW_STREAK 45
#define WALK_STREAK 4
#define STEP 16
#define MAX_INSET_LINES 160
#define MAX_SIDE_INSET 192
#define PROBE_INTERVAL 64

#define CONTENT 180 /* brightness of visible picture in the model */

/* --- detector state (mirrors the firmware) --------------------------- */
static unsigned inset[EDGE_COUNT];
static unsigned black_streak[EDGE_COUNT];
static unsigned walking[EDGE_COUNT];
static unsigned capture_count[EDGE_COUNT];
static unsigned edge_level[EDGE_COUNT];
static unsigned grow_count, reset_count, giveup_count, probe_count,
                probe_discard_count;

static unsigned inset_limit(unsigned edge)
{
    return (edge == EDGE_TOP || edge == EDGE_BOTTOM) ? MAX_INSET_LINES
                                                     : MAX_SIDE_INSET;
}

/* Transcribed from border_process_capture(); candidate_zones is always
 * complete here (perfect transport). Returns 1 = discard capture. */
static int process_capture(unsigned edge, int probe, unsigned max_level)
{
    unsigned black = max_level <= BLACK_LEVEL;
    unsigned content_elsewhere = 0;

    for (unsigned o = 0; o < EDGE_COUNT; o++) {
        if (o != edge && edge_level[o] >= CONTENT_LEVEL) {
            content_elsewhere = 1;
        }
    }

    if (probe) {
        if (black) {
            probe_discard_count++;
            return 1;
        }
        inset[edge] = 0;
        black_streak[edge] = 0;
        walking[edge] = 0;
        edge_level[edge] = max_level;
        reset_count++;
        return 0;
    }

    edge_level[edge] = max_level;

    if (black && content_elsewhere && inset_limit(edge) > 0) {
        unsigned needed = walking[edge] ? WALK_STREAK : GROW_STREAK;

        black_streak[edge]++;
        if (black_streak[edge] >= needed) {
            black_streak[edge] = 0;
            if (inset[edge] + STEP <= inset_limit(edge)) {
                inset[edge] += STEP;
                walking[edge] = 1;
                grow_count++;
            } else {
                inset[edge] = 0;
                walking[edge] = 0;
                giveup_count++;
            }
        }
    } else {
        black_streak[edge] = 0;
        if (!black) {
            walking[edge] = 0;
        }
    }
    return 0;
}

/* --- screen / capture model ------------------------------------------ */
typedef struct {
    unsigned bar_top, bar_bottom;   /* letterbox bar heights in lines  */
    unsigned bar_left, bar_right;   /* pillarbox bar widths in pixels  */
    int dark_scene;                 /* whole picture black             */
} scene_t;

/* What the edge capture sees at a given (possibly probe-zeroed) inset. */
static unsigned capture_level(const scene_t *s, unsigned edge, unsigned eff_inset)
{
    if (s->dark_scene) {
        return 0;
    }
    switch (edge) {
    case EDGE_TOP:    /* band rows [inset+32, inset+48) from the top */
        return (eff_inset + TOP_INSET + EDGE_BORDER > s->bar_top) ? CONTENT : 0;
    case EDGE_BOTTOM:
        return (eff_inset + BOTTOM_INSET + EDGE_BORDER > s->bar_bottom) ? CONTENT : 0;
    case EDGE_RIGHT:  /* 28 columns ending at width - inset */
        return (eff_inset + SIDE_CROP_W > s->bar_right) ? CONTENT : 0;
    default:          /* EDGE_LEFT */
        return (eff_inset + SIDE_CROP_W > s->bar_left) ? CONTENT : 0;
    }
}

/* Run the vertical scheduler for duration_ms; returns elapsed captures. */
static void run(const scene_t *s, double duration_ms)
{
    int steps = (int)(duration_ms / CAPTURE_MS);

    for (int i = 0; i < steps; i++) {
        unsigned edge = (unsigned)(i % EDGE_COUNT);
        int probe = 0;
        unsigned eff_inset = inset[edge];

        capture_count[edge]++;
        if (inset[edge] > 0 && (capture_count[edge] % PROBE_INTERVAL) == 0) {
            probe = 1;
            probe_count++;
            eff_inset = 0;
        }

        process_capture(edge, probe, capture_level(s, edge, eff_inset));
    }
}

/* Time until a predicate holds, in ms (or -1 if it never does). */
static double run_until(const scene_t *s, double max_ms,
                        int (*done)(void))
{
    int steps = (int)(max_ms / CAPTURE_MS);

    for (int i = 0; i < steps; i++) {
        unsigned edge = (unsigned)(i % EDGE_COUNT);
        int probe = 0;
        unsigned eff_inset = inset[edge];

        capture_count[edge]++;
        if (inset[edge] > 0 && (capture_count[edge] % PROBE_INTERVAL) == 0) {
            probe = 1;
            probe_count++;
            eff_inset = 0;
        }

        process_capture(edge, probe, capture_level(s, edge, eff_inset));
        if (done()) {
            return (i + 1) * CAPTURE_MS;
        }
    }
    return -1;
}

static int failures;

static void check(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
    }
}

static int letterbox_locked(void)
{
    return inset[EDGE_TOP] == 48 && inset[EDGE_BOTTOM] == 48;
}

static int insets_clear(void)
{
    return inset[EDGE_TOP] == 0 && inset[EDGE_BOTTOM] == 0 &&
           inset[EDGE_LEFT] == 0 && inset[EDGE_RIGHT] == 0;
}

static int pillarbox_locked(void)
{
    return inset[EDGE_LEFT] == 144 && inset[EDGE_RIGHT] == 144;
}

int main(void)
{
    scene_t fullscreen = {0, 0, 0, 0, 0};
    scene_t letterbox = {90, 90, 0, 0, 0};   /* 2.39:1 in 720p */
    scene_t dark = {90, 90, 0, 0, 1};        /* dark scene, bars present */
    scene_t pillarbox = {0, 0, 160, 160, 0}; /* 4:3 in 720p */
    double t;

    printf("Scenario 1: fullscreen content, 30 s\n");
    run(&fullscreen, 30000);
    check(insets_clear() && grow_count == 0, "insets stay 0, no growth");

    printf("Scenario 2: letterbox appears (90-line bars), 60 s\n");
    t = run_until(&letterbox, 60000, letterbox_locked);
    check(t > 0 && t < 10000, "top/bottom lock at 48 lines in < 10 s");
    printf("    (locked in %.1f s)\n", t / 1000.0);
    run(&letterbox, 20000);
    check(letterbox_locked(), "insets stable while letterbox continues");
    check(inset[EDGE_LEFT] == 0 && inset[EDGE_RIGHT] == 0,
          "side insets untouched by letterbox");
    check(probe_discard_count > 0, "outward probes discarded, not published");

    printf("Scenario 3: 10 s dark scene during the letterboxed video\n");
    run(&dark, 10000);
    check(letterbox_locked() && giveup_count == 0,
          "dark scene does not move or reset the insets");

    printf("Scenario 4: back to fullscreen content\n");
    t = run_until(&fullscreen, 20000, insets_clear);
    check(t > 0 && t < 6000, "probes snap insets back to 0 in < 6 s");
    printf("    (cleared in %.1f s)\n", t / 1000.0);

    printf("Scenario 5: pillarbox (160-px bars), 60 s\n");
    t = run_until(&pillarbox, 60000, pillarbox_locked);
    check(t > 0 && t < 15000, "left/right lock at 144 px in < 15 s");
    printf("    (locked in %.1f s)\n", t / 1000.0);
    check(inset[EDGE_TOP] == 0 && inset[EDGE_BOTTOM] == 0,
          "top/bottom untouched by pillarbox");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
