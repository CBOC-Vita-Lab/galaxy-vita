# Exact Final-Wrap Elision

Status: default-off local candidate with a modest observed opportunity. Depends on the final TEV byte-wrap repair
(`GXV_TEVWRAP=1`). No Vita executable, Vita compiler, console or FPS validation.

## Modes

- `GXV_TEVWRAP_ELIDE` unset, 0, or another parsed value: unchanged shader text.
- `GXV_TEVWRAP_ELIDE=1`: omit a final conversion only for a proven unit-range value.
- `GXV_TEVWRAP_ELIDE=2`: independently verify that proof and keep the conversion
  if either proof cannot establish the bound. Disagreement logs once per process.

Both switches are read once. The existing `EnvInt` parser is used without an
integer-width narrowing conversion. Values such as 4294967297 cannot become 1.
Elision has no effect when `GXV_TEVWRAP` is off.

## Exact Scope

The forward proof tracks RGB and alpha separately for each of four TEV
registers. Explicit stage clamp writes establish a 0..1 bound. Unclamped stages
propagate it only for two exact forms, with zero bias, addition and scale 1:

```text
mix(bounded, 0, 0) + 0
mix(0, 0, 0) + bounded
```

The literals 0, 0.5 and 1 also qualify. RGB may copy a register's replicated
alpha. Both components read pre-stage state before either destination changes.
Unknown writes invalidate previous bounds. Initial register uniforms, folded
vertex registers, textures, raster colours and uniform konst values are never
assumed bounded. Compare, bias and scale operations are not treated as copies.
All existing stage arithmetic and clamps remain byte-for-byte unchanged.

Mode 2 uses a separate backward last-writer walk and independently decoded
copy patterns. Its fallback protects against a false-positive forward proof,
not arbitrary compiler defects or errors shared by both proofs. The host test
mechanically injects an always-bounded forward proof: mode 1 fails, while mode
2 retains all required wrappers and passes, with only one diagnostic.

The out-of-range biased final alpha used by the eye repair does not qualify.
This is neither full integer TEV emulation nor a new visual approximation.

## Cache Contract

The final candidate retains generator version 3 from the byte-wrap baseline.
The initial proposal unnecessarily advanced it to 4; a separate narrow follow-up
removes that bump and pins version 3 in the native regression test. No shader
arithmetic, generated text or Vita cache policy changes in this follow-up.

This matters because `ShaderCacheFolder` includes the generator version in its
outer directory key, and `ShaderPackIdentity` includes it in the RAM-pack ABI.
Package seeds use that outer key. Existing migration moves only vitaGL v1/v2
subfolders within one outer directory, not generator-3 to generator-4 caches.
Had the bump remained, unchanged off-mode binaries would not automatically be
found; a matching new seed/cache or compilation would be needed. Source hashes
protect lookup within a namespace, not access to a previous namespace.

No extra bump is needed for this optional transformation: structural keys,
recipe records, shader inputs and host bindings are unchanged; accepted recipes
regenerate current source, and vitaGL hashes that source for binary lookup.
The mode is immutable within a process. Keeping version 3 preserves default-off
cache identity and existing recipes; changed enabled source gets a different
inner hash. This does not reverse the original byte-wrap patch's 2 -> 3 change.

## Verification

- Native ASan/UBSan: 386 source assertions per configuration; 18 configurations
  cover wrapping off/on and elision unset, 0, 1, 2, 3, -1, nonnumeric and two
  large values. Independent fault-injection fallback test passes.
- Original generator compiled separately establishes source digests:
  wrapping off `f212c4f4f5dbb80b`; wrapping on/elision off `cf8d2a776de174e2`.
  Elision 1 and 2 both produce `033f63751ded1a41` across the synthetic fixtures.
- Actual Mesa GLES2: 21,693 final-output/alpha checks in each of 18 runs,
  390,474 total. Exact framebuffer digests agree for baseline, optimized and
  verified modes. Coverage includes signed -1024..1023 output, fractions,
  independent destinations/clamps, copy-through clamps, actual discard RGB
  sentinels and the three-stage biased synthetic eyelid exit.
- Full Python discovery: 76 tests executed, with three release-inventory
  failures: checksum coverage and changed generator `.cpp`/`.h` hashes.
  Release metadata is intentionally not regenerated for this unintegrated patch.
- Existing shader-list replay/recovery suite: all 24 scenarios pass across both
  recipe-seed settings, including truncated records and deferred replay.
- Independent read-only proof/oracle review: no correctness defect found.

The GLES2, full Python discovery and codegen evidence above predates the
metadata-only 4 -> 3 follow-up. Native source/oracle tests and the 24 shader-list
cases are rerun for the follow-up. Their source digests are unchanged; the
GLES matrix is not rerun because no emitted shader text or math changes.

```sh
bash tools/test_vgx_tevwrap_elide.sh
# Linux Mesa EGL/GLES2 dependencies required:
bash tools/test_vgx_tevwrap_elide_gl.sh
bash tools/test_vgx_tevwrap_elide_codegen.sh
```

The codegen tool accepts an optional caller-supplied recipe and fragment index.
Its generated shader/IR logs can contain private material state and must remain
private. No recipe, texture, material key, game data or capture is included here.

## Observed Opportunity

An older 728-key recipe has 70 qualifying fragment keys: 67 alpha groups and
3 RGB groups. A fresh local Observatory route has 127 fragment keys, of which
10 qualify, all alpha. Actual generation reduces scalar wrap call sites from
56 to 46 and total fragment-source bytes from 165,033 to 162,563. Modes 1 and 2
have identical source digests on that fresh corpus, with no verifier disagreement.

The traced picture contains 39,024 frontend pre-batch draw records, 10,136
using the qualifying keys (25.97%). The mapping was checked against 74,459
stage color/alpha pairs. This is not the number of submitted GL draws, shaded
pixels or GPU time; batching merges many frontend records. One traced picture
does not establish scene-wide performance or coverage of other stages.

## Compiler Evidence

All ten qualifying real fragment recipe keys were also generated and compiled
in the same host renderer, paired with a synthetic vertex shader whose colour
attributes and position-derived texture coordinates stay dynamic. Uniforms and
the sampled 1x1 texture are synthetic. These are actual fragment-state keys,
not the original full-game linked programs or a gameplay visual comparison.

| Recipe key SHA-256 prefix | NIR floors off/on | Partial CPU instructions off/on | Whole CPU instructions off/on |
|---|---:|---:|---:|
| `3f2776e0c054` | 0 / 0 | 972 / 972 | 934 / 934 |
| `9229f983dfca` | 0 / 0 | 965 / 965 | 922 / 922 |
| `42986b0d85a5` | 0 / 0 | 950 / 950 | 910 / 910 |
| `3a1aaa9f0c45` | 0 / 0 | 939 / 939 | 899 / 899 |
| `95aac71d399b` | 0 / 0 | 934 / 934 | 894 / 894 |
| `b40c47b9f1ec` | 0 / 0 | 990 / 990 | 952 / 952 |
| `50444781e3ed` | 2 / 0 | 1077 / 1051 | 1036 / 1011 |
| `5594140dbdc5` | 2 / 0 | 1076 / 1051 | 1035 / 1010 |
| `446b141e2d70` | 2 / 0 | 1074 / 1049 | 1031 / 1006 |
| `b4db2263dec1` | 2 / 0 | 978 / 959 | 941 / 918 |

The six unchanged cases demonstrate why source simplification alone is not
performance evidence. The last four each lose two final `frintm` instructions
in both CPU variants. Every off/on pair has identical probe pixel digests.
Only these four showed a compiler reduction; they account for 417 frontend
pre-batch records (1.07% of the traced picture), not all 10,136 qualifying
records. No whole-frame gain is established. The codegen tool intentionally
fails its improvement gate on the six already-optimized cases.

On Mesa 25.2.8 llvmpipe with LLVM 20.1.2, a synthetic four-channel bounded-copy
probe retains eight floor operations after baseline NIR optimization. Elision
removes all eight. Final AArch64 CPU fragment assembly changes from 734 to 688
instructions for the partial variant, and from 694 to 647 for the whole variant;
each loses eight `frintm` instructions. Both return the same pixel digest.

This establishes that this host compiler did not already remove all redundant
work. It is not SGX/Vita assembly, a hardware GPU benchmark or a speed claim.
Matching Vita translation/compiler validation, full-game visual comparison and
owner-approved hardware measurement remain required before enabling for players.
