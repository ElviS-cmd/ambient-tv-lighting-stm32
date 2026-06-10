#ifndef WS2812_H
#define WS2812_H

#include "main.h"

#define WS2812_EDGE_ZONE_COUNT 136U  /* DCMI input zones; physical LED count is 135 */

void WS2812_Init(void);
void WS2812_TaskEdgeZonesRgb(const volatile uint32_t *zone_r,
                             const volatile uint32_t *zone_g,
                             const volatile uint32_t *zone_b,
                             uint32_t zone_count);
void WS2812_Flush(void);
void WS2812_Clear(void);

#endif
