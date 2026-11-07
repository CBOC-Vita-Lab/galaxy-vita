# Water Capture Allocation Investigation

Status: local diagnostic candidate, default OFF. No physical Vita has been accessed,
installed, or tested. This is not a proven fix for the reported water crash or system freeze.

## Evidence

The player reported intermittent crashes around water in Buoy Base and Beach Bowl,
including one complete Vita freeze requiring a reboot. A later attempt worked.
The photograph contains approximately 40 seconds of preceding cadence, not a crash
stack: 544x388 picture, 20.7-35.8 shown frames/s, 59.5-60.7 game turns/s,
122-197 skipped pictures per five-second window, no >100 ms hitch in the final
windows, and 57-88 ms longest frames. Those figures neither establish nor exclude
a later GPU allocation/fence failure. They do not support a battery/heat diagnosis.

The underwater camera filter follows this path:

1. `decomp/src/Game/Screen/WaterCameraFilter.cpp:114`: when not in the Air nerve,
   capture the full screen as RGB565, then apply the distortion/tint material.
2. `decomp/src/JSystem/JUtility/JUTTexture.cpp:198`: capture issues `GXCopyTex`.
3. `platform/vgx/src/vgx_draw.cpp:2806`: RGB565 sets output alpha to 1. Its matrix
   is not the identity required by `EfbSwap`, so this capture takes the ordinary copy.
4. In the release baseline, `CopyTexture` publishes width, height and
   `renderTarget=true` after unchecked `glTexImage2D`; `CopyTex` immediately
   attaches it and submits a copy draw. By contrast, `BoundTexHasStorage` and the
   EFB-swap allocation path explicitly handle a null vitaGL texture data pointer.
   The existing source notes that attaching such storage can crash the next scene.

This is a confirmed missing-allocation-handling branch. There is no captured
allocation failure proving it caused the player's freeze.

The actor sources, JUT capture, ordinary copy allocation/lifetime, depth plan,
and vitaGL depth-sampling build patch are unchanged between v1.0 (`941cfb0`) and
the investigated release-1.1 source (`f139459`). Consequently this particular
hole predates 1.1. A new code path or different resource pressure can still expose
an old defect; source ancestry alone cannot classify the actual report.

Other inspected boundaries:

- `OceanBowl`'s 24 strips x 50 vertices reference 1200 texture-coordinate pairs
  backed by 2400 floats per array. No overrun was found in this fixed grid.
- `WaterAreaHolder` allocates capacity eight for each actor category and appends
  without a bounds check. No stage data demonstrating nine entries was obtained;
  this is not evidence that the reported levels exceed the limit.
- The shared underwater filter captures color, not depth. A missing ordinary
  depth-texture capability produces a white copy in the existing backend. Do not
  turn that separate visual fallback into a water-freeze diagnosis.
- `settings_launch.cpp:66` enables half-size effects and target-30 quality only
  for Steady 30. Exact `settings.txt`/`env.txt` values are required before assigning
  the report to that 1.1 opt-in route. Default-off FX scans/position experiments
  cannot be assumed active.

## Candidate

`GXV_RT_ALLOC_GUARD=1` is read once. Unset or `0` preserves the existing path.
For ordinary non-fill copies the guarded path:

- Allocates new storage independently and checks the Vita data pointer before
  attaching it; checks a newly generated framebuffer name before publishing it.
- Rechecks existing same-size storage, since another unguarded path may have
  left valid-looking metadata without storage.
- Publishes a replacement only after its storage and framebuffer are available.
  Failure retains the previous capture when one exists and submits no copy draw.
- Preserves terminal/nonterminal pass handling and clears; invalidates the
  speculative depth plan for the rest of the failed frame.
- Retries on the next ordinary copy and logs up to four failures through
  `autoq::Log`, which also reaches the non-diagnostic player summary log.

Limitations are intentional: fill copies can be skipped across frames by the
frontend's constant-copy cache, so they remain unguarded rather than falsely
claiming retry safety. `BeginPass`, depth allocations, `ApplyRung`'s in-place
fallback, and the driver's lazy render-target/scene allocation are not repaired.
The helper cannot guarantee success after arbitrary GPU OOM. It also temporarily
needs both old and new color storage when replacing a capture. No success-path
performance claim is made. The existing EFB-swap fallback can consume an earlier
RGBA capture before the ordinary helper is entered; preservation applies to the
texture still present at helper entry. The water filter's RGB565 copy is rejected
by EFB-swap before any of that mutation.

## Tests

`bash tools/test_vgx_copy_alloc_guard.sh /tmp/galaxy-copy-alloc-20261026`
extracts the unmodified production functions into a host fixture and records
their SHA-256. Only GL calls and unrelated draw helpers are faked. The fixture
uses the Vita branch of `BoundTexHasStorage`, not the desktop unconditional true.
It runs under AddressSanitizer and UndefinedBehaviorSanitizer.

The initial test failed before implementation: `initial allocation failure must
not attach/draw a missing target`. A second red test caught same-size metadata
with absent storage. Current cases cover first/replacement failure, missing
texture/framebuffer names, exhausted FBO reuse, preexisting invalid storage,
fill-to-ordinary recovery, repeated pressure, pass continuation/end, atlas source
and destination, EFB-buffer ownership, recovery, read-once control, bounded
player-summary logging, and legacy unset/off behavior. The logging assertion
also failed before moving the breadcrumb from diagnostic-only `GPUPASS_NOTE`
to `autoq::Log`. The excluded fill case deliberately demonstrates its old
missing-storage draw rather than asserting that fill copies are repaired.

Local verification on 2026-10-26:

- All 16 fault/legacy fixture invocations passed under ASan/UBSan. The unchanged
  `f139459` backend fails the initial-allocation and same-size-invalid-storage
  regression cases, as expected; their extracted-source hash matches the
  release manifest (`f26d31aaf35044002080b789cb6a29b8db8056071f79b11b93ea434ab8d2cb07`).
- `tools/test_vgx_autoq_gl.sh` passed all four run variants with the switch off
  and all four with it on, using actual Mesa GLES in the offline local Docker
  image `codex-galaxy-native:20261007-arm64`. This includes capture/atlas pixel
  assertions, 28 per-frame rung changes, the default 3000-frame stress with
  normal texture retirement, 300 frames with KEEP_RT, and both target-30 settings.
  These are host pixel/control-flow checks, not a Vita performance measurement.
- All eight HD texture tests passed.
- The Python suite ran 111 tests: 106 passed, 3 skipped, and 2 frozen release
  metadata checks failed (`test_checksums_cover_the_repository` for the four
  added files; `test_source_manifest_matches_tree` for the changed backend).

The full GLES, HD and Python runs preceded the final logging-only change from
`GPUPASS_NOTE` to the bounded player-summary logger. All 16 fault/legacy fixture
invocations were rerun and passed after that change, including the four-line
limit under repeated failures. The broader suites were not rerun for that delta.

Release metadata is intentionally not regenerated for this diagnostic candidate.
`python3 -m unittest discover -s tests` has expected failures for added files in
the frozen checksum set and the modified backend's source hash; those are not
behavioral test passes. Refer to the handoff for complete current run counts.

## Reproduction Evidence Needed

Record exact executable hash/version, data/HD-pack identity, saved settings and
environment overrides, stage/mission, route, camera-water transition, cold vs
warm attempt, and whether only the game or the system stopped responding.
Keep saves, settings and artwork unchanged and obtain approval before any install.

First compare original 1.0 and 1.1 under matched default settings; do not silently
mix Steady 30, Fixed 30 and the uncapped path. Then compare this same candidate
with only `GXV_RT_ALLOC_GUARD=0/1` changed on the exact route. Repeated successful
attempts are needed because one retry already succeeded on the unmodified game.
Capture the final full log and any crash dump. A bounded failure line is evidence
of the guarded allocation branch, not evidence that all system-freeze causes are
removed. A remaining freeze without that line needs render-command/fence and
allocation telemetry rather than more speculative production changes.
