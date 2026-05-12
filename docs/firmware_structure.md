# Firmware Structure

## Current Modules

```text
Src/main.c
Inc/video_capture.h
Src/video_capture.c
Core/Src/stm32f4xx_it.c
```

## main.c

Responsibilities:

```text
HAL startup
system clock setup
video capture module init
main polling loop
error handler
```

It intentionally does not contain LED-strip patterns or video timing logic.

## video_capture.c

Responsibilities:

```text
configure TFP401 timing input pins
count VSYNC and HSYNC via EXTI interrupts
rough-check PCLK activity
publish CubeIDE watch variables
drive onboard status LEDs
```

## stm32f4xx_it.c

Responsibilities:

```text
route EXTI9_5 interrupt to HAL for PB7 / VSYNC
route EXTI9_5 interrupt to HAL for PB8 / HSYNC
```

The HAL callback is implemented in `main.c` and forwards to:

```c
VideoCapture_EXTI_Callback(GPIO_Pin);
```

## Future Modules

Planned:

```text
ws2812.c / ws2812.h
led_zones.c / led_zones.h
tfp401_rgb_capture.c / tfp401_rgb_capture.h
```

The final architecture should keep video capture, color processing, and LED output separate.
