# DCMI Capture Plan

## Goal

Move from simple timing-signal detection to real video sampling using the STM32F407 DCMI peripheral and DMA.

The current EXTI firmware proves that VSYNC and HSYNC are present. It is not intended to sample pixels. Pixel sampling must be done by hardware because ODCK/PCLK can be tens of MHz.

## Final Signal Flow

```text
HDMI source
  -> TFP401 HDMI/DVI decoder
  -> 40-pin TTL RGB/timing bus
  -> STM32F407 DCMI + DMA
  -> edge-zone color averaging
  -> WS2812B LED output
```

## Signals Needed

| TFP401 signal | Purpose |
|---|---|
| ODCK / DCLK / PCLK | Pixel sampling clock |
| VSYNC | Frame boundary |
| HSYNC | Line boundary |
| DE | Active pixel/data-valid window |
| RGB data bits | Pixel color data |

## First Capture Milestone

Do not wire all 24 RGB bits first. Start with a reduced bus:

```text
R7
G7
B7
VSYNC
HSYNC
DE
PCLK
GND
```

This proves that the STM32 can sample video-timed data without the wiring burden of full 24-bit RGB.

## Why DCMI

GPIO polling is too slow for pixel capture. DCMI samples parallel input data on the external pixel clock and can transfer samples into memory with DMA.

The CPU should not read each pixel manually. The expected model is:

```text
PCLK edge arrives
  -> DCMI samples data pins
  -> DMA writes captured data into memory
  -> CPU processes completed buffer
```

## STM32F407 DCMI Considerations

The STM32F407 DCMI supports external sync capture with:

```text
DCMI_PIXCLK
DCMI_HSYNC
DCMI_VSYNC
DCMI_D0..D13
```

The exact pin assignment must be chosen around STM32F407G-DISC1 board conflicts. Avoid Discovery pins connected to onboard audio, MEMS, USB, LEDs, and buttons.

The key DCMI timing pins are fixed by the STM32 alternate-function map:

```text
DCMI_HSYNC  -> PA4
DCMI_PIXCLK -> PA6
DCMI_VSYNC  -> PB7
```

The current bring-up wiring intentionally uses PB8 for HSYNC and PE6 for rough PCLK detection, because those pins were easier to validate during timing debug. The DCMI phase must retest PA4 and PA6.

## Proposed Firmware Modules

```text
video_capture.c/.h
  Current timing bring-up.

dcmi_capture.c/.h
  Placeholder now; later DCMI and DMA configuration.

video_zones.c/.h
  Edge-region accumulation and color averaging.

ws2812.c/.h
  LED-strip output driver.
```

## Capture Strategy

Avoid full-frame storage. The STM32F407 does not need to store the whole image for ambient lighting.

Instead:

```text
sample selected edge pixels
accumulate R/G/B sums per zone
compute average colors at end of frame
update LED strip
```

Target zones:

```text
top: 43 LEDs
right: 25 LEDs
bottom: 43 LEDs
left: 25 LEDs
```

The LED strip has 300 physical LEDs, but the first ambient prototype can drive only the perimeter zones while leaving unused LEDs off.

## Bench Test Sequence

1. Confirm VSYNC on PB7.
2. Confirm HSYNC on PB8.
3. Confirm ODCK/PCLK on the selected DCMI pixel-clock pin.
4. Retest HSYNC on PA4 for DCMI compatibility.
5. Retest ODCK/PCLK on PA6 for DCMI compatibility.
6. Confirm DE toggles.
7. Wire one RGB bit and confirm DCMI/DMA buffer changes with screen content.
8. Wire R7/G7/B7 and show coarse color detection.
9. Add zone accumulation.
10. Reconnect WS2812B output.

Current progress through this sequence:

```text
Steps 1 and 2 are confirmed.
Step 3 has rough activity confirmation only.
Step 4 is promising: PA4 saw high-rate HSYNC-like activity.
Step 5 is partially checked: PA6 saw activity, but needs DCMI/timer validation.
```

The next firmware milestone is not another GPIO counter. It is a small DCMI/DMA snapshot test after CubeMX adds the DCMI peripheral and driver files.

## Open Questions

- Exact DCMI pin map for the STM32F407G-DISC1 after avoiding onboard peripheral conflicts.
- Whether to use external sync mode with HSYNC/VSYNC or embedded/data-enable style gating.
- Whether the source resolution should be forced to 480p, 720p, or another low pixel-clock mode.
- Whether LED output should stay bit-banged initially or move to timer/DMA later.
