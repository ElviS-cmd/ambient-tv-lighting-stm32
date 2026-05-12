# TFP401 to STM32F407 Pin Map

## Current Bring-Up Wiring

The current firmware only tests HDMI decoder timing signals.

| TFP401 40-pin signal | TFP401 pin | STM32F407 pin | Firmware role |
|---|---:|---|---|
| GND | 3, 29, or 36 | GND | Common reference |
| DCLK / PCLK / DOTCLK | 30 | PE6 | Pixel clock activity check |
| HSYNC | 32 | PB8 | EXTI interrupt counter |
| VSYNC | 33 | PB7 | EXTI interrupt counter |

Do not connect RGB data yet.

## Onboard LED Status

| STM32F407 Discovery LED | Pin | Meaning |
|---|---|---|
| Green | PD12 | VSYNC was detected in the last 500 ms window |
| Orange | PD13 | HSYNC was detected in the last 500 ms window |
| Red | PD14 | PCLK activity was detected by rough polling |
| Blue | PD15 | Firmware heartbeat |

## CubeIDE Watch Variables

Add these to CubeIDE Expressions:

```c
g_video_vsync_total
g_video_hsync_total
g_video_pclk_total
g_video_vsync_window
g_video_hsync_window
g_video_pclk_window
```

Interpretation:

```text
*_total  = cumulative count since reset
*_window = count observed during the last 500 ms status window
```

Expected first successful state:

```text
g_video_vsync_total increases
g_video_hsync_total increases
g_video_pclk_total may be unreliable until DCMI/timer capture is added
```

## Notes

- TFP401 is powered by USB during bring-up.
- STM32 is powered/debugged through ST-LINK/USB.
- Grounds must be connected together.
- PCLK is too fast for the current polling method to measure accurately. The current PCLK check only proves rough activity.
- PA4 and PA6 are avoided on the STM32F407 Discovery board because they are shared with onboard audio/MEMS circuitry.
