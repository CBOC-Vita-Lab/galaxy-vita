# Resource Audit

`GXV_RESOURCE_AUDIT=1` enables bounded resource diagnostics in the basic player
log, including quiet-player builds. It is read once; unset, `0`, and invalid
values keep it off. Restart to change it. It does not change compilation,
retry timing, texture fallback, drawing, or native driver recovery.

Backend records include the command-buffer frame, allocation serial, logical
vertex/fragment IDs, available structural-key hashes, attempt, disposition,
and a short single-line compiler/linker excerpt. Unknown logical IDs are
`4294967295`; zero hashes mean no available structural hash. Useful stages:

- `create-shader` / `create-program`: invalid GL allocation name.
- `compile-vertex` / `compile-fragment` / `link`: rejected status, delayed retry,
  four-attempt exhaustion, or later recovery.
- `missing-source`: a logical shader has not been registered.
- `draw-skip`: nonempty geometry could not draw because its program was unavailable.
- `declared-white`: a declared texture slot uses the existing white fallback;
  `reason` distinguishes zero ID, out-of-range ID, and absent GL name.
- `copy-link`: the utility copy program did not link.

Ordinary untextured draws are not missing-texture events. There are at most 16
distinct detail records per backend category, deduplicated across frames and
repeated draws. Suppressed-event counts appear at powers of two, bounded to
64 summary records per category for the process lifetime. A later recovery is
logged when detail capacity remains. No full shader source is dumped.

Native driver errors use separate compiler, GXM, and allocation categories
before the ordinary 400-message throttle. Warnings and general informational
messages do not enter this path. Each category retains 16 error excerpts and
bounded power-of-two suppression summaries. Native counters are thread-safe;
native excerpts do not claim a logical shader ID or a precise command frame.

These records are evidence, not a diagnosis. A compile failure can mean invalid
source or resource pressure. Successful GL link status does not establish that
a later blend-dependent native shader patch succeeded. A cache miss does not
mean a shader is absent. No error records, especially after suppression, cannot
establish a correct scene or exclude a native failure that never logged.

## Tests

```sh
bash tools/test_vgx_program_recovery.sh
bash tools/test_vgx_resource_driver.sh
```

The recovery matrix uses the actual backend with Mesa, quiet stdio wrappers,
ASan/UBSan, real malformed-source and mismatched-varying failures, injected
allocation/link failures, exact four-attempt exhaustion, missing registration,
and declared-texture fallback. It compares rendering assertions and GL call
counts with audit off/on and cold tracing off/on. This is not a Vita build or
physical-device test. The native logger test extracts the production bridge
function and executes the real basic-log adapter with host platform stubs.

A caller-supplied compatible recipe can be checked without adding game data:

```sh
bash tools/test_vgx_program_recovery.sh seed-sweep 0 1 /absolute/path/to/vgx_shaderlist.bin
```

This compiles generated sources and links recorded pairs on Mesa only; it does
not run the Vita compiler, native patcher, recorded gameplay, or every possible
runtime shader variant.
