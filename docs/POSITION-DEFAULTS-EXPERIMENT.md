# Direct Position Default Fusion

Experimental, default-off CPU work reduction against
`392ee7009fe1152b5bd397e6398f00404303a264`. No simulation, rendering quality,
shader, matrix arithmetic, draw suppression, or GPU behavior changes.
This is not a measured game or Vita speedup.

## Production Work Removed

`DecodeVerticesDirect` first walks all vertices to set missing position z/w,
then the position attribute decoder walks those vertices again. For plans
whose attributes are all direct, whose position has two or three components,
and which have neither PNMTXIDX nor texture-matrix indices, the position
decoder can write exactly the same defaults in its existing traversal.
One full `count`-iteration loop disappears per eligible decode. The zero
stores remain; source conversion, output layout, and all other attributes
are unchanged. Indexed, matrix-index, missing-position, and unsupported-type
plans keep the old path.

`GXV_POSDEFAULT` is read once during plan construction:

- `0`, unset, or values that `EnvInt` does not parse as 1/2: original behavior.
- `1`: fused position kernel and no separate default loop for eligible plans.
- `2`: fusion plus exact comparison of all 16 position bytes against the old
  position kernel with the original zero defaults. A mismatch uses reference
  bytes and emits a bounded diagnostic. Cumulative eligible decoder calls and
  checked vertices are reported sparsely, including in quiet builds.

The last sparse summary is a cumulative lower bound, not a final route total.
It counts decoder invocations, not rendered draws or frames: retries, verification,
and cache builds may repeat decoding, while cached replay can omit it.

Selection is stored in the existing attribute function pointer. There is no
new per-draw or per-vertex switch check in modes 0/1. `directDefaults` occupies
the former padding before `Plan::n`; the original `defaults` remains intact
for the general decoder. Folded plans copy the new metadata with the plan.

The decoder already belongs to the single frontend player thread, as described
in `vgx_fesplit.h`. The finishing thread transforms existing decoded data and
does not call `DecodeVertices`. Mode-2 counters follow the same ownership as
the existing byte LUT and highest-position-matrix-index state.

## Local Evidence

`bash tools/test_vgx_position_defaults.sh` builds production code with ASan
and UBSan, with runtime diagnostics both disabled and enabled. It exercises
switch values 0, 1, 2, -1, 3, and invalid text; the legacy `GXV_DECODE=0` bypass;
and composition with `GXV_DECODE=2`. Each process checks 22,451 cases and
1,758,134 vertices, plus a missing-position case. Full output bytes, both
guards, array spans, and highest matrix index match the unchanged general
decoder. Fixtures cover every supported position type, all 32 fractions,
POS2/POS3, zero/short/large draws, folded layouts, matrix-index and indexed
fallbacks, out-of-range indices, and explicit floating-point bit patterns
(signed zero, infinities, NaNs, and subnormals). An environment change after
the first plan confirms read-once behavior.

The test was first run against unchanged production code: the new kernel
default assertion failed 10,240 times, while byte-equivalence checks passed.
With fusion, the synthetic corpus eliminates 1,000,964 default-loop iterations
per enabled process. This is deterministic synthetic work-count evidence,
not an observed gameplay frequency.

`bash tools/test_vgx_position_defaults.sh bench` runs the actual direct decoder
with original and fused kernels in alternating order for seven samples.
An initial Apple Silicon run measured fused/original median ratios of
0.711-0.917 across 24 cases: U8/S8/S16/F32, POS2/POS3, and 4/64/256 vertices,
each with TEX0. Each timing sample removes exactly 800,000 loop iterations.
These isolated host results do not establish ARM32 behavior, whole-frame
savings, GPU savings, or a Vita FPS improvement. Mode 2 is not a timing mode.

The complete Python command is
`python3 -m unittest discover -s tests -p 'test_*.py'`. It runs 76 tests with
three expected frozen-inventory failure records: repository checksum coverage,
and the source manifest entries for `vgx_state.cpp` and `vgx_state.h`. Release
metadata is intentionally not regenerated without a matching integrated build.

## Remaining Gates

Existing Observatory logs do not identify eligible direct vertex plans, so
actual route frequency is still unmeasured by this package. Use the mode-2
counts in a matching native build before recommending inclusion. Then compare
mode 0/1 with matching workloads and complete ARM32/compiler, emulator, and
physical-Vita correctness and performance validation. Nothing here establishes
consistent 30 FPS or authorizes a release.
