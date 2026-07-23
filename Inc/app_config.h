#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * Release configuration
 *
 * Keep hardware selection and optional bench instrumentation in one place.
 * Production builds leave diagnostics disabled to minimize CPU and RAM use.
 */

/* Video input: 0 = 720p splitter, 1 = stock TFP401 800x480,
 * 2 = custom 720p50 EDID, 3 = 480p splitter. */
#define APP_VIDEO_SOURCE_MODE 0U

/* Sample on the edge opposite the TFP401 data transition. */
#define APP_DCMI_PCLK_FALLING 0U

/* Standalone bench modes. These replace normal application execution. */
#define APP_ENABLE_TFP401_SIGNAL_DIAGNOSTIC 0U
#define APP_ENABLE_LED_SELF_TEST             0U

/* Passive instrumentation. Enable only for a focused debug session. */
#define APP_ENABLE_TFP401_SIGNAL_MONITOR 0U
#define APP_ENABLE_DCMI_DIAGNOSTICS      0U
#define APP_ENABLE_COLOR_DIAGNOSTICS     0U
#define APP_ENABLE_WS2812_DIAGNOSTICS    0U

#endif
