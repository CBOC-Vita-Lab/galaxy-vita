# Renderer plan scratch

`PlanFrame` builds depth, colour and alpha liveness plus the next read, write,
full-overwrite and scene-leave positions for each render command. The backward
pass writes every interior element. C184 keeps the vector storage and initializes
only the seven endpoint sentinels rather than bulk-filling those interiors first.
The event scan, recurrence, tail predictor, frame validity and draw policy are
unchanged. This is CPU planning work only; no shader, texture, resolution or
rendered geometry changes.

Run `node tools/test_c184_depth_plan.mjs`. Its candidate is extracted from the
production initializer and backward loop. ASan/UBSan checks all 65,536 two-event
combinations with both tail states (131,072 cases), empty input and 3,000 varying
size plans with previous output deliberately poisoned. Every derived element is
compared against the former initialization and recurrence.

The counter checks vector bulk initialization, not all stores: the seven endpoint
stores remain. On a same-size 512-command plan the bulk initialization falls from
19,475 bytes to zero. More generally, `38 * commands` redundant interior bytes
are no longer initialized. Newly grown storage is still initialized by `resize`.

An alternating-arm, plain-vector host benchmark compares the identical production
recurrence with the former bulk initialization. Sample runs saved roughly 3-5%
of this derived-plan step. It is a synthetic host microbenchmark, not total
`PlanFrame` time, emulator GPU performance, Vita frame time or an FPS gain.

Surrounding checks: `test_vgx_scenes26.sh` (30 checks, zero failures; one existing
unused-variable warning), and regular/sanitized `test_vgx_directdisplay.sh`
(including its 20,000-picture randomized buffer test) passed. A separate read-only
review found no missing sentinel or stale-storage read.

A render-texture read-name filter was prototyped and rejected: despite fewer
comparisons, the measured absolute host opportunity was tiny and short tracking
lists regressed. None of that prototype remains in the source.
