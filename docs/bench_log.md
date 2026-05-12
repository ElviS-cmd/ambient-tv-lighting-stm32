# Bench Log

## 2026-05-11 Timing Bring-Up

Hardware path:

```text
MacBook HDMI
  -> Adafruit TFP401 HDMI/DVI decoder
  -> 40-pin FPC/breakout
  -> STM32F407G-DISC1
```

Confirmed FPC mapping:

```text
decoder pin 1 -> breakout pin 1
```

So the adapter path is straight, not reversed.

## Confirmed Timing Signals

| TFP401 pin | Signal | STM32 pin | Result |
|---:|---|---|---|
| 33 | VSYNC | PB7 | Frame-rate count around 60/window |
| 32 | HSYNC | PB8 | High-rate count around 34k/window |
| 30 | DCLK/ODCK/PCLK | PE6 | High-speed activity seen by rough polling |

## DCMI Candidate Retest

The DCMI peripheral requires:

```text
DCMI_VSYNC  -> PB7
DCMI_HSYNC  -> PA4
DCMI_PIXCLK -> PA6
```

Parallel test result:

| Candidate | STM32 pin | Result |
|---|---|---|
| DCMI_HSYNC | PA4 | High-rate activity seen, about 57k/window |
| DCMI_PIXCLK | PA6 | Activity seen, but rough polling is not reliable |

During the PA4 test, `PD4 Audio_RST` was held low in firmware to keep the onboard CS43L22 codec in reset.

## Current Interpretation

- STM32 is alive and firmware is running.
- TFP401 VSYNC and HSYNC are confirmed.
- TFP401 PCLK/ODCK activity is present, but exact frequency has not been measured.
- PA4 is promising enough for DCMI bring-up.
- PA6 is not disproven, but PCLK must be validated by DCMI or hardware counter/scope.

## Next Bench Target

Enable DCMI/DMA scaffolding and attempt a small snapshot capture using the DCMI timing pins:

```text
TFP401 pin 33 -> PB7 / DCMI_VSYNC
TFP401 pin 32 -> PA4 / DCMI_HSYNC
TFP401 pin 30 -> PA6 / DCMI_PIXCLK
```

