#ifndef WS2812_H
#define WS2812_H

#include "main.h"

void WS2812_Init(void);
void WS2812_Task(uint32_t brightness_percent);
void WS2812_TaskZones(uint32_t zone0_percent,
                      uint32_t zone1_percent,
                      uint32_t zone2_percent,
                      uint32_t zone3_percent);
void WS2812_Clear(void);

#endif
