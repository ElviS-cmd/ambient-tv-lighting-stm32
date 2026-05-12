# Ambient TV Lighting System

Bare-metal STM32F407 firmware for an Ambilight-style TV backlight. The project uses an Adafruit TFP401 HDMI/DVI decoder to convert HDMI into TTL video timing/data signals, captures video bytes with the STM32F407 DCMI + DMA peripheral, computes brightness samples, and drives a WS2812B LED strip.

## Current Status

Current stage: working proof-of-concept.

Confirmed on hardware:

- HDMI video from a Mac display reaches the TFP401 decoder.
- TFP401 timing signals are readable by the STM32:
  - VSYNC on PB7
  - HSYNC on PA4
  - PIXCLK/ODCK on PA6
- DCMI + DMA captures an 8-bit video data stream into RAM.
- Captured values respond to screen content:
  - white screen produces high brightness values
  - black screen produces low brightness values
- WS2812B output on PA1 responds to captured screen brightness.
- The 300 LED strip is divided into four physical perimeter zones.

Working signal flow:

```text
Mac HDMI output
-> TFP401 HDMI/DVI decoder
-> STM32F407 DCMI + DMA
-> brightness analysis
-> WS2812B LED zones
```

## Hardware Stack

- STM32F407G-DISC1 Discovery board
- Adafruit TFP401 HDMI/DVI decoder to 40-pin TTL display
- 40-pin FPC extension board and 40-pin breakout
- WS2812B LED strip, 300 LEDs
- External 5 V LED power supply
- HDMI splitter / second-display HDMI path
- Logic analyzer for timing verification

## Current Wiring

### Timing Signals

| TFP401 signal | TFP401 pin | STM32F407 pin | STM32 function |
|---|---:|---|---|
| VSYNC | 33 | PB7 | DCMI_VSYNC |
| HSYNC | 32 | PA4 | DCMI_HSYNC |
| PIXCLK / ODCK | 30 | PA6 | DCMI_PIXCLK |
| GND | 3, 29, or 36 | GND | Common reference |

### 8-Bit Video Data

Current data bus uses the TFP401 green channel as an 8-bit brightness source.

| TFP401 signal | TFP401 pin | STM32F407 pin | STM32 function |
|---|---:|---|---|
| G0 | 13 | PC6 | DCMI_D0 |
| G1 | 14 | PC7 | DCMI_D1 |
| G2 | 15 | PC8 | DCMI_D2 |
| G3 | 16 | PC9 | DCMI_D3 |
| G4 | 17 | PE4 | DCMI_D4 |
| G5 | 18 | PB6 | DCMI_D5 |
| G6 | 19 | PE5 | DCMI_D6 |
| G7 | 20 | PE6 | DCMI_D7 |

### LED Strip

| STM32F407 pin | LED strip signal |
|---|---|
| PA1 | WS2812B DIN |
| GND | LED strip GND |

The LED strip is powered from an external 5 V supply. STM32 ground and LED ground must be common.

## LED Zone Layout

The mounted TV perimeter uses the last 136 LEDs of the 300 LED strip. Earlier LEDs are kept off.

Firmware uses zero-based LED indices:

| Physical section | Human LED numbers | Firmware indices | Count |
|---|---:|---:|---:|
| Bottom | 165-207 | 164-206 | 43 |
| Left | 208-232 | 207-231 | 25 |
| Top | 233-275 | 232-274 | 43 |
| Right | 276-300 | 275-299 | 25 |

Current zone assignment:

| Captured zone | LED section |
|---|---|
| Zone 0 | Right |
| Zone 1 | Top |
| Zone 2 | Left |
| Zone 3 | Bottom |

At this stage each zone is grayscale brightness, not full RGB color yet.

## Firmware Architecture

Main modules:

- `Src/main.c`: system initialization and task orchestration.
- `Src/dcmi_capture.c`: DCMI/DMA capture, byte analysis, zone brightness extraction.
- `Src/ws2812.c`: WS2812B timing and LED-zone output.
- `Src/video_capture.c`: earlier timing-debug path for EXTI/poll-based signal checks.

Current DCMI mode:

- continuous capture
- crop disabled
- small DMA buffer
- 8-bit DCMI data mode
- green-channel byte stream used as brightness input

Snapshot + crop mode was tested, but continuous/no-crop mode was more reliable for the current proof-of-concept.

## Debug Variables

Useful STM32CubeIDE Expressions for the current DCMI path:

```c
g_dcmi_frame_count
g_dcmi_error_count
g_dcmi_start_status
g_dcmi_average_byte
g_dcmi_brightness_percent
g_dcmi_zone0_brightness_percent
g_dcmi_zone1_brightness_percent
g_dcmi_zone2_brightness_percent
g_dcmi_zone3_brightness_percent
g_dcmi_buffer_checksum
```

Expected behavior:

- White HDMI display: high `g_dcmi_average_byte`, high `g_dcmi_brightness_percent`.
- Black HDMI display: low `g_dcmi_average_byte`, low `g_dcmi_brightness_percent`.
- `g_dcmi_frame_count` should increase.
- `g_dcmi_error_count` should stay at 0 during stable capture.

## Next Goals

### 1. Move From Brightness to Color

Current capture uses only `G0-G7`, so it is effectively brightness/green-channel capture. The next hardware test is a reduced RGB bus using the most significant bits from each color channel.

Proposed 8-bit packed RGB test:

| DCMI bit | Suggested source |
|---|---|
| D0 | B6 |
| D1 | B7 |
| D2 | G5 |
| D3 | G6 |
| D4 | G7 |
| D5 | R5 |
| D6 | R6 |
| D7 | R7 |

Firmware would unpack this into approximate RGB values for each captured sample.

### 2. Improve Zone Sampling

Current zones are simple chunks of the captured byte stream. The final version should sample real screen regions:

- top edge
- right edge
- bottom edge
- left edge

Then each region should drive the matching physical LED section.

### 3. Stabilize Final Capture Strategy

The reliable proof-of-concept currently uses continuous DCMI capture with crop disabled. Later firmware should evaluate whether to:

- keep continuous capture and use small repeated buffers, or
- reintroduce crop/windowing after sync polarity and display timing are fully understood.

### 4. Power and Signal Integrity

For a final build:

- add a 330-470 ohm resistor on the WS2812B data line
- add a large capacitor across LED 5 V/GND near strip input
- use a 3.3 V to 5 V level shifter for LED DIN if needed
- use a power supply sized for the intended brightness

## Documentation

- `docs/tfp401_pin_map.md`: TFP401 pin notes.
- `docs/tfp401_test_plan.md`: Timing bring-up sequence.
- `docs/dcmi_pin_map.md`: DCMI pin-map notes and Discovery-board conflicts.
- `docs/dcmi_capture_plan.md`: Capture plan and future work.
- `docs/firmware_structure.md`: Firmware module responsibilities.
- `docs/bench_log.md`: Bench observations.
- `docs/no_hardware_work_plan.md`: Tasks that can be done away from the circuit.
