#ifndef DCMI_CAPTURE_H
#define DCMI_CAPTURE_H

#include "main.h"

#define DCMI_LED_ZONE_COUNT 136U

extern volatile uint32_t g_dcmi_led_zone_r[DCMI_LED_ZONE_COUNT];
extern volatile uint32_t g_dcmi_led_zone_g[DCMI_LED_ZONE_COUNT];
extern volatile uint32_t g_dcmi_led_zone_b[DCMI_LED_ZONE_COUNT];

void DCMI_Capture_Init(void);
void DCMI_Capture_Task(void);
uint32_t DCMI_Capture_ConsumeLedUpdate(void);

#endif
