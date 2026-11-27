# Slip Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Track checkboxes below.

**Goal:** Attest and test the existing bounded-slowdown policy without changing gameplay or graphics.
**Architecture:** Opt-in startup/state-change basic logs on the existing game-thread slip path; same-binary physical control.
**Tech Stack:** C++17/20, existing host sanitizer fixtures, pinned Clang18 ThinLTO/Vita SDK.
**Spec:** `docs/superpowers/specs/2026-11-01-slip-control.md`

## Global Constraints

Exact literal GXV_SLIP_TRACE1; all new code inside __vita__&&PORT_LOGGING; no new clocks/counters/cross-thread reads; preserve old decisions/callback order/errno; no source changes outside assigned scope, manifests only after freeze; no commit/push/publication.

## Review Focus

Missing/malformed opt-in is inert; SLIP0 still attests disabled config; pause/gap leave is not a new engagement; logging cannot modify decision state or errno; full-module baseline assembly gates remain unchanged.

### Task1: Targeted Trace With Actual-Source Tests

Files: modify `platform/src/port/perf.cpp`; create `tests/slip_trace.cpp`, `tools/prepare_slip_trace_fixture.cjs`, `tools/test_slip_trace.sh`.
Interfaces: existing SlipMax()/SlipWorkDone()/port_frameskip_turn and port::pace::Slip only; no new public API.

- [x] Write behavioral tests for the spec using extracted production methods and actual Slip.
- [x] Run and retain a real behavioral RED before code changes.
- [x] Implement only the exact opt-in config/state rows and errno-preserving summary emission.
- [x] Run native/Linux ASan+UBSan, literal gates/errno/pacing equivalence, and existing phase43/four assembly comparisons; freeze files/hashes.
- [x] Independent spec/code review, resolve findings tests-first and rerun affected gates. No commit.

### Task2: Verified Runtime And Physical Control

Files: regenerated checksums/source manifest only after Task1 freeze; private artifacts/reports under `performance-phase/`.
Consumes: frozen source/tests; produces private runtime hash, activation read-back, two accepted captures/log appends.

- [ ] Root reruns focused tests/full Python suite, freezes source archive/metadata, rebuilds pinned runtime/SELF and preserves rollback.
- [ ] Activate only after full staged/active/rollback hash checks.
- [ ] Run the specified same-binary SLIP1 then0 controls; independently parse, validate exact one-variable environments and engagement brackets before interpreting differences.
- [ ] Restore normal config, fresh positive foreground capture, known save/config/pack/warm/SFO integrity. Update STATUS and report honest result; keep goal active unless sustained performance objective truly passes.
