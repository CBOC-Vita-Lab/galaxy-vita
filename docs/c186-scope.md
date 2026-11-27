# C186 release boundary

> **On integ-3.1 (c186merge, 17 Nov).** This page describes the C186 / C186.1 / C186.2 VPKs, which were built on
> `e44c45d8` before fix31 and c185merge. Merged into integ-3.1, the owner's later decisions stay in force where they
> differ from this boundary: C185's Honeyhive plant reveal (`GXV_HONEY_PLANT_REVEAL`) and the Observatory mask warm-up
> (`GXV_C185_ASTRO_MASK_WARM`) stay ON; the D41 depth-edge option is present but off; the C182 warp rows, `GXV_LA88_MIPS`,
> `GXV_STAGEQ_SCENARIO` and `GXV_C185_HOLD_UNKNOWN_VRAM` are OFF until the console A/B (docs/RELEASE-3.0-CHECKLIST.md, 0b).
> The known-pair priority list is gone on both lines; on integ-3.1 its records are slow tags in the packaged list (seedrun31).

Base: `integ-3.1` at `e44c45d8bed8f089d3dc75eb2b4b3091bbb1c491`
(includes C183 `07762429a1f0b79b856bb9a5782c90cf33310a23`). This is a
selective replay, not a merge of the complete C184/C185 branches. Integration
owns the general shader list and seed, distance/visibility policies and
performance tuning.

* Keep C184's small raw-archive read batching, compression-header lifetime
  fix, within-pose NEON joint-product reuse, and redundant plan-scratch
  initialization removal. No new CPU optimization. No change to graphics
  quality, input bindings, simulation speed or the integration defaults.
* Do not ship Honeyhive shader sidecars, prepended records, special-case hash
  filters or a seventh recipe. Also remove the older hard-coded C182/C183
  known-pair priority classifier from first-use queueing. A known pair still
  exists in the original seed; hard-coded pair identity no longer promotes it.
  Integration's slow tags, learned list, deduplication and menu-tier late
  registration remain. Never erase old learned files on a user's card.
* Do not ship the plant reveal, Observatory-specific mask warm-up, or
  experimental D41 depth-edge code. The depth-edge treatment is off by
  absence, not an undocumented enabled path. No new warp shader was written.
  Popping enemies, decorations and shadows are not claimed fixed.
* The sole new runtime switch, `GXV_DIAG_FULL_FAR_POSE=1`, is for one
  physical A/B. It bypasses only the far-logic level-3 half-rate pose skip.
  Absent, zero or malformed values leave the original policy alone.
  It neither changes actor movement nor enables a different pose fallback.
* Keep the native 60 Assist setting, automatic relaunch, room-poll scheduling,
  socket counters, log formatting, sampler and server tree from the base.
  No network deployment or changes to the current installation.

The earlier `GXV_VSOUT=0` Honeyhive run does not diagnose the retail Queen
Bee pause. The cause of the lily/flower/Luma vibration is unknown.
[Physical run card](console-tests-C186.md) lists the evidence that can
distinguish them. Capture colour/depth only after the Assist-off comparison
isolates the assisted path, never as a substitute for that comparison.

Run `bash tools/test_c186.sh` for the release host gates.
[Build procedure](../tools/release_c186/BUILD-C186.md). To reconstruct source
in an empty object store: `python3 tools/verify_c186_handoff.py BASE_REPO BUNDLE`.
The donor shader pack, toolchain, contributor archive and release PGO profile
are pinned build inputs, not source blobs in the incremental Git bundle.
