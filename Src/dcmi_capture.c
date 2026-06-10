#include "dcmi_capture.h"

/* NOTE: these dimensions describe what we EXPECT from the source. Tune them
 * to match the actual Computer's/TFP401 resolution. They drive the crop windows for
 * each edge — wrong values mean we sample the wrong part of the frame. */
#define VIDEO_ACTIVE_WIDTH 1280U
#define VIDEO_ACTIVE_HEIGHT 720U
#define EDGE_BORDER_PIXELS 16U
#define DCMI_SIDE_CROP_WIDTH 28U
#define DCMI_TOP_CROP_INSET_LINES 32U
#define DCMI_BOTTOM_CROP_INSET_LINES 32U
#define DCMI_EDGE_TOP_BOTTOM_SAMPLES (VIDEO_ACTIVE_WIDTH * EDGE_BORDER_PIXELS)
#define DCMI_EDGE_SIDE_SAMPLES (DCMI_SIDE_CROP_WIDTH * VIDEO_ACTIVE_HEIGHT)
#define DCMI_CAPTURE_MAX_SAMPLES \
    ((DCMI_EDGE_TOP_BOTTOM_SAMPLES > DCMI_EDGE_SIDE_SAMPLES) ? \
      DCMI_EDGE_TOP_BOTTOM_SAMPLES : DCMI_EDGE_SIDE_SAMPLES)
#define DCMI_CAPTURE_MAX_WORDS (DCMI_CAPTURE_MAX_SAMPLES / 4U)
/* Re-arm the next crop immediately after a good capture; the source frame
 * cadence is the natural pacing. Back off briefly only after errors so a
 * persistent fault (no video, HAL busy) cannot spin the restart path hot.
 */
#define DCMI_RESTART_DELAY_MS 0U
#define DCMI_ERROR_RESTART_BACKOFF_MS 2U
#define DCMI_CAPTURE_TIMEOUT_MS 50U
/* Large edge crops need enough samples to cover the full LED geometry.
 * With 1280x720:
 * - complete side crop (28x720) = 5040 words; only the outer 16 pixels
 *   are used for LED color so the crop still represents the screen edge.
 * - top/bottom crop (1280x16) = 5120 words
 */
/* Responsive low-data mode:
 * accept partial captures sooner to keep LED updates flowing even when DCMI
 * intermittently under-fills a crop window. */
/* Keep acceptance thresholds high enough to reject noisy partial crops,
 * but still low enough to avoid visible lag when DCMI occasionally under-fills.
 */
#define DCMI_TOP_BOTTOM_EARLY_ACCEPT 1600U
#define DCMI_SIDE_EARLY_ACCEPT 1700U
#define DCMI_TIMEOUT_ACCEPT_FLOOR_TOP_BOTTOM 1200U
#define DCMI_TIMEOUT_ACCEPT_FLOOR_SIDE 1100U
#define DCMI_SYNC_WAIT_TIMEOUT_MS 50U
/* Set to 1 only when validating crop hardware with a fixed 1280x16 test
 * rectangle. Normal ambilight operation keeps crop enabled but lets the
 * perimeter scheduler cycle all four edge windows.
 */
#define DCMI_CROP_TEST_MODE 0U
#define DCMI_CROP_TEST_CAPTURE_TIMEOUT_MS 50U
#define DCMI_USE_CROP 1U
#define DCMI_PREARM_CAPTURE 1U
#define DCMI_LED_ACTIVITY_WINDOW_MS 1000U
/* Set to 1 to compile the per-sample debug statistics in analyze_buffer()
 * (bit histogram, checksums, byte min/max, coarse 4-zone averages). These
 * cost more CPU per capture than the LED color path itself, and that CPU
 * time is dead time between captures. Keep 0 for normal ambilight use.
 */
#define DCMI_DIAGNOSTICS 0U
/* Controlled experiment: repeatedly capture a narrow vertical side crop.
 * Keep the height configurable so transport reliability can be compared
 * directly between a complete 720-line side and a short 180-line segment.
 * Set to 0 to restore the working horizontal-band path.
 */
#define DCMI_FULL_HEIGHT_SIDE_TEST_MODE 1U
#define DCMI_FULL_HEIGHT_SIDE_TEST_EDGE 3U /* DCMI_EDGE_LEFT */
#define DCMI_FULL_HEIGHT_SIDE_TEST_HEIGHT 180U
#define DCMI_FULL_HEIGHT_SIDE_TEST_Y \
    ((VIDEO_ACTIVE_HEIGHT - DCMI_FULL_HEIGHT_SIDE_TEST_HEIGHT) / 2U)
#define DCMI_FULL_HEIGHT_SIDE_TEST_TIMEOUT_MS 50U
/* A 28-pixel side row is seven DMA words. Permit at most one missing tail
 * row while validating that the rest of the vertical crop is aligned.
 */
#define DCMI_FULL_HEIGHT_SIDE_TEST_MAX_MISSING_WORDS 7U
/* Production side-edge strategy (used when the transport test mode is off):
 * 1 = capture each side as one full-height vertical crop (28x720). All 25
 *     side zones refresh every 4-capture perimeter cycle (~70-100 ms)
 *     instead of one zone pair per 3-capture cycle (~1.2-1.5 s worst case).
 *     Enable only after the side transport test reports reliable full-height
 *     fills on the installed source (g_dcmi_side_test_full_count dominating).
 * 0 = proven fallback: 1280x16 horizontal side bands, one zone pair per
 *     capture. The TFP401/DCMI path fills short bands reliably; full-height
 *     side crops can cross VSYNC and drop to zero words on some sources.
 */
#define DCMI_SIDE_VERTICAL_CROPS 1U
#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE || DCMI_SIDE_VERTICAL_CROPS
#define DCMI_SIDE_HORIZONTAL_BANDS 0U
#else
#define DCMI_SIDE_HORIZONTAL_BANDS 1U
#endif
/* Incremental publishing: each accepted capture clears, re-derives and
 * publishes only the zones its crop touches, so untouched edges keep their
 * last good colors. Both production side strategies use it. The transport
 * test mode keeps the legacy whole-perimeter path, which by design never
 * publishes LED updates.
 */
#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE
#define DCMI_INCREMENTAL_PUBLISH 0U
#else
#define DCMI_INCREMENTAL_PUBLISH 1U
#endif
/* A partially filled vertical side crop leaves its lower rows - and their
 * LED zones - unsampled, so sides must deliver the (nearly) complete
 * rectangle. 140 words is 20 missing 28-pixel rows, well under one zone's
 * 29-row span, so at worst the bottom zone is slightly under-sampled.
 */
#define DCMI_SIDE_VERTICAL_TIMEOUT_MISSING_WORDS 140U
/* Black-border (letterbox/pillarbox) detection. Letterboxed video would
 * otherwise park the top/bottom LEDs on the black bars for the whole movie.
 * Each edge capture that comes back fully black - while another edge shows
 * content, so dark scenes do not trigger it - grows that edge's inset after
 * a sustained streak, walking the crop inward until it lands on picture.
 * Periodic outward probes at inset 0 snap the edge back the moment the bars
 * disappear; probe captures that still see black are discarded so the LEDs
 * keep their content colors.
 *
 * Levels are on the baseline-rescaled 0..255 scale. Streaks count accepted
 * captures of that edge (each edge is captured every 4th slot in vertical
 * mode, roughly 15 per second).
 */
#define DCMI_BLACK_BORDER_DETECT 1U
#define DCMI_BORDER_BLACK_LEVEL 12U
#define DCMI_BORDER_CONTENT_LEVEL 40U
#define DCMI_BORDER_GROW_STREAK 45U
#define DCMI_BORDER_WALK_STREAK 4U
#define DCMI_BORDER_STEP 16U
#define DCMI_BORDER_MAX_INSET_LINES 160U
#define DCMI_BORDER_MAX_SIDE_INSET_PIXELS 192U
#define DCMI_BORDER_PROBE_INTERVAL 64U
#if DCMI_BLACK_BORDER_DETECT && !DCMI_CROP_TEST_MODE && !DCMI_FULL_HEIGHT_SIDE_TEST_MODE
#define DCMI_BORDER_ACTIVE 1U
#else
#define DCMI_BORDER_ACTIVE 0U
#endif
#define DCMI_BOTTOM_TIMED_BAND 0U
#define DCMI_BOTTOM_START_DELAY_MS 8U
/* Adaptive temporal smoothing. alpha is the fraction of the new raw value:
 * - weak alpha: more stable, used for tiny frame-to-frame noise
 * - medium alpha: normal motion
 * - strong alpha: fast response for trusted large changes
 */
#define DCMI_SMOOTH_ALPHA_WEAK_NUM 1U
#define DCMI_SMOOTH_ALPHA_WEAK_DEN 4U
#define DCMI_SMOOTH_ALPHA_MED_NUM 1U
#define DCMI_SMOOTH_ALPHA_MED_DEN 2U
#define DCMI_SMOOTH_ALPHA_STRONG_NUM 3U
#define DCMI_SMOOTH_ALPHA_STRONG_DEN 4U
/* Snap alpha: take the raw value outright. Used for per-zone scene cuts and
 * capture-wide scene changes, where easing toward the new color only reads
 * as lag.
 */
#define DCMI_SMOOTH_ALPHA_SNAP_NUM 1U
#define DCMI_SMOOTH_ALPHA_SNAP_DEN 1U
/* Delta thresholds for selecting smoothing strength (0..255 channel scale). */
#define DCMI_SMOOTH_DELTA_STRONG_MAX 8U
#define DCMI_SMOOTH_DELTA_MED_MAX 32U
#define DCMI_SMOOTH_SCENE_CUT_DELTA 96U
/* Capture-wide scene change: when at least GLOBAL_CUT_NUM/GLOBAL_CUT_DEN of
 * the zones sampled by one capture move past the medium delta, treat the
 * whole capture as a cut - snap every sampled zone to its raw color and skip
 * per-zone spike rejection, since a coherent jump across many zones is real
 * content, not transport noise. Needs a minimum population so a 2-zone side
 * band cannot trigger it.
 */
#define DCMI_GLOBAL_CUT_MIN_ZONES 8U
#define DCMI_GLOBAL_CUT_NUM 1U
#define DCMI_GLOBAL_CUT_DEN 3U
/* Zones with very few samples are noisy; suppress large jumps there. */
#define DCMI_ZONE_LOW_TRUST_SAMPLES 10U
#define DCMI_ZONE_LOW_TRUST_SPIKE_DELTA 140U
/* Keep the previous value for a short miss streak, then fade gently. */
#define DCMI_ZONE_MISS_HOLD_FRAMES 2U
#define DCMI_ZONE_MISS_DECAY_NUM 3U
#define DCMI_ZONE_MISS_DECAY_DEN 4U
/* Avoid publishing low-quality perimeter cycles to the LED driver. */
#define DCMI_FRAME_MIN_GOOD_ZONES_TO_PUBLISH 48U
#define DCMI_FRAME_MIN_QUALITY_TO_PUBLISH 32U

/* Observed per-channel black-floor from the data bus (after RGB332 expand).
 * Subtract these so black on the source produces (0,0,0) and the WS2812 gate
 * threshold becomes meaningful. Tune by displaying full black and reading the
 * g_dcmi_zone*_r/g/b values. */
#define DCMI_BASELINE_R 45U
#define DCMI_BASELINE_G 0U
#define DCMI_BASELINE_B 0U

/* Minimum samples per zone to consider data valid. Prevents stale color data
 * from previous frames when a zone doesn't receive enough samples in current frame. */
#define DCMI_ZONE_MIN_SAMPLES 4U

#define DCMI_RIGHT_ZONE_COUNT 25U
#define DCMI_TOP_ZONE_COUNT 43U
#define DCMI_LEFT_ZONE_COUNT 25U
#define DCMI_BOTTOM_ZONE_COUNT 43U

#define DCMI_RIGHT_ZONE_OFFSET 0U
#define DCMI_TOP_ZONE_OFFSET (DCMI_RIGHT_ZONE_OFFSET + DCMI_RIGHT_ZONE_COUNT)
#define DCMI_LEFT_ZONE_OFFSET (DCMI_TOP_ZONE_OFFSET + DCMI_TOP_ZONE_COUNT)
#define DCMI_BOTTOM_ZONE_OFFSET (DCMI_LEFT_ZONE_OFFSET + DCMI_LEFT_ZONE_COUNT)

/* RGB332 expand lookup tables. Hoisted out of the inner loop. */
static const uint8_t r3_to_8bit[8] = {
    0, 36, 73, 109, 146, 182, 219, 255
};
static const uint8_t g3_to_8bit[8] = {
    0, 36, 73, 109, 146, 182, 219, 255
};
static const uint8_t b2_to_8bit[4] = {
    0, 85, 170, 255
};

extern DCMI_HandleTypeDef hdcmi;

typedef enum {
    DCMI_CAPTURE_NOT_CONFIGURED = 0,
    DCMI_CAPTURE_READY,
    DCMI_CAPTURE_RUNNING,
    DCMI_CAPTURE_ERROR
} dcmi_capture_state_t;

typedef struct {
    dcmi_capture_state_t state;
    uint32_t frames_seen;
    uint32_t dma_half_callbacks;
    uint32_t dma_full_callbacks;
    uint32_t errors;
} dcmi_capture_status_t;

typedef enum {
    DCMI_EDGE_TOP = 0,
    DCMI_EDGE_RIGHT,
    DCMI_EDGE_BOTTOM,
    DCMI_EDGE_LEFT,
    DCMI_EDGE_COUNT
} dcmi_edge_t;

#define DCMI_PERIMETER_COMPLETE_MASK ((1UL << DCMI_EDGE_COUNT) - 1U)

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t zone;
} dcmi_edge_crop_t;

/* Project-space crop rectangles use a Cartesian TV model:
 *   origin = bottom-left, +x = right, +y = up.
 *
 * DCMI hardware still uses video coordinates:
 *   origin = top-left, +x = right, +y = down.
 *
 * start_snapshot() converts cartesian y to DCMI y before programming the crop.
 */
static const dcmi_edge_crop_t edge_crops[DCMI_EDGE_COUNT] = {
    [DCMI_EDGE_TOP] = {0U, VIDEO_ACTIVE_HEIGHT - EDGE_BORDER_PIXELS - DCMI_TOP_CROP_INSET_LINES, VIDEO_ACTIVE_WIDTH, EDGE_BORDER_PIXELS, 1U},
    [DCMI_EDGE_RIGHT] = {VIDEO_ACTIVE_WIDTH - DCMI_SIDE_CROP_WIDTH, 0U, DCMI_SIDE_CROP_WIDTH, VIDEO_ACTIVE_HEIGHT, 0U},
    [DCMI_EDGE_BOTTOM] = {0U, DCMI_BOTTOM_CROP_INSET_LINES, VIDEO_ACTIVE_WIDTH, EDGE_BORDER_PIXELS, 3U},
    [DCMI_EDGE_LEFT] = {0U, 0U, DCMI_SIDE_CROP_WIDTH, VIDEO_ACTIVE_HEIGHT, 2U},
};

static volatile uint32_t g_dcmi_state;
static volatile uint32_t g_dcmi_start_status;
static volatile uint32_t g_dcmi_frame_count;
static volatile uint32_t g_dcmi_error_count;
static volatile uint32_t g_dcmi_timeout_count;
static volatile uint32_t g_dcmi_restart_count;
#if DCMI_DIAGNOSTICS
static volatile uint32_t g_dcmi_first_word;
static volatile uint32_t g_dcmi_second_word;
static volatile uint32_t g_dcmi_last_word;
static volatile uint32_t g_dcmi_nonzero_words;
static volatile uint32_t g_dcmi_changing_words;
static volatile uint32_t g_dcmi_buffer_checksum;
static volatile uint32_t g_dcmi_sample_count;
static volatile uint32_t g_dcmi_average_byte;
static volatile uint32_t g_dcmi_min_byte;
static volatile uint32_t g_dcmi_max_byte;
static volatile uint32_t g_dcmi_brightness_percent;
static volatile uint32_t g_dcmi_average_r;
static volatile uint32_t g_dcmi_average_g;
static volatile uint32_t g_dcmi_average_b;
static volatile uint32_t g_dcmi_bit0_percent;
static volatile uint32_t g_dcmi_bit1_percent;
static volatile uint32_t g_dcmi_bit2_percent;
static volatile uint32_t g_dcmi_bit3_percent;
static volatile uint32_t g_dcmi_bit4_percent;
static volatile uint32_t g_dcmi_bit5_percent;
static volatile uint32_t g_dcmi_bit6_percent;
static volatile uint32_t g_dcmi_bit7_percent;
static volatile uint32_t g_dcmi_zone0_brightness_percent;
static volatile uint32_t g_dcmi_zone1_brightness_percent;
static volatile uint32_t g_dcmi_zone2_brightness_percent;
static volatile uint32_t g_dcmi_zone3_brightness_percent;
static volatile uint32_t g_dcmi_zone0_r;
static volatile uint32_t g_dcmi_zone0_g;
static volatile uint32_t g_dcmi_zone0_b;
static volatile uint32_t g_dcmi_zone1_r;
static volatile uint32_t g_dcmi_zone1_g;
static volatile uint32_t g_dcmi_zone1_b;
static volatile uint32_t g_dcmi_zone2_r;
static volatile uint32_t g_dcmi_zone2_g;
static volatile uint32_t g_dcmi_zone2_b;
static volatile uint32_t g_dcmi_zone3_r;
static volatile uint32_t g_dcmi_zone3_g;
static volatile uint32_t g_dcmi_zone3_b;
#endif /* DCMI_DIAGNOSTICS */
static volatile uint32_t g_dcmi_timing_vsync_count;
static volatile uint32_t g_dcmi_timing_line_count;
static volatile uint32_t g_dcmi_timing_lines_per_frame;
static volatile uint32_t g_dcmi_timing_frame_period_ms;
static volatile uint32_t g_dcmi_timing_frame_rate_hz;
static volatile uint32_t g_dcmi_timing_line_rate_hz;
static volatile uint32_t g_dcmi_measured_pixclk_khz;
static volatile uint32_t g_dcmi_pixclk_violation;
static volatile uint32_t g_dcmi_requested_words;
static volatile uint32_t g_dcmi_captured_words;
static volatile uint32_t g_dcmi_partial_frame_count;
static volatile uint32_t g_dcmi_zero_frame_count;
static volatile uint32_t g_dcmi_early_accept_count;
static volatile uint32_t g_dcmi_callback_complete_count;
static volatile uint32_t g_dcmi_callback_late_count;
static volatile uint32_t g_dcmi_early_complete_count;
static volatile uint32_t g_dcmi_timeout_accept_count;
static volatile uint32_t g_dcmi_edge_attempt_count[DCMI_EDGE_COUNT];
static volatile uint32_t g_dcmi_edge_success_count[DCMI_EDGE_COUNT];
static volatile uint32_t g_dcmi_edge_timeout_count[DCMI_EDGE_COUNT];
static volatile uint32_t g_dcmi_edge_zero_count[DCMI_EDGE_COUNT];
static volatile uint32_t g_dcmi_edge_last_words[DCMI_EDGE_COUNT];
static volatile uint32_t g_dcmi_sync_wait_status;
static volatile uint32_t g_dcmi_sync_wait_ms;
#if !DCMI_CROP_TEST_MODE && !DCMI_PREARM_CAPTURE
static volatile uint32_t g_dcmi_sync_period_ms;
static volatile uint32_t g_dcmi_sync_rate_hz;
#endif
static volatile uint32_t g_dcmi_dma_ndtr_last;
static volatile uint32_t g_dcmi_sr_last;
static volatile uint32_t g_dcmi_ris_last;
static volatile uint32_t g_dcmi_mis_last;
static volatile uint32_t g_dcmi_cr_last;
static volatile uint32_t g_dcmi_dma_lisr_last;
static volatile uint32_t g_dcmi_crop_test_mode = DCMI_CROP_TEST_MODE;
static volatile uint32_t g_dcmi_crop_config_status;
static volatile uint32_t g_dcmi_crop_enable_status;
static volatile uint32_t g_dcmi_crop_programmed_x;
static volatile uint32_t g_dcmi_crop_programmed_y;
static volatile uint32_t g_dcmi_crop_programmed_width;
static volatile uint32_t g_dcmi_crop_programmed_height;
static volatile uint32_t g_dcmi_crop_cwstrtr;
static volatile uint32_t g_dcmi_crop_cwsizer;
static volatile uint32_t g_dcmi_crop_cr_after_enable;
static volatile uint32_t g_dcmi_crop_test_last_words;
static volatile uint32_t g_dcmi_crop_test_max_words;
static volatile uint32_t g_dcmi_crop_test_full_count;
static volatile uint32_t g_dcmi_crop_test_start_mode;
/* 3 = pre-armed continuous capture without a manual VSYNC wait. */
static volatile uint32_t g_dcmi_crop_test_strategy;
static volatile uint32_t g_dcmi_crop_test_prearmed_count;
static volatile uint32_t g_dcmi_crop_test_hsync_high;
static volatile uint32_t g_dcmi_crop_test_hsync_high_attempts;
static volatile uint32_t g_dcmi_crop_test_hsync_low_attempts;
static volatile uint32_t g_dcmi_crop_test_hsync_high_max_words;
static volatile uint32_t g_dcmi_crop_test_hsync_low_max_words;
static volatile uint32_t g_dcmi_crop_test_hsync_high_full_count;
static volatile uint32_t g_dcmi_crop_test_hsync_low_full_count;
/* Sync-combo index: bit 0 = HSYNC high, bit 1 = VSYNC high. */
static volatile uint32_t g_dcmi_crop_test_sync_combo;
static volatile uint32_t g_dcmi_crop_test_sync_attempts[4];
static volatile uint32_t g_dcmi_crop_test_sync_max_words[4];
static volatile uint32_t g_dcmi_crop_test_sync_full_count[4];
static volatile uint32_t g_dcmi_side_test_active = DCMI_FULL_HEIGHT_SIDE_TEST_MODE;
static volatile uint32_t g_dcmi_side_test_edge = DCMI_FULL_HEIGHT_SIDE_TEST_EDGE;
static volatile uint32_t g_dcmi_side_test_expected_words;
static volatile uint32_t g_dcmi_side_test_last_words;
static volatile uint32_t g_dcmi_side_test_max_words;
static volatile uint32_t g_dcmi_side_test_full_count;
static volatile uint32_t g_dcmi_side_test_near_full_count;
static volatile uint32_t g_dcmi_side_test_partial_count;
static volatile uint32_t g_dcmi_side_test_zero_count;
static volatile uint32_t g_dcmi_side_test_missing_words;
static volatile uint32_t g_dcmi_side_test_complete_rows;
static volatile uint32_t g_dcmi_side_test_trailing_words;
static volatile uint32_t g_dcmi_side_test_last_capture_ms;
static volatile uint32_t g_dcmi_side_test_max_capture_ms;
static volatile uint32_t g_dcmi_side_test_nonzero_rows;
static volatile uint32_t g_dcmi_side_test_changing_rows;
static volatile uint32_t g_dcmi_side_test_first_row_checksum;
static volatile uint32_t g_dcmi_side_test_mid_row_checksum;
static volatile uint32_t g_dcmi_side_test_last_row_checksum;
static volatile uint32_t g_dcmi_side_test_first_row_nonzero_words;
static volatile uint32_t g_dcmi_side_test_mid_row_nonzero_words;
static volatile uint32_t g_dcmi_side_test_last_row_nonzero_words;
/* Transport completion and useful video content are separate questions.
 * A continuous-mode DMA can fill all 5040 words with zero-valued samples.
 */
static volatile uint32_t g_dcmi_side_test_analyzed_words;
static volatile uint32_t g_dcmi_side_test_full_nonzero_count;
static volatile uint32_t g_dcmi_side_test_full_allzero_count;
static volatile uint32_t g_dcmi_side_test_analysis_sequence;
static volatile uint32_t g_dcmi_led_update_pending;
static volatile uint32_t g_dcmi_zone_update_count;
/* Per-edge zone statistics */
/* Frame quality metrics */
/* Edge-specific quality (for diagnosing capture issues) */
/* Saturation detection */
/* Dead zone detection (zones that never receive data) */
/* Crop validation metrics */
/* Dynamic baseline tracking */
#if DCMI_DIAGNOSTICS
static volatile uint32_t g_dcmi_observed_baseline_r;                   /* Observed minimum R (for black floor) */
static volatile uint32_t g_dcmi_observed_baseline_g;                   /* Observed minimum G */
static volatile uint32_t g_dcmi_observed_baseline_b;                   /* Observed minimum B */
#endif /* DCMI_DIAGNOSTICS */
/* Color variance tracking (temporal stability) */
/* Zone response rate tracking */
static volatile uint32_t g_dcmi_zone_spike_reject_count;
static volatile uint32_t g_dcmi_zone_low_trust_spike_count;
static volatile uint32_t g_dcmi_global_cut_count;
#if DCMI_BORDER_ACTIVE
/* Detected border insets, indexed by dcmi_edge_t. TOP/BOTTOM are lines,
 * RIGHT/LEFT are pixels, all measured inward from that edge of the panel. */
static volatile uint32_t g_dcmi_border_inset[DCMI_EDGE_COUNT];
static volatile uint32_t g_dcmi_border_grow_count;
static volatile uint32_t g_dcmi_border_reset_count;
static volatile uint32_t g_dcmi_border_giveup_count;
static volatile uint32_t g_dcmi_border_probe_count;
static volatile uint32_t g_dcmi_border_probe_discard_count;
static uint32_t border_black_streak[DCMI_EDGE_COUNT];
static uint32_t border_walking[DCMI_EDGE_COUNT];
static uint32_t border_capture_count[DCMI_EDGE_COUNT];
static uint32_t border_edge_level[DCMI_EDGE_COUNT];
static uint32_t active_capture_is_probe;
#endif
static volatile uint32_t g_dcmi_zone_miss_hold_count;
static volatile uint32_t g_dcmi_zone_miss_decay_count;
static volatile uint32_t g_dcmi_frame_publish_skip_count;
static volatile uint32_t g_dcmi_frame_quality_last;
static volatile uint32_t g_dcmi_zone_color_delta_max;
static volatile uint32_t g_dcmi_zone_color_delta_avg;
static volatile uint32_t g_dcmi_top_good_zones;
static volatile uint32_t g_dcmi_top_color_spread;
static volatile uint32_t g_dcmi_top_first_r;
static volatile uint32_t g_dcmi_top_first_g;
static volatile uint32_t g_dcmi_top_first_b;
static volatile uint32_t g_dcmi_top_mid_r;
static volatile uint32_t g_dcmi_top_mid_g;
static volatile uint32_t g_dcmi_top_mid_b;
static volatile uint32_t g_dcmi_top_last_r;
static volatile uint32_t g_dcmi_top_last_g;
static volatile uint32_t g_dcmi_top_last_b;
#if DCMI_INCREMENTAL_PUBLISH
static volatile uint32_t g_dcmi_top_raw_color_spread;
static volatile uint32_t g_dcmi_top_raw_first_r;
static volatile uint32_t g_dcmi_top_raw_first_g;
static volatile uint32_t g_dcmi_top_raw_first_b;
static volatile uint32_t g_dcmi_top_raw_mid_r;
static volatile uint32_t g_dcmi_top_raw_mid_g;
static volatile uint32_t g_dcmi_top_raw_mid_b;
static volatile uint32_t g_dcmi_top_raw_last_r;
static volatile uint32_t g_dcmi_top_raw_last_g;
static volatile uint32_t g_dcmi_top_raw_last_b;
static volatile uint32_t g_dcmi_top_first_samples;
static volatile uint32_t g_dcmi_top_mid_samples;
static volatile uint32_t g_dcmi_top_last_samples;
#endif
static volatile uint32_t g_dcmi_right_band_index;
static volatile uint32_t g_dcmi_left_band_index;
static volatile uint32_t g_dcmi_right_band_last_words[DCMI_RIGHT_ZONE_COUNT];
static volatile uint32_t g_dcmi_left_band_last_words[DCMI_LEFT_ZONE_COUNT];
static volatile uint32_t g_dcmi_right_band_max_words[DCMI_RIGHT_ZONE_COUNT];
static volatile uint32_t g_dcmi_left_band_max_words[DCMI_LEFT_ZONE_COUNT];
volatile uint32_t g_dcmi_led_zone_r[DCMI_LED_ZONE_COUNT];
volatile uint32_t g_dcmi_led_zone_g[DCMI_LED_ZONE_COUNT];
volatile uint32_t g_dcmi_led_zone_b[DCMI_LED_ZONE_COUNT];

static uint32_t dcmi_buffer[DCMI_CAPTURE_MAX_WORDS];
static dcmi_capture_status_t dcmi_status = {
    .state = DCMI_CAPTURE_NOT_CONFIGURED,
    .frames_seen = 0,
    .dma_half_callbacks = 0,
    .dma_full_callbacks = 0,
    .errors = 0,
};
static uint32_t last_restart_ms;
static uint32_t timing_window_start_ms;
static volatile uint32_t timing_window_vsyncs;
static volatile uint32_t timing_window_lines;
static volatile uint32_t lines_this_frame;
static volatile uint32_t dcmi_buffer_ready;
static volatile uint32_t last_frame_time_ms;
static volatile uint32_t last_error_time_ms;
static volatile uint32_t dcmi_stop_requested;
#if !DCMI_CROP_TEST_MODE && !DCMI_PREARM_CAPTURE
static uint32_t last_sync_tick_ms;
#endif
static uint32_t active_crop_index;
static uint32_t next_crop_index;
static uint32_t active_capture_samples;
static uint32_t active_capture_words;
static uint32_t active_crop_width;
static uint32_t active_crop_height;
static uint32_t active_side_band_index;
static uint32_t next_right_band_index;
static uint32_t next_left_band_index;
#if DCMI_BOTTOM_TIMED_BAND
static uint32_t next_side_band_start_ms = 0U;
static uint32_t next_bottom_band_start_ms = 0U;
#endif
#if !DCMI_INCREMENTAL_PUBLISH
static uint32_t perimeter_edges_done_mask;
#endif
static volatile uint32_t active_captured_words;
static uint32_t led_zone_red_sum[DCMI_LED_ZONE_COUNT];
static uint32_t led_zone_green_sum[DCMI_LED_ZONE_COUNT];
static uint32_t led_zone_blue_sum[DCMI_LED_ZONE_COUNT];
static uint32_t led_zone_weight_sum[DCMI_LED_ZONE_COUNT];
static uint32_t led_zone_sample_count[DCMI_LED_ZONE_COUNT];
static uint32_t led_zone_update_counter[DCMI_LED_ZONE_COUNT];
static uint32_t led_zone_missed_count[DCMI_LED_ZONE_COUNT];
static uint32_t smoothed_led_zone_r[DCMI_LED_ZONE_COUNT];
static uint32_t smoothed_led_zone_g[DCMI_LED_ZONE_COUNT];
static uint32_t smoothed_led_zone_b[DCMI_LED_ZONE_COUNT];
/* Per-byte lookup tables for the analysis hot loop. Each RGB332 byte value
 * maps directly to its baseline-rescaled 8-bit channels and its averaging
 * weight, so the per-sample cost is four table reads and four adds.
 */
static uint8_t rgb332_red_lut[256];
static uint8_t rgb332_green_lut[256];
static uint8_t rgb332_blue_lut[256];
static uint16_t rgb332_weight_lut[256];

/* Column-to-LED-zone map for the active crop, rebuilt once per capture.
 * Entry values:
 *   DCMI_COLUMN_SKIP      column not used for LED color
 *   DCMI_COLUMN_ROW_ZONE  zone determined per row (vertical side crops)
 *   otherwise             the LED zone index for this column
 * This removes the per-sample x/y division/modulo from analyze_buffer().
 */
#define DCMI_COLUMN_SKIP 0xFFFFU
#define DCMI_COLUMN_ROW_ZONE 0xFFFEU
static uint16_t column_zone_map[VIDEO_ACTIVE_WIDTH];
static uint32_t column_map_uses_row_zone;
static volatile uint32_t g_dcmi_unaligned_crop_skip_count;

static void init_rgb332_luts(void);
static uint32_t rescale_with_baseline(uint32_t value, uint32_t baseline);
static uint32_t cartesian_y_to_dcmi_y(uint32_t cart_y, uint32_t height);
static uint32_t edge_zone_count(uint32_t edge);
static uint32_t edge_zone_offset(uint32_t edge);
static uint32_t edge_local_zone(uint32_t edge, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height);
static void side_band_zone_pair(uint32_t *right_zone, uint32_t *left_zone);
static void accumulate_led_zone_sample(uint32_t led_zone, uint32_t sample);
static void build_column_zone_map(void);
#if DCMI_BORDER_ACTIVE
static uint32_t border_inset_limit(uint32_t edge);
static void border_adjust_crop(uint32_t *crop_x, uint32_t *crop_y,
                               uint32_t *crop_width, uint32_t *crop_height);
static uint32_t border_process_capture(uint32_t max_level,
                                       uint32_t candidate_zones);
#endif
#if DCMI_DIAGNOSTICS
static void analyze_buffer_diagnostics(uint32_t words_to_analyze);
#endif
static uint32_t capture_accept_words(void);
static uint32_t capture_min_accept_words(void);
#if DCMI_SIDE_HORIZONTAL_BANDS
static uint32_t side_band_cartesian_y(uint32_t edge, uint32_t band_index);
#endif
#if !DCMI_INCREMENTAL_PUBLISH
static void begin_perimeter_cycle_if_needed(void);
static uint32_t mark_current_perimeter_edge_done(void);
#endif
static uint32_t blend_channel(uint32_t prev_value,
                              uint32_t raw_value,
                              uint32_t alpha_num,
                              uint32_t alpha_den);
#if DCMI_INCREMENTAL_PUBLISH
static void reset_zone_accumulator(uint32_t zone_index);
static uint32_t apply_sampled_zone(uint32_t zone_index,
                                   uint32_t force_snap,
                                   uint32_t *frame_delta_max,
                                   uint32_t *frame_delta_sum,
                                   uint32_t *frame_delta_count,
                                   uint32_t *frame_spike_reject,
                                   uint32_t *frame_low_trust_spike);
static uint32_t zone_in_current_capture(uint32_t zone_index);
static void clear_current_capture_zones(void);
static void finalize_incremental_capture(void);
#endif
#if !DCMI_INCREMENTAL_PUBLISH
static void finalize_perimeter_cycle(void);
#endif
static void mark_crop_accept(uint32_t early_accept);
static void mark_crop_zero(void);
#if DCMI_SIDE_HORIZONTAL_BANDS
static void advance_side_band_index(void);
#endif
static void record_side_band_words(uint32_t words);
static void record_full_height_side_result(uint32_t words, uint32_t full);
#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE
static void analyze_full_height_side_rows(void);
#endif

static void start_snapshot(void);
#if !DCMI_CROP_TEST_MODE && !DCMI_PREARM_CAPTURE
static uint32_t wait_for_frame_boundary(void);
#endif
static void analyze_buffer(void);

void DCMI_Capture_Init(void)
{
    g_dcmi_crop_test_mode = DCMI_CROP_TEST_MODE;
    g_dcmi_side_test_active = DCMI_FULL_HEIGHT_SIDE_TEST_MODE;
    g_dcmi_side_test_edge = DCMI_FULL_HEIGHT_SIDE_TEST_EDGE;
    init_rgb332_luts();
    dcmi_status.state = DCMI_CAPTURE_READY;
    g_dcmi_state = DCMI_CAPTURE_READY;
    /* active_capture_* are recomputed per crop in start_snapshot. */
    start_snapshot();
}

void DCMI_Capture_Task(void)
{

    if (dcmi_buffer_ready != 0U) {
        if (dcmi_stop_requested != 0U) {
            HAL_DCMI_Stop(&hdcmi);
            dcmi_stop_requested = 0U;
                }

        dcmi_buffer_ready = 0U;
        analyze_buffer();
    }

    if (dcmi_status.state == DCMI_CAPTURE_RUNNING) {
        if (hdcmi.DMA_Handle != NULL &&
            hdcmi.DMA_Handle->Instance->NDTR == 0U) {
            HAL_DCMI_Stop(&hdcmi);
            active_captured_words = active_capture_words;
            g_dcmi_captured_words = active_captured_words;
            g_dcmi_crop_test_last_words = active_captured_words;
            if (active_captured_words > g_dcmi_crop_test_max_words) {
                g_dcmi_crop_test_max_words = active_captured_words;
            }
            g_dcmi_crop_test_full_count++;
            record_full_height_side_result(active_captured_words, 1U);
#if DCMI_CROP_TEST_MODE
            g_dcmi_crop_test_sync_max_words[g_dcmi_crop_test_sync_combo] =
                active_captured_words;
            g_dcmi_crop_test_sync_full_count[g_dcmi_crop_test_sync_combo]++;
            if (g_dcmi_crop_test_hsync_high != 0U) {
                g_dcmi_crop_test_hsync_high_max_words = active_captured_words;
                g_dcmi_crop_test_hsync_high_full_count++;
            } else {
                g_dcmi_crop_test_hsync_low_max_words = active_captured_words;
                g_dcmi_crop_test_hsync_low_full_count++;
            }
#endif
            mark_crop_accept(0U);
            last_frame_time_ms = HAL_GetTick();
            dcmi_status.frames_seen++;
            g_dcmi_frame_count = dcmi_status.frames_seen;
            dcmi_status.state = DCMI_CAPTURE_READY;
            g_dcmi_state = DCMI_CAPTURE_READY;
            dcmi_buffer_ready = 1U;
                    return;
        }

        if (hdcmi.DMA_Handle != NULL &&
            hdcmi.DMA_Handle->Instance->NDTR <= active_capture_words) {
            uint32_t captured_words = active_capture_words - hdcmi.DMA_Handle->Instance->NDTR;
            uint32_t accept_words = capture_accept_words();

            if (captured_words >= accept_words) {
                HAL_DCMI_Stop(&hdcmi);
                active_captured_words = captured_words;
                g_dcmi_captured_words = active_captured_words;
                g_dcmi_early_complete_count++;
                record_full_height_side_result(active_captured_words, 1U);
                mark_crop_accept(1U);

                if (active_captured_words < active_capture_words) {
                    g_dcmi_partial_frame_count++;
                }

                last_frame_time_ms = HAL_GetTick();
                dcmi_status.frames_seen++;
                g_dcmi_frame_count = dcmi_status.frames_seen;
                dcmi_status.state = DCMI_CAPTURE_READY;
                g_dcmi_state = DCMI_CAPTURE_READY;
                dcmi_buffer_ready = 1U;
                            return;
            }
        }

        if ((HAL_GetTick() - last_restart_ms) >=
#if DCMI_CROP_TEST_MODE
            DCMI_CROP_TEST_CAPTURE_TIMEOUT_MS
#elif DCMI_FULL_HEIGHT_SIDE_TEST_MODE
            DCMI_FULL_HEIGHT_SIDE_TEST_TIMEOUT_MS
#else
            DCMI_CAPTURE_TIMEOUT_MS
#endif
            ) {
            active_captured_words = 0U;

            if (hdcmi.DMA_Handle != NULL &&
                hdcmi.DMA_Handle->Instance->NDTR <= active_capture_words) {
                g_dcmi_dma_ndtr_last = hdcmi.DMA_Handle->Instance->NDTR;
                active_captured_words = active_capture_words - hdcmi.DMA_Handle->Instance->NDTR;
            }

            g_dcmi_sr_last = DCMI->SR;
            g_dcmi_ris_last = DCMI->RISR;
            g_dcmi_mis_last = DCMI->MISR;
            g_dcmi_cr_last = DCMI->CR;
            g_dcmi_dma_lisr_last = DMA2->LISR;
            HAL_DCMI_Stop(&hdcmi);
            g_dcmi_captured_words = active_captured_words;
            g_dcmi_crop_test_last_words = active_captured_words;
            if (active_captured_words > g_dcmi_crop_test_max_words) {
                g_dcmi_crop_test_max_words = active_captured_words;
            }
            record_full_height_side_result(active_captured_words, 0U);
#if DCMI_CROP_TEST_MODE
            if (active_captured_words >
                g_dcmi_crop_test_sync_max_words[g_dcmi_crop_test_sync_combo]) {
                g_dcmi_crop_test_sync_max_words[g_dcmi_crop_test_sync_combo] =
                    active_captured_words;
            }
            if (g_dcmi_crop_test_hsync_high != 0U) {
                if (active_captured_words > g_dcmi_crop_test_hsync_high_max_words) {
                    g_dcmi_crop_test_hsync_high_max_words = active_captured_words;
                }
            } else if (active_captured_words > g_dcmi_crop_test_hsync_low_max_words) {
                g_dcmi_crop_test_hsync_low_max_words = active_captured_words;
            }
#endif
            g_dcmi_timeout_count++;
            if (active_crop_index < DCMI_EDGE_COUNT) {
                g_dcmi_edge_timeout_count[active_crop_index]++;
                g_dcmi_edge_last_words[active_crop_index] = active_captured_words;
            }
            dcmi_status.state = DCMI_CAPTURE_READY;
            g_dcmi_state = DCMI_CAPTURE_READY;

            if (active_captured_words >= capture_min_accept_words()) {
                if (active_captured_words < active_capture_words) {
                    g_dcmi_partial_frame_count++;
                }

                last_frame_time_ms = HAL_GetTick();
                dcmi_status.frames_seen++;
                g_dcmi_frame_count = dcmi_status.frames_seen;
                g_dcmi_timeout_accept_count++;
                mark_crop_accept(0U);
                dcmi_buffer_ready = 1U;
            } else {
                mark_crop_zero();
            }
        }

        return;
    }

    {
        uint32_t restart_delay_ms =
            dcmi_status.state == DCMI_CAPTURE_ERROR ?
            DCMI_ERROR_RESTART_BACKOFF_MS : DCMI_RESTART_DELAY_MS;

        if ((HAL_GetTick() - last_restart_ms) >= restart_delay_ms) {
            start_snapshot();
        }
    }
}


uint32_t DCMI_Capture_ConsumeLedUpdate(void)
{
    uint32_t pending;

    pending = g_dcmi_led_update_pending;
    g_dcmi_led_update_pending = 0U;

    return pending;
}


void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi_arg)
{
    if (hdcmi_arg != &hdcmi) {
        return;
    }

    if (dcmi_status.state != DCMI_CAPTURE_RUNNING) {
        g_dcmi_callback_late_count++;
        return;
    }

    /* Completion ownership stays in DCMI_Capture_Task(), where DCMI/DMA can
     * be stopped before the CPU analyzes dcmi_buffer. Treat the frame IRQ as
     * telemetry only; using it to publish dcmi_buffer_ready can race DMA.
     */
    g_dcmi_callback_complete_count++;
    __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_FRAME);
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *hdcmi_arg)
{
    if (hdcmi_arg != &hdcmi) {
        return;
    }

    dcmi_status.errors++;
    g_dcmi_error_count = dcmi_status.errors;
    last_error_time_ms = HAL_GetTick();
    dcmi_status.state = DCMI_CAPTURE_ERROR;
    g_dcmi_state = DCMI_CAPTURE_ERROR;
}

void HAL_DCMI_LineEventCallback(DCMI_HandleTypeDef *hdcmi_arg)
{
    if (hdcmi_arg != &hdcmi) {
        return;
    }

    lines_this_frame++;
    g_dcmi_timing_line_count++;
    timing_window_lines++;
}

void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *hdcmi_arg)
{
    uint32_t now;
    uint32_t lines;
    uint32_t window_ms;

    if (hdcmi_arg != &hdcmi) {
        return;
    }

    now = HAL_GetTick();
    lines = lines_this_frame;
    lines_this_frame = 0U;
    g_dcmi_timing_vsync_count++;
    g_dcmi_timing_lines_per_frame = lines;
    timing_window_vsyncs++;

    if (timing_window_start_ms == 0U) {
        timing_window_start_ms = now;
        return;
    }

    window_ms = now - timing_window_start_ms;

    if (window_ms >= 1000U) {
        g_dcmi_timing_frame_period_ms = timing_window_vsyncs > 0U ? window_ms / timing_window_vsyncs : 0U;
        g_dcmi_timing_frame_rate_hz = (timing_window_vsyncs * 1000U) / window_ms;
        g_dcmi_timing_line_rate_hz = (timing_window_lines * 1000U) / window_ms;
        /* For 720p video, total pixels are roughly 1650 x 750 per frame.
         * This is a diagnostic estimate so we can spot sources that are too
         * fast for reliable F407 DCMI sampling.
         */
        g_dcmi_measured_pixclk_khz = (g_dcmi_timing_frame_rate_hz * 1238U);
        g_dcmi_pixclk_violation = g_dcmi_measured_pixclk_khz > 54000U ? 1U : 0U;
        timing_window_vsyncs = 0U;
        timing_window_lines = 0U;
        timing_window_start_ms = now;
    }
}

static void start_snapshot(void)
{
#if DCMI_SIDE_HORIZONTAL_BANDS
    if (next_crop_index == DCMI_EDGE_LEFT) {
        next_crop_index = DCMI_EDGE_TOP;
    }
#endif

    const dcmi_edge_crop_t *crop = &edge_crops[next_crop_index];
    HAL_StatusTypeDef crop_status;
    uint32_t crop_x = crop->x;
    uint32_t crop_y = crop->y;
    uint32_t crop_width = crop->width;
    uint32_t crop_height = crop->height;
    #if DCMI_BOTTOM_TIMED_BAND
    uint32_t side_start_delay_ms = 0U;
    #endif
    uint32_t dcmi_y;

    HAL_DCMI_Stop(&hdcmi);
    __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_LINE | DCMI_IT_VSYNC |
                                  DCMI_IT_ERR | DCMI_IT_OVR |
                                  DCMI_IT_FRAME);
    __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI |
                                  DCMI_FLAG_ERRRI | DCMI_FLAG_VSYNCRI |
                                  DCMI_FLAG_LINERI);

#if DCMI_CROP_TEST_MODE
    /* Sweep all hardware-sync polarity combinations. Display-style HSYNC and
     * VSYNC are short blanking pulses, while DCMI expects active line/frame
     * windows. The four-way result separates a polarity issue from a signal
     * semantics/wiring issue.
     */
    g_dcmi_crop_test_sync_combo = g_dcmi_crop_test_prearmed_count & 3U;
    g_dcmi_crop_test_sync_attempts[g_dcmi_crop_test_sync_combo]++;
    g_dcmi_crop_test_hsync_high = g_dcmi_crop_test_sync_combo & 1U;
    if (g_dcmi_crop_test_hsync_high != 0U) {
        DCMI->CR |= DCMI_CR_HSPOL;
        g_dcmi_crop_test_hsync_high_attempts++;
    } else {
        DCMI->CR &= ~DCMI_CR_HSPOL;
        g_dcmi_crop_test_hsync_low_attempts++;
    }
    if ((g_dcmi_crop_test_sync_combo & 2U) != 0U) {
        DCMI->CR |= DCMI_CR_VSPOL;
    } else {
        DCMI->CR &= ~DCMI_CR_VSPOL;
    }
#endif

    active_crop_index = next_crop_index;
#if DCMI_CROP_TEST_MODE
    /* Use the simplest possible crop: first active line/pixel, full width,
     * 16 lines deep. The Cartesian Y value converts to DCMI Y=0 below.
     */
    active_crop_index = DCMI_EDGE_TOP;
    crop_x = 0U;
    crop_y = VIDEO_ACTIVE_HEIGHT - EDGE_BORDER_PIXELS;
    crop_width = VIDEO_ACTIVE_WIDTH;
    crop_height = EDGE_BORDER_PIXELS;
#endif
#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE
    /* Hold the scheduler on one vertical side segment. Exact-length
     * acceptance below makes this a transport test, not an LED-output mode.
     */
    active_crop_index = DCMI_FULL_HEIGHT_SIDE_TEST_EDGE;
    crop_x = edge_crops[active_crop_index].x;
    crop_y = DCMI_FULL_HEIGHT_SIDE_TEST_Y;
    crop_width = edge_crops[active_crop_index].width;
    crop_height = DCMI_FULL_HEIGHT_SIDE_TEST_HEIGHT;
#endif
    if (active_crop_index == DCMI_EDGE_RIGHT) {
        active_side_band_index = next_right_band_index;
    } else if (active_crop_index == DCMI_EDGE_LEFT) {
        active_side_band_index = next_left_band_index;
    } else {
        active_side_band_index = 0U;
    }
    if (active_crop_index < DCMI_EDGE_COUNT) {
        g_dcmi_edge_attempt_count[active_crop_index]++;
    }

    #if DCMI_SIDE_HORIZONTAL_BANDS
    if (active_crop_index == DCMI_EDGE_RIGHT ||
        active_crop_index == DCMI_EDGE_LEFT) {
        crop_x = 0U;
        crop_y = side_band_cartesian_y(active_crop_index, active_side_band_index);
        crop_width = VIDEO_ACTIVE_WIDTH;
        crop_height = EDGE_BORDER_PIXELS;
    }
    #endif

    #if DCMI_BOTTOM_TIMED_BAND
    if (active_crop_index == DCMI_EDGE_RIGHT ||
        active_crop_index == DCMI_EDGE_LEFT) {
        uint32_t side_zone_count =
            active_crop_index == DCMI_EDGE_RIGHT ? DCMI_RIGHT_ZONE_COUNT : DCMI_LEFT_ZONE_COUNT;
        uint32_t side_divisor = side_zone_count > 1U ? side_zone_count - 1U : 1U;

        crop_x = 0U;
        crop_y = 0U;
        crop_width = VIDEO_ACTIVE_WIDTH;
        crop_height = EDGE_BORDER_PIXELS;
        side_start_delay_ms =
            ((side_divisor - active_side_band_index) * DCMI_BOTTOM_START_DELAY_MS) / side_divisor;
        next_side_band_start_ms = HAL_GetTick() + side_start_delay_ms;
    }
    #endif

#if DCMI_BORDER_ACTIVE
    border_adjust_crop(&crop_x, &crop_y, &crop_width, &crop_height);
#endif

    dcmi_y = cartesian_y_to_dcmi_y(crop_y, crop_height);

    #if DCMI_BOTTOM_TIMED_BAND
    if (active_crop_index == DCMI_EDGE_BOTTOM && dcmi_y == 0U) {
        next_bottom_band_start_ms = HAL_GetTick() + DCMI_BOTTOM_START_DELAY_MS;
    } else if (active_crop_index == DCMI_EDGE_RIGHT ||
               active_crop_index == DCMI_EDGE_LEFT) {
        dcmi_y = 0U;
    }
    #endif

    active_crop_width = crop_width;
    active_crop_height = crop_height;
    active_capture_samples = crop_width * crop_height;
    active_capture_words = active_capture_samples / 4U;
    if (active_capture_words > DCMI_CAPTURE_MAX_WORDS) {
        active_capture_words = DCMI_CAPTURE_MAX_WORDS;
    }
    active_captured_words = 0U;
    dcmi_stop_requested = 0U;
    g_dcmi_requested_words = active_capture_words;
    g_dcmi_captured_words = 0U;
    g_dcmi_side_test_expected_words = active_capture_words;
    g_dcmi_right_band_index = next_right_band_index;
    g_dcmi_left_band_index = next_left_band_index;

    #if DCMI_USE_CROP
    g_dcmi_crop_programmed_x = crop_x;
    g_dcmi_crop_programmed_y = dcmi_y;
    g_dcmi_crop_programmed_width = crop_width;
    g_dcmi_crop_programmed_height = crop_height;
    crop_status = HAL_DCMI_ConfigCrop(&hdcmi,
                                      crop_x,
                                      dcmi_y,
                                      crop_width - 1U,
                                      crop_height - 1U);
    g_dcmi_crop_config_status = (uint32_t)crop_status;
    if (crop_status == HAL_OK) {
        HAL_StatusTypeDef crop_enable_status = HAL_DCMI_EnableCrop(&hdcmi);
        g_dcmi_crop_enable_status = (uint32_t)crop_enable_status;
        if (crop_enable_status != HAL_OK) {
            (void)HAL_DCMI_DisableCrop(&hdcmi);
        }
    } else {
        g_dcmi_crop_enable_status = 0xFFFFFFFFU;
        /* Keep capture alive even when per-crop programming fails. */
        (void)HAL_DCMI_DisableCrop(&hdcmi);
    }
    g_dcmi_crop_cwstrtr = DCMI->CWSTRTR;
    g_dcmi_crop_cwsizer = DCMI->CWSIZER;
    g_dcmi_crop_cr_after_enable = DCMI->CR;
    #else
    crop_status = HAL_DCMI_DisableCrop(&hdcmi);
    (void)crop_status;
    #endif

    for (uint32_t i = 0; i < active_capture_words; i++) {
        dcmi_buffer[i] = 0U;
    }

    /* Edge crops are tied to the source scanout. If a crop starts in the
     * middle of a frame, the target rows may already be gone and DMA will only
     * fill a partial buffer. Wait for the next VSYNC for every crop, but keep
     * the wait bounded so this remains soft-real-time: if sync is missed, start
     * anyway and let the timeout path recover.
     */
#if DCMI_CROP_TEST_MODE || DCMI_PREARM_CAPTURE
    /* Arm capture before the next frame instead of consuming a VSYNC event
     * first. With PA4 fixed, the crop diagnostic proved this start strategy can
     * fill a full crop; it also avoids starting just after the desired rows.
     */
    g_dcmi_sync_wait_status = DCMI_CROP_TEST_MODE ? 2U : 3U;
    g_dcmi_sync_wait_ms = 0U;
#if DCMI_CROP_TEST_MODE
    g_dcmi_crop_test_start_mode = DCMI_MODE_CONTINUOUS;
    g_dcmi_crop_test_strategy = 3U;
    g_dcmi_crop_test_prearmed_count++;
#endif
#else
    g_dcmi_sync_wait_status = wait_for_frame_boundary();
#endif

    #if DCMI_BOTTOM_TIMED_BAND
    if (active_crop_index == DCMI_EDGE_BOTTOM && next_bottom_band_start_ms > 0U) {
        if ((int32_t)(HAL_GetTick() - next_bottom_band_start_ms) < 0) {
            dcmi_status.state = DCMI_CAPTURE_READY;
            g_dcmi_state = DCMI_CAPTURE_READY;
                    return;
        }
        next_bottom_band_start_ms = 0U;
    } else if ((active_crop_index == DCMI_EDGE_RIGHT ||
                active_crop_index == DCMI_EDGE_LEFT) &&
               next_side_band_start_ms > 0U) {
        if ((int32_t)(HAL_GetTick() - next_side_band_start_ms) < 0) {
            dcmi_status.state = DCMI_CAPTURE_READY;
            g_dcmi_state = DCMI_CAPTURE_READY;
                    return;
        }
        next_side_band_start_ms = 0U;
    }
    #endif

    g_dcmi_start_status = HAL_DCMI_Start_DMA(&hdcmi,
#if DCMI_CROP_TEST_MODE
                                             DCMI_MODE_CONTINUOUS,
#elif DCMI_FULL_HEIGHT_SIDE_TEST_MODE
                                             /* Snapshot mode stops at the current frame boundary.
                                              * If the side test arms during active video, that
                                              * produces zero or truncated vertical crops. Let the
                                              * normal-mode DMA length stop this transport test
                                              * after the complete side rectangle arrives instead.
                                              */
                                             DCMI_MODE_CONTINUOUS,
#else
                                             DCMI_MODE_CONTINUOUS,
#endif
                                             (uint32_t)dcmi_buffer,
                                             active_capture_words);
    last_restart_ms = HAL_GetTick();

    if (g_dcmi_start_status == HAL_OK) {
        /* Do not enable DCMI_IT_FRAME here.
         *
         * In HAL_DCMI_Start_DMA(), the HAL-owned DMA-complete callback
         * enables the FRAME interrupt only after the requested DMA transfer
         * is complete. If we enable FRAME immediately, a video frame boundary
         * can arrive before DMA fills the crop buffer, making the firmware
         * analyze 0 or partial words as if capture had completed.
         */
        __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_ERR);
        dcmi_status.state = DCMI_CAPTURE_RUNNING;
        g_dcmi_state = DCMI_CAPTURE_RUNNING;
        g_dcmi_restart_count++;
#if DCMI_CROP_TEST_MODE
        next_crop_index = DCMI_EDGE_TOP;
#elif DCMI_FULL_HEIGHT_SIDE_TEST_MODE
        next_crop_index = DCMI_FULL_HEIGHT_SIDE_TEST_EDGE;
#else
        next_crop_index = (next_crop_index + 1U) % DCMI_EDGE_COUNT;
#endif
    } else {
        dcmi_status.errors++;
        g_dcmi_error_count = dcmi_status.errors;
        last_error_time_ms = HAL_GetTick();
        dcmi_status.state = DCMI_CAPTURE_ERROR;
        g_dcmi_state = DCMI_CAPTURE_ERROR;
    }

}

#if !DCMI_CROP_TEST_MODE && !DCMI_PREARM_CAPTURE
static uint32_t wait_for_frame_boundary(void)
{
    uint32_t start_ms = HAL_GetTick();

    __HAL_DCMI_ENABLE(&hdcmi);
    __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_VSYNCRI | DCMI_FLAG_FRAMERI |
                                  DCMI_FLAG_OVRRI | DCMI_FLAG_ERRRI |
                                  DCMI_FLAG_LINERI);

    while ((HAL_GetTick() - start_ms) < DCMI_SYNC_WAIT_TIMEOUT_MS) {
        if (__HAL_DCMI_GET_FLAG(&hdcmi, DCMI_FLAG_VSYNCRI) != 0U) {
            uint32_t now = HAL_GetTick();

            g_dcmi_sync_wait_ms = now - start_ms;
            if (last_sync_tick_ms != 0U) {
                g_dcmi_sync_period_ms = now - last_sync_tick_ms;
                g_dcmi_sync_rate_hz =
                    g_dcmi_sync_period_ms > 0U ? (1000U / g_dcmi_sync_period_ms) : 0U;
            }
            last_sync_tick_ms = now;
            __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_VSYNCRI);
            __HAL_DCMI_DISABLE(&hdcmi);
            /* 1 means a VSYNC boundary was observed successfully. */
            return 1U;
        }
    }

    g_dcmi_sync_wait_ms = HAL_GetTick() - start_ms;
    __HAL_DCMI_DISABLE(&hdcmi);
    return 0U;
}
#endif

static void analyze_buffer(void)
{
    uint32_t crop_width = active_crop_width;
    uint32_t words_to_analyze = active_captured_words;

    if (words_to_analyze > active_capture_words) {
        words_to_analyze = active_capture_words;
    }

#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE
    analyze_full_height_side_rows();
#endif

#if DCMI_INCREMENTAL_PUBLISH
    /* Horizontal side-band mode updates only a small slice each capture.
     * Clear just the zones this crop can touch so previous good values for
     * untouched LEDs stay live and the output can publish incrementally.
     */
    clear_current_capture_zones();
#else
    begin_perimeter_cycle_if_needed();
#endif

    /* Hot path: accumulate weighted zone colors with table lookups only.
     * Walking the buffer row by row keeps the pixel coordinate implicit, so
     * there is no per-sample division/modulo. All real crops have widths
     * that are multiples of four (1280-wide bands, 28-wide side columns);
     * anything else is counted and skipped rather than mis-assigned.
     */
    if (crop_width >= 4U && (crop_width % 4U) == 0U &&
        crop_width <= VIDEO_ACTIVE_WIDTH) {
        uint32_t words_per_row = crop_width / 4U;
        uint32_t row = 0U;
        uint32_t row_word = 0U;
        uint32_t row_zone = DCMI_LED_ZONE_COUNT;

        build_column_zone_map();
        if (column_map_uses_row_zone != 0U) {
            row_zone = edge_zone_offset(active_crop_index) +
                       edge_local_zone(active_crop_index, 0U, 0U,
                                       crop_width, active_crop_height);
        }

        for (uint32_t i = 0U; i < words_to_analyze; i++) {
            uint32_t word = dcmi_buffer[i];
            uint32_t x = row_word * 4U;

            for (uint32_t k = 0U; k < 4U; k++) {
                uint32_t zone = column_zone_map[x + k];

                if (zone == DCMI_COLUMN_ROW_ZONE) {
                    zone = row_zone;
                }
                if (zone < DCMI_LED_ZONE_COUNT) {
                    accumulate_led_zone_sample(zone, (word >> (8U * k)) & 0xFFU);
                }
            }

            row_word++;
            if (row_word == words_per_row) {
                row_word = 0U;
                row++;
                if (column_map_uses_row_zone != 0U) {
                    row_zone = edge_zone_offset(active_crop_index) +
                               edge_local_zone(active_crop_index, 0U, row,
                                               crop_width, active_crop_height);
                }
            }
        }
    } else {
        g_dcmi_unaligned_crop_skip_count++;
    }

#if DCMI_DIAGNOSTICS
    analyze_buffer_diagnostics(words_to_analyze);
#endif

#if DCMI_INCREMENTAL_PUBLISH
    finalize_incremental_capture();
#else
    if (mark_current_perimeter_edge_done() != 0U) {
        finalize_perimeter_cycle();
    }
#endif
}

static void build_column_zone_map(void)
{
    uint32_t width = active_crop_width;

    column_map_uses_row_zone = 0U;

    if (width > VIDEO_ACTIVE_WIDTH) {
        width = VIDEO_ACTIVE_WIDTH;
    }

#if DCMI_SIDE_HORIZONTAL_BANDS
    if (active_crop_index == DCMI_EDGE_RIGHT ||
        active_crop_index == DCMI_EDGE_LEFT) {
        uint32_t right_zone;
        uint32_t left_zone;

        /* A 1280x16 side-band contains both screen sides. Use the first 16
         * columns for the left edge and the last 16 columns for the right
         * edge, so one reliable crop updates both sides.
         */
        side_band_zone_pair(&right_zone, &left_zone);
        for (uint32_t x = 0U; x < width; x++) {
            if (x < EDGE_BORDER_PIXELS) {
                column_zone_map[x] = (uint16_t)left_zone;
            } else if (x + EDGE_BORDER_PIXELS >= width) {
                column_zone_map[x] = (uint16_t)right_zone;
            } else {
                column_zone_map[x] = DCMI_COLUMN_SKIP;
            }
        }
        return;
    }
#endif

#if DCMI_BOTTOM_TIMED_BAND
    /* Timed-band fallback: each side capture is a horizontal band delayed
     * after VSYNC, feeding exactly one vertical LED zone per capture.
     */
    if (active_crop_index == DCMI_EDGE_RIGHT) {
        uint16_t zone = (uint16_t)(DCMI_RIGHT_ZONE_OFFSET +
            (active_side_band_index % DCMI_RIGHT_ZONE_COUNT));

        for (uint32_t x = 0U; x < width; x++) {
            column_zone_map[x] =
                (x + EDGE_BORDER_PIXELS < width) ? DCMI_COLUMN_SKIP : zone;
        }
        return;
    }
    if (active_crop_index == DCMI_EDGE_LEFT) {
        uint16_t zone = (uint16_t)(DCMI_LEFT_ZONE_OFFSET +
            (active_side_band_index % DCMI_LEFT_ZONE_COUNT));

        for (uint32_t x = 0U; x < width; x++) {
            column_zone_map[x] =
                (x >= EDGE_BORDER_PIXELS) ? DCMI_COLUMN_SKIP : zone;
        }
        return;
    }
#endif

    if (active_crop_index == DCMI_EDGE_TOP ||
        active_crop_index == DCMI_EDGE_BOTTOM) {
        for (uint32_t x = 0U; x < width; x++) {
            column_zone_map[x] = (uint16_t)(edge_zone_offset(active_crop_index) +
                edge_local_zone(active_crop_index, x, 0U,
                                width, active_crop_height));
        }
        return;
    }

    /* Wide vertical side crops capture 28 columns so the crop engine has a
     * wider window; the zone follows the row, the inner 12 filler columns
     * are discarded, and LEDs are colored only from the true outer
     * 16-pixel screen edge.
     */
    column_map_uses_row_zone = 1U;
    for (uint32_t x = 0U; x < width; x++) {
        uint32_t keep = 1U;

        if (active_crop_index == DCMI_EDGE_RIGHT &&
            width > EDGE_BORDER_PIXELS &&
            x + EDGE_BORDER_PIXELS < width) {
            keep = 0U;
        } else if (active_crop_index == DCMI_EDGE_LEFT &&
                   width > EDGE_BORDER_PIXELS &&
                   x >= EDGE_BORDER_PIXELS) {
            keep = 0U;
        }

        column_zone_map[x] = keep != 0U ? DCMI_COLUMN_ROW_ZONE : DCMI_COLUMN_SKIP;
    }
}

#if DCMI_BORDER_ACTIVE
static uint32_t border_inset_limit(uint32_t edge)
{
    if (edge == DCMI_EDGE_TOP || edge == DCMI_EDGE_BOTTOM) {
        return DCMI_BORDER_MAX_INSET_LINES;
    }
#if DCMI_SIDE_HORIZONTAL_BANDS
    /* Horizontal side bands read the panel's outer columns directly and
     * cannot reposition in x, so pillarbox detection is off in band mode. */
    return 0U;
#else
    return DCMI_BORDER_MAX_SIDE_INSET_PIXELS;
#endif
}

/* Reshape the table crop so the active capture samples the detected content
 * rectangle instead of the panel edge, and decide whether this capture is an
 * outward probe (own inset forced to 0 to check whether the bars are gone).
 * Works in the same cartesian coordinates as the crop table. The zone math
 * needs no changes: edge_local_zone() divides whatever crop geometry it is
 * given proportionally, so zones redistribute over the visible picture.
 */
static void border_adjust_crop(uint32_t *crop_x, uint32_t *crop_y,
                               uint32_t *crop_width, uint32_t *crop_height)
{
    uint32_t inset_top;
    uint32_t inset_bottom;
    uint32_t inset_left;
    uint32_t inset_right;

    active_capture_is_probe = 0U;

    if (active_crop_index >= DCMI_EDGE_COUNT) {
        return;
    }

    border_capture_count[active_crop_index]++;
    if (g_dcmi_border_inset[active_crop_index] > 0U &&
        (border_capture_count[active_crop_index] %
         DCMI_BORDER_PROBE_INTERVAL) == 0U) {
        active_capture_is_probe = 1U;
        g_dcmi_border_probe_count++;
    }

    inset_top = g_dcmi_border_inset[DCMI_EDGE_TOP];
    inset_bottom = g_dcmi_border_inset[DCMI_EDGE_BOTTOM];
    inset_left = g_dcmi_border_inset[DCMI_EDGE_LEFT];
    inset_right = g_dcmi_border_inset[DCMI_EDGE_RIGHT];

    if (active_capture_is_probe != 0U) {
        switch (active_crop_index) {
        case DCMI_EDGE_TOP:
            inset_top = 0U;
            break;
        case DCMI_EDGE_BOTTOM:
            inset_bottom = 0U;
            break;
        case DCMI_EDGE_RIGHT:
            inset_right = 0U;
            break;
        default:
            inset_left = 0U;
            break;
        }
    }

    if (active_crop_index == DCMI_EDGE_TOP ||
        active_crop_index == DCMI_EDGE_BOTTOM) {
        /* Track pillarbox bars in x. Width stays a multiple of four for the
         * word-aligned analysis fast path. */
        uint32_t content_x = inset_left & ~3U;

        if ((inset_left + inset_right) < VIDEO_ACTIVE_WIDTH) {
            uint32_t content_width =
                (VIDEO_ACTIVE_WIDTH - inset_right - content_x) & ~3U;

            if (content_width >= 256U) {
                *crop_x = content_x;
                *crop_width = content_width;
            }
        }

        if (active_crop_index == DCMI_EDGE_TOP) {
            *crop_y = VIDEO_ACTIVE_HEIGHT - inset_top -
                      DCMI_TOP_CROP_INSET_LINES - EDGE_BORDER_PIXELS;
        } else {
            *crop_y = inset_bottom + DCMI_BOTTOM_CROP_INSET_LINES;
        }
        return;
    }

    /* Side edges: span the content rows. The 32-line top/bottom design
     * insets apply here too, so side zones sample the same vertical extent
     * the top/bottom bands consider real picture.
     */
    {
        uint32_t y0 = inset_bottom + DCMI_BOTTOM_CROP_INSET_LINES;
        uint32_t y1 = VIDEO_ACTIVE_HEIGHT - inset_top - DCMI_TOP_CROP_INSET_LINES;
#if DCMI_SIDE_HORIZONTAL_BANDS
        /* Fallback path: keep band y positions inside the content rows;
         * bands aimed at a bar compress onto the content boundary. */
        uint32_t max_band_y = y1 >= EDGE_BORDER_PIXELS ?
                              y1 - EDGE_BORDER_PIXELS : 0U;

        if (*crop_y < y0) {
            *crop_y = y0;
        }
        if (*crop_y > max_band_y) {
            *crop_y = max_band_y;
        }
        (void)crop_x;
        (void)crop_width;
        (void)crop_height;
#else
        if (y1 > y0 && (y1 - y0) >= 128U) {
            *crop_y = y0;
            *crop_height = y1 - y0;
        }
        if (active_crop_index == DCMI_EDGE_RIGHT) {
            *crop_x = VIDEO_ACTIVE_WIDTH - inset_right - DCMI_SIDE_CROP_WIDTH;
        } else {
            *crop_x = inset_left;
        }
        (void)crop_width;
#endif
    }
}

/* Update border state from one accepted capture. Returns 1 when the capture
 * was an outward probe that still saw black: the caller must discard its
 * samples so the LEDs keep their content colors.
 */
static uint32_t border_process_capture(uint32_t max_level,
                                       uint32_t candidate_zones)
{
    uint32_t edge = active_crop_index;
    uint32_t probe;
    uint32_t black;
    uint32_t content_elsewhere = 0U;

    if (edge >= DCMI_EDGE_COUNT) {
        return 0U;
    }

    probe = active_capture_is_probe;
    active_capture_is_probe = 0U;

    /* Too few valid zones is transport trouble, not a border verdict. */
    if (candidate_zones < (edge_zone_count(edge) / 2U)) {
        if (probe != 0U) {
            g_dcmi_border_probe_discard_count++;
        }
        return probe;
    }

    black = max_level <= DCMI_BORDER_BLACK_LEVEL ? 1U : 0U;

    for (uint32_t other = 0U; other < DCMI_EDGE_COUNT; other++) {
        if (other != edge &&
            border_edge_level[other] >= DCMI_BORDER_CONTENT_LEVEL) {
            content_elsewhere = 1U;
        }
    }

    if (probe != 0U) {
        if (black != 0U) {
            /* Bars still present: keep the content crop, drop this data. */
            g_dcmi_border_probe_discard_count++;
            return 1U;
        }
        /* Picture reaches the panel edge again: snap fully out. The probe
         * samples are the real new edge, so publish them. */
        g_dcmi_border_inset[edge] = 0U;
        border_black_streak[edge] = 0U;
        border_walking[edge] = 0U;
        border_edge_level[edge] = max_level;
        g_dcmi_border_reset_count++;
        return 0U;
    }

    border_edge_level[edge] = max_level;

    if (black != 0U && content_elsewhere != 0U &&
        border_inset_limit(edge) > 0U) {
        uint32_t streak_needed = border_walking[edge] != 0U ?
            DCMI_BORDER_WALK_STREAK : DCMI_BORDER_GROW_STREAK;

        border_black_streak[edge]++;
        if (border_black_streak[edge] >= streak_needed) {
            border_black_streak[edge] = 0U;
            if ((g_dcmi_border_inset[edge] + DCMI_BORDER_STEP) <=
                border_inset_limit(edge)) {
                g_dcmi_border_inset[edge] += DCMI_BORDER_STEP;
                border_walking[edge] = 1U;
                g_dcmi_border_grow_count++;
            } else {
                /* Walked to the limit without finding picture: a dark scene
                 * slipped the guard, not a border. Snap back out. */
                g_dcmi_border_inset[edge] = 0U;
                border_walking[edge] = 0U;
                g_dcmi_border_giveup_count++;
            }
        }
    } else {
        border_black_streak[edge] = 0U;
        if (black == 0U) {
            border_walking[edge] = 0U;
        }
    }

    return 0U;
}
#endif /* DCMI_BORDER_ACTIVE */

#if DCMI_DIAGNOSTICS
/* Debug-only whole-buffer statistics: the slow per-sample pass the LED
 * color path no longer pays for. Changes no LED state.
 */
static void analyze_buffer_diagnostics(uint32_t words_to_analyze)
{
    uint32_t previous = 0U;
    const dcmi_edge_crop_t *crop = &edge_crops[active_crop_index];
    uint32_t nonzero = 0U;
    uint32_t changing = 0U;
    uint32_t checksum = 0U;
    uint32_t sample_sum = 0U;
    uint32_t sample_count = 0U;
    uint32_t min_byte = 0xFFU;
    uint32_t max_byte = 0U;
    uint32_t zone_sum[4] = {0U, 0U, 0U, 0U};
    uint32_t red_sum = 0U;
    uint32_t green_sum = 0U;
    uint32_t blue_sum = 0U;
    uint32_t frame_min_r = 0xFFU;
    uint32_t frame_min_g = 0xFFU;
    uint32_t frame_min_b = 0xFFU;
    uint32_t zone_red_sum[4] = {0U, 0U, 0U, 0U};
    uint32_t zone_green_sum[4] = {0U, 0U, 0U, 0U};
    uint32_t zone_blue_sum[4] = {0U, 0U, 0U, 0U};
    uint32_t bit_count[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    uint32_t zone_count[4] = {0U, 0U, 0U, 0U};

    if (words_to_analyze > 0U) {
        previous = dcmi_buffer[0];
    }

    for (uint32_t i = 0U; i < words_to_analyze; i++) {
        uint32_t word = dcmi_buffer[i];

        if (word != 0U) {
            nonzero++;
        }

        if (i > 0U && word != previous) {
            changing++;
        }

        checksum += word;
        previous = word;

        for (uint32_t shift = 0U; shift < 32U; shift += 8U) {
            uint32_t sample = (word >> shift) & 0xFFU;
            uint32_t zone = crop->zone;
            uint32_t raw_red = r3_to_8bit[(sample >> 5U) & 0x07U];
            uint32_t raw_green = g3_to_8bit[(sample >> 2U) & 0x07U];
            uint32_t raw_blue = b2_to_8bit[sample & 0x03U];
            uint32_t red = rgb332_red_lut[sample];
            uint32_t green = rgb332_green_lut[sample];
            uint32_t blue = rgb332_blue_lut[sample];

            if (raw_red < frame_min_r) {
                frame_min_r = raw_red;
            }
            if (raw_green < frame_min_g) {
                frame_min_g = raw_green;
            }
            if (raw_blue < frame_min_b) {
                frame_min_b = raw_blue;
            }

            if (zone > 3U) {
                zone = 3U;
            }

            sample_sum += sample;
            red_sum += red;
            green_sum += green;
            blue_sum += blue;
            zone_sum[zone] += sample;
            zone_red_sum[zone] += red;
            zone_green_sum[zone] += green;
            zone_blue_sum[zone] += blue;

            for (uint32_t bit = 0U; bit < 8U; bit++) {
                if ((sample & (1U << bit)) != 0U) {
                    bit_count[bit]++;
                }
            }

            zone_count[zone]++;
            sample_count++;

            if (sample < min_byte) {
                min_byte = sample;
            }

            if (sample > max_byte) {
                max_byte = sample;
            }
        }
    }

    g_dcmi_first_word = words_to_analyze > 0U ? dcmi_buffer[0] : 0U;
    g_dcmi_second_word = words_to_analyze > 1U ? dcmi_buffer[1] : 0U;
    g_dcmi_last_word = words_to_analyze > 0U ? dcmi_buffer[words_to_analyze - 1U] : 0U;
    g_dcmi_nonzero_words = nonzero;
    g_dcmi_changing_words = changing;
    g_dcmi_buffer_checksum = checksum;
    g_dcmi_sample_count = sample_count;
    if (sample_count > 0U) {
        g_dcmi_observed_baseline_r = frame_min_r;
        g_dcmi_observed_baseline_g = frame_min_g;
        g_dcmi_observed_baseline_b = frame_min_b;
    }

    if (sample_count > 0U) {
        g_dcmi_average_byte = sample_sum / sample_count;
        g_dcmi_brightness_percent = (g_dcmi_average_byte * 100U) / 255U;
        g_dcmi_average_r = red_sum / sample_count;
        g_dcmi_average_g = green_sum / sample_count;
        g_dcmi_average_b = blue_sum / sample_count;
        g_dcmi_bit0_percent = (bit_count[0] * 100U) / sample_count;
        g_dcmi_bit1_percent = (bit_count[1] * 100U) / sample_count;
        g_dcmi_bit2_percent = (bit_count[2] * 100U) / sample_count;
        g_dcmi_bit3_percent = (bit_count[3] * 100U) / sample_count;
        g_dcmi_bit4_percent = (bit_count[4] * 100U) / sample_count;
        g_dcmi_bit5_percent = (bit_count[5] * 100U) / sample_count;
        g_dcmi_bit6_percent = (bit_count[6] * 100U) / sample_count;
        g_dcmi_bit7_percent = (bit_count[7] * 100U) / sample_count;
    } else {
        g_dcmi_average_byte = 0U;
        g_dcmi_brightness_percent = 0U;
        g_dcmi_average_r = 0U;
        g_dcmi_average_g = 0U;
        g_dcmi_average_b = 0U;
        g_dcmi_bit0_percent = 0U;
        g_dcmi_bit1_percent = 0U;
        g_dcmi_bit2_percent = 0U;
        g_dcmi_bit3_percent = 0U;
        g_dcmi_bit4_percent = 0U;
        g_dcmi_bit5_percent = 0U;
        g_dcmi_bit6_percent = 0U;
        g_dcmi_bit7_percent = 0U;
        min_byte = 0U;
    }

    g_dcmi_min_byte = min_byte;
    g_dcmi_max_byte = max_byte;

    g_dcmi_zone0_brightness_percent = 0U;
    g_dcmi_zone0_r = 0U;
    g_dcmi_zone0_g = 0U;
    g_dcmi_zone0_b = 0U;
    g_dcmi_zone1_brightness_percent = 0U;
    g_dcmi_zone1_r = 0U;
    g_dcmi_zone1_g = 0U;
    g_dcmi_zone1_b = 0U;
    g_dcmi_zone2_brightness_percent = 0U;
    g_dcmi_zone2_r = 0U;
    g_dcmi_zone2_g = 0U;
    g_dcmi_zone2_b = 0U;
    g_dcmi_zone3_brightness_percent = 0U;
    g_dcmi_zone3_r = 0U;
    g_dcmi_zone3_g = 0U;
    g_dcmi_zone3_b = 0U;

    if (zone_count[0] > 0U) {
        g_dcmi_zone0_brightness_percent = ((zone_sum[0] / zone_count[0]) * 100U) / 255U;
        g_dcmi_zone0_r = zone_red_sum[0] / zone_count[0];
        g_dcmi_zone0_g = zone_green_sum[0] / zone_count[0];
        g_dcmi_zone0_b = zone_blue_sum[0] / zone_count[0];
    }

    if (zone_count[1] > 0U) {
        g_dcmi_zone1_brightness_percent = ((zone_sum[1] / zone_count[1]) * 100U) / 255U;
        g_dcmi_zone1_r = zone_red_sum[1] / zone_count[1];
        g_dcmi_zone1_g = zone_green_sum[1] / zone_count[1];
        g_dcmi_zone1_b = zone_blue_sum[1] / zone_count[1];
    }

    if (zone_count[2] > 0U) {
        g_dcmi_zone2_brightness_percent = ((zone_sum[2] / zone_count[2]) * 100U) / 255U;
        g_dcmi_zone2_r = zone_red_sum[2] / zone_count[2];
        g_dcmi_zone2_g = zone_green_sum[2] / zone_count[2];
        g_dcmi_zone2_b = zone_blue_sum[2] / zone_count[2];
    }

    if (zone_count[3] > 0U) {
        g_dcmi_zone3_brightness_percent = ((zone_sum[3] / zone_count[3]) * 100U) / 255U;
        g_dcmi_zone3_r = zone_red_sum[3] / zone_count[3];
        g_dcmi_zone3_g = zone_green_sum[3] / zone_count[3];
        g_dcmi_zone3_b = zone_blue_sum[3] / zone_count[3];
    }
}
#endif /* DCMI_DIAGNOSTICS */

static void init_rgb332_luts(void)
{
    for (uint32_t sample = 0U; sample < 256U; sample++) {
        uint32_t red = r3_to_8bit[(sample >> 5U) & 0x07U];
        uint32_t green = g3_to_8bit[(sample >> 2U) & 0x07U];
        uint32_t blue = b2_to_8bit[sample & 0x03U];
        uint32_t weight;

        red = rescale_with_baseline(red, DCMI_BASELINE_R);
        green = rescale_with_baseline(green, DCMI_BASELINE_G);
        blue = rescale_with_baseline(blue, DCMI_BASELINE_B);

        rgb332_red_lut[sample] = (uint8_t)red;
        rgb332_green_lut[sample] = (uint8_t)green;
        rgb332_blue_lut[sample] = (uint8_t)blue;

        /* Favor vivid edge pixels over dark background pixels, while still
         * letting low-light scenes contribute. This gives each LED a
         * nearby-object color instead of a washed panel average.
         */
        weight = red;
        if (green > weight) {
            weight = green;
        }
        if (blue > weight) {
            weight = blue;
        }

        rgb332_weight_lut[sample] =
            weight > 0U ? (uint16_t)(weight + 16U) : 1U;
    }
}



static uint32_t rescale_with_baseline(uint32_t value, uint32_t baseline)
{
    if (value <= baseline) {
        return 0U;
    }

    if (baseline >= 255U) {
        return 0U;
    }

    return ((value - baseline) * 255U) / (255U - baseline);
}

static uint32_t cartesian_y_to_dcmi_y(uint32_t cart_y, uint32_t height)
{
    if ((cart_y + height) >= VIDEO_ACTIVE_HEIGHT) {
        return 0U;
    }

    return VIDEO_ACTIVE_HEIGHT - cart_y - height;
}

static uint32_t edge_zone_count(uint32_t edge)
{
    switch (edge) {
    case DCMI_EDGE_RIGHT:
        return DCMI_RIGHT_ZONE_COUNT;
    case DCMI_EDGE_TOP:
        return DCMI_TOP_ZONE_COUNT;
    case DCMI_EDGE_LEFT:
        return DCMI_LEFT_ZONE_COUNT;
    case DCMI_EDGE_BOTTOM:
        return DCMI_BOTTOM_ZONE_COUNT;
    default:
        return 1U;
    }
}

static uint32_t edge_zone_offset(uint32_t edge)
{
    switch (edge) {
    case DCMI_EDGE_RIGHT:
        return DCMI_RIGHT_ZONE_OFFSET;
    case DCMI_EDGE_TOP:
        return DCMI_TOP_ZONE_OFFSET;
    case DCMI_EDGE_LEFT:
        return DCMI_LEFT_ZONE_OFFSET;
    case DCMI_EDGE_BOTTOM:
        return DCMI_BOTTOM_ZONE_OFFSET;
    default:
        return 0U;
    }
}

static uint32_t edge_local_zone(uint32_t edge, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height)
{
    uint32_t count = edge_zone_count(edge);
    uint32_t local = 0U;

    if (count == 0U) {
        return 0U;
    }

    switch (edge) {
    case DCMI_EDGE_RIGHT:
        /* Physical strip order on the right side is bottom -> top, while DCMI
         * samples rows top -> bottom. Reverse Y to keep zone 0 at bottom-right.
         * Use (2*y*count + height) / (2*height) for better rounding distribution.
         */
        local = (2U * y * count + height) / (2U * height);
        local = (count - 1U) - local;
        break;
    case DCMI_EDGE_TOP:
        /* Physical top segment is top-right -> top-left. Reverse X.
         * Improved rounding: (2*x*count + width) / (2*width)
         */
        local = (2U * x * count + width) / (2U * width);
        local = (count - 1U) - local;
        break;
    case DCMI_EDGE_LEFT:
        /* Physical left segment is top-left -> bottom-left, same as DCMI Y.
         * Improved rounding for better zone distribution.
         */
        local = (2U * y * count + height) / (2U * height);
        break;
    case DCMI_EDGE_BOTTOM:
        /* Physical bottom segment is bottom-left -> bottom-right, same as X.
         * Improved rounding for better zone distribution.
         */
        local = (2U * x * count + width) / (2U * width);
        break;
    default:
        local = 0U;
        break;
    }

    if (local >= count) {
        local = count - 1U;
    }

    return local;
}

static void side_band_zone_pair(uint32_t *right_zone, uint32_t *left_zone)
{
    uint32_t right_index = active_side_band_index % DCMI_RIGHT_ZONE_COUNT;
    uint32_t left_index = active_side_band_index % DCMI_LEFT_ZONE_COUNT;

    if (active_crop_index == DCMI_EDGE_RIGHT) {
        /* Right band index 0 is bottom. The current left-zone convention uses
         * index 0 at top, so mirror the same physical Y position.
         */
        left_index = (DCMI_LEFT_ZONE_COUNT - 1U) -
                     (active_side_band_index % DCMI_LEFT_ZONE_COUNT);
    } else if (active_crop_index == DCMI_EDGE_LEFT) {
        /* Kept for diagnostic fallback: a left-scheduled band mirrors into
         * the corresponding right-side physical Y position.
         */
        right_index = (DCMI_RIGHT_ZONE_COUNT - 1U) -
                      (active_side_band_index % DCMI_RIGHT_ZONE_COUNT);
    }

    *right_zone = DCMI_RIGHT_ZONE_OFFSET + right_index;
    *left_zone = DCMI_LEFT_ZONE_OFFSET + left_index;
}

static void accumulate_led_zone_sample(uint32_t led_zone, uint32_t sample)
{
    uint32_t weight;

    if (led_zone >= DCMI_LED_ZONE_COUNT) {
        return;
    }

    weight = rgb332_weight_lut[sample];
    led_zone_red_sum[led_zone] += (uint32_t)rgb332_red_lut[sample] * weight;
    led_zone_green_sum[led_zone] += (uint32_t)rgb332_green_lut[sample] * weight;
    led_zone_blue_sum[led_zone] += (uint32_t)rgb332_blue_lut[sample] * weight;
    led_zone_weight_sum[led_zone] += weight;
    led_zone_sample_count[led_zone]++;
}

static uint32_t capture_accept_words(void)
{
#if DCMI_CROP_TEST_MODE || DCMI_FULL_HEIGHT_SIDE_TEST_MODE
    /* A transport test succeeds only if the complete programmed rectangle
     * reaches DMA. Do not hide under-fills behind responsive early acceptance.
     */
    return active_capture_words;
#else
#if DCMI_SIDE_HORIZONTAL_BANDS
    uint32_t accept_words = DCMI_SIDE_EARLY_ACCEPT;
#else
    /* Vertical side crops must cover every row or the lower zones starve;
     * no early acceptance for sides.
     */
    uint32_t accept_words = active_capture_words;
#endif

    /* Wide top/bottom crops give every horizontal LED zone useful data even
     * from a partial band: 1000 words = 4000 pixels, roughly 90 samples per
     * 43-zone edge. Keep a lower early-accept threshold here than on side
     * crops, where coverage depends on completing the rectangle.
     */
    if (active_crop_index == DCMI_EDGE_TOP ||
        active_crop_index == DCMI_EDGE_BOTTOM) {
        accept_words = DCMI_TOP_BOTTOM_EARLY_ACCEPT;
    }

    if (accept_words > active_capture_words) {
        accept_words = active_capture_words;
    }

    return accept_words;
#endif
}

static uint32_t capture_min_accept_words(void)
{
#if DCMI_CROP_TEST_MODE
    return active_capture_words;
#elif DCMI_FULL_HEIGHT_SIDE_TEST_MODE
    if (active_capture_words > DCMI_FULL_HEIGHT_SIDE_TEST_MAX_MISSING_WORDS) {
        return active_capture_words - DCMI_FULL_HEIGHT_SIDE_TEST_MAX_MISSING_WORDS;
    }
    return active_capture_words;
#else
#if DCMI_SIDE_HORIZONTAL_BANDS
    uint32_t min_words = DCMI_TIMEOUT_ACCEPT_FLOOR_SIDE;
#else
    uint32_t min_words =
        active_capture_words > DCMI_SIDE_VERTICAL_TIMEOUT_MISSING_WORDS ?
        active_capture_words - DCMI_SIDE_VERTICAL_TIMEOUT_MISSING_WORDS :
        active_capture_words;
#endif

    if (active_crop_index == DCMI_EDGE_TOP ||
        active_crop_index == DCMI_EDGE_BOTTOM) {
        min_words = DCMI_TIMEOUT_ACCEPT_FLOOR_TOP_BOTTOM;
    }

    if (min_words > active_capture_words) {
        min_words = active_capture_words;
    }

    return min_words;
#endif
}

#if DCMI_SIDE_HORIZONTAL_BANDS
static uint32_t side_band_cartesian_y(uint32_t edge, uint32_t band_index)
{
    uint32_t count = edge_zone_count(edge);
    uint32_t max_y = VIDEO_ACTIVE_HEIGHT - EDGE_BORDER_PIXELS;
    uint32_t divisor;

    if (count <= 1U) {
        return 0U;
    }

    if (band_index >= count) {
        band_index = count - 1U;
    }

    divisor = count - 1U;

    if (edge == DCMI_EDGE_LEFT) {
        band_index = divisor - band_index;
    }

    return (band_index * max_y) / divisor;
}
#endif

#if !DCMI_INCREMENTAL_PUBLISH
static void begin_perimeter_cycle_if_needed(void)
{
    if (perimeter_edges_done_mask != 0U) {
        return;
    }

    for (uint32_t zone_index = 0U; zone_index < DCMI_LED_ZONE_COUNT; zone_index++) {
        led_zone_red_sum[zone_index] = 0U;
        led_zone_green_sum[zone_index] = 0U;
        led_zone_blue_sum[zone_index] = 0U;
        led_zone_weight_sum[zone_index] = 0U;
        led_zone_sample_count[zone_index] = 0U;
    }
}

static uint32_t mark_current_perimeter_edge_done(void)
{
    if (active_crop_index < DCMI_EDGE_COUNT) {
#if DCMI_SIDE_HORIZONTAL_BANDS
        if (active_crop_index == DCMI_EDGE_RIGHT ||
            active_crop_index == DCMI_EDGE_LEFT) {
            uint32_t count = edge_zone_count(active_crop_index);

            if ((active_side_band_index + 1U) < count) {
                return 0U;
            }

            perimeter_edges_done_mask |= (1UL << DCMI_EDGE_RIGHT);
            perimeter_edges_done_mask |= (1UL << DCMI_EDGE_LEFT);
            return perimeter_edges_done_mask == DCMI_PERIMETER_COMPLETE_MASK;
        }
#endif
        perimeter_edges_done_mask |= (1UL << active_crop_index);
    }

    if ((perimeter_edges_done_mask & DCMI_PERIMETER_COMPLETE_MASK) !=
        DCMI_PERIMETER_COMPLETE_MASK) {
        return 0U;
    }

    perimeter_edges_done_mask = 0U;
    return 1U;
}
#endif

static uint32_t blend_channel(uint32_t prev_value,
                              uint32_t raw_value,
                              uint32_t alpha_num,
                              uint32_t alpha_den)
{
    if (alpha_den == 0U || alpha_num > alpha_den) {
        return raw_value;
    }

    return ((prev_value * (alpha_den - alpha_num)) +
            (raw_value * alpha_num)) / alpha_den;
}

#if DCMI_INCREMENTAL_PUBLISH
static void reset_zone_accumulator(uint32_t zone_index)
{
    if (zone_index >= DCMI_LED_ZONE_COUNT) {
        return;
    }

    led_zone_red_sum[zone_index] = 0U;
    led_zone_green_sum[zone_index] = 0U;
    led_zone_blue_sum[zone_index] = 0U;
    led_zone_weight_sum[zone_index] = 0U;
    led_zone_sample_count[zone_index] = 0U;
}

static uint32_t apply_sampled_zone(uint32_t zone_index,
                                   uint32_t force_snap,
                                   uint32_t *frame_delta_max,
                                   uint32_t *frame_delta_sum,
                                   uint32_t *frame_delta_count,
                                   uint32_t *frame_spike_reject,
                                   uint32_t *frame_low_trust_spike)
{
    uint32_t raw_r;
    uint32_t raw_g;
    uint32_t raw_b;
    uint32_t prev_r;
    uint32_t prev_g;
    uint32_t prev_b;
    uint32_t delta_r;
    uint32_t delta_g;
    uint32_t delta_b;
    uint32_t color_delta;
    uint32_t alpha_num = DCMI_SMOOTH_ALPHA_WEAK_NUM;
    uint32_t alpha_den = DCMI_SMOOTH_ALPHA_WEAK_DEN;
    uint32_t low_trust_zone;

    if (zone_index >= DCMI_LED_ZONE_COUNT ||
        led_zone_sample_count[zone_index] < DCMI_ZONE_MIN_SAMPLES ||
        led_zone_weight_sum[zone_index] == 0U) {
        return 0U;
    }

    raw_r = led_zone_red_sum[zone_index] / led_zone_weight_sum[zone_index];
    raw_g = led_zone_green_sum[zone_index] / led_zone_weight_sum[zone_index];
    raw_b = led_zone_blue_sum[zone_index] / led_zone_weight_sum[zone_index];
    prev_r = smoothed_led_zone_r[zone_index];
    prev_g = smoothed_led_zone_g[zone_index];
    prev_b = smoothed_led_zone_b[zone_index];
    delta_r = (raw_r > prev_r) ? (raw_r - prev_r) : (prev_r - raw_r);
    delta_g = (raw_g > prev_g) ? (raw_g - prev_g) : (prev_g - raw_g);
    delta_b = (raw_b > prev_b) ? (raw_b - prev_b) : (prev_b - raw_b);
    color_delta = delta_r;

    if (delta_g > color_delta) {
        color_delta = delta_g;
    }
    if (delta_b > color_delta) {
        color_delta = delta_b;
    }

    if (frame_delta_max != NULL && color_delta > *frame_delta_max) {
        *frame_delta_max = color_delta;
    }
    if (frame_delta_sum != NULL) {
        *frame_delta_sum += color_delta;
    }
    if (frame_delta_count != NULL) {
        (*frame_delta_count)++;
    }

    low_trust_zone =
        led_zone_sample_count[zone_index] < DCMI_ZONE_LOW_TRUST_SAMPLES ? 1U : 0U;

    if (force_snap == 0U &&
        low_trust_zone != 0U &&
        color_delta >= DCMI_ZONE_LOW_TRUST_SPIKE_DELTA) {
        /* Too few samples plus a huge jump is usually transport noise. */
        if (frame_spike_reject != NULL) {
            (*frame_spike_reject)++;
        }
        if (frame_low_trust_spike != NULL) {
            (*frame_low_trust_spike)++;
        }
        if (led_zone_missed_count[zone_index] < 0xFFFFFFFFU) {
            led_zone_missed_count[zone_index]++;
        }
        return 0U;
    }

    if (force_snap != 0U ||
        (low_trust_zone == 0U &&
         color_delta >= DCMI_SMOOTH_SCENE_CUT_DELTA)) {
        /* Trusted cuts take the raw color outright; easing in reads as lag. */
        alpha_num = DCMI_SMOOTH_ALPHA_SNAP_NUM;
        alpha_den = DCMI_SMOOTH_ALPHA_SNAP_DEN;
    } else if (color_delta <= DCMI_SMOOTH_DELTA_MED_MAX) {
        if (color_delta > DCMI_SMOOTH_DELTA_STRONG_MAX) {
            alpha_num = DCMI_SMOOTH_ALPHA_MED_NUM;
            alpha_den = DCMI_SMOOTH_ALPHA_MED_DEN;
        }
    } else if (low_trust_zone == 0U) {
        alpha_num = DCMI_SMOOTH_ALPHA_STRONG_NUM;
        alpha_den = DCMI_SMOOTH_ALPHA_STRONG_DEN;
    }

    smoothed_led_zone_r[zone_index] =
        blend_channel(smoothed_led_zone_r[zone_index], raw_r, alpha_num, alpha_den);
    smoothed_led_zone_g[zone_index] =
        blend_channel(smoothed_led_zone_g[zone_index], raw_g, alpha_num, alpha_den);
    smoothed_led_zone_b[zone_index] =
        blend_channel(smoothed_led_zone_b[zone_index], raw_b, alpha_num, alpha_den);

    g_dcmi_led_zone_r[zone_index] = smoothed_led_zone_r[zone_index];
    g_dcmi_led_zone_g[zone_index] = smoothed_led_zone_g[zone_index];
    g_dcmi_led_zone_b[zone_index] = smoothed_led_zone_b[zone_index];
    led_zone_missed_count[zone_index] = 0U;

    if (led_zone_update_counter[zone_index] < 0xFFFFFFFFU) {
        led_zone_update_counter[zone_index]++;
    }

    return 1U;
}

static uint32_t zone_in_current_capture(uint32_t zone_index)
{
    if (zone_index >= DCMI_LED_ZONE_COUNT) {
        return 0U;
    }

    if (active_crop_index == DCMI_EDGE_TOP) {
        return (zone_index >= DCMI_TOP_ZONE_OFFSET &&
                zone_index < DCMI_LEFT_ZONE_OFFSET) ? 1U : 0U;
    }

    if (active_crop_index == DCMI_EDGE_BOTTOM) {
        return (zone_index >= DCMI_BOTTOM_ZONE_OFFSET &&
                zone_index < DCMI_LED_ZONE_COUNT) ? 1U : 0U;
    }

#if DCMI_SIDE_HORIZONTAL_BANDS
    if (active_crop_index == DCMI_EDGE_RIGHT ||
        active_crop_index == DCMI_EDGE_LEFT) {
        uint32_t right_zone;
        uint32_t left_zone;

        side_band_zone_pair(&right_zone, &left_zone);
        return (zone_index == right_zone || zone_index == left_zone) ? 1U : 0U;
    }
#else
    /* Vertical side crops sample the full edge, so every zone on that side
     * belongs to this capture.
     */
    if (active_crop_index == DCMI_EDGE_RIGHT) {
        return (zone_index >= DCMI_RIGHT_ZONE_OFFSET &&
                zone_index < DCMI_RIGHT_ZONE_OFFSET + DCMI_RIGHT_ZONE_COUNT) ? 1U : 0U;
    }
    if (active_crop_index == DCMI_EDGE_LEFT) {
        return (zone_index >= DCMI_LEFT_ZONE_OFFSET &&
                zone_index < DCMI_LEFT_ZONE_OFFSET + DCMI_LEFT_ZONE_COUNT) ? 1U : 0U;
    }
#endif

    return 0U;
}

static void clear_current_capture_zones(void)
{
    for (uint32_t zone_index = 0U; zone_index < DCMI_LED_ZONE_COUNT; zone_index++) {
        if (zone_in_current_capture(zone_index) != 0U) {
            reset_zone_accumulator(zone_index);
        }
    }
}

static void finalize_incremental_capture(void)
{
    uint32_t expected_zones = 0U;
    uint32_t updated_zones = 0U;
    uint32_t force_snap = 0U;
    uint32_t frame_delta_max = 0U;
    uint32_t frame_delta_sum = 0U;
    uint32_t frame_delta_count = 0U;
    uint32_t frame_spike_reject = 0U;
    uint32_t frame_low_trust_spike = 0U;
    uint32_t top_good_zones = 0U;
    uint32_t top_min_level = 0xFFFFFFFFU;
    uint32_t top_max_level = 0U;
    uint32_t top_raw_valid_zones = 0U;
    uint32_t top_raw_min_level = 0xFFFFFFFFU;
    uint32_t top_raw_max_level = 0U;

    /* Pass 1: measure how much of this capture moved. A coherent jump across
     * a third of the sampled zones is a real scene change (cut, explosion,
     * lightsaber sweep), so pass 2 snaps instead of easing and skips the
     * per-zone spike rejection that would otherwise eat it.
     */
    {
        uint32_t candidate_zones = 0U;
        uint32_t big_delta_zones = 0U;
        uint32_t capture_max_level = 0U;

        for (uint32_t zone_index = 0U; zone_index < DCMI_LED_ZONE_COUNT; zone_index++) {
            uint32_t raw_r;
            uint32_t raw_g;
            uint32_t raw_b;
            uint32_t level;
            uint32_t delta;
            uint32_t channel_delta;

            if (zone_in_current_capture(zone_index) == 0U ||
                led_zone_sample_count[zone_index] < DCMI_ZONE_MIN_SAMPLES ||
                led_zone_weight_sum[zone_index] == 0U) {
                continue;
            }

            raw_r = led_zone_red_sum[zone_index] / led_zone_weight_sum[zone_index];
            raw_g = led_zone_green_sum[zone_index] / led_zone_weight_sum[zone_index];
            raw_b = led_zone_blue_sum[zone_index] / led_zone_weight_sum[zone_index];
            level = raw_r;
            if (raw_g > level) {
                level = raw_g;
            }
            if (raw_b > level) {
                level = raw_b;
            }
            if (level > capture_max_level) {
                capture_max_level = level;
            }
            delta = (raw_r > smoothed_led_zone_r[zone_index]) ?
                    (raw_r - smoothed_led_zone_r[zone_index]) :
                    (smoothed_led_zone_r[zone_index] - raw_r);
            channel_delta = (raw_g > smoothed_led_zone_g[zone_index]) ?
                            (raw_g - smoothed_led_zone_g[zone_index]) :
                            (smoothed_led_zone_g[zone_index] - raw_g);
            if (channel_delta > delta) {
                delta = channel_delta;
            }
            channel_delta = (raw_b > smoothed_led_zone_b[zone_index]) ?
                            (raw_b - smoothed_led_zone_b[zone_index]) :
                            (smoothed_led_zone_b[zone_index] - raw_b);
            if (channel_delta > delta) {
                delta = channel_delta;
            }

            candidate_zones++;
            if (delta > DCMI_SMOOTH_DELTA_MED_MAX) {
                big_delta_zones++;
            }
        }

#if DCMI_BORDER_ACTIVE
        if (border_process_capture(capture_max_level, candidate_zones) != 0U) {
            /* Outward probe still sees the black bar: discard this capture
             * so the LEDs keep their content colors. */
            g_dcmi_frame_publish_skip_count++;
            return;
        }
#endif

        if (candidate_zones >= DCMI_GLOBAL_CUT_MIN_ZONES &&
            big_delta_zones * DCMI_GLOBAL_CUT_DEN >=
            candidate_zones * DCMI_GLOBAL_CUT_NUM) {
            force_snap = 1U;
            g_dcmi_global_cut_count++;
        }
    }

    for (uint32_t zone_index = 0U; zone_index < DCMI_LED_ZONE_COUNT; zone_index++) {
        if (zone_in_current_capture(zone_index) == 0U) {
            continue;
        }

        expected_zones++;
        updated_zones += apply_sampled_zone(zone_index,
                                            force_snap,
                                            &frame_delta_max,
                                            &frame_delta_sum,
                                            &frame_delta_count,
                                            &frame_spike_reject,
                                            &frame_low_trust_spike);
    }

    for (uint32_t zone_index = DCMI_TOP_ZONE_OFFSET;
         zone_index < DCMI_LEFT_ZONE_OFFSET;
         zone_index++) {
        uint32_t level = g_dcmi_led_zone_r[zone_index];
        uint32_t raw_r = 0U;
        uint32_t raw_g = 0U;
        uint32_t raw_b = 0U;
        uint32_t raw_level = 0U;

        if (g_dcmi_led_zone_g[zone_index] > level) {
            level = g_dcmi_led_zone_g[zone_index];
        }
        if (g_dcmi_led_zone_b[zone_index] > level) {
            level = g_dcmi_led_zone_b[zone_index];
        }
        if (level > 0U) {
            top_good_zones++;
        }
        if (level < top_min_level) {
            top_min_level = level;
        }
        if (level > top_max_level) {
            top_max_level = level;
        }

        if (led_zone_sample_count[zone_index] >= DCMI_ZONE_MIN_SAMPLES &&
            led_zone_weight_sum[zone_index] > 0U) {
            raw_r = led_zone_red_sum[zone_index] / led_zone_weight_sum[zone_index];
            raw_g = led_zone_green_sum[zone_index] / led_zone_weight_sum[zone_index];
            raw_b = led_zone_blue_sum[zone_index] / led_zone_weight_sum[zone_index];
            raw_level = raw_r;
            if (raw_g > raw_level) {
                raw_level = raw_g;
            }
            if (raw_b > raw_level) {
                raw_level = raw_b;
            }
            if (raw_level < top_raw_min_level) {
                top_raw_min_level = raw_level;
            }
            if (raw_level > top_raw_max_level) {
                top_raw_max_level = raw_level;
            }
            top_raw_valid_zones++;
        }

        if (zone_index == DCMI_TOP_ZONE_OFFSET) {
            g_dcmi_top_raw_first_r = raw_r;
            g_dcmi_top_raw_first_g = raw_g;
            g_dcmi_top_raw_first_b = raw_b;
            g_dcmi_top_first_samples = led_zone_sample_count[zone_index];
        } else if (zone_index == (DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U))) {
            g_dcmi_top_raw_mid_r = raw_r;
            g_dcmi_top_raw_mid_g = raw_g;
            g_dcmi_top_raw_mid_b = raw_b;
            g_dcmi_top_mid_samples = led_zone_sample_count[zone_index];
        } else if (zone_index == (DCMI_LEFT_ZONE_OFFSET - 1U)) {
            g_dcmi_top_raw_last_r = raw_r;
            g_dcmi_top_raw_last_g = raw_g;
            g_dcmi_top_raw_last_b = raw_b;
            g_dcmi_top_last_samples = led_zone_sample_count[zone_index];
        }
    }

    g_dcmi_zone_spike_reject_count += frame_spike_reject;
    g_dcmi_zone_low_trust_spike_count += frame_low_trust_spike;
    /* Incremental publishing preserves untouched zones instead of treating
     * them as missed every mini-frame, so these legacy full-cycle counters
     * intentionally do not change here.
     */
    g_dcmi_zone_miss_hold_count += 0U;
    g_dcmi_zone_miss_decay_count += 0U;
    g_dcmi_zone_color_delta_max = frame_delta_max;
    g_dcmi_zone_color_delta_avg =
        frame_delta_count > 0U ? (frame_delta_sum / frame_delta_count) : 0U;
    g_dcmi_frame_quality_last =
        expected_zones > 0U ? (updated_zones * 100U) / expected_zones : 0U;
    g_dcmi_top_good_zones = top_good_zones;
    g_dcmi_top_color_spread =
        top_max_level >= top_min_level ? (top_max_level - top_min_level) : 0U;
    g_dcmi_top_raw_color_spread =
        top_raw_valid_zones > 0U ? (top_raw_max_level - top_raw_min_level) : 0U;
    g_dcmi_top_first_r = g_dcmi_led_zone_r[DCMI_TOP_ZONE_OFFSET];
    g_dcmi_top_first_g = g_dcmi_led_zone_g[DCMI_TOP_ZONE_OFFSET];
    g_dcmi_top_first_b = g_dcmi_led_zone_b[DCMI_TOP_ZONE_OFFSET];
    g_dcmi_top_mid_r = g_dcmi_led_zone_r[DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U)];
    g_dcmi_top_mid_g = g_dcmi_led_zone_g[DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U)];
    g_dcmi_top_mid_b = g_dcmi_led_zone_b[DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U)];
    g_dcmi_top_last_r = g_dcmi_led_zone_r[DCMI_LEFT_ZONE_OFFSET - 1U];
    g_dcmi_top_last_g = g_dcmi_led_zone_g[DCMI_LEFT_ZONE_OFFSET - 1U];
    g_dcmi_top_last_b = g_dcmi_led_zone_b[DCMI_LEFT_ZONE_OFFSET - 1U];

    g_dcmi_zone_update_count++;
    if (updated_zones > 0U) {
        g_dcmi_led_update_pending = 1U;
    } else {
        g_dcmi_frame_publish_skip_count++;
    }
}
#endif

#if !DCMI_INCREMENTAL_PUBLISH
static void finalize_perimeter_cycle(void)
{
    uint32_t frame_good_zones = 0U;
    uint32_t frame_delta_max = 0U;
    uint32_t frame_delta_sum = 0U;
    uint32_t frame_delta_count = 0U;
    uint32_t frame_spike_reject = 0U;
    uint32_t frame_low_trust_spike = 0U;
    uint32_t frame_miss_hold = 0U;
    uint32_t frame_miss_decay = 0U;
    uint32_t top_good_zones = 0U;
    uint32_t top_min_level = 0xFFFFFFFFU;
    uint32_t top_max_level = 0U;

    for (uint32_t zone_index = 0U; zone_index < DCMI_LED_ZONE_COUNT; zone_index++) {
        if (led_zone_sample_count[zone_index] >= DCMI_ZONE_MIN_SAMPLES &&
            led_zone_weight_sum[zone_index] > 0U) {
            uint32_t raw_r = led_zone_red_sum[zone_index] / led_zone_weight_sum[zone_index];
            uint32_t raw_g = led_zone_green_sum[zone_index] / led_zone_weight_sum[zone_index];
            uint32_t raw_b = led_zone_blue_sum[zone_index] / led_zone_weight_sum[zone_index];
            uint32_t prev_r = smoothed_led_zone_r[zone_index];
            uint32_t prev_g = smoothed_led_zone_g[zone_index];
            uint32_t prev_b = smoothed_led_zone_b[zone_index];
            uint32_t delta_r = (raw_r > prev_r) ? (raw_r - prev_r) : (prev_r - raw_r);
            uint32_t delta_g = (raw_g > prev_g) ? (raw_g - prev_g) : (prev_g - raw_g);
            uint32_t delta_b = (raw_b > prev_b) ? (raw_b - prev_b) : (prev_b - raw_b);
            uint32_t color_delta = delta_r;
            uint32_t alpha_num = DCMI_SMOOTH_ALPHA_WEAK_NUM;
            uint32_t alpha_den = DCMI_SMOOTH_ALPHA_WEAK_DEN;
            uint32_t low_trust_zone;

            if (delta_g > color_delta) {
                color_delta = delta_g;
            }
            if (delta_b > color_delta) {
                color_delta = delta_b;
            }

            if (color_delta > frame_delta_max) {
                frame_delta_max = color_delta;
            }
            frame_delta_sum += color_delta;
            frame_delta_count++;

            low_trust_zone = led_zone_sample_count[zone_index] < DCMI_ZONE_LOW_TRUST_SAMPLES ? 1U : 0U;

            if (low_trust_zone != 0U &&
                color_delta >= DCMI_ZONE_LOW_TRUST_SPIKE_DELTA) {
                /* The zone has too little data and a huge jump: hold previous. */
                frame_spike_reject++;
                frame_low_trust_spike++;
                if (led_zone_missed_count[zone_index] < 0xFFFFFFFFU) {
                    led_zone_missed_count[zone_index]++;
                }
                continue;
            }

            if (low_trust_zone == 0U &&
                color_delta >= DCMI_SMOOTH_SCENE_CUT_DELTA) {
                /* Trusted scene cuts take the raw color outright. */
                alpha_num = DCMI_SMOOTH_ALPHA_SNAP_NUM;
                alpha_den = DCMI_SMOOTH_ALPHA_SNAP_DEN;
            } else if (color_delta <= DCMI_SMOOTH_DELTA_MED_MAX) {
                if (color_delta > DCMI_SMOOTH_DELTA_STRONG_MAX) {
                    alpha_num = DCMI_SMOOTH_ALPHA_MED_NUM;
                    alpha_den = DCMI_SMOOTH_ALPHA_MED_DEN;
                }
            } else if (low_trust_zone == 0U) {
                /* Meaningful local changes should not crawl across frames. */
                alpha_num = DCMI_SMOOTH_ALPHA_STRONG_NUM;
                alpha_den = DCMI_SMOOTH_ALPHA_STRONG_DEN;
            }

            smoothed_led_zone_r[zone_index] =
                blend_channel(smoothed_led_zone_r[zone_index], raw_r, alpha_num, alpha_den);
            smoothed_led_zone_g[zone_index] =
                blend_channel(smoothed_led_zone_g[zone_index], raw_g, alpha_num, alpha_den);
            smoothed_led_zone_b[zone_index] =
                blend_channel(smoothed_led_zone_b[zone_index], raw_b, alpha_num, alpha_den);

            g_dcmi_led_zone_r[zone_index] = smoothed_led_zone_r[zone_index];
            g_dcmi_led_zone_g[zone_index] = smoothed_led_zone_g[zone_index];
            g_dcmi_led_zone_b[zone_index] = smoothed_led_zone_b[zone_index];
            led_zone_missed_count[zone_index] = 0U;

            if (led_zone_update_counter[zone_index] < 0xFFFFFFFFU) {
                led_zone_update_counter[zone_index]++;
            }
            if (zone_index >= DCMI_TOP_ZONE_OFFSET &&
                zone_index < DCMI_LEFT_ZONE_OFFSET) {
                uint32_t level = g_dcmi_led_zone_r[zone_index];

                if (g_dcmi_led_zone_g[zone_index] > level) {
                    level = g_dcmi_led_zone_g[zone_index];
                }
                if (g_dcmi_led_zone_b[zone_index] > level) {
                    level = g_dcmi_led_zone_b[zone_index];
                }
                if (level < top_min_level) {
                    top_min_level = level;
                }
                if (level > top_max_level) {
                    top_max_level = level;
                }
                top_good_zones++;
            }
            frame_good_zones++;
        } else {
            if (led_zone_missed_count[zone_index] < DCMI_ZONE_MISS_HOLD_FRAMES) {
                frame_miss_hold++;
            } else {
                smoothed_led_zone_r[zone_index] =
                    (smoothed_led_zone_r[zone_index] * DCMI_ZONE_MISS_DECAY_NUM) /
                    DCMI_ZONE_MISS_DECAY_DEN;
                smoothed_led_zone_g[zone_index] =
                    (smoothed_led_zone_g[zone_index] * DCMI_ZONE_MISS_DECAY_NUM) /
                    DCMI_ZONE_MISS_DECAY_DEN;
                smoothed_led_zone_b[zone_index] =
                    (smoothed_led_zone_b[zone_index] * DCMI_ZONE_MISS_DECAY_NUM) /
                    DCMI_ZONE_MISS_DECAY_DEN;
                frame_miss_decay++;
            }
            g_dcmi_led_zone_r[zone_index] = smoothed_led_zone_r[zone_index];
            g_dcmi_led_zone_g[zone_index] = smoothed_led_zone_g[zone_index];
            g_dcmi_led_zone_b[zone_index] = smoothed_led_zone_b[zone_index];

            if (led_zone_missed_count[zone_index] < 0xFFFFFFFFU) {
                led_zone_missed_count[zone_index]++;
            }
        }
    }

    g_dcmi_zone_spike_reject_count += frame_spike_reject;
    g_dcmi_zone_low_trust_spike_count += frame_low_trust_spike;
    g_dcmi_zone_miss_hold_count += frame_miss_hold;
    g_dcmi_zone_miss_decay_count += frame_miss_decay;
    g_dcmi_zone_color_delta_max = frame_delta_max;
    g_dcmi_zone_color_delta_avg =
        frame_delta_count > 0U ? (frame_delta_sum / frame_delta_count) : 0U;
    g_dcmi_frame_quality_last = (frame_good_zones * 100U) / DCMI_LED_ZONE_COUNT;
    g_dcmi_top_good_zones = top_good_zones;
    g_dcmi_top_color_spread =
        top_good_zones > 0U ? (top_max_level - top_min_level) : 0U;
    g_dcmi_top_first_r = g_dcmi_led_zone_r[DCMI_TOP_ZONE_OFFSET];
    g_dcmi_top_first_g = g_dcmi_led_zone_g[DCMI_TOP_ZONE_OFFSET];
    g_dcmi_top_first_b = g_dcmi_led_zone_b[DCMI_TOP_ZONE_OFFSET];
    g_dcmi_top_mid_r = g_dcmi_led_zone_r[DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U)];
    g_dcmi_top_mid_g = g_dcmi_led_zone_g[DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U)];
    g_dcmi_top_mid_b = g_dcmi_led_zone_b[DCMI_TOP_ZONE_OFFSET + (DCMI_TOP_ZONE_COUNT / 2U)];
    g_dcmi_top_last_r = g_dcmi_led_zone_r[DCMI_LEFT_ZONE_OFFSET - 1U];
    g_dcmi_top_last_g = g_dcmi_led_zone_g[DCMI_LEFT_ZONE_OFFSET - 1U];
    g_dcmi_top_last_b = g_dcmi_led_zone_b[DCMI_LEFT_ZONE_OFFSET - 1U];

    g_dcmi_zone_update_count++;
    if (frame_good_zones >= DCMI_FRAME_MIN_GOOD_ZONES_TO_PUBLISH &&
        g_dcmi_frame_quality_last >= DCMI_FRAME_MIN_QUALITY_TO_PUBLISH) {
        g_dcmi_led_update_pending = 1U;
    } else {
        g_dcmi_frame_publish_skip_count++;
    }
}
#endif

static void mark_crop_accept(uint32_t early_accept)
{
    if (early_accept != 0U) {
        g_dcmi_early_accept_count++;
    }

    if (active_crop_index < DCMI_EDGE_COUNT) {
        g_dcmi_edge_success_count[active_crop_index]++;
        g_dcmi_edge_last_words[active_crop_index] = active_captured_words;
#if DCMI_SIDE_HORIZONTAL_BANDS
        if (active_crop_index == DCMI_EDGE_RIGHT) {
            g_dcmi_edge_success_count[DCMI_EDGE_LEFT]++;
            g_dcmi_edge_last_words[DCMI_EDGE_LEFT] = active_captured_words;
        } else if (active_crop_index == DCMI_EDGE_LEFT) {
            g_dcmi_edge_success_count[DCMI_EDGE_RIGHT]++;
            g_dcmi_edge_last_words[DCMI_EDGE_RIGHT] = active_captured_words;
        }
#endif
    }

    record_side_band_words(active_captured_words);

#if DCMI_SIDE_HORIZONTAL_BANDS
    advance_side_band_index();
#endif

    #if DCMI_BOTTOM_TIMED_BAND
    switch (active_crop_index) {
    case DCMI_EDGE_RIGHT:
        next_right_band_index = (active_side_band_index + 1U) % DCMI_RIGHT_ZONE_COUNT;
        g_dcmi_right_band_index = next_right_band_index;
        break;
    case DCMI_EDGE_BOTTOM:
        break;
    case DCMI_EDGE_LEFT:
        next_left_band_index = (active_side_band_index + 1U) % DCMI_LEFT_ZONE_COUNT;
        g_dcmi_left_band_index = next_left_band_index;
        break;
    default:
        break;
    }
    #endif
}

static void mark_crop_zero(void)
{
    g_dcmi_zero_frame_count++;

    if (active_crop_index < DCMI_EDGE_COUNT) {
        g_dcmi_edge_zero_count[active_crop_index]++;
        g_dcmi_edge_last_words[active_crop_index] = active_captured_words;
#if DCMI_SIDE_HORIZONTAL_BANDS
        if (active_crop_index == DCMI_EDGE_RIGHT) {
            g_dcmi_edge_zero_count[DCMI_EDGE_LEFT]++;
            g_dcmi_edge_last_words[DCMI_EDGE_LEFT] = active_captured_words;
        } else if (active_crop_index == DCMI_EDGE_LEFT) {
            g_dcmi_edge_zero_count[DCMI_EDGE_RIGHT]++;
            g_dcmi_edge_last_words[DCMI_EDGE_RIGHT] = active_captured_words;
        }
#endif
    }

    record_side_band_words(active_captured_words);

    /* A zero/underfilled capture is a transport miss, not black screen data.
     * Preserve the last good LED values, but do not wedge the perimeter scanner
     * on one flaky crop/band. The next capture should keep walking so other LEDs
     * remain responsive while this band gets another chance on the next pass.
     */
#if DCMI_SIDE_HORIZONTAL_BANDS
    advance_side_band_index();
#endif
}

#if DCMI_SIDE_HORIZONTAL_BANDS
static void advance_side_band_index(void)
{
    switch (active_crop_index) {
    case DCMI_EDGE_RIGHT:
        next_right_band_index = (active_side_band_index + 1U) % DCMI_RIGHT_ZONE_COUNT;
        g_dcmi_right_band_index = next_right_band_index;
        next_left_band_index = (DCMI_LEFT_ZONE_COUNT - 1U) -
                               (next_right_band_index % DCMI_LEFT_ZONE_COUNT);
        g_dcmi_left_band_index = next_left_band_index;
        break;
    case DCMI_EDGE_LEFT:
        next_left_band_index = (active_side_band_index + 1U) % DCMI_LEFT_ZONE_COUNT;
        g_dcmi_left_band_index = next_left_band_index;
        next_right_band_index = (DCMI_RIGHT_ZONE_COUNT - 1U) -
                                (next_left_band_index % DCMI_RIGHT_ZONE_COUNT);
        g_dcmi_right_band_index = next_right_band_index;
        break;
    default:
        break;
    }
}
#endif

static void record_side_band_words(uint32_t words)
{
    if (DCMI_SIDE_HORIZONTAL_BANDS != 0U &&
        (active_crop_index == DCMI_EDGE_RIGHT ||
         active_crop_index == DCMI_EDGE_LEFT)) {
        uint32_t right_zone;
        uint32_t left_zone;
        uint32_t right_index;
        uint32_t left_index;

        side_band_zone_pair(&right_zone, &left_zone);
        right_index = right_zone - DCMI_RIGHT_ZONE_OFFSET;
        left_index = left_zone - DCMI_LEFT_ZONE_OFFSET;

        if (right_index < DCMI_RIGHT_ZONE_COUNT) {
            g_dcmi_right_band_last_words[right_index] = words;
            if (words > g_dcmi_right_band_max_words[right_index]) {
                g_dcmi_right_band_max_words[right_index] = words;
            }
        }

        if (left_index < DCMI_LEFT_ZONE_COUNT) {
            g_dcmi_left_band_last_words[left_index] = words;
            if (words > g_dcmi_left_band_max_words[left_index]) {
                g_dcmi_left_band_max_words[left_index] = words;
            }
        }
        return;
    }

    if (active_crop_index == DCMI_EDGE_RIGHT &&
        active_side_band_index < DCMI_RIGHT_ZONE_COUNT) {
        g_dcmi_right_band_last_words[active_side_band_index] = words;
        if (words > g_dcmi_right_band_max_words[active_side_band_index]) {
            g_dcmi_right_band_max_words[active_side_band_index] = words;
        }
    } else if (active_crop_index == DCMI_EDGE_LEFT &&
               active_side_band_index < DCMI_LEFT_ZONE_COUNT) {
        g_dcmi_left_band_last_words[active_side_band_index] = words;
        if (words > g_dcmi_left_band_max_words[active_side_band_index]) {
            g_dcmi_left_band_max_words[active_side_band_index] = words;
        }
    }
}

static void record_full_height_side_result(uint32_t words, uint32_t full)
{
#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE
    uint32_t capture_ms = HAL_GetTick() - last_restart_ms;
    uint32_t missing_words =
        words <= active_capture_words ? active_capture_words - words : 0U;

    g_dcmi_side_test_last_words = words;
    g_dcmi_side_test_missing_words = missing_words;
    g_dcmi_side_test_last_capture_ms = capture_ms;
    if (words > g_dcmi_side_test_max_words) {
        g_dcmi_side_test_max_words = words;
    }
    if (capture_ms > g_dcmi_side_test_max_capture_ms) {
        g_dcmi_side_test_max_capture_ms = capture_ms;
    }

    if (full != 0U && words == active_capture_words) {
        g_dcmi_side_test_full_count++;
    } else if (words > 0U &&
               missing_words <= DCMI_FULL_HEIGHT_SIDE_TEST_MAX_MISSING_WORDS) {
        g_dcmi_side_test_near_full_count++;
    } else if (words == 0U) {
        g_dcmi_side_test_zero_count++;
    } else {
        g_dcmi_side_test_partial_count++;
    }
#else
    (void)words;
    (void)full;
#endif
}

#if DCMI_FULL_HEIGHT_SIDE_TEST_MODE
static void analyze_full_height_side_rows(void)
{
    uint32_t words_per_row;
    uint32_t complete_rows;
    uint32_t previous_checksum = 0U;
    uint32_t nonzero_rows = 0U;
    uint32_t changing_rows = 0U;

    if (active_crop_height == 0U ||
        active_crop_width == 0U ||
        (active_crop_width % 4U) != 0U) {
        return;
    }

    words_per_row = active_crop_width / 4U;
    complete_rows = active_captured_words / words_per_row;
    if (complete_rows > active_crop_height) {
        complete_rows = active_crop_height;
    }
    g_dcmi_side_test_complete_rows = complete_rows;
    g_dcmi_side_test_trailing_words = active_captured_words % words_per_row;

    for (uint32_t row = 0U; row < complete_rows; row++) {
        uint32_t checksum = 0U;
        uint32_t nonzero_words = 0U;
        uint32_t base = row * words_per_row;

        for (uint32_t word_index = 0U; word_index < words_per_row; word_index++) {
            uint32_t word = dcmi_buffer[base + word_index];

            checksum += word;
            if (word != 0U) {
                nonzero_words++;
            }
        }

        if (nonzero_words > 0U) {
            nonzero_rows++;
        }
        if (row > 0U && checksum != previous_checksum) {
            changing_rows++;
        }
        previous_checksum = checksum;

        if (row == 0U) {
            g_dcmi_side_test_first_row_checksum = checksum;
            g_dcmi_side_test_first_row_nonzero_words = nonzero_words;
        } else if (row == (complete_rows / 2U)) {
            g_dcmi_side_test_mid_row_checksum = checksum;
            g_dcmi_side_test_mid_row_nonzero_words = nonzero_words;
        } else if (row == (complete_rows - 1U)) {
            g_dcmi_side_test_last_row_checksum = checksum;
            g_dcmi_side_test_last_row_nonzero_words = nonzero_words;
        }
    }

    g_dcmi_side_test_nonzero_rows = nonzero_rows;
    g_dcmi_side_test_changing_rows = changing_rows;
    g_dcmi_side_test_analyzed_words = active_captured_words;

    if (active_captured_words == active_capture_words &&
        complete_rows == active_crop_height) {
        if (nonzero_rows > 0U) {
            g_dcmi_side_test_full_nonzero_count++;
        } else {
            g_dcmi_side_test_full_allzero_count++;
        }
    }

    /* Write this last so a debugger can use it as a generation marker. */
    g_dcmi_side_test_analysis_sequence++;
}
#endif /* DCMI_FULL_HEIGHT_SIDE_TEST_MODE */
