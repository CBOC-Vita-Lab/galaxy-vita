# Long present waits: optional Vita3K diagnostic

Prepared, tested with a fake clock and syntax-checked against the real native
translation units; **not linked or exercised in-game**. This is separate
from the checkpoint A/B. It does not contribute to the VPK, fix a stall,
change a synchronization primitive, alter a timeout, or change vsync.

## What has been established

The C186.1 retail guest profile (Performance, `GXV_VSOUT=1`, no forced
packs) in macOS Vita3K occasionally spent almost a second in the game's
inclusive present. In steady-a frame 2475 the guest recorded 951 ms,
including 951 ms in present; the adjacent stock cache save ended 2.673 s
later. In moving-on2 frame 3070 the interval was 957 ms, including 953 ms
in present. The preceding checkpoint disk write finished 3.237 s before
that frame ended. Neither event establishes a shader, GPU or disk cause.

Vita3K 92bda548 passes an unlimited timeout to both the Vulkan swapchain
acquire and fence wait. Farther downstream, [MoltenVK 1.4.1](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm#L1429-L1451)
can retry a failed Metal drawable; [Apple's default timeout](https://developer.apple.com/documentation/quartzcore/cametallayer/allowsnextdrawabletimeout)
is one second. No per-call trace or matching exhausted-retry error shows
that this happened here. The similar durations alone do not establish it.

## Apply and validate

In a **separate clean Vita3K source checkout** at
`92bda548f3aa0691102471a72725587e00676383`, use these commands. Supply an
absolute path for `tools`, not a path to the Vita game build. The first
patch is optional; applying the two in this order was verified in isolation.

```sh
tools=/absolute/path/to/tools/emu_hitch
git apply --check "$tools/vita3k-92bda548.patch"
git apply "$tools/vita3k-92bda548.patch"
git apply --check "$tools/long_trace/vita3k-92bda548-trace.patch"
git apply "$tools/long_trace/vita3k-92bda548-trace.patch"
git diff --check
python3 -I -B "$tools/long_trace/verify.py" "$PWD"
```

For a prepared native Vita3K build, also pass
`--compile-db /path/to/that/checkout/build/compile_commands.json`.
This checks both real changed translation units, in a fresh temporary
directory, without emitting a production object or replacing a tested
binary. Standard dependencies: Python, Git, C++17; native syntax validation
also needs that build's configured native compiler and dependencies.

For an approved future replay, use a separately built and signed host
executable in a disposable copied emulator installation. Supply
`VITA3K_LONG_PRESENT_TRACE=1` to the **host process**, not the game's
`env.txt`. Absent, `0` or any other value disables logging and clock reads.
Keep the checkpoint flag and all other inputs fixed. This is a diagnosis
run, not another performance comparison; only the original nontrace
executable hashes apply to the reported A/B. Do not touch the default
emulator installation or a player's cache, settings or saves.

## Reading the trace

- A single enable marker is expected. Only returned calls taking strictly
  more than 50,000 microseconds log. A control line at exit is not gameplay.
- GXM caller: `gxm.add_entry_inclusive` includes `gxm.push` and, in
  double-buffer mode only, `gxm.double_buffer_wait_empty`. An outer wait
  without either child leaves setup or other calls unaccounted for.
- Display consumer: `display.wait_old`, `display.wait_new` (0 ready, 1
  timed out, 2 shutdown), and `display.guest_callback` (guest return value).
  Vulkan screen path: `vk.acquire`, `vk.swap_fence`, `vk.screen_submit`,
  and `vk.present`. Numeric Vulkan results are the original signed result;
  `void` means the configured API exposes no result here, **not** proof of
  a successful GPU submission. These are calling-thread times, not GPU time.
- Read `elapsed_us` and `ended_ago_us`: subtract the latter from the
  host log timestamp for an approximate event end (millisecond log
  resolution), then subtract elapsed duration for approximate start.
  Events drain after the owning call/iteration; nested slow logs don't
  inflate the inclusive measurement. Different threads/phases can overlap:
  do not sum their times. An exception or never-returning call need not log.
- For a reproduced near-second guest stall, align host/guest clocks with
  independent archive-read markers first. Compare overlapping intervals and
  the C186.2 whole-frame GXM metric, when sampled. A slow acquire/submit/
  present alone does **not** prove a drawable retry. A driver/Metal trace
  is needed for that. If the hitch does not recur, or no measured phase
  overlaps, record an inconclusive result rather than changing timeouts.
