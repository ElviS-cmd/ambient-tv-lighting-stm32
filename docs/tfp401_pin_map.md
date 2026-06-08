# TFP401 to STM32F407 Pin Map

## Current DCMI Wiring

The current firmware captures an 8-bit RGB332 DCMI stream from the TFP401,
decodes approximate RGB values, and drives the WS2812 zones from those values.

| TFP401 40-pin signal | TFP401 pin | STM32F407 pin | Firmware role |
|---|---:|---|---|
| GND | 3, 29, or 36 | GND | Common reference |
| DCLK / PCLK / DOTCLK | 30 | PA6 | DCMI_PIXCLK |
| HSYNC | 32 | PA4 | DCMI_HSYNC |
| VSYNC | 33 | PB7 | DCMI_VSYNC |
| B6 | 27 | PC6 | DCMI_D0 |
| B7 | 28 | PC7 | DCMI_D1 |
| G5 | 18 | PC8 | DCMI_D2 |
| G6 | 19 | PC9 | DCMI_D3 |
| G7 | 20 | PE4 | DCMI_D4 |
| R5 | 10 | PB6 | DCMI_D5 |
| R6 | 11 | PE5 | DCMI_D6 |
| R7 | 12 | PE6 | DCMI_D7 |

## RGB332 Color Wiring

The DCMI data bus packs the strongest color bits as RGB332:

```text
bit 7..5 = R7..R5
bit 4..2 = G7..G5
bit 1..0 = B7..B6
```

| TFP401 signal | TFP401 pin | STM32F407 DCMI pin | Captured bit |
|---|---:|---|---:|
| B6 | 27 | PC6 / DCMI_D0 | 0 |
| B7 | 28 | PC7 / DCMI_D1 | 1 |
| G5 | 18 | PC8 / DCMI_D2 | 2 |
| G6 | 19 | PC9 / DCMI_D3 | 3 |
| G7 | 20 | PE4 / DCMI_D4 | 4 |
| R5 | 10 | PB6 / DCMI_D5 | 5 |
| R6 | 11 | PE5 / DCMI_D6 | 6 |
| R7 | 12 | PE6 / DCMI_D7 | 7 |

Watch `g_dcmi_average_r`, `g_dcmi_average_g`, and `g_dcmi_average_b` while
showing full-screen red, green, blue, white, and black.

## Notes

- TFP401 is powered by USB during bring-up.
- STM32 is powered and flashed through ST-LINK.
- Grounds must be connected together.
- PA4 and PA6 were tested and are usable for DCMI on this STM32F407 Discovery board.
- The RGB332 mapping is a first color milestone. If color banding is too visible,
  the upgrade path is a wider DCMI mode such as RGB444.
- The processing pipeline maintains 136 logical perimeter zones and maps them
  to 135 physical LEDs. The left edge is resampled from 25 logical zones to
  24 installed LEDs.
- The WS2812 data signal is driven through an SN74AHCT125 level shifter. See
  [hardware_wiring.md](hardware_wiring.md) for the output and power wiring.
