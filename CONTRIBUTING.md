# Contributing

## Development setup

Install the Arm GNU toolchain so `arm-none-eabi-gcc` is available, then run:

```sh
make check
```

This compiles the release configuration, diagnostics, every supported video
source, and all standalone bench modes. A change is not ready for review if any
configuration fails or emits an application warning.

STM32CubeIDE remains the supported path for CubeMX regeneration, interactive
debugging, flashing, and peripheral inspection.

## Change boundaries

- Keep generated edits inside `USER CODE` sections whenever possible.
- Do not combine hardware tuning with structural refactoring.
- Preserve interrupt and foreground ownership of shared state.
- Add `volatile` only for interrupt-shared state or an intentional debugger
  watchpoint.
- Document the measurement behind timing, geometry, smoothing, or power
  changes.
- Never commit build outputs, launch files, credentials, or local IDE state.

## Pull requests

Keep pull requests focused and describe:

- the problem or maintenance goal;
- the behavior that changed;
- automated checks performed;
- hardware tests performed or still required;
- flash and RAM impact when firmware behavior changes.

Hardware-dependent pull requests should remain drafts until the release
checklist has been completed on the target board.
