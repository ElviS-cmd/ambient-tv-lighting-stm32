#ifndef DCMI_CAPTURE_H
#define DCMI_CAPTURE_H

#include "main.h"

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

extern volatile uint32_t g_dcmi_debug_marker;
extern volatile uint32_t g_dcmi_state;
extern volatile uint32_t g_dcmi_crop_status;
extern volatile uint32_t g_dcmi_start_status;
extern volatile uint32_t g_dcmi_frame_count;
extern volatile uint32_t g_dcmi_error_count;
extern volatile uint32_t g_dcmi_timeout_count;
extern volatile uint32_t g_dcmi_restart_count;
extern volatile uint32_t g_dcmi_dma_state;
extern volatile uint32_t g_dcmi_dma_error;
extern volatile uint32_t g_dcmi_dma_ndtr;
extern volatile uint32_t g_dcmi_hal_state;
extern volatile uint32_t g_dcmi_hal_error;
extern volatile uint32_t g_dcmi_sr;
extern volatile uint32_t g_dcmi_ris;
extern volatile uint32_t g_dcmi_mis;
extern volatile uint32_t g_dcmi_cr;
extern volatile uint32_t g_dcmi_first_word;
extern volatile uint32_t g_dcmi_second_word;
extern volatile uint32_t g_dcmi_last_word;
extern volatile uint32_t g_dcmi_nonzero_words;
extern volatile uint32_t g_dcmi_changing_words;
extern volatile uint32_t g_dcmi_buffer_checksum;
extern volatile uint32_t g_dcmi_sample_count;
extern volatile uint32_t g_dcmi_average_byte;
extern volatile uint32_t g_dcmi_min_byte;
extern volatile uint32_t g_dcmi_max_byte;
extern volatile uint32_t g_dcmi_brightness_percent;
extern volatile uint32_t g_dcmi_zone0_brightness_percent;
extern volatile uint32_t g_dcmi_zone1_brightness_percent;
extern volatile uint32_t g_dcmi_zone2_brightness_percent;
extern volatile uint32_t g_dcmi_zone3_brightness_percent;

void DCMI_Capture_Init(void);
void DCMI_Capture_Task(void);
dcmi_capture_status_t DCMI_Capture_GetStatus(void);

#endif
