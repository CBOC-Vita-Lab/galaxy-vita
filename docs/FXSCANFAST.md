# Inactive FX Scan

`GXV_FXSCANFAST` is read once by the renderer. The default, `0`, retains the
existing scan. `1` bypasses it only when FX reduction is explicitly disabled
(`GXV_FXHALF` resolves to zero), runtime diagnostics are compiled out, the
workload observer is disabled, no verification/reference/shadow run is active,
and the command buffer has no pass records. The existing mask clear and FX
option initialization still occur. Capability failure with FX enabled does not
qualify.

`2` evaluates the same eligibility but always executes the original scan once.
It compares the proposed empty mask with the actual mask, reports at most three
mismatches, and always keeps the actual scanned result. It does not replay a
frame or issue extra GPU operations. Other parsed values select the old scan.
Parsing uses the project's `EnvInt` conventions, including numeric prefixes,
and the new option read preserves `errno`.

The workload setting must be unset or exactly `0` when the otherwise-eligible
guard first initializes. Other spellings, including empty, `00`, invalid and
overflowing values, conservatively retain the scan. This avoids moving the
legacy workload parser's conditional overflow side effect. An already-enabled
workload observer still prohibits bypass even if its environment later changes.
Pass records also prohibit bypass because pass-target calculation has floating
point and environment-read side effects.

Only unobserved FX candidate counting is omitted. The diagnostic CSV/profile,
pass verification and workload histogram/census consumers all retain the old
scan. No simulation, physics, draw commands, GPU state, resolution or art changes.
The measured scene has roughly 626 draws per picture, but neither time saved nor
an FPS gain has been established. Physical-hardware acceptance still requires
consistent minimum 30 FPS across gameplay, not a native fixture or one scene.

## Host Checks

On a Linux host with EGL/GLES2 and Mesa:

```sh
bash tools/test_vgx_fxscan.sh
bash tools/test_vgx_fxscan_gl.sh
```

The dispatch fixture includes the real backend privately, checks option
initialization, gates, stale-mask clearing, workload counters, `errno` and FP
state, and injects a changed slow mask through a test-only callback wrapper to
check bounded oracle reporting and fallback. No production test API is added.
The GL fixture runs real indexed draws through Mesa and compares exact RGBA
bytes and ordered traces of the GL calls listed in the runner's `wraps` array,
not every GL API. It runs unset/0/1/2 separately with FX disabled and enabled at
divisors 2 and 4: 12 process runs yielding nine mode-versus-0 comparison pairs.
These are correctness checks, not timing benchmarks or Vita validation.

The initial dispatch test failed against the unchanged scan, then passed after
the guard. Local verification passed 22 dispatch cases, the five-call injected
oracle check, 12 Mesa process runs with nine comparison pairs, six selected
ASan/UBSan cases and the existing workload suite. The first sanitizer launch failed because the container's ASan
runtime was not first in its library list; explicitly preloading that installed
runtime allowed the six cases to run successfully.

The full Python suite ran 76 tests with five failures in the frozen release
checksum/source-inventory checks; this is not a passing full suite. Metadata was
not regenerated. The required native HD command first failed for missing NumPy;
with the existing dependency environment it reached the known three undefined
host-hook linker symbols. A disposable overlay with the existing test-only WP-H
hook definitions then passed all eight HD sanitizer scenarios. WP-H is not
included in this follow-up.
