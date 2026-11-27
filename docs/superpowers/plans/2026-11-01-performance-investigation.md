# Gameplay Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Reduce redundant renderer work while preserving rendering and obtain defensible physical performance evidence.

**Architecture:** Keep existing state/key/uniform machinery, canonicalizing only effective LOD bias. Keep telemetry changes separate from the optimization so correctness and timing evidence remain attributable.

**Tech Stack:** Existing C++ runtime, native sanitizer fixtures, pinned Vita SDK/vitaGL and physical Vita.

**Spec:** `docs/superpowers/specs/2026-11-01-performance-investigation.md`

## Global Constraints

Preserve fresh user logs, saves and settings before tests. No publication. No unconditional per-draw clocks. No final forced stage/zoom/diagnostic overrides. No graphics-quality reduction as a substitute for optimization.

## Review Focus

- Signed LOD bias, mip-disabled and zero maximum LOD must retain exact effective semantics.
- Texture bindings must still invalidate for sampler-only changes.
- Direct BP writes and replayed state lists must agree.
- Hidden jobs must not reset measured presentation-completion gaps.
- Stationary benchmarks must use matching stage, camera, quality, warm state and screenshots.

## Task 1: LOD State Canonicalization

Files: `platform/vgx/src/vgx_state.cpp`, focused native fixture and runner.

- [x] Add behavioral tests for unchanged effective bias, nonzero value changes, zero/nonzero transitions and both write paths.
- [x] Run against old code and retain expected failure evidence.
- [x] Canonicalize hash contribution and dirty bits without changing sampler invalidation.
- [x] Run native sanitizer fixture and existing pipeline/key tests.

## Task 2: Basic Log Evidence

Files: `platform/src/gx/vgx_galaxy.cpp`, `tests/vgx_fixed_cap_test.cpp`.

- [x] Add failing cases for hidden-job gaps and program-only preparation output.
- [x] Add accurately labeled fixed-cap completion gaps and pending-work counts.
- [x] Run focused fixtures, source guards and full Python suite.

## Task 3: Physical Comparison and Delivery

Artifacts: private audit directory outside the source tree.

- [x] Preserve current runtime, session, saves and settings; verify no-sleep.
- [x] Capture matching stationary zoom 1.0 and 2.0 samples, then coarse workload samples.
- [x] Refresh source manifest, build runtime/link incrementally with pinned dependencies.
- [x] Review diff independently, deploy via verified temporary upload and rename, retaining rollback.
- [ ] Repeat matched samples, inspect images and compare performance without overstating gains.
- [ ] Restore user configuration and confirm runtime/settings/save readback; document unresolved risks.

## Task 4: Locate Main-Thread Cost

Files: `platform/src/port/perf.cpp`, private `phase_cpu.h`, existing phase fixture and runner.

- [ ] Extend the actual phase profiler fixture to quiet Vita availability, keeping default-off clock/TLS behavior inert.
- [ ] Retain the failing test, then reuse existing checkpoints and move profiler begin/end before quiet-build early returns.
- [ ] Run phase sanitizer tests, source guards, full suite and independent review.
- [ ] Build a private runtime-only profiling candidate and measure the matched wide view with GXV_PHASECPU=1.
- [ ] Use phase CPU deltas to choose the next optimization; never count instrumented FPS as a release-performance win.

## Task 5: Avoid Obsolete Render-Texture Resamples

Files: `platform/vgx/src/vgx_backend_gl.cpp`, focused real-GL test and runner, debug header only if needed for deterministic counters.

- [ ] Fail first on eager obsolete-work counts using full-overwrite-before-read, alongside read-first textured gradient/checkerboard references.
- [ ] Prove the first incoming-buffer access is a qualifying full-EFB ordinary copy; omit only the obsolete Resample after successful eager allocation.
- [ ] Retain the original path for reads, hoisted/partial/atlas/depth/pass/unknown commands and allocation/sanitizer uncertainty; verify high-frequency pixels, KEEP_RT and EFBSWAP behavior.
- [ ] Run the existing full auto-quality GL matrix and independent review before a private build.
- [ ] Measure controlled device size changes and restore normal adaptive configuration; report eliminated work and hitch change, not speculative FPS gains.
