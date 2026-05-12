# No-Hardware Work Plan

Use this checklist when the STM32/TFP401 circuit is not available.

## Firmware Cleanup

- Keep the timing bring-up firmware stable.
- Do not change pin mappings without a documented reason.
- Keep PB7 for VSYNC and PB8 for HSYNC.
- Leave PCLK as a known limitation until tested with hardware.

## Documentation

- Keep the README current.
- Record confirmed wiring and measured values after each bench session.
- Add screenshots of CubeIDE expressions when a milestone is confirmed.
- Record FPC adapter continuity results, especially pin 1 orientation and pins 30, 32, 33, and 34.

## Design Decisions To Lock

- Final video capture peripheral: STM32 DCMI.
- Final pixel transfer method: DMA.
- First RGB capture bus: reduced bits, likely R7/G7/B7.
- LED output: WS2812B with level shifter recommended.
- LED power: external 5 V supply, common ground with STM32.

## Next Bench Session Checklist

1. Confirm common ground.
2. Confirm FPC continuity:
   ```text
   decoder pin 33 -> PB7 jumper path
   decoder pin 32 -> PB8 jumper path
   decoder pin 30 -> PE6 jumper path
   decoder pin 34 -> DE candidate path
   ```
3. Flash current firmware.
4. Confirm:
   ```text
   g_video_vsync_window ~= 60
   g_video_hsync_window much greater than VSYNC
   ```
5. Probe PCLK with logic analyzer.
6. Decide DCMI pin map.

## Portfolio Notes

Capture these artifacts for the final project page:

- System block diagram
- Wiring diagram
- TFP401 timing screenshots
- CubeIDE expression screenshots showing VSYNC/HSYNC
- Logic analyzer captures for VSYNC/HSYNC/PCLK
- Explanation of why DCMI/DMA is required
- Short demo video once LEDs respond to screen color

