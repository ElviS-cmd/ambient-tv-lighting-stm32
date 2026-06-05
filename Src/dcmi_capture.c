#include "dcmi_capture.h"

/* NOTE: these dimensions describe what we EXPECT from the source. Tune them
 * to match the actual Computer's/TFP401 resolution. They drive the crop windows for
 * each edge — wrong values mean we sample the wrong part of the frame. */
#define VIDEO_ACTIVE_WIDTH 1280U
#define VIDEO_ACTIVE_HEIGHT 720U
#define EDGE_BORDER_PIXELS 16U
#define DCMI_EDGE_TOP_BOTTOM_SAMPLES (VIDEO_ACTIVE_WIDTH * EDGE_BORDER_PIXELS)
#define DCMI_EDGE_SIDE_SAMPLES (EDGE_BORDER_PIXELS * VIDEO_ACTIVE_HEIGHT)
#define DCMI_CAPTURE_MAX_SAMPLES \
    ((DCMI_EDGE_TOP_BOTTOM_SAMPLES > DCMI_EDGE_SIDE_SAMPLES) ? \
      DCMI_EDGE_TOP_BOTTOM_SAMPLES : DCMI_EDGE_SIDE_SAMPLES)
#define DCMI_CAPTURE_MAX_WORDS (DCMI_CAPTURE_MAX_SAMPLES / 4U)
#define DCMI_RESTART_DELAY_MS 1U
#define DCMI_CAPTURE_TIMEOUT_MS 24U
/* Large edge crops need enough samples to cover the full LED geometry.
 * With 1280x720:
 * - complete side crop (16x720) = 2880 words
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
#define DCMI_SYNC_WAIT_TIMEOUT_MS 20U
/* Diagnostic mode: capture from the raw active stream without DCMI crop
 * programming. If DMA fills here, the signal path is healthy and the fault
 * is isolated to crop coordinates/timing rather than PIXCLK/HSYNC/data.
 */
#define DCMI_USE_CROP 0U
#define DCMI_LED_ACTIVITY_WINDOW_MS 1000U
/* Approach B: capture true full-resolution perimeter crops. The sides are
 * 16 x full-height rectangles again, not timed horizontal bands, so every side
 * zone can update once per perimeter cycle.
 */
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
/* Delta thresholds for selecting smoothing strength (0..255 channel scale). */
#define DCMI_SMOOTH_DELTA_STRONG_MAX 8U
#define DCMI_SMOOTH_DELTA_MED_MAX 32U
#define DCMI_SMOOTH_SCENE_CUT_DELTA 96U
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
    [DCMI_EDGE_TOP] = {0U, VIDEO_ACTIVE_HEIGHT - EDGE_BORDER_PIXELS, VIDEO_ACTIVE_WIDTH, EDGE_BORDER_PIXELS, 1U},
    [DCMI_EDGE_RIGHT] = {VIDEO_ACTIVE_WIDTH - EDGE_BORDER_PIXELS, 0U, EDGE_BORDER_PIXELS, VIDEO_ACTIVE_HEIGHT, 0U},
    [DCMI_EDGE_BOTTOM] = {0U, 0U, VIDEO_ACTIVE_WIDTH, EDGE_BORDER_PIXELS, 3U},
    [DCMI_EDGE_LEFT] = {0U, 0U, EDGE_BORDER_PIXELS, VIDEO_ACTIVE_HEIGHT, 2U},
};

static uint32_t g_dcmi_state;
static uint32_t g_dcmi_start_status;
static uint32_t g_dcmi_frame_count;
static uint32_t g_dcmi_error_count;
static uint32_t g_dcmi_timeout_count;
static uint32_t g_dcmi_restart_count;
static uint32_t g_dcmi_first_word;
static uint32_t g_dcmi_second_word;
static uint32_t g_dcmi_last_word;
static uint32_t g_dcmi_nonzero_words;
static uint32_t g_dcmi_changing_words;
static uint32_t g_dcmi_buffer_checksum;
static uint32_t g_dcmi_sample_count;
static uint32_t g_dcmi_average_byte;
static uint32_t g_dcmi_min_byte;
static uint32_t g_dcmi_max_byte;
static uint32_t g_dcmi_brightness_percent;
static uint32_t g_dcmi_average_r;
static uint32_t g_dcmi_average_g;
static uint32_t g_dcmi_average_b;
static uint32_t g_dcmi_bit0_percent;
static uint32_t g_dcmi_bit1_percent;
static uint32_t g_dcmi_bit2_percent;
static uint32_t g_dcmi_bit3_percent;
static uint32_t g_dcmi_bit4_percent;
static uint32_t g_dcmi_bit5_percent;
static uint32_t g_dcmi_bit6_percent;
static uint32_t g_dcmi_bit7_percent;
static uint32_t g_dcmi_zone0_brightness_percent;
static uint32_t g_dcmi_zone1_brightness_percent;
static uint32_t g_dcmi_zone2_brightness_percent;
static uint32_t g_dcmi_zone3_brightness_percent;
static uint32_t g_dcmi_zone0_r;
static uint32_t g_dcmi_zone0_g;
static uint32_t g_dcmi_zone0_b;
static uint32_t g_dcmi_zone1_r;
static uint32_t g_dcmi_zone1_g;
static uint32_t g_dcmi_zone1_b;
static uint32_t g_dcmi_zone2_r;
static uint32_t g_dcmi_zone2_g;
static uint32_t g_dcmi_zone2_b;
static uint32_t g_dcmi_zone3_r;
static uint32_t g_dcmi_zone3_g;
static uint32_t g_dcmi_zone3_b;
static uint32_t g_dcmi_timing_vsync_count;
static uint32_t g_dcmi_timing_line_count;
static uint32_t g_dcmi_timing_lines_per_frame;
static uint32_t g_dcmi_timing_frame_period_ms;
static uint32_t g_dcmi_timing_frame_rate_hz;
static uint32_t g_dcmi_timing_line_rate_hz;
static uint32_t g_dcmi_measured_pixclk_khz;
static uint32_t g_dcmi_pixclk_violation;
static uint32_t g_dcmi_requested_words;
static uint32_t g_dcmi_captured_words;
static uint32_t g_dcmi_partial_frame_count;
static uint32_t g_dcmi_zero_frame_count;
static uint32_t g_dcmi_early_accept_count;
static uint32_t g_dcmi_callback_complete_count;
static uint32_t g_dcmi_callback_late_count;
static uint32_t g_dcmi_early_complete_count;
static uint32_t g_dcmi_timeout_accept_count;
static uint32_t g_dcmi_edge_attempt_count[DCMI_EDGE_COUNT];
static uint32_t g_dcmi_edge_success_count[DCMI_EDGE_COUNT];
static uint32_t g_dcmi_edge_timeout_count[DCMI_EDGE_COUNT];
static uint32_t g_dcmi_edge_zero_count[DCMI_EDGE_COUNT];
static uint32_t g_dcmi_edge_last_words[DCMI_EDGE_COUNT];
static uint32_t g_dcmi_sync_wait_status;
static uint32_t g_dcmi_sync_wait_ms;
static uint32_t g_dcmi_sync_period_ms;
static uint32_t g_dcmi_sync_rate_hz;
static uint32_t g_dcmi_dma_ndtr_last;
static uint32_t g_dcmi_sr_last;
static uint32_t g_dcmi_ris_last;
static uint32_t g_dcmi_mis_last;
static uint32_t g_dcmi_cr_last;
static uint32_t g_dcmi_dma_lisr_last;
static uint32_t g_dcmi_led_update_pending;
static uint32_t g_dcmi_zone_update_count;
/* Per-edge zone statistics */
/* Frame quality metrics */
/* Edge-specific quality (for diagnosing capture issues) */
/* Saturation detection */
/* Dead zone detection (zones that never receive data) */
/* Crop validation metrics */
/* Dynamic baseline tracking */
static uint32_t g_dcmi_observed_baseline_r;                   /* Observed minimum R (for black floor) */
static uint32_t g_dcmi_observed_baseline_g;                   /* Observed minimum G */
static uint32_t g_dcmi_observed_baseline_b;                   /* Observed minimum B */
/* Color variance tracking (temporal stability) */
/* Zone response rate tracking */
static uint32_t g_dcmi_zone_spike_reject_count;
static uint32_t g_dcmi_zone_low_trust_spike_count;
static uint32_t g_dcmi_zone_miss_hold_count;
static uint32_t g_dcmi_zone_miss_decay_count;
static uint32_t g_dcmi_frame_publish_skip_count;
static uint32_t g_dcmi_frame_quality_last;
static uint32_t g_dcmi_zone_color_delta_max;
static uint32_t g_dcmi_zone_color_delta_avg;
static uint32_t g_dcmi_top_good_zones;
static uint32_t g_dcmi_top_color_spread;
static uint32_t g_dcmi_top_first_r;
static uint32_t g_dcmi_top_first_g;
static uint32_t g_dcmi_top_first_b;
static uint32_t g_dcmi_top_mid_r;
static uint32_t g_dcmi_top_mid_g;
static uint32_t g_dcmi_top_mid_b;
static uint32_t g_dcmi_top_last_r;
static uint32_t g_dcmi_top_last_g;
static uint32_t g_dcmi_top_last_b;
static uint32_t g_dcmi_right_band_index;
static uint32_t g_dcmi_left_band_index;
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
static uint32_t last_sync_tick_ms;
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
static uint32_t perimeter_edges_done_mask;
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
static uint16_t rgb332_weight_lut[256];

static void init_rgb332_weight_lut(void);
static uint32_t rescale_with_baseline(uint32_t value, uint32_t baseline);
static uint32_t zone_expected_in_current_capture(uint32_t zone);
static uint32_t cartesian_y_to_dcmi_y(uint32_t cart_y, uint32_t height);
static uint32_t edge_zone_count(uint32_t edge);
static uint32_t edge_zone_offset(uint32_t edge);
static uint32_t edge_local_zone(uint32_t edge, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height);
static uint32_t capture_accept_words(void);
static uint32_t capture_min_accept_words(void);
static void begin_perimeter_cycle_if_needed(void);
static uint32_t mark_current_perimeter_edge_done(void);
static uint32_t blend_channel(uint32_t prev_value,
                              uint32_t raw_value,
                              uint32_t alpha_num,
                              uint32_t alpha_den);
static void finalize_perimeter_cycle(void);
static void clear_current_capture_zones(void);
static void mark_crop_accept(uint32_t early_accept);
static void mark_crop_zero(void);

static void start_snapshot(void);
static uint32_t wait_for_frame_boundary(void);
static void analyze_buffer(void);

void DCMI_Capture_Init(void)
{
    init_rgb332_weight_lut();
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

        if ((HAL_GetTick() - last_restart_ms) >= DCMI_CAPTURE_TIMEOUT_MS) {
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

    if ((HAL_GetTick() - last_restart_ms) >= DCMI_RESTART_DELAY_MS) {
        start_snapshot();
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

    active_crop_index = next_crop_index;
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
    g_dcmi_right_band_index = next_right_band_index;
    g_dcmi_left_band_index = next_left_band_index;

    #if DCMI_USE_CROP
    crop_status = HAL_DCMI_ConfigCrop(&hdcmi,
                                      crop_x,
                                      dcmi_y,
                                      crop_width - 1U,
                                      crop_height - 1U);
    if (crop_status == HAL_OK) {
        HAL_StatusTypeDef crop_enable_status = HAL_DCMI_EnableCrop(&hdcmi);
        if (crop_enable_status != HAL_OK) {
            (void)HAL_DCMI_DisableCrop(&hdcmi);
        }
    } else {
        /* Keep capture alive even when per-crop programming fails. */
        (void)HAL_DCMI_DisableCrop(&hdcmi);
    }
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
    g_dcmi_sync_wait_status = wait_for_frame_boundary();

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
                                             DCMI_MODE_CONTINUOUS,
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
        next_crop_index = (next_crop_index + 1U) % DCMI_EDGE_COUNT;
    } else {
        dcmi_status.errors++;
        g_dcmi_error_count = dcmi_status.errors;
        last_error_time_ms = HAL_GetTick();
        dcmi_status.state = DCMI_CAPTURE_ERROR;
        g_dcmi_state = DCMI_CAPTURE_ERROR;
    }

}

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

static void analyze_buffer(void)
{
    uint32_t previous = 0U;
    const dcmi_edge_crop_t *crop = &edge_crops[active_crop_index];
    uint32_t crop_width = active_crop_width;
    uint32_t crop_height = active_crop_height;
    uint32_t nonzero = 0;
    uint32_t changing = 0;
    uint32_t checksum = 0;
    uint32_t sample_sum = 0;
    uint32_t sample_count = 0;
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
    uint32_t words_to_analyze = active_captured_words;

    if (words_to_analyze > active_capture_words) {
        words_to_analyze = active_capture_words;
    }

    if (words_to_analyze > 0U) {
        previous = dcmi_buffer[0];
    }

    begin_perimeter_cycle_if_needed();

    for (uint32_t i = 0; i < words_to_analyze; i++) {
        uint32_t word = dcmi_buffer[i];

        if (word != 0U) {
            nonzero++;
        }

        if (i > 0U && word != previous) {
            changing++;
        }

        checksum += word;
        previous = word;

        for (uint32_t shift = 0; shift < 32U; shift += 8U) {
            uint32_t sample = (word >> shift) & 0xFFU;
            uint32_t sample_index = (i * 4U) + (shift / 8U);
            uint32_t pixel_x = sample_index % crop_width;
            uint32_t pixel_y = sample_index / crop_width;
            uint32_t zone = crop->zone;
            uint32_t led_zone = DCMI_LED_ZONE_COUNT;
            uint32_t use_sample_for_led_zone = 1U;
            uint32_t red = r3_to_8bit[(sample >> 5U) & 0x07U];
            uint32_t green = g3_to_8bit[(sample >> 2U) & 0x07U];
            uint32_t blue = b2_to_8bit[sample & 0x03U];

            if (red < frame_min_r) {
                frame_min_r = red;
            }
            if (green < frame_min_g) {
                frame_min_g = green;
            }
            if (blue < frame_min_b) {
                frame_min_b = blue;
            }

            red = rescale_with_baseline(red, DCMI_BASELINE_R);
            green = rescale_with_baseline(green, DCMI_BASELINE_G);
            blue = rescale_with_baseline(blue, DCMI_BASELINE_B);

            if (zone > 3U) {
                zone = 3U;
            }

            /* In timed-band fallback mode, side captures are horizontal bands
             * delayed after VSYNC, so only one side LED zone is updated per
             * capture. In the normal commercial path, the side crops are true
             * 16 x full-height edge rectangles and are divided by Y so every
             * side LED gets a nearby screen sample each side capture.
             */
            if (DCMI_BOTTOM_TIMED_BAND != 0U &&
                active_crop_index == DCMI_EDGE_RIGHT) {
                if (pixel_x + EDGE_BORDER_PIXELS < crop_width) {
                    use_sample_for_led_zone = 0U;
                }
                led_zone = DCMI_RIGHT_ZONE_OFFSET +
                           (active_side_band_index % DCMI_RIGHT_ZONE_COUNT);
            } else if (DCMI_BOTTOM_TIMED_BAND != 0U &&
                       active_crop_index == DCMI_EDGE_LEFT) {
                if (pixel_x >= EDGE_BORDER_PIXELS) {
                    use_sample_for_led_zone = 0U;
                }
                led_zone = DCMI_LEFT_ZONE_OFFSET +
                           (active_side_band_index % DCMI_LEFT_ZONE_COUNT);
            } else {
                led_zone = edge_zone_offset(active_crop_index) +
                           edge_local_zone(active_crop_index,
                                           pixel_x,
                                           pixel_y,
                                           crop_width,
                                           crop_height);
            }

            sample_sum += sample;
            red_sum += red;
            green_sum += green;
            blue_sum += blue;
            zone_sum[zone] += sample;
            zone_red_sum[zone] += red;
            zone_green_sum[zone] += green;
            zone_blue_sum[zone] += blue;

            if (use_sample_for_led_zone != 0U && led_zone < DCMI_LED_ZONE_COUNT) {
                /* Favor vivid edge pixels over dark background pixels, while
                 * still letting low-light scenes contribute. This gives each
                 * LED a more "nearby object" color instead of a washed panel
                 * average.
                 */
                uint32_t weight = rgb332_weight_lut[sample];

                led_zone_red_sum[led_zone] += red * weight;
                led_zone_green_sum[led_zone] += green * weight;
                led_zone_blue_sum[led_zone] += blue * weight;
                led_zone_weight_sum[led_zone] += weight;
                led_zone_sample_count[led_zone]++;
            }

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

    if (mark_current_perimeter_edge_done() != 0U) {
        finalize_perimeter_cycle();
    }

}

static void init_rgb332_weight_lut(void)
{
    for (uint32_t sample = 0U; sample < 256U; sample++) {
        uint32_t red = r3_to_8bit[(sample >> 5U) & 0x07U];
        uint32_t green = g3_to_8bit[(sample >> 2U) & 0x07U];
        uint32_t blue = b2_to_8bit[sample & 0x03U];
        uint32_t weight;

        red = rescale_with_baseline(red, DCMI_BASELINE_R);
        green = rescale_with_baseline(green, DCMI_BASELINE_G);
        blue = rescale_with_baseline(blue, DCMI_BASELINE_B);

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

static uint32_t zone_expected_in_current_capture(uint32_t zone)
{
    switch (active_crop_index) {
    case DCMI_EDGE_TOP:
        return zone >= DCMI_TOP_ZONE_OFFSET &&
               zone < DCMI_LEFT_ZONE_OFFSET;
    case DCMI_EDGE_BOTTOM:
        return zone >= DCMI_BOTTOM_ZONE_OFFSET &&
               zone < DCMI_LED_ZONE_COUNT;
    case DCMI_EDGE_RIGHT:
        if (DCMI_BOTTOM_TIMED_BAND == 0U) {
            return zone < DCMI_TOP_ZONE_OFFSET;
        }

        return zone == (DCMI_RIGHT_ZONE_OFFSET +
                        (active_side_band_index % DCMI_RIGHT_ZONE_COUNT));
    case DCMI_EDGE_LEFT:
        if (DCMI_BOTTOM_TIMED_BAND == 0U) {
            return zone >= DCMI_LEFT_ZONE_OFFSET &&
                   zone < DCMI_BOTTOM_ZONE_OFFSET;
        }

        return zone == (DCMI_LEFT_ZONE_OFFSET +
                        (active_side_band_index % DCMI_LEFT_ZONE_COUNT));
    default:
        return 0U;
    }
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

static uint32_t capture_accept_words(void)
{
    uint32_t accept_words = DCMI_SIDE_EARLY_ACCEPT;

    /* Wide top/bottom crops give every horizontal LED zone useful data even
     * from a partial band: 1000 words = 4000 pixels, roughly 90 samples per
     * 43-zone edge. Keep a lower early-accept threshold here than on timed
     * side bands, where each capture feeds one vertical LED zone.
     */
    if (active_crop_index == DCMI_EDGE_TOP ||
        active_crop_index == DCMI_EDGE_BOTTOM) {
        accept_words = DCMI_TOP_BOTTOM_EARLY_ACCEPT;
    }

    if (accept_words > active_capture_words) {
        accept_words = active_capture_words;
    }

    return accept_words;
}

static uint32_t capture_min_accept_words(void)
{
    uint32_t min_words = DCMI_TIMEOUT_ACCEPT_FLOOR_SIDE;

    if (active_crop_index == DCMI_EDGE_TOP ||
        active_crop_index == DCMI_EDGE_BOTTOM) {
        min_words = DCMI_TIMEOUT_ACCEPT_FLOOR_TOP_BOTTOM;
    }

    if (min_words > active_capture_words) {
        min_words = active_capture_words;
    }

    return min_words;
}

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
        perimeter_edges_done_mask |= (1UL << active_crop_index);
    }

    if ((perimeter_edges_done_mask & DCMI_PERIMETER_COMPLETE_MASK) !=
        DCMI_PERIMETER_COMPLETE_MASK) {
        return 0U;
    }

    perimeter_edges_done_mask = 0U;
    return 1U;
}

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
                /* Trusted scene cuts should react fast. */
                alpha_num = DCMI_SMOOTH_ALPHA_STRONG_NUM;
                alpha_den = DCMI_SMOOTH_ALPHA_STRONG_DEN;
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


static void clear_current_capture_zones(void)
{
    for (uint32_t zone = 0U; zone < DCMI_LED_ZONE_COUNT; zone++) {
        if (zone_expected_in_current_capture(zone) != 0U) {
            led_zone_red_sum[zone] = 0U;
            led_zone_green_sum[zone] = 0U;
            led_zone_blue_sum[zone] = 0U;
            led_zone_weight_sum[zone] = 0U;
            led_zone_sample_count[zone] = 0U;
        }
    }
}


static void mark_crop_accept(uint32_t early_accept)
{
    if (early_accept != 0U) {
        g_dcmi_early_accept_count++;
    }

    if (active_crop_index < DCMI_EDGE_COUNT) {
        g_dcmi_edge_success_count[active_crop_index]++;
        g_dcmi_edge_last_words[active_crop_index] = active_captured_words;
    }

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
    begin_perimeter_cycle_if_needed();
    clear_current_capture_zones();

    if (active_crop_index < DCMI_EDGE_COUNT) {
        g_dcmi_edge_zero_count[active_crop_index]++;
        g_dcmi_edge_last_words[active_crop_index] = active_captured_words;
    }

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

    if (mark_current_perimeter_edge_done() != 0U) {
        finalize_perimeter_cycle();
    }
}
