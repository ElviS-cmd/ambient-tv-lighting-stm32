#include "ws2812.h"

#define WS2812_GPIO_PORT GPIOA
#define WS2812_GPIO_PIN  GPIO_PIN_1
#define WS2812_LED_COUNT      135U   /* Array index 0 is physical LED 1. */
#define WS2812_LEFT_START       0U
#define WS2812_LEFT_COUNT      24U
#define WS2812_TOP_START       24U
#define WS2812_TOP_COUNT       43U
#define WS2812_RIGHT_START     67U
#define WS2812_RIGHT_COUNT     25U
#define WS2812_BOTTOM_START    92U
#define WS2812_BOTTOM_COUNT    43U
/* DCMI still supplies right/top/left/bottom as 25/43/25/43 zones.
 * The 25 left source zones are resampled onto the 24 physical left LEDs.
 */
#define WS2812_INPUT_ZONE_COUNT    136U
#define WS2812_INPUT_RIGHT_OFFSET    0U
#define WS2812_INPUT_TOP_OFFSET     25U
#define WS2812_INPUT_LEFT_OFFSET    68U
#define WS2812_INPUT_BOTTOM_OFFSET  93U
#define WS2812_INPUT_RIGHT_COUNT    25U
#define WS2812_INPUT_TOP_COUNT      43U
#define WS2812_INPUT_LEFT_COUNT     25U
#define WS2812_INPUT_BOTTOM_COUNT   43U
#define WS2812_MAX_CHANNEL 255U
/* Perceptual gamma (2.2) for the LED output. WS2812s are linear emitters
 * while the captured values are display-referred, so a linear mapping makes
 * mid-tones washed-out and dark scenes glow. Gamma also crushes any residual
 * black floor: an input of 36/255 lands at 1/96. Set to 0 to compare with
 * the old linear mapping.
 */
#define WS2812_GAMMA_CORRECTION 0U
/* Keep color summaries available while leaving DMA waveform validation off. */
#define WS2812_COLOR_DIAGNOSTICS 1U
#define WS2812_DMA_DIAGNOSTICS 1U
/* Per-channel white balance, percent of the gamma output. Tune with a full
 * white screen until the strip's white matches the display: WS2812 greens
 * usually dominate, so G is typically the channel to pull down first
 * (try 100/80/95 if white looks green-cyan).
 */
#define WS2812_WB_R_PCT 100U
#define WS2812_WB_G_PCT 100U
#define WS2812_WB_B_PCT 100U
/* WS2812 green remains visible at very low codes. Treat values whose
 * brightest channel is at or below this floor as true black so dim edge
 * noise cannot linger as a stale tint on nominally black content.
 */
#define WS2812_DARK_FRAME_FLOOR 1U
/* Output glide engine. The capture pipeline updates each zone's target color
 * in discrete steps; the glide ticks at 100 Hz and crossfades every RGB
 * channel with one symmetric response. Matching rise/fall times keeps the
 * outgoing color from lingering after the incoming color has already arrived.
 */
#define WS2812_GLIDE_TICK_MS 10U
#define WS2812_GLIDE_SLOW_MS 50U
#define WS2812_GLIDE_MEDIUM_MS 35U
#define WS2812_GLIDE_FAST_MS 20U
/* Per-tick easing coefficient in q8: alpha = tick / (tick + tau). */
#define WS2812_GLIDE_K(tau_ms) \
    ((256U * WS2812_GLIDE_TICK_MS) / (WS2812_GLIDE_TICK_MS + (tau_ms)))
#define WS2812_GLIDE_K_SLOW WS2812_GLIDE_K(WS2812_GLIDE_SLOW_MS)
#define WS2812_GLIDE_K_MEDIUM WS2812_GLIDE_K(WS2812_GLIDE_MEDIUM_MS)
#define WS2812_GLIDE_K_FAST WS2812_GLIDE_K(WS2812_GLIDE_FAST_MS)
#define WS2812_GLIDE_MEDIUM_DELTA 40U
#define WS2812_GLIDE_FAST_DELTA 96U
#define WS2812_UPDATE_PERIOD_MS 12U
#define WS2812_FAST_UPDATE_PERIOD_MS 7U
#define WS2812_FAST_DELTA_THRESHOLD 24U
#define WS2812_SCENE_DELTA_BYPASS 170U
/* DCMI already rejects low-trust capture spikes. Do not suppress isolated
 * per-zone changes here; those are exactly what make the top/bottom edges
 * feel individually addressed instead of behaving like one coarse panel.
 */
#define WS2812_ISOLATED_SPIKE_FILTER 0U
#define WS2812_ISOLATED_SPIKE_DELTA 44U
#define WS2812_NEIGHBOR_CALM_DELTA 22U
/* TIM2 is on APB1. With SYSCLK=168MHz and APB1 prescaler /4, PCLK1 is
 * 42MHz, but STM32F4 timer kernels run at 2x PCLK when the APB prescaler is
 * not 1. TIM2 therefore ticks at 84MHz: 105 ticks is about 1.25us.
 */
#define WS2812_TIM_PERIOD_TICKS 104U
#define WS2812_PWM_ZERO_TICKS 34U
#define WS2812_PWM_ONE_TICKS 67U
#define WS2812_RESET_SLOTS 48U
#define WS2812_PWM_BUF_LEN ((WS2812_LED_COUNT * 24U) + WS2812_RESET_SLOTS)
#define WS2812_DMA_BUSY_TIMEOUT_MS 20U

typedef struct {
    uint8_t g;
    uint8_t r;
    uint8_t b;
} ws2812_color_t;

extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_tim2_ch2_ch4;

volatile uint32_t g_ws2812_show_count;
volatile uint32_t g_ws2812_dma_started_count;
volatile uint32_t g_ws2812_dma_complete_count;
volatile uint32_t g_ws2812_dma_error_count;
volatile uint32_t g_ws2812_dma_busy;
volatile uint32_t g_ws2812_clock_ok;
volatile uint32_t g_ws2812_tim2_kernel_hz;
volatile uint32_t g_ws2812_arr_expected;
volatile uint32_t g_ws2812_arr_actual;
volatile uint32_t g_ws2812_slot_hz;
volatile uint32_t g_ws2812_nonzero_perimeter;
volatile uint32_t g_ws2812_first_nonzero_index;
volatile uint32_t g_ws2812_last_nonzero_index;
volatile uint32_t g_ws2812_max_channel;
volatile uint32_t g_ws2812_target_min_r;
volatile uint32_t g_ws2812_target_max_r;
volatile uint32_t g_ws2812_target_spread_r;
volatile uint32_t g_ws2812_target_min_g;
volatile uint32_t g_ws2812_target_max_g;
volatile uint32_t g_ws2812_target_spread_g;
volatile uint32_t g_ws2812_target_min_b;
volatile uint32_t g_ws2812_target_max_b;
volatile uint32_t g_ws2812_target_spread_b;
volatile uint32_t g_ws2812_output_min_r;
volatile uint32_t g_ws2812_output_max_r;
volatile uint32_t g_ws2812_output_spread_r;
volatile uint32_t g_ws2812_output_min_g;
volatile uint32_t g_ws2812_output_max_g;
volatile uint32_t g_ws2812_output_spread_g;
volatile uint32_t g_ws2812_output_min_b;
volatile uint32_t g_ws2812_output_max_b;
volatile uint32_t g_ws2812_output_spread_b;
/* Buffer-integrity instrumentation — populated by encode_dma_buffer()
 * and show_tim_dma() so you can verify CCR duty values in the debugger.
 * All should be 34 or 67 (data slots) / 0 (reset slots); overrange == 0. */
volatile uint32_t g_ws2812_buf_min;          /* min value written to pwm buf  */
volatile uint32_t g_ws2812_buf_max;          /* max value written to pwm buf  */
volatile uint32_t g_ws2812_buf_overrange;    /* entries > ARR (should be 0)   */
volatile uint32_t g_ws2812_buf_entry0;       /* pwm_buf[0] after encode       */
volatile uint32_t g_ws2812_buf_entry1;       /* pwm_buf[1]                    */
volatile uint32_t g_ws2812_buf_entry2;       /* pwm_buf[2]                    */
volatile uint32_t g_ws2812_buf_entry3;       /* pwm_buf[3]                    */
volatile uint32_t g_ws2812_ccr2_snapshot;    /* TIM2->CCR2 read after DMA start */
/* Runtime DMA alignment — read from DMA1_Stream6->CR after each DMA start.
 * Encoding: 0=byte, 1=half-word, 2=word.  Both should be 2 (word). */
volatile uint32_t g_ws2812_dma_msize;        /* CR bits[14:13] — memory width  */
volatile uint32_t g_ws2812_dma_psize;        /* CR bits[12:11] — periph width  */

#if WS2812_GAMMA_CORRECTION
/* round(96 * (v/255)^2.2) for v = 0..255. */
static const uint8_t ws2812_gamma_lut[256] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  2,
     2,  3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,
     5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,
     7,  8,  8,  8,  8,  9,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11,
    11, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 15, 15, 15, 15,
    16, 16, 16, 17, 17, 17, 18, 18, 18, 19, 19, 19, 20, 20, 20, 21,
    21, 21, 22, 22, 23, 23, 23, 24, 24, 24, 25, 25, 26, 26, 26, 27,
    27, 28, 28, 29, 29, 29, 30, 30, 31, 31, 32, 32, 33, 33, 33, 34,
    34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41, 42,
    42, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48, 49, 49, 50, 50, 51,
    51, 52, 53, 53, 54, 54, 55, 56, 56, 57, 57, 58, 59, 59, 60, 61,
    61, 62, 63, 63, 64, 65, 65, 66, 67, 67, 68, 69, 69, 70, 71, 71,
    72, 73, 74, 74, 75, 76, 77, 77, 78, 79, 79, 80, 81, 82, 82, 83,
    84, 85, 86, 86, 87, 88, 89, 89, 90, 91, 92, 93, 94, 94, 95, 96
};
#endif

/* ws2812_leds holds the TARGET colors from the capture pipeline; the glide
 * engine eases ws2812_output toward them and the DMA buffer is encoded from
 * ws2812_output. The q8.8 state keeps sub-LSB progress so slow fades have
 * no visible stair-stepping. */
static ws2812_color_t ws2812_leds[WS2812_LED_COUNT];
static ws2812_color_t ws2812_output[WS2812_LED_COUNT];
static uint16_t ws2812_glide_g_q88[WS2812_LED_COUNT];
static uint16_t ws2812_glide_r_q88[WS2812_LED_COUNT];
static uint16_t ws2812_glide_b_q88[WS2812_LED_COUNT];
static uint32_t last_glide_tick_ms;
static uint32_t ws2812_glide_active;
static uint8_t ws2812_glide_k_q8 = WS2812_GLIDE_K_SLOW;
static uint32_t ws2812_pwm_buf[WS2812_PWM_BUF_LEN] __attribute__((aligned(4)));
static uint32_t ws2812_dma_available;
/* Written from the TIM2 DMA-complete interrupt via ws2812_force_idle_low();
 * volatile so optimized builds re-read them in the task context. */
static volatile uint32_t ws2812_dma_busy;
static volatile uint32_t ws2812_dma_start_ms;
/* A frame arrived while the previous strip transmission was still on the
 * wire. WS2812_Flush() retransmits the current colors once DMA frees up,
 * so a fast scene change landing mid-transmit is deferred, not dropped. */
static volatile uint32_t ws2812_frame_pending;
static uint32_t last_update_ms;
static uint8_t ws2812_prev_zone_raw_r[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_prev_zone_raw_g[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_prev_zone_raw_b[WS2812_INPUT_ZONE_COUNT];
#if WS2812_ISOLATED_SPIKE_FILTER
static uint8_t ws2812_zone_filtered_r[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_zone_filtered_g[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_zone_filtered_b[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_zone_delta_cache[WS2812_INPUT_ZONE_COUNT];
#endif

/* Two four-bit lookups replace 24 tests/branches for every encoded LED. */
#define WS2812_PWM_Z WS2812_PWM_ZERO_TICKS
#define WS2812_PWM_O WS2812_PWM_ONE_TICKS
static const uint32_t ws2812_pwm_nibble_lut[16][4] = {
    {WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_Z},
    {WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_O},
    {WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_Z},
    {WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_O},
    {WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_Z},
    {WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_O},
    {WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_Z},
    {WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_O},
    {WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_Z},
    {WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_Z, WS2812_PWM_O},
    {WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_Z},
    {WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_O, WS2812_PWM_O},
    {WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_Z},
    {WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_Z, WS2812_PWM_O},
    {WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_Z},
    {WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_O, WS2812_PWM_O}
};
#undef WS2812_PWM_Z
#undef WS2812_PWM_O

/* Physical LED index to DCMI zone index. Precomputing this removes the four
 * mapping loops and the 25-to-24 left-edge division from every update.
 */
static const uint8_t ws2812_source_zone[WS2812_LED_COUNT] = {
    92U, 91U, 90U, 89U, 88U, 87U, 86U, 85U, 84U, 83U, 82U, 81U, 79U, 78U, 77U,
    76U, 75U, 74U, 73U, 72U, 71U, 70U, 69U, 68U,
    67U, 66U, 65U, 64U, 63U, 62U, 61U, 60U, 59U, 58U, 57U, 56U, 55U, 54U, 53U,
    52U, 51U, 50U, 49U, 48U, 47U, 46U, 45U, 44U, 43U, 42U, 41U, 40U, 39U, 38U,
    37U, 36U, 35U, 34U, 33U, 32U, 31U, 30U, 29U, 28U, 27U, 26U, 25U,
    24U, 23U, 22U, 21U, 20U, 19U, 18U, 17U, 16U, 15U, 14U, 13U, 12U, 11U, 10U,
    9U, 8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U, 0U,
    135U, 134U, 133U, 132U, 131U, 130U, 129U, 128U, 127U, 126U, 125U, 124U,
    123U, 122U, 121U, 120U, 119U, 118U, 117U, 116U, 115U, 114U, 113U, 112U,
    111U, 110U, 109U, 108U, 107U, 106U, 105U, 104U, 103U, 102U, 101U, 100U,
    99U, 98U, 97U, 96U, 95U, 94U, 93U
};


static void configure_data_gpio_output(void);
static void configure_data_gpio_tim2(void);
static uint32_t tim2_kernel_clock_hz(void);
static uint32_t tim_dma_init(void);
static void encode_dma_buffer(void);
static inline uint32_t *encode_pwm_byte(uint32_t *dst, uint8_t value);
static uint32_t show_tim_dma(void);
static uint32_t show(void);
static uint8_t rgb_to_channel(uint32_t value, uint32_t wb_pct);
static uint16_t glide_channel(uint16_t current_q88, uint8_t target, uint8_t glide_k_q8);
static inline uint32_t channel_delta_u8(uint8_t a, uint8_t b);
static void ws2812_force_idle_low(void);

void WS2812_Init(void)
{
    for (uint32_t zone = 0U; zone < WS2812_INPUT_ZONE_COUNT; zone++) {
        ws2812_prev_zone_raw_r[zone] = 0U;
        ws2812_prev_zone_raw_g[zone] = 0U;
        ws2812_prev_zone_raw_b[zone] = 0U;
#if WS2812_ISOLATED_SPIKE_FILTER
        ws2812_zone_filtered_r[zone] = 0U;
        ws2812_zone_filtered_g[zone] = 0U;
        ws2812_zone_filtered_b[zone] = 0U;
        ws2812_zone_delta_cache[zone] = 0U;
#endif
    }

    configure_data_gpio_output();
    ws2812_dma_available = tim_dma_init();
    ws2812_force_idle_low();
    WS2812_Clear();
}

void WS2812_TaskEdgeZonesRgb(const volatile uint32_t *zone_r,
                             const volatile uint32_t *zone_g,
                             const volatile uint32_t *zone_b,
                             uint32_t zone_count)
{
    uint32_t now = HAL_GetTick();
    uint32_t raw_delta_max = 0U;
#if WS2812_ISOLATED_SPIKE_FILTER
    uint32_t scene_bypass;
#endif
#if WS2812_ISOLATED_SPIKE_FILTER
    uint32_t min_update_period_ms;
#endif
    uint32_t elapsed_ms;
    const uint8_t *mapped_r;
    const uint8_t *mapped_g;
    const uint8_t *mapped_b;
    uint8_t zone_target_r[WS2812_INPUT_ZONE_COUNT];
    uint8_t zone_target_g[WS2812_INPUT_ZONE_COUNT];
    uint8_t zone_target_b[WS2812_INPUT_ZONE_COUNT];

    if (zone_r == NULL || zone_g == NULL || zone_b == NULL) {
        return;
    }

    if (zone_count < WS2812_INPUT_ZONE_COUNT) {
        return;
    }

    elapsed_ms = now - last_update_ms;
    if (elapsed_ms < WS2812_FAST_UPDATE_PERIOD_MS) {
        return;
    }

#if !WS2812_ISOLATED_SPIKE_FILTER
    if (elapsed_ms >= WS2812_UPDATE_PERIOD_MS) {
        for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
            uint8_t target_r = rgb_to_channel(zone_r[z], WS2812_WB_R_PCT);
            uint8_t target_g = rgb_to_channel(zone_g[z], WS2812_WB_G_PCT);
            uint8_t target_b = rgb_to_channel(zone_b[z], WS2812_WB_B_PCT);

            zone_target_r[z] = target_r;
            zone_target_g[z] = target_g;
            zone_target_b[z] = target_b;
            ws2812_prev_zone_raw_r[z] = target_r;
            ws2812_prev_zone_raw_g[z] = target_g;
            ws2812_prev_zone_raw_b[z] = target_b;
        }
    } else {
        for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
            uint8_t target_r = rgb_to_channel(zone_r[z], WS2812_WB_R_PCT);
            uint8_t target_g = rgb_to_channel(zone_g[z], WS2812_WB_G_PCT);
            uint8_t target_b = rgb_to_channel(zone_b[z], WS2812_WB_B_PCT);
            uint32_t delta_r;
            uint32_t delta_g;
            uint32_t delta_b;
            uint32_t zone_delta;

            zone_target_r[z] = target_r;
            zone_target_g[z] = target_g;
            zone_target_b[z] = target_b;
            delta_r = channel_delta_u8(target_r, ws2812_prev_zone_raw_r[z]);
            delta_g = channel_delta_u8(target_g, ws2812_prev_zone_raw_g[z]);
            delta_b = channel_delta_u8(target_b, ws2812_prev_zone_raw_b[z]);
            zone_delta = delta_r;
            if (delta_g > zone_delta) { zone_delta = delta_g; }
            if (delta_b > zone_delta) { zone_delta = delta_b; }
            if (zone_delta > raw_delta_max) { raw_delta_max = zone_delta; }
        }

        if (raw_delta_max < WS2812_FAST_DELTA_THRESHOLD) {
            return;
        }
        for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
            ws2812_prev_zone_raw_r[z] = zone_target_r[z];
            ws2812_prev_zone_raw_g[z] = zone_target_g[z];
            ws2812_prev_zone_raw_b[z] = zone_target_b[z];
        }
    }
#else
    for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
        uint8_t target_r = rgb_to_channel(zone_r[z], WS2812_WB_R_PCT);
        uint8_t target_g = rgb_to_channel(zone_g[z], WS2812_WB_G_PCT);
        uint8_t target_b = rgb_to_channel(zone_b[z], WS2812_WB_B_PCT);
        uint32_t delta_r;
        uint32_t delta_g;
        uint32_t delta_b;
        uint32_t zone_delta;

        zone_target_r[z] = target_r;
        zone_target_g[z] = target_g;
        zone_target_b[z] = target_b;

        delta_r = channel_delta_u8(target_r, ws2812_prev_zone_raw_r[z]);
        delta_g = channel_delta_u8(target_g, ws2812_prev_zone_raw_g[z]);
        delta_b = channel_delta_u8(target_b, ws2812_prev_zone_raw_b[z]);
        zone_delta = delta_r;
        if (delta_g > zone_delta) {
            zone_delta = delta_g;
        }
        if (delta_b > zone_delta) {
            zone_delta = delta_b;
        }

        if (zone_delta > raw_delta_max) {
            raw_delta_max = zone_delta;
        }
    }

    min_update_period_ms =
        raw_delta_max >= WS2812_FAST_DELTA_THRESHOLD ?
        WS2812_FAST_UPDATE_PERIOD_MS : WS2812_UPDATE_PERIOD_MS;
    if (elapsed_ms < min_update_period_ms) {
        return;
    }
#endif
    /* Advance raw-history only when this frame is accepted for processing.
     * This keeps delta detection honest during skipped update periods. */
#if WS2812_ISOLATED_SPIKE_FILTER
    for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
        ws2812_prev_zone_raw_r[z] = zone_target_r[z];
        ws2812_prev_zone_raw_g[z] = zone_target_g[z];
        ws2812_prev_zone_raw_b[z] = zone_target_b[z];
    }

    for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
        uint32_t delta_r = channel_delta_u8(zone_target_r[z], ws2812_zone_filtered_r[z]);
        uint32_t delta_g = channel_delta_u8(zone_target_g[z], ws2812_zone_filtered_g[z]);
        uint32_t delta_b = channel_delta_u8(zone_target_b[z], ws2812_zone_filtered_b[z]);
        uint32_t zone_delta = delta_r;

        if (delta_g > zone_delta) {
            zone_delta = delta_g;
        }
        if (delta_b > zone_delta) {
            zone_delta = delta_b;
        }
        ws2812_zone_delta_cache[z] = (uint8_t)zone_delta;
    }

    scene_bypass = raw_delta_max >= WS2812_SCENE_DELTA_BYPASS ? 1U : 0U;
    for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
        uint32_t prev_zone = z == 0U ? (WS2812_INPUT_ZONE_COUNT - 1U) : (z - 1U);
        uint32_t next_zone = z + 1U;
        uint32_t zone_delta = ws2812_zone_delta_cache[z];

        if (next_zone >= WS2812_INPUT_ZONE_COUNT) {
            next_zone = 0U;
        }

        if (WS2812_ISOLATED_SPIKE_FILTER != 0U &&
            scene_bypass == 0U &&
            zone_delta >= WS2812_ISOLATED_SPIKE_DELTA &&
            ws2812_zone_delta_cache[prev_zone] <= WS2812_NEIGHBOR_CALM_DELTA &&
            ws2812_zone_delta_cache[next_zone] <= WS2812_NEIGHBOR_CALM_DELTA) {
            continue;
        }

        ws2812_zone_filtered_r[z] = zone_target_r[z];
        ws2812_zone_filtered_g[z] = zone_target_g[z];
        ws2812_zone_filtered_b[z] = zone_target_b[z];
    }
    mapped_r = ws2812_zone_filtered_r;
    mapped_g = ws2812_zone_filtered_g;
    mapped_b = ws2812_zone_filtered_b;
#else
    mapped_r = zone_target_r;
    mapped_g = zone_target_g;
    mapped_b = zone_target_b;
#endif

    if (raw_delta_max >= WS2812_GLIDE_FAST_DELTA) {
        ws2812_glide_k_q8 = WS2812_GLIDE_K_FAST;
    } else if (raw_delta_max >= WS2812_GLIDE_MEDIUM_DELTA) {
        ws2812_glide_k_q8 = WS2812_GLIDE_K_MEDIUM;
    } else {
        ws2812_glide_k_q8 = WS2812_GLIDE_K_SLOW;
    }

    for (uint32_t led = 0U; led < WS2812_LED_COUNT; led++) {
        uint32_t src = ws2812_source_zone[led];
        uint8_t r = mapped_r[src];
        uint8_t g = mapped_g[src];
        uint8_t b = mapped_b[src];
        uint8_t level = r;

        if (g > level) {
            level = g;
        }
        if (b > level) {
            level = b;
        }
        if (level <= WS2812_DARK_FRAME_FLOOR) {
            r = 0U;
            g = 0U;
            b = 0U;
        }

        ws2812_leds[led].r = r;
        ws2812_leds[led].g = g;
        ws2812_leds[led].b = b;
    }

#if WS2812_COLOR_DIAGNOSTICS
    {
        uint32_t nonzero = 0U;
        uint32_t first = 0xFFFFFFFFU;
        uint32_t last = 0U;
        uint32_t max_channel = 0U;
        uint32_t min_r = 0xFFU;
        uint32_t min_g = 0xFFU;
        uint32_t min_b = 0xFFU;
        uint32_t max_r = 0U;
        uint32_t max_g = 0U;
        uint32_t max_b = 0U;

        for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
            uint32_t local_max = ws2812_leds[i].r;

            if (ws2812_leds[i].r < min_r) { min_r = ws2812_leds[i].r; }
            if (ws2812_leds[i].g < min_g) { min_g = ws2812_leds[i].g; }
            if (ws2812_leds[i].b < min_b) { min_b = ws2812_leds[i].b; }
            if (ws2812_leds[i].r > max_r) { max_r = ws2812_leds[i].r; }
            if (ws2812_leds[i].g > max_g) { max_g = ws2812_leds[i].g; }
            if (ws2812_leds[i].b > max_b) { max_b = ws2812_leds[i].b; }

            if (ws2812_leds[i].g > local_max) {
                local_max = ws2812_leds[i].g;
            }
            if (ws2812_leds[i].b > local_max) {
                local_max = ws2812_leds[i].b;
            }

            if (local_max > max_channel) {
                max_channel = local_max;
            }

            if (local_max != 0U) {
                if (first == 0xFFFFFFFFU) {
                    first = i;
                }
                last = i;
                nonzero++;
            }
        }

        g_ws2812_nonzero_perimeter = nonzero;
        g_ws2812_first_nonzero_index = first == 0xFFFFFFFFU ? 0U : first;
        g_ws2812_last_nonzero_index = last;
        g_ws2812_max_channel = max_channel;
        g_ws2812_target_min_r = min_r;
        g_ws2812_target_max_r = max_r;
        g_ws2812_target_spread_r = max_r - min_r;
        g_ws2812_target_min_g = min_g;
        g_ws2812_target_max_g = max_g;
        g_ws2812_target_spread_g = max_g - min_g;
        g_ws2812_target_min_b = min_b;
        g_ws2812_target_max_b = max_b;
        g_ws2812_target_spread_b = max_b - min_b;
    }
#endif

    /* Targets are set; WS2812_GlideTask() owns the actual transmission. */
    ws2812_glide_active = 1U;
    last_update_ms = now;
}

static uint16_t glide_channel(uint16_t current_q88, uint8_t target, uint8_t glide_k_q8)
{
    uint16_t target_q88 = (uint16_t)((uint16_t)target << 8);
    uint32_t step;

    if (target_q88 > current_q88) {
        step = ((uint32_t)(target_q88 - current_q88) * glide_k_q8) >> 8;
        if (step == 0U) {
            /* Close the sub-LSB remainder instead of parking one below. */
            return target_q88;
        }
        return (uint16_t)(current_q88 + step);
    }
    if (target_q88 < current_q88) {
        step = ((uint32_t)(current_q88 - target_q88) * glide_k_q8) >> 8;
        if (step == 0U) {
            return target_q88;
        }
        return (uint16_t)(current_q88 - step);
    }
    return current_q88;
}

void WS2812_GlideTask(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t changed = 0U;

    if ((now - last_glide_tick_ms) < WS2812_GLIDE_TICK_MS) {
        return;
    }
    last_glide_tick_ms = now;
    if (ws2812_glide_active == 0U) {
        return;
    }

    ws2812_glide_active = 0U;

    for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
        uint8_t out;

        ws2812_glide_g_q88[i] =
            glide_channel(ws2812_glide_g_q88[i], ws2812_leds[i].g, ws2812_glide_k_q8);
        ws2812_glide_r_q88[i] =
            glide_channel(ws2812_glide_r_q88[i], ws2812_leds[i].r, ws2812_glide_k_q8);
        ws2812_glide_b_q88[i] =
            glide_channel(ws2812_glide_b_q88[i], ws2812_leds[i].b, ws2812_glide_k_q8);

        if (ws2812_glide_g_q88[i] != ((uint16_t)ws2812_leds[i].g << 8) ||
            ws2812_glide_r_q88[i] != ((uint16_t)ws2812_leds[i].r << 8) ||
            ws2812_glide_b_q88[i] != ((uint16_t)ws2812_leds[i].b << 8)) {
            ws2812_glide_active = 1U;
        }

        out = (uint8_t)(ws2812_glide_g_q88[i] >> 8);
        if (out != ws2812_output[i].g) {
            ws2812_output[i].g = out;
            changed = 1U;
        }
        out = (uint8_t)(ws2812_glide_r_q88[i] >> 8);
        if (out != ws2812_output[i].r) {
            ws2812_output[i].r = out;
            changed = 1U;
        }
        out = (uint8_t)(ws2812_glide_b_q88[i] >> 8);
        if (out != ws2812_output[i].b) {
            ws2812_output[i].b = out;
            changed = 1U;
        }
    }

#if WS2812_COLOR_DIAGNOSTICS
    {
        uint32_t min_r = 0xFFU;
        uint32_t min_g = 0xFFU;
        uint32_t min_b = 0xFFU;
        uint32_t max_r = 0U;
        uint32_t max_g = 0U;
        uint32_t max_b = 0U;

        for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
            if (ws2812_output[i].r < min_r) { min_r = ws2812_output[i].r; }
            if (ws2812_output[i].g < min_g) { min_g = ws2812_output[i].g; }
            if (ws2812_output[i].b < min_b) { min_b = ws2812_output[i].b; }
            if (ws2812_output[i].r > max_r) { max_r = ws2812_output[i].r; }
            if (ws2812_output[i].g > max_g) { max_g = ws2812_output[i].g; }
            if (ws2812_output[i].b > max_b) { max_b = ws2812_output[i].b; }
        }

        g_ws2812_output_min_r = min_r;
        g_ws2812_output_max_r = max_r;
        g_ws2812_output_spread_r = max_r - min_r;
        g_ws2812_output_min_g = min_g;
        g_ws2812_output_max_g = max_g;
        g_ws2812_output_spread_g = max_g - min_g;
        g_ws2812_output_min_b = min_b;
        g_ws2812_output_max_b = max_b;
        g_ws2812_output_spread_b = max_b - min_b;
    }
#endif

    if (changed != 0U) {
        (void)show();
    }
}

void WS2812_Flush(void)
{
    if (ws2812_frame_pending == 0U) {
        return;
    }

    /* ws2812_leds still holds the deferred frame; re-encode and send it.
     * If DMA is still busy this re-latches the pending flag and we try
     * again on the next main-loop pass.
     */
    if (show() == 1U) {
        last_update_ms = HAL_GetTick();
    }
}

void WS2812_Clear(void)
{
    for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
        ws2812_leds[i].r = 0U;
        ws2812_leds[i].g = 0U;
        ws2812_leds[i].b = 0U;
        ws2812_output[i].r = 0U;
        ws2812_output[i].g = 0U;
        ws2812_output[i].b = 0U;
        ws2812_glide_g_q88[i] = 0U;
        ws2812_glide_r_q88[i] = 0U;
        ws2812_glide_b_q88[i] = 0U;
    }

    for (uint32_t zone = 0U; zone < WS2812_INPUT_ZONE_COUNT; zone++) {
        ws2812_prev_zone_raw_r[zone] = 0U;
        ws2812_prev_zone_raw_g[zone] = 0U;
        ws2812_prev_zone_raw_b[zone] = 0U;
#if WS2812_ISOLATED_SPIKE_FILTER
        ws2812_zone_filtered_r[zone] = 0U;
        ws2812_zone_filtered_g[zone] = 0U;
        ws2812_zone_filtered_b[zone] = 0U;
        ws2812_zone_delta_cache[zone] = 0U;
#endif
    }

    ws2812_force_idle_low();
    ws2812_frame_pending = 0U;
    ws2812_glide_active = 0U;

    (void)show();

    last_update_ms = HAL_GetTick();
}

void WS2812_TestSolidRgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
        ws2812_leds[i].r = r;
        ws2812_leds[i].g = g;
        ws2812_leds[i].b = b;
        ws2812_output[i].r = r;
        ws2812_output[i].g = g;
        ws2812_output[i].b = b;
        ws2812_glide_r_q88[i] = (uint16_t)r << 8;
        ws2812_glide_g_q88[i] = (uint16_t)g << 8;
        ws2812_glide_b_q88[i] = (uint16_t)b << 8;
    }

    ws2812_frame_pending = 0U;
    ws2812_glide_active = 0U;
    (void)show();
    last_update_ms = HAL_GetTick();
}


static uint8_t rgb_to_channel(uint32_t value, uint32_t wb_pct)
{
    if (value > 255U) {
        value = 255U;
    }

#if WS2812_GAMMA_CORRECTION
    value = ws2812_gamma_lut[value];
#else
    value = (value * WS2812_MAX_CHANNEL) / 255U;
#endif

    return (uint8_t)((value * wb_pct) / 100U);
}

static inline uint32_t channel_delta_u8(uint8_t a, uint8_t b)
{
    return a >= b ? (uint32_t)(a - b) : (uint32_t)(b - a);
}


static void configure_data_gpio_output(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    HAL_GPIO_WritePin(WS2812_GPIO_PORT, WS2812_GPIO_PIN, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = WS2812_GPIO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(WS2812_GPIO_PORT, &GPIO_InitStruct);
}

static void configure_data_gpio_tim2(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = WS2812_GPIO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(WS2812_GPIO_PORT, &GPIO_InitStruct);
}

static uint32_t tim2_kernel_clock_hz(void)
{
    RCC_ClkInitTypeDef clk_config;
    uint32_t flash_latency;
    uint32_t pclk1_hz = HAL_RCC_GetPCLK1Freq();

    HAL_RCC_GetClockConfig(&clk_config, &flash_latency);
    if (clk_config.APB1CLKDivider == RCC_HCLK_DIV1) {
        return pclk1_hz;
    }

    return pclk1_hz * 2U;
}

static uint32_t tim_dma_init(void)
{
    uint32_t tim_clk_hz;
    uint32_t arr_expected;
    uint32_t slot_hz;

    if (htim2.Instance != TIM2) {
        return 0U;
    }

    tim_clk_hz = tim2_kernel_clock_hz();
    arr_expected = ((tim_clk_hz + 400000U) / 800000U) - 1U;
    g_ws2812_tim2_kernel_hz = tim_clk_hz;
    g_ws2812_arr_expected = arr_expected;

    if (TIM2->ARR != arr_expected) {
        __HAL_TIM_SET_AUTORELOAD(&htim2, arr_expected);
    }

    slot_hz = tim_clk_hz / (TIM2->ARR + 1U);
    g_ws2812_arr_actual = TIM2->ARR;
    g_ws2812_slot_hz = slot_hz;
    if (slot_hz < 790000U || slot_hz > 810000U) {
        g_ws2812_clock_ok = 0U;
        return 0U;
    }

    g_ws2812_clock_ok = 1U;
    return 1U;
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) {
        return;
    }

    ws2812_force_idle_low();
    g_ws2812_dma_complete_count++;
}

void HAL_TIM_ErrorCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) {
        return;
    }

    if (ws2812_dma_busy != 0U) {
        g_ws2812_dma_error_count++;
        ws2812_force_idle_low();
    }
}

static void encode_dma_buffer(void)
{
    uint32_t *dst = ws2812_pwm_buf;
#if WS2812_DMA_DIAGNOSTICS
    uint32_t buf_min = WS2812_PWM_ONE_TICKS;
    uint32_t buf_max = 0U;
    uint32_t overrange = 0U;
    uint32_t arr = TIM2->ARR;
#endif

    for (uint32_t led = 0U; led < WS2812_LED_COUNT; led++) {
        dst = encode_pwm_byte(dst, ws2812_output[led].g);
        dst = encode_pwm_byte(dst, ws2812_output[led].r);
        dst = encode_pwm_byte(dst, ws2812_output[led].b);
    }

    while (dst < &ws2812_pwm_buf[WS2812_PWM_BUF_LEN]) {
        *dst++ = 0U;
    }

#if WS2812_DMA_DIAGNOSTICS
    for (uint32_t i = 0U; i < WS2812_LED_COUNT * 24U; i++) {
        uint32_t value = ws2812_pwm_buf[i];

        if (value < buf_min) { buf_min = value; }
        if (value > buf_max) { buf_max = value; }
        if (value > arr)     { overrange++; }
    }
    g_ws2812_buf_min       = buf_min;
    g_ws2812_buf_max       = buf_max;
    g_ws2812_buf_overrange = overrange;
    g_ws2812_buf_entry0   = ws2812_pwm_buf[0];
    g_ws2812_buf_entry1   = ws2812_pwm_buf[1];
    g_ws2812_buf_entry2   = ws2812_pwm_buf[2];
    g_ws2812_buf_entry3   = ws2812_pwm_buf[3];
#endif
}

static inline uint32_t *encode_pwm_byte(uint32_t *dst, uint8_t value)
{
    const uint32_t *bits = ws2812_pwm_nibble_lut[value >> 4U];

    dst[0] = bits[0];
    dst[1] = bits[1];
    dst[2] = bits[2];
    dst[3] = bits[3];
    bits = ws2812_pwm_nibble_lut[value & 0x0FU];
    dst[4] = bits[0];
    dst[5] = bits[1];
    dst[6] = bits[2];
    dst[7] = bits[3];
    return dst + 8;
}

static uint32_t show_tim_dma(void)
{
    HAL_StatusTypeDef status;

    if (ws2812_dma_available == 0U) {
        return 0U;
    }

    if (ws2812_dma_busy != 0U) {
        if ((HAL_GetTick() - ws2812_dma_start_ms) >= WS2812_DMA_BUSY_TIMEOUT_MS) {
            __disable_irq();
            if (ws2812_dma_busy != 0U) {
                ws2812_force_idle_low();
            }
            __enable_irq();
            return 0U;
        }
        ws2812_frame_pending = 1U;
        return 2U;
    }

    configure_data_gpio_tim2();
    encode_dma_buffer();

    ws2812_dma_busy = 1U;
    g_ws2812_dma_busy = 1U;
    status = HAL_TIM_PWM_Start_DMA(&htim2,
                                   TIM_CHANNEL_2,
                                   ws2812_pwm_buf,
                                   WS2812_PWM_BUF_LEN);
    if (status != HAL_OK) {
        g_ws2812_dma_error_count++;
        ws2812_force_idle_low();
        return 0U;
    }

#if WS2812_DMA_DIAGNOSTICS
    /* Sample CCR2 immediately after DMA start. With WORD DMA this should
     * read as 0 (preload not yet updated) or the first duty value once the
     * first UEV fires. A value >> ARR here confirms a data-width problem. */
    g_ws2812_ccr2_snapshot = TIM2->CCR2;
    /* Read actual MSIZE/PSIZE from the live DMA CR register.
     * Both should be 2 (word) after the MSP fix.  If either reads 1
     * (half-word), the MSP change didn't reach the running binary. */
    {
        uint32_t cr = DMA1_Stream6->CR;
        g_ws2812_dma_msize = (cr >> 13U) & 0x3U;
        g_ws2812_dma_psize = (cr >> 11U) & 0x3U;
    }
#endif
    g_ws2812_dma_started_count++;
    ws2812_dma_start_ms = HAL_GetTick();
    ws2812_frame_pending = 0U;
    return 1U;
}

static uint32_t show(void)
{
    uint32_t dma_result = show_tim_dma();

    g_ws2812_show_count++;
    return dma_result == 1U ? 1U : 0U;
}


static void ws2812_force_idle_low(void)
{
    (void)HAL_TIM_PWM_Stop_DMA(&htim2, TIM_CHANNEL_2);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0U);
    __HAL_TIM_DISABLE(&htim2);
    __HAL_TIM_SET_COUNTER(&htim2, 0U);
    htim2.Instance->EGR = TIM_EGR_UG;
    ws2812_dma_busy = 0U;
    g_ws2812_dma_busy = 0U;
    ws2812_dma_start_ms = 0U;
    configure_data_gpio_output();
}
