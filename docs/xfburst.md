# WP-O: Exact 12-Word XF FIFO Burst

Experimental, default off. This changes command serialization, not matrix
arithmetic, simulation, rendering state, or GPU work. No game or Vita speedup
has been established. Whole-game physical-hardware consistency at 30 fps or
better remains the acceptance boundary.

## Scope and Controls

Only the port fallback `WriteMTXPS4x3` calls the new helper, and only under
`TARGET_PC && __ARM_NEON`. This writer serves position and 3x4 texture matrices.
Projection6, normal9, 2x4 texture8, command headers, projection tails, and the
legacy invalid texture-type behavior remain unchanged. Non-NEON entrypoints
use the original writer, with no new option read or dispatch.

`GXV_XFBURST` is parsed once through the existing `EnvInt`, preserving `errno`:

- unset/0 or any parsed value other than 1/2: original writes;
- 1: guarded exact burst;
- 2: diagnostic oracle, constructing a local candidate but writing the original
  path exactly once. Quiet builds fall back to original writes without an
  oracle claim. Runtime diagnostics and logging must both be enabled.

The existing parser accepts prefixes such as `0x1` and `1suffix`; tests retain
that behavior. Diagnostic success messages identify `words=12` at powers of
two, and mismatches are limited to the first four. Reporting preserves `errno`,
FP rounding and exception state. Fast mode has no added counters or logs.

Eligibility is checked after the unchanged 5-byte header. The 48-byte payload
must finish strictly before the current limit; reaching the limit retains the
old publication/overflow behavior. Null, wrapping, overlapping source/output,
and source/output overlap with the cursor or limit globals all fall back.
Three exact 16-byte integer loads/reversals/stores preserve every float storage
bit without FP operations or overreads. A scalar exact helper fallback is
available in its implementation, but non-NEON GX entrypoints never dispatch it.

## Work and Overhead Evidence

An initial all-shape scalar draft was rejected because guard/dispatch overhead
could erase the smaller payload savings. This version is limited to 12 words.
Clang 18.1.3 Cortex-A9 Thumb2 `-O2 -mthumb -munaligned-access` emits three each
of `vld1.8`, `vrev32.8`, and `vst1.8`, with no separate payload call.

Independent whole-call warm, nonalias, no-overflow instruction accounting:

| Actual `GXLoadPosMtxImm` path | Executed mnemonics | Encountered slots including failed predicates |
| --- | ---: | ---: |
| Pinned original | 150 | 151 |
| New mode1 | 112 | 115 |
| New mode0 | 169 | 171 |

These totals include the unchanged header, caller/writer wrapper, helper
prologue/return, static-initializer guard and `dmb`, every eligibility guard,
copy and cursor commit. They are not cycle estimates. In particular, default
off adds 19 executed mnemonics; the barrier and memory costs still matter.

A bounded native ARM64 quiet `-O3` microcheck linked the actual C entrypoint and
production FIFO implementation, with a no-op external renderer consumer.
Five alternating fresh processes per configuration each made 1,000,000 timed
calls after 10,000 warm calls. All produced 1617 publications and the same final
payload checksum. Median thread-CPU time per call was 11.081 ns for the pristine
entrypoint, 11.857 ns for new mode0, and 6.384 ns for new mode1.

The approximately 7% disabled-path microregression is an adoption risk. The
first process sets also showed CPU warmup drift. The enabled result supports
further exact validation only: it is not a game FPS result, a GPU result, or a
native-to-Vita timing transfer. Mode2 is not performance evidence. Source-only
ARM-state `-O3` inspection is supplementary, not the actual Vita build mode.
An independent repeat found medians of 11.590/11.543/6.772 ns for pristine,
mode0/mode1 respectively, with the same publications/checksum. That repeat
shows no clear disabled-path slowdown amid host noise; it does not erase the
first result or the additional ARM instructions.
A second independent repeat measured 11.476/12.562/6.571 ns for the same three
configurations, again with equal publications/checksum and warmup drift. The
enabled front-end reduction repeated; the disabled-path result remains a risk,
not a demonstrated zero-cost control.

## Verification

The first real-production fixture failed before implementation because the
requested option was never read. Final focused checks:

- 21 native ASan/UBSan process cases passed, plus two independent 21-case repeats;
- 21 native non-NEON-exclusion process cases passed, plus an independent repeat;
- 21 Cortex-A9 Thumb2 Linux/QEMU process cases passed (no sanitizer there);
- entire non-NEON GXTransform assembly matched the pinned source, after only
  removing `.file` metadata.

Fixtures invoke the actual C `GXTransform.c` and actual FIFO overflow code,
with the pinned original translation unit as an independent command oracle.
They cover all four payload widths and public position/normal/texture paths,
3x4 and invalid texture types, unaligned output, all nearby limits, header
rebinds, same/forward/backward aliases including header-straddling sources,
hard/soft publication, synchronous consume, repeated sink wrap, full written
tails and canaries, signed zeros/subnormals/infinities/qNaNs/sNaNs, all four
rounding modes, FP flags, `errno`, unset/invalid/overflow/read-once controls.
Test-only NEON load corruption proves that mode1 really commits a candidate
only for the 12-word entrypoints; mode2 detects it, bounds logging, and retains
the exact original bytes, pointer and callback trace. No production test hook.

Reproduce focused native checks with:

```sh
bash tools/test_xfburst.sh /tmp/xfburst-test
XF_BURST_NO_NEON=1 bash tools/test_xfburst.sh /tmp/xfburst-noneon
bash tools/bench_xfburst.sh /tmp/xfburst-bench
```

For the existing offline ARM Linux image, set `XF_BURST_ARM32=1` and run the
same test script. If Git is absent in that image, set `XF_BURST_BASELINE_PATH`
to a mounted copy of `GXTransform.c` from commit
`392ee7009fe1152b5bd397e6398f00404303a264`. `tools/inspect_xfburst.sh` emits
native/ARM-state `-O3`, Cortex-A9 Thumb2 `-O2`, and non-NEON identity evidence.
QEMU runs are correctness checks, not performance measurements.

The required Python suite ran 76 tests and failed 4 frozen release-metadata
records: repository inventory plus the three modified production hashes. The
required `CXX=clang++ bash tools/test_vgx_hdtex.sh` actually ran and failed to
link the baseline's three weak host symbols (`port_host_time_ns`, `port_log`,
`port_thread_cpu_us`). Neither suite is reported as wholly green; release
metadata was intentionally not regenerated. An early expanded fixture also
exposed C++ enum-range checking of the legacy invalid-type oracle; only that
C++ oracle check is disabled, while actual production C retains sanitizers.
The first benchmark setup aborted with exactly adjacent synthetic staging
allocations at the existing one-past segment boundary; padding those fixture
allocations fixed setup without changing production FIFO behavior. An initial
runner also hit Bash3's empty-array/nounset behavior before executing tests;
the host runner now explicitly invokes `env`.

## Build Boundary

`GXTransform.c` is in `libgalaxy_vita_sources.a`, not just the platform runtime
archive. A future Vita build must rebuild the source archive (including the
default LTO source archive when selected), runtime archive, and final link.
A runtime-only rebuild can silently leave the old matrix writer in place.
No Vita SDK build, game run, device installation, or release change is part of
this isolated source/test work.
