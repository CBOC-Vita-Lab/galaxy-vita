# Native Phase CPU Diagnostic (WP-N)

`GXV_PHASECPU=1` enables a native diagnostic only. It defaults off and reads
`vgx::EnvInt` once, at the first frame begin (not library loading). Only parsed
value 1 enables it; established prefix parsing means `0x1` and `1suffix` also
enable it. Other values, invalid strings and overflow remain off. The hook
preserves incoming `errno` around parsing, clocks and reporting.

The entire observer and native macro dispatch compile out for Vita, the ARMHF
harness, `PORT_RUNTIME_DIAGNOSTICS=0`, or `PORT_LOGGING=0`. Vita and ARMHF retain
their existing progress macro paths. Native GPU progress remains a no-op. The
native phase entry never updates the old global progress lanes or `sTurn`.

## Measurement Contract

- `port_perf_frame_begin` seeds an unclassified interval after existing begin
  bookkeeping. The preceding `FRAME_BEGIN` checkpoint is outside the scope.
  `port_perf_frame_work_done` samples the last endpoint before other end
  bookkeeping and reporting. Subsequent retrace and frame-complete markers are
  outside the scope. Composition must preserve that observer ordering.
- Each active thread owns its start, last endpoint, label and cumulative totals.
  A thread without its own begin cannot read clocks or add intervals. Anonymous
  thread serials are assigned only at first enabled begin; they are not OS TIDs
  or proof that a thread is the main game thread. Counters and serials are finite
  unsigned values; sessions must not approach their integer wrap limits.
- A checkpoint closes the interval belonging to the previous marker, then sets
  the next label. Nested markers replace labels rather than creating a call
  stack. These are last-marker intervals, **not inclusive functions, exclusive
  function self-time, or standalone port-owned cost**. For example, a finish-wait
  marker with no return marker retains later work until the next checkpoint.
- Every boundary reads the monotonic host wall clock followed by the calling
  thread's `CLOCK_THREAD_CPUTIME_ID` through existing port helpers. The readings
  are adjacent, not simultaneous. Wall units are ns; CPU units are integer us.
  CPU excludes other threads but can include interrupts/work executed on this
  calling thread. It is neither GPU time nor displayed frame latency.
- Whole-turn and phase intervals share the same boundary snapshots. `intervals`
  counts all candidate spans; `wall-samples`/`wall-ns` contain valid wall spans.
  `matched`/`matched-wall-ns`/`cpu-us` contain only spans whose wall **and** CPU
  endpoints are positive and nondecreasing. Only these matched totals support
  a wall-minus-CPU comparison. Positive equal endpoints are valid zero spans.
  Zero/nonpositive readings (including helper failure sentinels) and backwards
  endpoints are invalid, never silently substituted or bridged.
- `invalid-wall` and `invalid-cpu` count invalid candidate spans independently.
  Header `*-faults` count nonpositive reads, and `*-backwards` count adjacent
  positive backwards boundaries once. A failed middle boundary invalidates
  both neighboring spans; it can leave the whole-turn span valid. Therefore
  phase/whole reconciliation requires fault-free matching coverage, not just
  similar sample counts. CPU us quantization also limits tiny-span meaning.
- A nested begin discards the unfinished turn's pending phase totals and counts
  `abandoned`. Only completed turns contribute phase and whole rows. A repeated
  end on a previously participating thread counts `orphan-ends`; other-thread
  ends are ignored. Unknown/zero explicit labels count `unknown` and select
  unclassified; the initial seed does not count unknown. Header fault/unknown
  counters include abandoned-turn activity and are not a completed-only rate.

## Output And Interpretation

At every 256 completed turns **per thread**, output is cumulative: one header,
one `phase=whole` row and one row for every phase used so far. There are at most
66 lines. No automatic final flush is promised for shorter sessions or `_exit`.
Compare successive snapshots from the same thread serial to form windows; the
denominator is completed work turns, not rendered or displayed pictures.

Checkpoints perform no allocations, locks, environment reads or logging. They
do two clocks per active checkpoint and integer bookkeeping. Option setup can
serialize at first begin; report formatting occurs after the final sampled
endpoint with the TLS scope inactive. The surrounding game still pays observer
cost: checkpoint dispatch/clock overhead is inside measured intervals, existing
in-work diagnostics remain included, and end logging can perturb the next turn
or compete with other threads. Reports are not free simply because they are
outside the sampled interval. This is a phase-ranking diagnostic, not an FPS
benchmark or proof of a speedup. Use separate probe-off matched runs for timing
claims; retain capture/probe/cgroup/software-renderer qualifications. Native
results do not establish Vita cost, GPU latency, or sustained 30 FPS acceptance.

## Focused Tests

Run `bash tools/test_phase_cpu.sh /tmp/phase-cpu-test`. The fixture includes the
actual `perf.cpp` dispatch and actual parser, substituting only deterministic
clock reads and an exact-name environment-read counter. Independent literal
expectations cover cached controls, interval/whole reconciliation, foreign and
participating threads, nested begin/unknown labels, fault/backwards endpoints,
matched coverage, errno/FP state, inactive reporting and bounded cumulative
output. ASan/UBSan apply to the fixture. Quiet, silent, Vita-gate and ARMHF-gate
full `perf.cpp` assembly is compared with the pinned base after removing only
filename directives; no phase observer symbol may remain in those fixtures.
For a composed tree with unrelated prior hooks, set `PHASE_CPU_BASELINE_PERF`
to its exact pre-WP-N `perf.cpp` snapshot instead. The comparison then proves
only the WP-N delta is absent, without discarding the other hooks.

The synthetic test is not a native full-game run, hardware measurement or a
measurement of diagnostic overhead. No graphics operation or arithmetic kernel
is added or changed.
