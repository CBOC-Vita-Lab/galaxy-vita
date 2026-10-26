# Native Matrix Inverse Count Probe

This diagnostic ranks actual `C_MTXInverse` frequency and exact consecutive
input repeats. It is not a cache, a math optimization, a time measurement, or
evidence of a Vita performance improvement. Simulation and matrix arithmetic
are unchanged. Consistent minimum 30 FPS across gameplay on physical hardware
remains the acceptance requirement; Observatory counts alone cannot establish it.

## Controls

Build both the SDK matrix code and the port performance code with
`PORT_RUNTIME_DIAGNOSTICS=1` and `PORT_LOGGING=1`, then set `GXV_MTXPROFILE=1`
before process startup. The control defaults off and uses the existing
`vgx::EnvInt` parser, read once at first use. Thus its existing integer-prefix
parsing also accepts `0x1` and `1suffix`; parsed values other than 1 are off.
Changing the environment after first use does not change the control.

The entire probe, including the inverse and frame hooks, is compiled out when
either compile control is zero or `__vita__` is defined. Normal quiet player
builds and every Vita build ignore `GXV_MTXPROFILE`. All translation units must
use consistent build controls. The standalone GX frame test explicitly compiles
its matrix fixture without runtime diagnostics because it does not link the
port performance provider.

## Counts and Ownership

Every 256 completed marked work turns, the probe emits a cumulative header and
at most 33 cumulative counter rows (34 lines total). There is no exit flush.
Fewer than 256 turns produce no report; a final partial interval is unreported.
There are no per-call logs and no matrix data or pointers are logged.

- `turns`: cumulative completed `port_perf_frame_begin` / `work_done` scopes.
- `threads`: assigned individual host-thread slots, capped at 32.
- `overflow-threads`: additional host threads using the shared overflow row.
- `thread`: anonymous process-local slot number, not an OS thread ID or name.
- `calls`: completed inverse calls, including `PSMTXInverse` wrapper calls once.
- `repeats`: the current input's 48 bytes exactly match the preceding inverse
  input on the same host thread. Signed zeros and NaN payloads are distinct.
- `singular`: calls for which the original inverse returns zero.
- `work`: calls begun inside a marked work scope on that same host thread.
- `work-repeats` and `work-singular`: the corresponding subsets of `work`.
- `guest`: calls whose existing SDK `port_is_game_thread()` TLS flag is true.
  This is SDK thread ownership, not proof that the caller is the main thread.

The overflow row is an aggregate, never a unique thread. Repeat detection still
uses each contributing thread's own previous input, so overflow does not lose
calls or invent cross-thread repeats. Slots are never reused; exited threads
remain in cumulative totals. Counters use unsigned 64-bit arithmetic.

Work scopes use existing frame hooks directly, independent of optional game
phase macros. A work repeat compares against the preceding call even if that
preceding call was outside work. The history is not reset at frame or report
boundaries. `work-repeats / work` is therefore the fraction of current in-work
calls matching their immediate predecessor, not a count of pairs wholly within
one turn. Scope ownership must be read from actual marked callers; neither
`guest` nor slot 1 proves main-thread identity.

Rows are rolling atomic snapshots, not a coherent cross-thread transaction.
Concurrent calls can advance individual counters between loads. Exact totals
are available when callers are quiescent, as in the focused tests. Differences
between live cumulative reports are approximate interval counts, not exact
scene boundaries. Startup work remains included unless separately accounted for.

## Behavior and Cost Boundary

The entry hook copies input bytes before the original arithmetic or any aliasing
output write. The exit hook records the original result after its last output
write, or before the singular return. Neither hook performs floating-point
arithmetic, a floating-point comparison, a clock read, or logging. The original
inverse expression order and singular no-write behavior are retained. This is
an observer only: every inverse still executes, including repeated inputs.

The first control lookup uses atomic initialization serialization and preserves
incoming `errno` around `EnvInt`. Subsequent disabled calls have an out-of-line
entry hook plus a cached atomic control read; diagnostic-disabled is not the
same cost as compiled-out. Diagnostic and quiet binaries therefore are not an
unchanged-code FPS benchmark pair. Enabled calls add TLS accesses, a 48-byte copy and
comparison, SDK ownership lookup, and lock-free integer atomic counters. The
translation unit requires always-lock-free unsigned and 64-bit atomics with
compile-time assertions. No mutex, allocation, or clock is requested by the
inverse hooks. First concurrent lookup callers may briefly wait for the atomic
initializer; this is not a wait-free initialization scheme.

The intended measurement environment is a native ELF main executable with POD
TLS. There is no claim that all TLS implementations avoid first-access
allocation. In particular, this probe is excluded from Vita builds rather than
relying on Vita emulated TLS or ARM32 runtime helpers. Reporting may allocate
or lock in the existing logging provider, outside the inverse itself. Native
counts cannot transfer native timing or diagnostic overhead to Vita.

## Focused Verification

Run `bash tools/test_mtx_profile.sh <temporary-output-directory>` with Clang.
This compiles actual production `mtx.c` and `perf.cpp` with ASan and UBSan,
and compares against the pinned uninstrumented inverse expression. The tests
cover the default, off and invalid controls, enabled parsing, read-once behavior,
all four rounding modes, pre-existing FP flags, raw float bits including NaNs
and subnormals, same and partially overlapping aliases, singular no-write,
incoming `errno`, no inverse clock reads, work/SDK ownership counts, 41 concurrent
host threads, overflow accounting and bounded logs. Each main fixture process performs
2,726 production/reference inverse comparisons.

A separate new-process fixture holds a test-only redirection of the first
matrix-control read while 15 follower threads attempt their first inverse. The
wrapper then invokes the real `EnvInt`. Off, on, invalid and out-of-range cases
verify one parse, consistent publication, each thread's incoming `errno`, and
all 32 completed calls and 16 repeats when enabled. No production test hook or
concurrent environment mutation is used. Unrelated environment reads delegate
directly to the real parser without holding or counting them.

Quiet, logging-off and `__vita__` fixtures verify no probe symbols in either
production matrix code or the linked performance fixture. They also compare
the entire generated matrix assembly against pinned release
`392ee7009fe1152b5bd397e6398f00404303a264`, ignoring only filename directives.
The `__vita__` fixture exercises preprocessor exclusion on the host, not a Vita
SDK build or hardware test. No game assets or game launch are needed.
