#include "video_capture.h"

volatile uint32_t g_video_hsync_total;
volatile uint32_t g_video_vsync_total;
volatile uint32_t g_video_pclk_total;
volatile uint32_t g_video_hsync_window;
volatile uint32_t g_video_vsync_window;
volatile uint32_t g_video_pclk_window;
volatile uint32_t g_video_hsync_seen;
volatile uint32_t g_video_vsync_seen;
volatile uint32_t g_video_pclk_seen;
volatile uint32_t g_video_hsync_level;
volatile uint32_t g_video_vsync_level;
volatile uint32_t g_video_pclk_level;
volatile uint32_t g_video_hsync_poll_total;
volatile uint32_t g_video_vsync_poll_total;
volatile uint32_t g_video_hsync_poll_window;
volatile uint32_t g_video_vsync_poll_window;
volatile uint32_t g_video_debug_marker;
volatile uint32_t g_video_loop_count;
volatile uint32_t g_video_gpiob_idr;
volatile uint32_t g_video_exti_imr;
volatile uint32_t g_video_exti_rtsr;
volatile uint32_t g_video_exti_ftsr;
volatile uint32_t g_video_exticr2;
volatile uint32_t g_video_exticr3;
volatile uint32_t g_video_pa4_total;
volatile uint32_t g_video_pa4_window;
volatile uint32_t g_video_pa4_level;
volatile uint32_t g_video_pa6_total;
volatile uint32_t g_video_pa6_window;
volatile uint32_t g_video_pa6_level;
volatile uint32_t g_video_pb8_total;
volatile uint32_t g_video_pb8_window;
volatile uint32_t g_video_pb8_level;

static volatile uint32_t video_hsync_irq_count;
static volatile uint32_t video_vsync_irq_count;
static volatile uint32_t video_pb8_irq_count;

static uint32_t take_counter_snapshot(volatile uint32_t *counter);
static uint32_t count_edges(GPIO_TypeDef *port, uint16_t pin, uint32_t sample_ms);
static void update_status_leds(void);

void VideoCapture_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    HAL_GPIO_WritePin(VIDEO_STATUS_LED_PORT, VIDEO_STATUS_LED_PINS, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(VIDEO_AUDIO_RST_PORT, VIDEO_AUDIO_RST_PIN, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = VIDEO_STATUS_LED_PINS | VIDEO_AUDIO_RST_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(VIDEO_STATUS_LED_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = VIDEO_PCLK_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VIDEO_PCLK_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = VIDEO_HSYNC_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VIDEO_HSYNC_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = VIDEO_VSYNC_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VIDEO_VSYNC_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = VIDEO_HSYNC_REF_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VIDEO_HSYNC_REF_PORT, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(EXTI4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);

    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    g_video_debug_marker = 20260510U;
}

void VideoCapture_Task(void)
{
    static uint32_t hsync_accumulated = 0;
    static uint32_t vsync_accumulated = 0;
    static uint32_t pclk_accumulated = 0;
    static uint32_t hsync_poll_accumulated = 0;
    static uint32_t vsync_poll_accumulated = 0;
    static uint32_t pa4_accumulated = 0;
    static uint32_t pa6_accumulated = 0;
    static uint32_t pb8_accumulated = 0;
    static uint32_t last_report_ms = 0;

    uint32_t hsync_edges = take_counter_snapshot(&video_hsync_irq_count);
    uint32_t vsync_edges = take_counter_snapshot(&video_vsync_irq_count);
    uint32_t pb8_edges = take_counter_snapshot(&video_pb8_irq_count);
    uint32_t hsync_poll_edges = count_edges(VIDEO_HSYNC_PORT, VIDEO_HSYNC_PIN, 2);
    uint32_t vsync_poll_edges = count_edges(VIDEO_VSYNC_PORT, VIDEO_VSYNC_PIN, 2);
    uint32_t pclk_edges = count_edges(VIDEO_PCLK_PORT, VIDEO_PCLK_PIN, 2);
    uint32_t pa4_edges = hsync_edges;
    uint32_t pa6_edges = pclk_edges;

    g_video_loop_count++;
    g_video_gpiob_idr = GPIOB->IDR;
    g_video_exti_imr = EXTI->IMR;
    g_video_exti_rtsr = EXTI->RTSR;
    g_video_exti_ftsr = EXTI->FTSR;
    g_video_exticr2 = SYSCFG->EXTICR[1];
    g_video_exticr3 = SYSCFG->EXTICR[2];

    g_video_hsync_level = HAL_GPIO_ReadPin(VIDEO_HSYNC_PORT, VIDEO_HSYNC_PIN) == GPIO_PIN_SET ? 1U : 0U;
    g_video_vsync_level = HAL_GPIO_ReadPin(VIDEO_VSYNC_PORT, VIDEO_VSYNC_PIN) == GPIO_PIN_SET ? 1U : 0U;
    g_video_pclk_level = HAL_GPIO_ReadPin(VIDEO_PCLK_PORT, VIDEO_PCLK_PIN) == GPIO_PIN_SET ? 1U : 0U;
    g_video_pa4_level = g_video_hsync_level;
    g_video_pa6_level = g_video_pclk_level;
    g_video_pb8_level = HAL_GPIO_ReadPin(VIDEO_HSYNC_REF_PORT, VIDEO_HSYNC_REF_PIN) == GPIO_PIN_SET ? 1U : 0U;

    hsync_accumulated += hsync_edges;
    vsync_accumulated += vsync_edges;
    pclk_accumulated += pclk_edges;
    hsync_poll_accumulated += hsync_poll_edges;
    vsync_poll_accumulated += vsync_poll_edges;
    pa4_accumulated += pa4_edges;
    pa6_accumulated += pa6_edges;
    pb8_accumulated += pb8_edges;

    g_video_hsync_total += hsync_edges;
    g_video_vsync_total += vsync_edges;
    g_video_pclk_total += pclk_edges;
    g_video_hsync_poll_total += hsync_poll_edges;
    g_video_vsync_poll_total += vsync_poll_edges;
    g_video_pa4_total += pa4_edges;
    g_video_pa6_total += pa6_edges;
    g_video_pb8_total += pb8_edges;

    if (hsync_edges > 0U) {
        g_video_hsync_seen = 1U;
    }

    if (vsync_edges > 0U) {
        g_video_vsync_seen = 1U;
    }

    if (pclk_edges > 0U) {
        g_video_pclk_seen = 1U;
    }

    if ((HAL_GetTick() - last_report_ms) < 500U) {
        return;
    }

    last_report_ms = HAL_GetTick();

    g_video_hsync_window = hsync_accumulated;
    g_video_vsync_window = vsync_accumulated;
    g_video_pclk_window = pclk_accumulated;
    g_video_hsync_poll_window = hsync_poll_accumulated;
    g_video_vsync_poll_window = vsync_poll_accumulated;
    g_video_pa4_window = pa4_accumulated;
    g_video_pa6_window = pa6_accumulated;
    g_video_pb8_window = pb8_accumulated;

    update_status_leds();

    hsync_accumulated = 0;
    vsync_accumulated = 0;
    pclk_accumulated = 0;
    hsync_poll_accumulated = 0;
    vsync_poll_accumulated = 0;
    pa4_accumulated = 0;
    pa6_accumulated = 0;
    pb8_accumulated = 0;
}

void VideoCapture_EXTI_Callback(uint16_t gpio_pin)
{
    if (gpio_pin == VIDEO_HSYNC_PIN) {
        video_hsync_irq_count++;
    } else if (gpio_pin == VIDEO_VSYNC_PIN) {
        video_vsync_irq_count++;
    } else if (gpio_pin == VIDEO_HSYNC_REF_PIN) {
        video_pb8_irq_count++;
    }
}

video_capture_status_t VideoCapture_GetStatus(void)
{
    video_capture_status_t status;

    status.hsync_total = g_video_hsync_total;
    status.vsync_total = g_video_vsync_total;
    status.pclk_total = g_video_pclk_total;
    status.hsync_window = g_video_hsync_window;
    status.vsync_window = g_video_vsync_window;
    status.pclk_window = g_video_pclk_window;
    status.hsync_seen = g_video_hsync_seen;
    status.vsync_seen = g_video_vsync_seen;
    status.pclk_seen = g_video_pclk_seen;
    status.hsync_level = g_video_hsync_level;
    status.vsync_level = g_video_vsync_level;
    status.pclk_level = g_video_pclk_level;

    return status;
}

static uint32_t take_counter_snapshot(volatile uint32_t *counter)
{
    uint32_t snapshot;

    __disable_irq();
    snapshot = *counter;
    *counter = 0;
    __enable_irq();

    return snapshot;
}

static uint32_t count_edges(GPIO_TypeDef *port, uint16_t pin, uint32_t sample_ms)
{
    uint32_t edges = 0;
    uint32_t start = HAL_GetTick();
    GPIO_PinState previous = HAL_GPIO_ReadPin(port, pin);

    while ((HAL_GetTick() - start) < sample_ms) {
        GPIO_PinState current = HAL_GPIO_ReadPin(port, pin);

        if (current != previous) {
            edges++;
            previous = current;
        }
    }

    return edges;
}

static void update_status_leds(void)
{
    static GPIO_PinState heartbeat = GPIO_PIN_RESET;

    heartbeat = heartbeat == GPIO_PIN_RESET ? GPIO_PIN_SET : GPIO_PIN_RESET;

    HAL_GPIO_WritePin(VIDEO_STATUS_LED_PORT, VIDEO_STATUS_LED_ALIVE, heartbeat);
    HAL_GPIO_WritePin(VIDEO_STATUS_LED_PORT, VIDEO_STATUS_LED_VSYNC,
                      g_video_vsync_window > 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(VIDEO_STATUS_LED_PORT, VIDEO_STATUS_LED_HSYNC,
                      g_video_hsync_window > 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(VIDEO_STATUS_LED_PORT, VIDEO_STATUS_LED_PCLK,
                      g_video_pclk_window > 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
