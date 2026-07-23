# Firmware Architecture

## Execution model

The application uses a cooperative foreground loop with interrupt-driven DMA.
DCMI callbacks publish completed capture buffers, and TIM2 DMA callbacks mark
the LED transmitter idle. Interrupt handlers do not perform image processing
or encode LED frames.

## Modules

- `main.c` owns generated peripheral initialization and application startup.
- `dcmi_capture.c` owns video synchronization, crop scheduling, RGB332 zone
  accumulation, temporal filtering, and black-border detection.
- `ws2812.c` owns physical LED mapping, output glide, PWM encoding, and TIM2
  DMA transmission.
- `app_config.h` contains release-facing hardware and diagnostics switches.

## Data flow

1. DCMI captures one perimeter crop into a DMA buffer.
2. The capture task maps samples into 136 logical edge zones.
3. A completed zone update is published to the foreground loop.
4. The WS2812 module maps 136 logical zones onto 135 physical LEDs.
5. The glide task smooths output transitions and encodes the next DMA frame.
6. TIM2 Channel 2 transmits the GRB waveform at 800 kHz.

## State ownership

Peripheral handles remain global because STM32Cube-generated initialization
and interrupt code share them. Application state is module-private unless it
is part of a declared header interface. `volatile` is reserved for state that
can change in an interrupt or that is intentionally retained for a debugger
watch; optional telemetry is compiled out in the release configuration.

## Change policy

Keep CubeMX-generated regions intact. Add application code only in `USER CODE`
sections of generated files. Validate changes with a clean `Debug` build and a
hardware smoke test covering video lock, all four edges, scene changes, black
frames, and WS2812 output recovery.
