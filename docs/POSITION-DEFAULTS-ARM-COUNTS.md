# Direct Position Defaults: ARM Entry Counts

## Scope

Local synthetic Linux ARMv7/Thumb2 evidence for runtime change
`fbd6d5b29f92968f3b42814983b6b7c7d5c222ab`, compared with pristine
`392ee7009fe1152b5bd397e6398f00404303a264`. No runtime source changed for this
measurement. `GXV_POSDEFAULT` remains default-off and read once.

Toolchain: Clang 18.1.3, Cortex-A9, NEON, hard float, Thumb2, unaligned access,
quiet diagnostics, `-O2`, no fast math, no strict aliasing, static Linux link,
no LTO. QEMU 8.2.2 executes one guest instruction per translation block.
The existing offline image is `codex-galaxy-arm-profile:20261023`, identity
`sha256:b1818409343c92c116061814adc549148743e983153c39928cb97eb40a28e443`.
The run used one CPU, 2 GiB, no network, and a 600-second external bound; exit 0.

The public `DecodeVertices` entry, its cached dispatch, spans initialization,
all callees, and terminal return are counted. Caller/marker instructions are
excluded. Warm dirty `GetPlanIn` rebuilds are counted separately. Cold selector
initialization and byte-LUT construction are excluded, not assumed free.
The fixture and matching production state source are separate translation units;
each pristine/current fixture uses its corresponding header. Baseline source
hashes are checked even when the runner accepts an exported source directory.

Counts distinguish encountered instruction slots from predicate-executed
instructions using CPSR flags, Thumb IT state, and raw ARM instruction bytes.
A separate known ARM/Thumb fixture passes with 13 slots / 11 predicate-executed
instructions, including two failed predicates. The numbers below use the latter.
They are not hardware retired-counter readings, cycles, or QEMU timings.

## Coverage And Equality

- 80 eligible entries: U8/S8/U16/S16/F32, POS2/POS3, 4/16/64/256 vertices,
  position-only and position-plus-F32-TEX0 layouts, fraction 7.
- 12 excluded entries: S16/POS3 with PNMTX, texture-matrix index, or indexed
  position, each at all four counts.
- 92 cases / 7,820 vertices; 80 entered the actual fused kernel in mode 1,
  covering 6,800 vertices. Off/pristine and excluded cases entered none.
- All 167,552 output bytes, both guards, array spans, and maximum matrix rows
  match exactly across pristine, off, on, oracle, and general decoding.
  Output SHA-256: `22a7d4a4f5ba5b42b9659801f9702709f708c1efe30bc43919b9bc9451a1b512`.
- Runtime mode 2 reports no mismatch. This focused fixture does not replace
  the existing all-fractions, folded-layout, read-once, and sanitizer corpus.

## Counts

Across all five numeric types and both eligible layouts, complete warm decoder
entry savings versus pristine are:

| Vertices | POS2 Removed | POS3 Removed |
| ---: | ---: | ---: |
| 4 | 10 | 11 |
| 16 | 46 | 47 |
| 64 | 190 | 191 |
| 256 | 766 | 767 |

For example, U8/POS2 position-only pristine/off/on entries are respectively
163/163/153, 343/343/297, 1063/1063/873, and 3943/3943/3177 instructions.
All 92 mode-0 decoder entries have zero count delta versus pristine.

Plan construction is not free. Every measured dirty eligible plan adds 15
instructions when off and 54 when on for position-only layouts; POS+TEX adds
14 when off and 53 when on. Excluded PNMTX/TMI/indexed plans add respectively
16/15/15 when off and 16/19/15 when on; their decoder counts do not change.

| Eligible Corpus | Pristine | Off | On |
| --- | ---: | ---: | ---: |
| 80 Complete Decoder Entries | 182,296 | 182,296 | 162,016 |
| 80 Dirty Plan Rebuilds | 60,424 | 61,584 | 64,704 |
| One Rebuild Plus One Decode Per Case | 242,720 | 243,880 | 226,720 |

Thus eligible decoding removes 20,280 instructions; including one dirty plan
rebuild per case removes 16,000. Off adds 1,160 in that combined scope. Including
all 12 excluded cases, combined totals are 305,175 / 306,519 / 289,375, a net
15,800 reduction for on versus pristine in this synthetic corpus.

With a dirty plan rebuild for every draw, the measured eligible 4-vertex cases are
42-44 instructions worse and 16-vertex cases are 6-8 worse. The 64/256-vertex
eligible cases remove 136-138 / 712-714. Reused plans avoid that rebuild cost, but actual
game plan churn and format distribution are not measured here. These synthetic
eligibility proportions must not be treated as a gameplay census.

Arithmetic amortization thresholds from these measured counts, across all five
types and both eligible layouts: after one dirty rebuild, on first uses fewer
instructions than pristine after six 4-vertex POS2 decodes or five 4-vertex POS3
decodes; both 16-vertex forms need two decodes; 64/256 need one. Against patched
off, both 4-vertex forms need four decodes; 16/64/256 need one. These are count
break-even calculations, not additional timed reuse trials or game measurements.
Actual eligible invocations may be mostly small. Without their plan-rebuild
frequency, this result does not establish a net runtime gain.

## Reproduce

From the patched checkout, export the pinned baseline for the read-only mount:

```sh
out=$(mktemp -d)
mkdir "$out/baseline"
base=392ee7009fe1152b5bd397e6398f00404303a264
git show "$base:platform/vgx/src/vgx_state.cpp" > "$out/baseline/vgx_state.cpp"
git show "$base:platform/vgx/src/vgx_state.h" > "$out/baseline/vgx_state.h"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=1 --memory=2g \
  --mount "type=bind,source=$PWD,target=/work,readonly" \
  --mount "type=bind,source=$out,target=/out" \
  -e POSITION_PRISTINE_DIR=/out/baseline -w /work \
  codex-galaxy-arm-profile:20261023 \
  timeout 600 bash tools/test_vgx_position_defaults_arm.sh /out/results
```

The runner emits per-entry JSON, complete counts CSV, disassembly, and synthetic
output comparisons into the output directory. These local outputs are not
committed. No game data, SDK, Vita executable, cloud service, device, or public
branch is involved. Concurrent desktop correctness checks do not change these
instruction counts; no concurrent host wall-time measurement is reported.

This establishes removed work in these isolated ARM entries, not matching Vita
compiler output, hardware speed, renderer/GPU savings, broad gameplay equality,
or a proportional FPS improvement. Keep the feature experimental and off by
default pending matching target correctness and repeated hardware measurements.
