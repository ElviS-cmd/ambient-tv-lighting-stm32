#ifndef DCMI_CAPTURE_H
#define DCMI_CAPTURE_H

#include "main.h"

/* Video source mode. Selects the expected frame geometry (dcmi_capture.c)
 * and the DCMI sync polarities (main.c) together, so geometry and polarity
 * can never disagree:
 *
 * 0 = CEA 1280x720 via the splitter's EDID. Positive sync pulses, so the
 *     blanking level on the sync pins is HIGH/HIGH. The historical config.
 * 1 = stock TFP401 EDID, Mac direct: 800x480 @ 65.7 Hz, 32 MHz. Negative
 *     sync pulses -> LOW/LOW. Bench-validated 2026-06: ~4700 captures,
 *     100% full, zero sync timeouts.
 * 2 = AMBILIGHT EDID (tools/edid): 1280x720 @ 49.94 Hz CVT-RB, 53 MHz.
 *     HSync positive / VSync negative -> HIGH/LOW. Use after programming
 *     the TFP401 EEPROM (direct or through the EDID-clone dongle).
 * 3 = splitter-compatible EDID: standard 720x480p @ 59.94/60 Hz, 27 MHz.
 *     Both sync pulses are negative -> LOW/LOW.
 */
#define DCMI_SOURCE_MODE 0U

/* The TFP401 updates its parallel data around one ODCK edge. Capture on the
 * opposite edge so DCMI samples in the middle of the stable data window.
 * Keep this switch explicit while validating the installed breakout.
 */
#define DCMI_PCLK_CAPTURE_FALLING 0U

#define DCMI_LED_ZONE_COUNT 136U

extern volatile uint32_t g_dcmi_led_zone_r[DCMI_LED_ZONE_COUNT];
extern volatile uint32_t g_dcmi_led_zone_g[DCMI_LED_ZONE_COUNT];
extern volatile uint32_t g_dcmi_led_zone_b[DCMI_LED_ZONE_COUNT];

void DCMI_Capture_Init(void);
void DCMI_Capture_Task(void);
uint32_t DCMI_Capture_ConsumeLedUpdate(void);

#endif
