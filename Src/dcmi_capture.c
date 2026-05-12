#include "dcmi_capture.h"

#define DCMI_CROP_WIDTH_PIXELS 8U
#define DCMI_CROP_HEIGHT_LINES 8U
#define DCMI_CAPTURE_WORDS ((DCMI_CROP_WIDTH_PIXELS * DCMI_CROP_HEIGHT_LINES) / 4U)
#define DCMI_RESTART_DELAY_MS 500U
#define DCMI_CAPTURE_TIMEOUT_MS 500U
#define DCMI_USE_CROP 0U

extern DCMI_HandleTypeDef hdcmi;

volatile uint32_t g_dcmi_debug_marker;
volatile uint32_t g_dcmi_state;
volatile uint32_t g_dcmi_crop_status;
volatile uint32_t g_dcmi_start_status;
volatile uint32_t g_dcmi_frame_count;
volatile uint32_t g_dcmi_error_count;
volatile uint32_t g_dcmi_timeout_count;
volatile uint32_t g_dcmi_restart_count;
volatile uint32_t g_dcmi_dma_state;
volatile uint32_t g_dcmi_dma_error;
volatile uint32_t g_dcmi_dma_ndtr;
volatile uint32_t g_dcmi_hal_state;
volatile uint32_t g_dcmi_hal_error;
volatile uint32_t g_dcmi_sr;
volatile uint32_t g_dcmi_ris;
volatile uint32_t g_dcmi_mis;
volatile uint32_t g_dcmi_cr;
volatile uint32_t g_dcmi_first_word;
volatile uint32_t g_dcmi_second_word;
volatile uint32_t g_dcmi_last_word;
volatile uint32_t g_dcmi_nonzero_words;
volatile uint32_t g_dcmi_changing_words;
volatile uint32_t g_dcmi_buffer_checksum;
volatile uint32_t g_dcmi_sample_count;
volatile uint32_t g_dcmi_average_byte;
volatile uint32_t g_dcmi_min_byte;
volatile uint32_t g_dcmi_max_byte;
volatile uint32_t g_dcmi_brightness_percent;
volatile uint32_t g_dcmi_zone0_brightness_percent;
volatile uint32_t g_dcmi_zone1_brightness_percent;
volatile uint32_t g_dcmi_zone2_brightness_percent;
volatile uint32_t g_dcmi_zone3_brightness_percent;

static uint32_t dcmi_buffer[DCMI_CAPTURE_WORDS];
static dcmi_capture_status_t dcmi_status = {
    .state = DCMI_CAPTURE_NOT_CONFIGURED,
    .frames_seen = 0,
    .dma_half_callbacks = 0,
    .dma_full_callbacks = 0,
    .errors = 0,
};
static uint32_t last_restart_ms;
static uint32_t last_heartbeat_ms;
static volatile uint32_t dcmi_buffer_ready;

static void start_snapshot(void);
static void analyze_buffer(void);
static void update_debug_registers(void);
static void update_status_leds(void);

void DCMI_Capture_Init(void)
{
    g_dcmi_debug_marker = 20260511U;
    #if DCMI_USE_CROP
    g_dcmi_crop_status = HAL_DCMI_ConfigCrop(&hdcmi,
                                             0U,
                                             0U,
                                             DCMI_CROP_WIDTH_PIXELS - 1U,
                                             DCMI_CROP_HEIGHT_LINES - 1U);
    HAL_DCMI_EnableCrop(&hdcmi);
    #else
    g_dcmi_crop_status = HAL_DCMI_DisableCrop(&hdcmi);
    #endif
    dcmi_status.state = DCMI_CAPTURE_READY;
    g_dcmi_state = DCMI_CAPTURE_READY;
    start_snapshot();
}

void DCMI_Capture_Task(void)
{
    update_debug_registers();
    update_status_leds();

    if (dcmi_buffer_ready != 0U) {
        dcmi_buffer_ready = 0U;
        analyze_buffer();
    }

    if (dcmi_status.state == DCMI_CAPTURE_RUNNING) {
        if (g_dcmi_dma_ndtr == 0U) {
            HAL_DCMI_Stop(&hdcmi);
            dcmi_status.frames_seen++;
            g_dcmi_frame_count = dcmi_status.frames_seen;
            dcmi_status.state = DCMI_CAPTURE_READY;
            g_dcmi_state = DCMI_CAPTURE_READY;
            analyze_buffer();
            return;
        }

        if ((HAL_GetTick() - last_restart_ms) >= DCMI_CAPTURE_TIMEOUT_MS) {
            HAL_DCMI_Stop(&hdcmi);
            g_dcmi_timeout_count++;
            dcmi_status.errors++;
            g_dcmi_error_count = dcmi_status.errors;
            dcmi_status.state = DCMI_CAPTURE_ERROR;
            g_dcmi_state = DCMI_CAPTURE_ERROR;
        }

        return;
    }

    if ((HAL_GetTick() - last_restart_ms) >= DCMI_RESTART_DELAY_MS) {
        start_snapshot();
    }
}

dcmi_capture_status_t DCMI_Capture_GetStatus(void)
{
    return dcmi_status;
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi_arg)
{
    if (hdcmi_arg != &hdcmi) {
        return;
    }

    dcmi_status.frames_seen++;
    g_dcmi_frame_count = dcmi_status.frames_seen;
    dcmi_status.state = DCMI_CAPTURE_READY;
    g_dcmi_state = DCMI_CAPTURE_READY;
    dcmi_buffer_ready = 1U;
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *hdcmi_arg)
{
    if (hdcmi_arg != &hdcmi) {
        return;
    }

    dcmi_status.errors++;
    g_dcmi_error_count = dcmi_status.errors;
    dcmi_status.state = DCMI_CAPTURE_ERROR;
    g_dcmi_state = DCMI_CAPTURE_ERROR;
    update_debug_registers();
}

static void start_snapshot(void)
{
    for (uint32_t i = 0; i < DCMI_CAPTURE_WORDS; i++) {
        dcmi_buffer[i] = 0U;
    }

    g_dcmi_start_status = HAL_DCMI_Start_DMA(&hdcmi,
                                             DCMI_MODE_CONTINUOUS,
                                             (uint32_t)dcmi_buffer,
                                             DCMI_CAPTURE_WORDS);
    last_restart_ms = HAL_GetTick();

    if (g_dcmi_start_status == HAL_OK) {
        dcmi_status.state = DCMI_CAPTURE_RUNNING;
        g_dcmi_state = DCMI_CAPTURE_RUNNING;
        g_dcmi_restart_count++;
    } else {
        dcmi_status.errors++;
        g_dcmi_error_count = dcmi_status.errors;
        dcmi_status.state = DCMI_CAPTURE_ERROR;
        g_dcmi_state = DCMI_CAPTURE_ERROR;
    }

    update_debug_registers();
}

static void analyze_buffer(void)
{
    uint32_t previous = dcmi_buffer[0];
    uint32_t nonzero = 0;
    uint32_t changing = 0;
    uint32_t checksum = 0;
    uint32_t sample_sum = 0;
    uint32_t sample_count = 0;
    uint32_t min_byte = 0xFFU;
    uint32_t max_byte = 0U;
    uint32_t zone_sum[4] = {0U, 0U, 0U, 0U};
    uint32_t zone_count[4] = {0U, 0U, 0U, 0U};

    for (uint32_t i = 0; i < DCMI_CAPTURE_WORDS; i++) {
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
            uint32_t zone = sample_count / 16U;

            if (zone > 3U) {
                zone = 3U;
            }

            sample_sum += sample;
            zone_sum[zone] += sample;
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

    g_dcmi_first_word = dcmi_buffer[0];
    g_dcmi_second_word = dcmi_buffer[1];
    g_dcmi_last_word = dcmi_buffer[DCMI_CAPTURE_WORDS - 1U];
    g_dcmi_nonzero_words = nonzero;
    g_dcmi_changing_words = changing;
    g_dcmi_buffer_checksum = checksum;
    g_dcmi_sample_count = sample_count;

    if (sample_count > 0U) {
        g_dcmi_average_byte = sample_sum / sample_count;
        g_dcmi_brightness_percent = (g_dcmi_average_byte * 100U) / 255U;
    } else {
        g_dcmi_average_byte = 0U;
        g_dcmi_brightness_percent = 0U;
        min_byte = 0U;
    }

    g_dcmi_min_byte = min_byte;
    g_dcmi_max_byte = max_byte;

    g_dcmi_zone0_brightness_percent = zone_count[0] > 0U ? ((zone_sum[0] / zone_count[0]) * 100U) / 255U : 0U;
    g_dcmi_zone1_brightness_percent = zone_count[1] > 0U ? ((zone_sum[1] / zone_count[1]) * 100U) / 255U : 0U;
    g_dcmi_zone2_brightness_percent = zone_count[2] > 0U ? ((zone_sum[2] / zone_count[2]) * 100U) / 255U : 0U;
    g_dcmi_zone3_brightness_percent = zone_count[3] > 0U ? ((zone_sum[3] / zone_count[3]) * 100U) / 255U : 0U;
    update_debug_registers();
}

static void update_debug_registers(void)
{
    g_dcmi_hal_state = HAL_DCMI_GetState(&hdcmi);
    g_dcmi_hal_error = HAL_DCMI_GetError(&hdcmi);
    g_dcmi_sr = DCMI->SR;
    g_dcmi_ris = DCMI->RISR;
    g_dcmi_mis = DCMI->MISR;
    g_dcmi_cr = DCMI->CR;

    if (hdcmi.DMA_Handle != NULL) {
        g_dcmi_dma_state = HAL_DMA_GetState(hdcmi.DMA_Handle);
        g_dcmi_dma_error = HAL_DMA_GetError(hdcmi.DMA_Handle);
        g_dcmi_dma_ndtr = hdcmi.DMA_Handle->Instance->NDTR;
    }
}

static void update_status_leds(void)
{
    static GPIO_PinState heartbeat = GPIO_PIN_RESET;

    if ((HAL_GetTick() - last_heartbeat_ms) >= 500U) {
        heartbeat = heartbeat == GPIO_PIN_RESET ? GPIO_PIN_SET : GPIO_PIN_RESET;
        last_heartbeat_ms = HAL_GetTick();
    }

    HAL_GPIO_WritePin(GPIOD, LD6_Pin, heartbeat);
    HAL_GPIO_WritePin(GPIOD, LD4_Pin,
                      g_dcmi_frame_count > 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOD, LD3_Pin,
                      dcmi_status.state == DCMI_CAPTURE_RUNNING ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOD, LD5_Pin,
                      g_dcmi_error_count > 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
