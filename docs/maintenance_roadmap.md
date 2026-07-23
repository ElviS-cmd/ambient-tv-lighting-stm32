# Maintenance Roadmap

This roadmap begins after the hardware-validated `v1.0.0` release. Refactors
should preserve externally observed timing and land in independently testable
pull requests.

## Priority 1: Capture task decomposition

`DCMI_Capture_Task()` is the largest control-flow risk in the application.
Separate its state transitions into small handlers for ready, sync-wait,
running, completion, timeout, and error states. Keep one state transition per
foreground-loop call and preserve the existing ISR/foreground ownership.

Success criteria:

- the full build matrix remains warning-free;
- release flash and RAM do not regress unexpectedly;
- capture success, timeout, and restart rates match the `v1.0.0` baseline;
- all four edges retain their validated update latency.

## Priority 2: Capture processing modules

After the state machine is stable, extract pure processing code in this order:

1. RGB332 lookup and weighted accumulation;
2. zone geometry and source-to-zone mapping;
3. temporal smoothing and missed-zone handling;
4. black-border detection.

Functions that do not touch registers, HAL handles, DMA buffers, or time should
be host-testable. Hardware-facing capture scheduling should remain in one
module.

## Priority 3: Diagnostics

Replace individual debugger globals with grouped snapshots:

- capture transport counters;
- synchronization timing;
- zone quality;
- WS2812 transport state.

Keep diagnostic structures module-private and expose read-only snapshot
functions only when a maintenance tool needs them. `volatile` should remain
limited to interrupt-shared fields and deliberate debugger watchpoints.

## Priority 4: Dependency footprint

The repository contains a broad CMSIS distribution. After confirming CubeMX
regeneration and CI reproducibility, retain only the device headers and
libraries required by this firmware. Perform this as a standalone repository
cleanup with no application-code changes.

## Deferred work

Do not combine these architectural changes with tuning capture geometry,
smoothing constants, LED timing, or hardware wiring. Behavioral tuning requires
a separate bench result and an explicit before/after measurement.
