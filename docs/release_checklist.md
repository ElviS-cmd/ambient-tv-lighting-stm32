# Release Checklist

## Automated checks

- [ ] `make check` passes the complete build matrix without warnings.
- [ ] GitHub Actions passes.
- [ ] `git diff --check` reports no whitespace errors.
- [ ] Build size remains within STM32F407 flash and RAM limits.

## Hardware smoke test

- [ ] The firmware flashes and boots without entering a fault handler.
- [ ] The TFP401 locks to the intended source mode.
- [ ] Top, right, bottom, and left LED edges follow the corresponding image.
- [ ] A full black frame clears the strip without stale color.
- [ ] Scene cuts update without visible multi-frame lag.
- [ ] Letterbox and pillarbox content move sampling onto the active picture.
- [ ] Disconnecting and reconnecting the source recovers video capture.
- [ ] WS2812 output recovers after a rapid sequence of frame updates.
- [ ] MCU and LED power remain stable during a bright full-frame test.

## Release

- [ ] Update the README status if hardware behavior differs from documentation.
- [ ] Mark the draft pull request ready for review.
- [ ] Merge the pull request into `main`.
- [ ] Tag the merged commit as `v1.0.0`.
- [ ] Record the tested source, display mode, LED count, and power supply.
