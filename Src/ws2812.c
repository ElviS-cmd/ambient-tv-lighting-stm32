#include "ws2812.h"

#define WS2812_GPIO_PORT GPIOA
#define WS2812_GPIO_PIN  GPIO_PIN_1
#define WS2812_LED_COUNT 300U
#define WS2812_ZONE_COUNT 4U
#define WS2812_BOTTOM_START 164U
#define WS2812_BOTTOM_COUNT 43U
#define WS2812_LEFT_START   207U
#define WS2812_LEFT_COUNT   25U
#define WS2812_TOP_START    232U
#define WS2812_TOP_COUNT    43U
#define WS2812_RIGHT_START  275U
#define WS2812_RIGHT_COUNT  25U
#define WS2812_MAX_CHANNEL 32U
#define WS2812_UPDATE_PERIOD_MS 100U

typedef struct {
    uint8_t g;
    uint8_t r;
    uint8_t b;
} ws2812_color_t;

static ws2812_color_t ws2812_leds[WS2812_LED_COUNT];
static uint32_t last_update_ms;
static uint32_t last_brightness_percent = 0xFFFFFFFFU;
static uint32_t last_zone_percent[WS2812_ZONE_COUNT] = {
    0xFFFFFFFFU,
    0xFFFFFFFFU,
    0xFFFFFFFFU,
    0xFFFFFFFFU
};

static void dwt_delay_init(void);
static inline void delay_cycles(uint32_t cycles);
static inline void write_bit(uint8_t bit);
static void write_byte(uint8_t byte);
static void show(void);
static void fill(uint8_t r, uint8_t g, uint8_t b);
static void set_range(uint32_t start, uint32_t count, uint8_t r, uint8_t g, uint8_t b);
static uint8_t percent_to_channel(uint32_t percent);

void WS2812_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    HAL_GPIO_WritePin(WS2812_GPIO_PORT, WS2812_GPIO_PIN, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = WS2812_GPIO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(WS2812_GPIO_PORT, &GPIO_InitStruct);

    dwt_delay_init();
    WS2812_Clear();
}

void WS2812_Task(uint32_t brightness_percent)
{
    uint32_t now = HAL_GetTick();

    if (brightness_percent > 100U) {
        brightness_percent = 100U;
    }

    if (brightness_percent == last_brightness_percent &&
        (now - last_update_ms) < WS2812_UPDATE_PERIOD_MS) {
        return;
    }

    if ((now - last_update_ms) < WS2812_UPDATE_PERIOD_MS) {
        return;
    }

    uint8_t level = (uint8_t)((brightness_percent * WS2812_MAX_CHANNEL) / 100U);

    fill(level, level, level);
    last_brightness_percent = brightness_percent;
    last_update_ms = now;
}

void WS2812_TaskZones(uint32_t zone0_percent,
                      uint32_t zone1_percent,
                      uint32_t zone2_percent,
                      uint32_t zone3_percent)
{
    uint32_t now = HAL_GetTick();
    uint32_t zones[WS2812_ZONE_COUNT] = {
        zone0_percent,
        zone1_percent,
        zone2_percent,
        zone3_percent
    };
    uint32_t changed = 0U;

    for (uint32_t i = 0; i < WS2812_ZONE_COUNT; i++) {
        if (zones[i] > 100U) {
            zones[i] = 100U;
        }

        if (zones[i] != last_zone_percent[i]) {
            changed = 1U;
        }
    }

    if (changed == 0U && (now - last_update_ms) < WS2812_UPDATE_PERIOD_MS) {
        return;
    }

    if ((now - last_update_ms) < WS2812_UPDATE_PERIOD_MS) {
        return;
    }

    for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
        ws2812_leds[i].r = 0U;
        ws2812_leds[i].g = 0U;
        ws2812_leds[i].b = 0U;
    }

    set_range(WS2812_RIGHT_START,
              WS2812_RIGHT_COUNT,
              percent_to_channel(zones[0]),
              percent_to_channel(zones[0]),
              percent_to_channel(zones[0]));
    set_range(WS2812_TOP_START,
              WS2812_TOP_COUNT,
              percent_to_channel(zones[1]),
              percent_to_channel(zones[1]),
              percent_to_channel(zones[1]));
    set_range(WS2812_LEFT_START,
              WS2812_LEFT_COUNT,
              percent_to_channel(zones[2]),
              percent_to_channel(zones[2]),
              percent_to_channel(zones[2]));
    set_range(WS2812_BOTTOM_START,
              WS2812_BOTTOM_COUNT,
              percent_to_channel(zones[3]),
              percent_to_channel(zones[3]),
              percent_to_channel(zones[3]));

    for (uint32_t zone = 0; zone < WS2812_ZONE_COUNT; zone++) {
        last_zone_percent[zone] = zones[zone];
    }

    show();
    last_update_ms = now;
}

void WS2812_Clear(void)
{
    fill(0, 0, 0);
    last_brightness_percent = 0U;
}

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
        ws2812_leds[i].r = r;
        ws2812_leds[i].g = g;
        ws2812_leds[i].b = b;
    }

    show();
}

static void set_range(uint32_t start, uint32_t count, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t end = start + count;

    if (end > WS2812_LED_COUNT) {
        end = WS2812_LED_COUNT;
    }

    for (uint32_t i = start; i < end; i++) {
        ws2812_leds[i].r = r;
        ws2812_leds[i].g = g;
        ws2812_leds[i].b = b;
    }
}

static uint8_t percent_to_channel(uint32_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

    return (uint8_t)((percent * WS2812_MAX_CHANNEL) / 100U);
}

static void show(void)
{
    __disable_irq();

    for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
        write_byte(ws2812_leds[i].g);
        write_byte(ws2812_leds[i].r);
        write_byte(ws2812_leds[i].b);
    }

    __enable_irq();

    HAL_GPIO_WritePin(WS2812_GPIO_PORT, WS2812_GPIO_PIN, GPIO_PIN_RESET);
    HAL_Delay(1);
}

static void write_byte(uint8_t byte)
{
    for (int8_t bit = 7; bit >= 0; bit--) {
        write_bit((byte >> bit) & 0x01U);
    }
}

__attribute__((always_inline, optimize("O3"))) static inline void write_bit(uint8_t bit)
{
    if (bit) {
        WS2812_GPIO_PORT->BSRR = WS2812_GPIO_PIN;
        delay_cycles(105);
        WS2812_GPIO_PORT->BSRR = (uint32_t)WS2812_GPIO_PIN << 16U;
        delay_cycles(80);
    } else {
        WS2812_GPIO_PORT->BSRR = WS2812_GPIO_PIN;
        delay_cycles(40);
        WS2812_GPIO_PORT->BSRR = (uint32_t)WS2812_GPIO_PIN << 16U;
        delay_cycles(145);
    }
}

static void dwt_delay_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

__attribute__((always_inline, optimize("O3"))) static inline void delay_cycles(uint32_t cycles)
{
    uint32_t start = DWT->CYCCNT;

    while ((DWT->CYCCNT - start) < cycles) {
    }
}
