# STM32F407 DCMI Pin Map

This document captures the likely DCMI wiring target for the next project stage. The current firmware still uses EXTI timing bring-up on PB7/PB8 and does not enable DCMI yet.

## Required Timing Pins

| DCMI signal | STM32F407 pin | Notes |
|---|---|---|
| DCMI_VSYNC | PB7 | Already proven with TFP401 pin 33 |
| DCMI_HSYNC | PA4 | Retested in parallel with PB8; high-rate activity observed while audio reset is held low |
| DCMI_PIXCLK | PA6 | Retested in parallel with PE6; activity observed, but GPIO polling is not a real frequency check |

The current bring-up firmware uses PB8 for HSYNC and PE6 for rough PCLK detection because those are easier bench-test pins. DCMI itself has fixed alternate-function pins, so the DCMI stage must revisit PA4 and PA6.

Latest bench result:

```text
PB7 VSYNC reference: about 80 edges/window
PB8 HSYNC reference: about 34k edges/window
PE6 PCLK rough poll: about 66k transitions/window
PA4 HSYNC candidate: about 57k edges/window
PA6 PCLK candidate: activity observed, rough poll not comparable
```

The PA4 result is good enough to continue toward DCMI bring-up. PA6 still needs hardware capture through DCMI or a proper timer/logic-analyzer measurement.

## Candidate 8-Bit Data Bus

One valid 8-bit DCMI data arrangement to investigate:

| DCMI data | STM32F407 pin | Discovery-board concern |
|---|---|---|
| D0 | PC6 | Check header access |
| D1 | PC7 | Shared with I2S3_MCK label in CubeMX board defaults |
| D2 | PC8 | Check header access |
| D3 | PC9 | Check header access |
| D4 | PC11 | Check header access |
| D5 | PB6 | Shared with I2C1_SCL/audio codec default |
| D6 | PE5 | Check header access |
| D7 | PE6 | Also used as current PCLK test pin; DCMI data use would conflict |

This bus is not final. It is a starting point for CubeMX validation and Discovery-board conflict review.

## Reduced RGB First Test

The first meaningful DCMI test should avoid full 24-bit video wiring. A practical reduced test is:

```text
VSYNC
HSYNC
PCLK
DE
R7
G7
B7
GND
```

However, DCMI captures a parallel data bus, so the reduced test still needs a DCMI-compatible data-pin arrangement. Unused data bits may need defined levels or a deliberate wiring plan.

## CubeMX/HAL Work Needed

Before enabling DCMI code:

1. Enable the DCMI peripheral in CubeMX.
2. Enable DMA for DCMI.
3. Enable `HAL_DCMI_MODULE_ENABLED` in `stm32f4xx_hal_conf.h`.
4. Add the HAL DCMI driver source to the build.
5. Configure DCMI GPIO pins as `GPIO_AF13_DCMI`.
6. Confirm the generated pin map does not silently steal SWD, USB, or required board resources.

Current project note:

```text
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dcmi.c
```

is not currently present in the copied HAL driver sources, so DCMI cannot be enabled cleanly until CubeMX adds the DCMI driver files or the source is copied from the STM32CubeF4 firmware package.

## Bench Re-Test Before DCMI

When hardware is available again:

1. Confirm TFP401 pin 33 to PB7 still gives VSYNC.
2. Confirm TFP401 pin 32 to PB8 still gives HSYNC.
3. Probe TFP401 pin 30 ODCK/PCLK directly.
4. Keep HSYNC wired to PA4 and verify it still counts.
5. Keep PCLK wired to PA6 and verify it is electrically present.
6. Enable DCMI in CubeMX and generate the HAL/DMA scaffolding.
7. Only then replace the EXTI timing bring-up with a DCMI snapshot test.
