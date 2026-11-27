# C186.1 Honeyhive emulator comparison

Run `python3 -B tools/c186_emu_ab/replay.py --self-test` for the profile
guard, with no game data or Docker. It rejects forced packs, a nonretail
vertex profile, and applying the assist override to the wrong arm.

The optional full replay needs the local C186.1 package, licensed game data,
test saves and firmware, Vita3K AppImage 4124, and the existing offline
Linux ARM64 emulator image. The runner looks for the documented workspace
layout; edit the input paths in a private copy for a different layout.

With an exclusive emulator slot, run:

```sh
python3 -B tools/c186_emu_ab/replay.py --out /path/to/new/run A B
python3 -B tools/c186_emu_ab/compare.py /path/to/new/run
```

No online services, host display, physical GPU, normal emulator profile or
Vita are used. Large input data stays read-only. Each arm has a fresh
container, empty guest writable shader cache and empty emulator/driver cache.
Both receive the same packaged seed, hashed input script, saves, firmware
and binary. B adds only `GXV_HRW=0` to A's retail Performance profile.
Output folders must be new. Inspect the cropped game thumbnails as well as
the numerical nonblack guard: they are **not** frame-matched visual evidence.

One observation runs from the first controllable turn through the first
retry. Only complete FPS samples inside that traversal are counted: omit
windows straddling the first anchor or the next load and one second before
the inferred load boundary. Collect the second visual witness outside that
interval: wait at least seven seconds after the retry's controllable anchor
and require two consecutive color/nonblack probes from inside the game
viewport. If the return remains black for 35 seconds, fail the run rather
than accepting the wipe. Rates are time-weighted; hitch counts cover
different durations and are not directly comparable. The comparator
withholds a delta if inputs, mode, snapshots, anchors, or sample coverage
fail its checks. Both arms must have positive measured game progress and
contiguous complete windows. The larger sampled turn count must be within
8% of the smaller. The first and last retained sample edges must also agree
within eight percentage points of their respective anchor-to-retry intervals
(or one second of the shorter interval, whichever is larger). This checks
guest-time coverage, not exact turn alignment or matching pixels. One A/B is an
exploratory software-renderer observation, not a repeatability study or a
projection of Vita FPS, GPU time, or first-play pause savings.
