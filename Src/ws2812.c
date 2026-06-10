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
#define WS2812_MAX_CHANNEL 96U
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

static ws2812_color_t ws2812_leds[WS2812_LED_COUNT];
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
static uint8_t ws2812_zone_filtered_r[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_zone_filtered_g[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_zone_filtered_b[WS2812_INPUT_ZONE_COUNT];
static uint8_t ws2812_zone_delta_cache[WS2812_INPUT_ZONE_COUNT];


static void configure_data_gpio_output(void);
static void configure_data_gpio_tim2(void);
static uint32_t tim2_kernel_clock_hz(void);
static uint32_t tim_dma_init(void);
static void encode_dma_buffer(void);
static uint32_t show_tim_dma(void);
static uint32_t show(void);
static uint8_t rgb_to_channel(uint32_t value);
static inline uint32_t channel_delta_u8(uint8_t a, uint8_t b);
static void ws2812_force_idle_low(void);

void WS2812_Init(void)
{
    for (uint32_t zone = 0U; zone < WS2812_INPUT_ZONE_COUNT; zone++) {
        ws2812_prev_zone_raw_r[zone] = 0U;
        ws2812_prev_zone_raw_g[zone] = 0U;
        ws2812_prev_zone_raw_b[zone] = 0U;
        ws2812_zone_filtered_r[zone] = 0U;
        ws2812_zone_filtered_g[zone] = 0U;
        ws2812_zone_filtered_b[zone] = 0U;
        ws2812_zone_delta_cache[zone] = 0U;
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
    uint32_t idx;
    uint32_t src;
    uint32_t raw_delta_max = 0U;
    uint32_t scene_bypass;
    uint32_t min_update_period_ms;
    uint32_t elapsed_ms;
    uint8_t zone_target_r[WS2812_INPUT_ZONE_COUNT];
    uint8_t zone_target_g[WS2812_INPUT_ZONE_COUNT];
    uint8_t zone_target_b[WS2812_INPUT_ZONE_COUNT];

    if (zone_r == NULL || zone_g == NULL || zone_b == NULL) {
        return;
    }

    if (zone_count < WS2812_INPUT_ZONE_COUNT) {
        return;
    }

    for (uint32_t z = 0U; z < WS2812_INPUT_ZONE_COUNT; z++) {
        uint8_t target_r = rgb_to_channel(zone_r[z]);
        uint8_t target_g = rgb_to_channel(zone_g[z]);
        uint8_t target_b = rgb_to_channel(zone_b[z]);
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

    elapsed_ms = now - last_update_ms;
    min_update_period_ms =
        raw_delta_max >= WS2812_FAST_DELTA_THRESHOLD ?
        WS2812_FAST_UPDATE_PERIOD_MS : WS2812_UPDATE_PERIOD_MS;
    if (elapsed_ms < min_update_period_ms) {
        return;
    }
    /* Advance raw-history only when this frame is accepted for processing.
     * This keeps delta detection honest during skipped update periods. */
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

    for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
        ws2812_leds[i].r = 0U;
        ws2812_leds[i].g = 0U;
        ws2812_leds[i].b = 0U;
    }

    /* Left physical LEDs 1..24 run bottom -> top. DCMI left zones run
     * top -> bottom, so reverse and resample 25 source zones onto 24 LEDs.
     */
    for (uint32_t z = 0U; z < WS2812_LEFT_COUNT; z++) {
        idx = WS2812_LEFT_START + z;
        src = WS2812_INPUT_LEFT_OFFSET +
              (WS2812_INPUT_LEFT_COUNT - 1U) -
              ((z * (WS2812_INPUT_LEFT_COUNT - 1U) +
                ((WS2812_LEFT_COUNT - 1U) / 2U)) /
               (WS2812_LEFT_COUNT - 1U));
        ws2812_leds[idx].r = ws2812_zone_filtered_r[src];
        ws2812_leds[idx].g = ws2812_zone_filtered_g[src];
        ws2812_leds[idx].b = ws2812_zone_filtered_b[src];
    }

    /* Top physical LEDs 25..67 run left -> right. DCMI top zones run
     * right -> left, so reverse them.
     */
    for (uint32_t z = 0U; z < WS2812_TOP_COUNT; z++) {
        idx = WS2812_TOP_START + z;
        src = WS2812_INPUT_TOP_OFFSET + (WS2812_INPUT_TOP_COUNT - 1U) - z;
        ws2812_leds[idx].r = ws2812_zone_filtered_r[src];
        ws2812_leds[idx].g = ws2812_zone_filtered_g[src];
        ws2812_leds[idx].b = ws2812_zone_filtered_b[src];
    }

    /* Right physical LEDs 68..92 run top -> bottom. DCMI right zones run
     * bottom -> top, so reverse them.
     */
    for (uint32_t z = 0U; z < WS2812_RIGHT_COUNT; z++) {
        idx = WS2812_RIGHT_START + z;
        src = WS2812_INPUT_RIGHT_OFFSET + (WS2812_INPUT_RIGHT_COUNT - 1U) - z;
        ws2812_leds[idx].r = ws2812_zone_filtered_r[src];
        ws2812_leds[idx].g = ws2812_zone_filtered_g[src];
        ws2812_leds[idx].b = ws2812_zone_filtered_b[src];
    }

    /* Bottom physical LEDs 93..135 run right -> left. DCMI bottom zones run
     * left -> right, so reverse them.
     */
    for (uint32_t z = 0U; z < WS2812_BOTTOM_COUNT; z++) {
        idx = WS2812_BOTTOM_START + z;
        src = WS2812_INPUT_BOTTOM_OFFSET + (WS2812_INPUT_BOTTOM_COUNT - 1U) - z;
        ws2812_leds[idx].r = ws2812_zone_filtered_r[src];
        ws2812_leds[idx].g = ws2812_zone_filtered_g[src];
        ws2812_leds[idx].b = ws2812_zone_filtered_b[src];
    }

    {
        uint32_t nonzero = 0U;
        uint32_t first = 0xFFFFFFFFU;
        uint32_t last = 0U;
        uint32_t max_channel = 0U;

        for (uint32_t i = 0U; i < WS2812_LED_COUNT; i++) {
            uint32_t local_max = ws2812_leds[i].r;

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
    }

    if (show() != 0U) {
        last_update_ms = now;
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
    }

    for (uint32_t zone = 0U; zone < WS2812_INPUT_ZONE_COUNT; zone++) {
        ws2812_prev_zone_raw_r[zone] = 0U;
        ws2812_prev_zone_raw_g[zone] = 0U;
        ws2812_prev_zone_raw_b[zone] = 0U;
        ws2812_zone_filtered_r[zone] = 0U;
        ws2812_zone_filtered_g[zone] = 0U;
        ws2812_zone_filtered_b[zone] = 0U;
        ws2812_zone_delta_cache[zone] = 0U;
    }

    ws2812_force_idle_low();
    ws2812_frame_pending = 0U;

    (void)show();

    last_update_ms = HAL_GetTick();
}


static uint8_t rgb_to_channel(uint32_t value)
{
    if (value > 255U) {
        value = 255U;
    }

    return (uint8_t)((value * WS2812_MAX_CHANNEL) / 255U);
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
    uint32_t index = 0U;
    uint32_t buf_min = WS2812_PWM_ONE_TICKS;
    uint32_t buf_max = 0U;
    uint32_t overrange = 0U;
    uint32_t arr = TIM2->ARR;

    for (uint32_t led = 0U; led < WS2812_LED_COUNT; led++) {
        uint8_t bytes[3] = {
            ws2812_leds[led].g,
            ws2812_leds[led].r,
            ws2812_leds[led].b
        };

        for (uint32_t byte_index = 0U; byte_index < 3U; byte_index++) {
            for (int8_t bit = 7; bit >= 0; bit--) {
                uint32_t v = (bytes[byte_index] & (1U << bit)) != 0U ?
                             WS2812_PWM_ONE_TICKS : WS2812_PWM_ZERO_TICKS;
                ws2812_pwm_buf[index++] = v;
                if (v < buf_min) { buf_min = v; }
                if (v > buf_max) { buf_max = v; }
                if (v > arr)     { overrange++; }
            }
        }
    }

    while (index < WS2812_PWM_BUF_LEN) {
        ws2812_pwm_buf[index++] = 0U;
    }

    g_ws2812_buf_min      = buf_min;
    g_ws2812_buf_max      = buf_max;
    g_ws2812_buf_overrange = overrange;
    g_ws2812_buf_entry0   = ws2812_pwm_buf[0];
    g_ws2812_buf_entry1   = ws2812_pwm_buf[1];
    g_ws2812_buf_entry2   = ws2812_pwm_buf[2];
    g_ws2812_buf_entry3   = ws2812_pwm_buf[3];
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
