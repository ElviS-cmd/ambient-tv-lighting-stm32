# TFP401 Bring-Up Test Plan

## Stage 1: Firmware Build

1. Build the STM32CubeIDE Debug configuration.
2. Flash the STM32F407 board.
3. Confirm the blue onboard LED blinks.

If blue does not blink, the firmware is not running.

## Stage 2: VSYNC Only

Wiring:

```text
TFP401 GND -> STM32 GND
TFP401 pin 33 VSYNC -> STM32 PB7
```

Expected:

```text
Green LED blinks with heartbeat
g_video_vsync_total increases
```

If VSYNC is not detected:

```text
Check HDMI source output
Check TFP401 USB power
Check FPC orientation
Check pin 33 to PB7 continuity
```

## Stage 3: HSYNC

Wiring:

```text
TFP401 pin 32 HSYNC -> STM32 PB8
```

Expected:

```text
Orange LED blinks with heartbeat
g_video_hsync_total increases faster than g_video_vsync_total
```

If VSYNC works but HSYNC does not:

```text
Re-check pin 32 orientation
Check PB8 header location
Probe pin 32 with logic analyzer if available
```

## Stage 4: PCLK Activity

Wiring:

```text
TFP401 pin 30 DCLK/PCLK -> STM32 PE6
```

Expected:

```text
Red LED may blink if rough polling catches activity
g_video_pclk_total may increase
```

Important: PCLK is high frequency, often tens of MHz. A zero count here does not prove PCLK is missing. Final firmware should use DCMI or timer hardware for pixel clock capture.

## Stage 5: Logic Analyzer Sanity

Before probing video signals, prove the analyzer works on a simple STM32 GPIO square wave.

Recommended analyzer settings for VSYNC/HSYNC:

```text
Mode: digital
Protocol decoder: none
Sample rate: 1 MHz
Duration: 1 second
Trigger: none / auto
```

Recommended wiring:

```text
Analyzer GND -> STM32/TFP401 common GND
CH0 -> PB7 VSYNC
CH1 -> PB8 HSYNC
CH2 -> PE6 PCLK
```

PCLK may alias on a 24 MHz analyzer. VSYNC and HSYNC should be clear.

## Stage 6: Next Firmware Step

After VSYNC and HSYNC are reliable:

1. Move timing capture from generic EXTI bring-up into DCMI configuration.
2. Add reduced RGB bus wiring.
3. Capture sparse edge pixels only.
4. Feed averaged edge colors into LED-zone output.
