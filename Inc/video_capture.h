#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include "main.h"

#define VIDEO_HSYNC_PORT    GPIOA
#define VIDEO_HSYNC_PIN     GPIO_PIN_4
#define VIDEO_PCLK_PORT     GPIOA
#define VIDEO_PCLK_PIN      GPIO_PIN_6
#define VIDEO_VSYNC_PORT    GPIOB
#define VIDEO_VSYNC_PIN     GPIO_PIN_7

#define VIDEO_HSYNC_REF_PORT    GPIOB
#define VIDEO_HSYNC_REF_PIN     GPIO_PIN_8

#define VIDEO_STATUS_LED_PORT    GPIOD
#define VIDEO_STATUS_LED_VSYNC   GPIO_PIN_12
#define VIDEO_STATUS_LED_HSYNC   GPIO_PIN_13
#define VIDEO_STATUS_LED_PCLK    GPIO_PIN_14
#define VIDEO_STATUS_LED_ALIVE   GPIO_PIN_15
#define VIDEO_STATUS_LED_PINS    (VIDEO_STATUS_LED_VSYNC | \
                                  VIDEO_STATUS_LED_HSYNC | \
                                  VIDEO_STATUS_LED_PCLK | \
                                  VIDEO_STATUS_LED_ALIVE)

#define VIDEO_DCMI_TEST_HSYNC_PORT    VIDEO_HSYNC_PORT
#define VIDEO_DCMI_TEST_HSYNC_PIN     VIDEO_HSYNC_PIN
#define VIDEO_DCMI_TEST_PCLK_PORT     VIDEO_PCLK_PORT
#define VIDEO_DCMI_TEST_PCLK_PIN      VIDEO_PCLK_PIN
#define VIDEO_AUDIO_RST_PORT          GPIOD
#define VIDEO_AUDIO_RST_PIN           GPIO_PIN_4

typedef struct {
    uint32_t hsync_total;
    uint32_t vsync_total;
    uint32_t pclk_total;
    uint32_t hsync_window;
    uint32_t vsync_window;
    uint32_t pclk_window;
    uint32_t hsync_seen;
    uint32_t vsync_seen;
    uint32_t pclk_seen;
    uint32_t hsync_level;
    uint32_t vsync_level;
    uint32_t pclk_level;
} video_capture_status_t;

extern volatile uint32_t g_video_hsync_total;
extern volatile uint32_t g_video_vsync_total;
extern volatile uint32_t g_video_pclk_total;
extern volatile uint32_t g_video_hsync_window;
extern volatile uint32_t g_video_vsync_window;
extern volatile uint32_t g_video_pclk_window;
extern volatile uint32_t g_video_hsync_seen;
extern volatile uint32_t g_video_vsync_seen;
extern volatile uint32_t g_video_pclk_seen;
extern volatile uint32_t g_video_hsync_level;
extern volatile uint32_t g_video_vsync_level;
extern volatile uint32_t g_video_pclk_level;
extern volatile uint32_t g_video_hsync_poll_total;
extern volatile uint32_t g_video_vsync_poll_total;
extern volatile uint32_t g_video_hsync_poll_window;
extern volatile uint32_t g_video_vsync_poll_window;
extern volatile uint32_t g_video_debug_marker;
extern volatile uint32_t g_video_loop_count;
extern volatile uint32_t g_video_gpiob_idr;
extern volatile uint32_t g_video_exti_imr;
extern volatile uint32_t g_video_exti_rtsr;
extern volatile uint32_t g_video_exti_ftsr;
extern volatile uint32_t g_video_exticr2;
extern volatile uint32_t g_video_exticr3;
extern volatile uint32_t g_video_pa4_total;
extern volatile uint32_t g_video_pa4_window;
extern volatile uint32_t g_video_pa4_level;
extern volatile uint32_t g_video_pa6_total;
extern volatile uint32_t g_video_pa6_window;
extern volatile uint32_t g_video_pa6_level;
extern volatile uint32_t g_video_pb8_total;
extern volatile uint32_t g_video_pb8_window;
extern volatile uint32_t g_video_pb8_level;

void VideoCapture_Init(void);
void VideoCapture_Task(void);
void VideoCapture_EXTI_Callback(uint16_t gpio_pin);
video_capture_status_t VideoCapture_GetStatus(void);

#endif
