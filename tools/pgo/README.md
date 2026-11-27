# Profile-guided optimisation of the game code and the renderer

The game units (`decomp/`, 1,834 of them) are built by `tools/vgx_build/build.py` as ThinLTO bitcode. With a profile,
Clang compiles them with `-fprofile-use` (branch weights, hot and cold functions, call counts and indirect-call targets
for the inliner and the code layout), and ThinLTO uses the same counts across units. The renderer (the vgx front end and
back end in `platform/vgx/src` and the GX glue in `platform/src/gx`, built by the runtime stage) takes the same profile. The profile is recorded on 32-bit
ARM Linux, not on the Vita: the armhf harness (`armhf/`, run under qemu-arm) compiles the same game units with the same
code-generation flags as the Vita build, and a Clang IR profile is keyed by function name and a hash of the function's
control-flow graph, which are the same on both targets. No Vita is needed to record it.

`smg.profdata.gz` is the profile recorded for this tree (see "What it covers"). It is off by default.

## Build with a profile

```sh
python3 -I tools/vgx_build/build.py all --build-dir /build --jobs 4 --pgo-profile tools/pgo/smg.profdata.gz
```

`--pgo-profile` (default none: the plain build) takes an indexed `.profdata`, plain or gzipped. The driver writes it to
`<build>/pgo/<hash>.profdata` and passes `-DVITA_PGO_PROFILE=` to CMake for the source stage, so every game unit gets
`-fprofile-use=...`; the LTO stage reuses those compile commands. The runtime stage (`vita_runtime_gen_vgx.py`, environment
`VITA_PGO_PROFILE`) adds `-fprofile-use=...` to the renderer's units only: `platform/vgx/src/*.cpp` and
`platform/src/gx/*.cpp` (the rest of the runtime has no counts in the profile). The file name changes with the content, so a
new profile rebuilds those units.

A profile names functions with internal linkage (`static`, anonymous namespaces; about 6 % of the executed functions) as
`<source path>;<name>`. `smg.profdata.gz` was written with the path `/source` (the container mount of docs/BUILDING.md);
when the checkout is somewhere else, the driver rewrites the paths with `llvm-profdata` (LLVM 18 tools, `llvm-profdata` or
`llvm-profdata-18` on `PATH`) before use. Without that, those functions simply get no profile.

## What it covers

Recorded with the armhf harness at Clang 18.1.3 from these routes (game units at `-O2`, renderer units at `-O3` with the
Vita runtime flags, both instrumented; the rest of the platform layer is not):

| route | what runs |
| --- | --- |
| `opening.txt` (140 s) | boot, strap screens, title, the 3.0 entry screen (Single Player), file select, storybook, Peach's letter, castle path |
| `tour.txt` | AstroGalaxy (Comet Observatory), EggStarGalaxy (Good Egg) scenarios 1 and 2, HeavenlyBeachGalaxy (water), KoopaBattleVs3Galaxy |
| `tour2.txt` | OceanRingGalaxy (water), SurfingLv1Galaxy, HoneyBeeKingdomGalaxy, IceVolcanoGalaxy, BattleShipGalaxy, FactoryGalaxy |
| `tour3.txt`, `tour4.txt` | KoopaBattleVs1Galaxy; TamakoroExLv1Galaxy, StarDustGalaxy, FlipPanelExGalaxy, TriLegLv1Galaxy |
| `tour5.txt` (335 s) | HoneyBeeKingdomGalaxy:1 alone for 90 s: with its 45 s in `tour2` the main benchmark carries three times the weight of an ordinary stage |
| `tour6.txt` (565 s) | EggStarGalaxy:1, CosmosGardenGalaxy, PhantomGalaxy, SandClockGalaxy (45 s each) |
| `tour7.txt` (685 s) | HellProminenceGalaxy, CannonFleetGalaxy, KoopaBattleVs2Galaxy, OceanPhantomCaveGalaxy, ReverseKingdomGalaxy (45 s each) |

(`tour5` to `tour7` were written on 10 Nov 2026 for the next recording and have not been played yet: the durations are
76 s + 115 s a stage, the measure of `tour2`/`tour4`; a boss that does not end holds a tour, as `tour3` showed.)

Each tour stage is 45 s of gameplay with a wandering pad script (stick with turns, spin, jump, pointer sweeps and star-bit
shots). The routes were merged with `--sparse`: a function that never ran has no record, so Clang treats it as unknown
(compiled normally) and not as cold. Areas that no route reached (most galaxies, later boss phases, the ending) are therefore
neither optimised for speed nor for size.

The renderer runs the way the Vita runs it: the harness is built with the Vita's libstdc++ headers, its OpenGL ES header
(with the mask-guard macros around the draw calls) and the quiet runtime defines, and the routes are played with the Vita
release defaults of `vita/vgx-game/entry.cpp` (front-end split on, and so on). Each of these changes the control-flow graph
of hot functions, and a function whose graph differs on the Vita loses its counts there. The GL calls go to a stub, so the
driver is not profiled; renderer functions that exist only on the Vita (`__vita__` branches, the auto-quality sampler's
Vita path) get no counts and are compiled normally.

## Record a profile again

A profile goes stale when the game code changes: a function whose control flow changed loses its counts (a
`-Wprofile-instr-out-of-date` warning in `logs/source.log`), everything else keeps them. Re-record for a release.

0. Tools (Ubuntu 24.04; installed in the cloud workspace on 18 Nov 2026): `qemu-user-static` (its binary is `qemu-arm-static`; the
   scripts take `qemu-arm` or, failing that, `qemu-arm-static`), `g++-arm-linux-gnueabihf`, `libc6-dev-armhf-cross`,
   `libstdc++-13-dev-armhf-cross` (the sysroot `/usr/arm-linux-gnueabihf`), plus clang 18 / llvm-profdata-18 and VITASDK. Since 3.0 the
   harness also takes the Vita SDK's `psp2/` headers for the back end (an overlay of links in the build directory, nothing else of the SDK).
   `tools/pgo/record_all.sh <work-dir> <cooked-dir>` does steps 1 to 4 (`ROUTES="opening:140"` for one route).
1. Profile runtime for armhf (the Ubuntu clang package ships only the x86 runtimes):
   `tools/pgo/build_profile_rt.sh <dir>` makes `<dir>/libclang_rt.profile-armhf.a` from the pinned LLVM 18.1.3 sources.
2. Instrumented harness (the renderer is built the way the Vita builds it: the console's libstdc++ headers and GL header,
   the runtime defines, `-O3`; `ARMHF_VGX_BACKEND=gl-stub` compiles the real GL back end against a stub, so its code is
   profiled although no GPU is there):
   ```sh
   python3 -I tools/vgx_build/make_case_shim.py . <case-shim> && export CPATH=<case-shim> VITASDK=/opt/vitasdk
   cmake -S armhf -B <obj> -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/armhf/toolchain.cmake -DCMAKE_BUILD_TYPE= \
         -DARMHF_PGO_GEN=ON -DARMHF_PGO_GEN_RENDERER=ON -DARMHF_VGX_BACKEND=gl-stub -DARMHF_VITA_RUNTIME_FLAGS=ON \
         -DARMHF_PROFILE_RT=<dir>/libclang_rt.profile-armhf.a
   nice ninja -C <obj> -j2 game
   ```
   The build is the usual armhf one (about 40 min at `-j2`) with `-fprofile-generate` on the game units and, with
   `ARMHF_PGO_GEN_RENDERER`, on `platform/vgx/src` and `platform/src/gx`, plus `armhf/pgo/pgo_exit.cpp`, which writes the
   profile from `_exit` (the runner ends with it). `VITASDK` is needed for the console's libstdc++ headers.
3. Run the routes under qemu-arm with the console's release environment (`tools/pgo/vita_env.sh`, which
   `tools/pgo/gen_vita_env.py` writes from the defaults of `vita/vgx-game/entry.cpp`: run it again after every change of those
   defaults; on 10 Nov 2026 the old file lacked 158 of the console's switches that are on by default). Since 18 Nov the generator
   also applies the Settings start-up of a fresh console (`port::settings::ApplyStartup` with the default Config, built from
   `tools/pgo/settings_env.cpp` on the host): the picture mode's fan-out changes about 40 switches and adds about 25 (Performance:
   `GXV_PICTURE_MODE=60 GXV_AUTOQ_TARGET=60 GXV_HRW=5 GXV_CPULOD=2 GXV_FE_GOV=3 ...`), which entry.cpp's defaults alone do not show.
   `--picture-mode 30` gives Quality ("Full Detail"), `--entry-only` the old file. The game data is read in place, never copied;
   a tree in the Vita's bucket layout (`files/ObjectData/_bk00/...`, any tree the console build or Vita3K has run on) is read
   through a flat view of links (`tools/pgo/flat_view.py <cooked> <view>`; `record_all.sh` makes it), since the harness's file
   layer has no buckets. About 1.6 to 3 s of wall time per game second (qemu without the counting plugin):
   ```sh
   . tools/pgo/vita_env.sh; export EXTRA_ENV="$VITA_ENV" QEMU_ARM=<qemu-arm>
   tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/opening.txt <runs>/opening 140
   tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/tour.txt    <runs>/tour    485
   ```
   (`tour2.txt` 805, `tour3.txt` 525, `tour4.txt` 425, `tour5.txt` 335, `tour6.txt` 565, `tour7.txt` 685.) The game runs on a virtual clock, so the counts do not depend on how
   fast qemu is. `make_tour_route.py` writes the tour routes. Since 3.0 an entry screen (Single Player / Multiplayer / Settings)
   comes between the title and the file select: every route taps A on Single Player at 30 s (its cursor's start; a pointer moved
   onto a pill would choose that pill) and everything after it is 5 s later than before. `run_route.sh` exits 1 when no picture
   was saved (the routes save none); the route's own `#!env` lines win over `EXTRA_ENV`.
4. Merge, make the source paths neutral, compress:
   ```sh
   llvm-profdata-18 merge --sparse -o all.profdata <runs>/*/route.profraw
   llvm-profdata-18 merge --text -o all.txt all.profdata
   sed -E -i 's#^<checkout>/(decomp|platform|vita)/#/source/\1/#' all.txt   # the path the harness was built in
   llvm-profdata-18 merge -o smg.profdata all.txt && gzip -9 -n smg.profdata && mv smg.profdata.gz tools/pgo/
   ```
   The profile holds function names, CFG hashes and counts only; nothing of the game data. Check that it names no home
   directory: `zcat tools/pgo/smg.profdata.gz | llvm-profdata-18 merge --text -o - /dev/stdin | grep -c /home` is 0.
5. Check how much of it applies to the Vita target, without running anything:
   ```sh
   # game units: a random sample compiled for both targets with -fprofile-generate (IR only), names and hashes compared
   python3 -I tools/pgo/match_report.py --vita-db <vita-source>/compile_commands.json \
       --armhf-db <obj>/compile_commands.json --profile all.profdata --units 150
   # renderer units: the runtime stage's own commands (tools/vgx_build/vita_runtime_gen_vgx.py writes <rt>/build.ninja)
   python3 -I tools/pgo/match_report.py --vita-ninja <rt>/build.ninja --armhf-db <obj>/compile_commands.json \
       --select '/platform/vgx/src/|/platform/src/gx/' --units 0 --flags "" --profile all.profdata
   # the definitive test: compile the runtime stage's units with the Vita's flags and count Clang's hash-mismatch warnings
   python3 -I tools/pgo/check_vita_profile.py --ninja <rt>/build.ninja --profile <build>/pgo/<hash>.profdata
   ```
   (`--profile` of `match_report.py` takes the profile with the checkout's own paths, here before step 4's `sed`; the profile
   for `check_vita_profile.py` is the file `build.py` prepared.)

## How stale is the profile

`tools/pgo/scan_units.py` compiles every unit of the console build (the compile commands of `<build>/vita-source` and the runtime stage's
`build.ninja`, read only, nothing kept) with `-fprofile-use` and records the functions each defines and Clang's hash-mismatch
warnings; `tools/pgo/stale_report.py` turns that and the text form of the profile into the share of the profile's weight that still
matches (game and renderer units apart) and the list of the hottest functions that lost their counts. `tools/pgo/record_all.sh` is the
whole recording in one script (not yet run).

## Measure in the harness

`-DARMHF_DECOMP_BITCODE=ON` builds the harness with the game units as ThinLTO bitcode linked by `ld.lld -r --lto-O2`, like
the Vita build (about 40 min of compiling and a few minutes of link); `-DARMHF_PGO_USE=<profdata with this checkout's paths>`
adds `-fprofile-use` to them and `-DARMHF_PGO_USE_RENDERER=<same file>` to the renderer units. Run both with the harness's
qemu runner (the instruction-counting plugin, `PETARI_QEMU_MARK=1` frame marks) on the same route and compare the per-frame
instruction counts per thread: the game thread (`game-main`) and the renderer's threads (the front-end player, the back end
`vgx-render` and the split finishing thread). Counts, not time: qemu models neither cycles nor caches, and the GL calls go to
a stub, so the driver's cost is not in them.

Compare frames in which both runs did the same work. A route is deterministic only up to the point where the renderer's speed
changes what the game does (frame slip, auto quality); later frames differ between builds, so a whole-run mean mixes
different scenes. Dividing the renderer's instructions by the game thread's over the same frame window removes most of it.

Measured at the head that added the renderer profile (plain `-O2` game units, no LTO; renderer units `-O3` with the Vita
runtime flags; frames 4800 to 7000 of the EggStarGalaxy route and 4800 to 6000 of HeavenlyBeachGalaxy, Vita release
environment, 3 s of game per 100 frames):

| route | renderer threads, M instructions per frame | renderer / game thread | whole process |
| --- | --- | --- | --- |
| EggStarGalaxy | 5.027 to 4.806 (-4.4 %) | 2.311 to 2.236 (-3.2 %) | 7.909 to 7.744 (-2.1 %) |
| HeavenlyBeachGalaxy | 4.607 to 4.463 (-3.1 %) | 2.256 to 2.188 (-3.0 %) | 7.485 to 7.344 (-1.9 %) |

ThinLTO over the renderer units (one `ld.lld -r` object of the vgx units and `platform/src/gx`) was tried and measured in
the same way: no change in the renderer / game-thread ratio (2.299 to 2.292 on EggStarGalaxy), so the harness has no option
for it and the console build does not use it.

## How well the profile fits the console build

Measured with `match_report.py` and `check_vita_profile.py` on the renderer units of the runtime stage (22 units that exist in
both builds): 1,016 of the 1,126 instrumented functions have the same name and CFG hash on both targets, and 99.6 % of the
executed counter weight in functions present on both applies to the console (451 functions matched, 11 not). Compiling the
console's own commands with `-fprofile-use` gives no hash-mismatch warning in 22 of 25 units. The 11 warnings are in three
units and are code that only one target has: `vgx_galaxy.cpp` (the `__vita__` branches: thread pinning and priorities, and the
harness's virtual-clock hooks, in `PlayerMain`, `PaceMode`, `FrameSink`, `RenderEntry`, `SplitThreadInit`,
`port_vgx_publish`, `port_gx_select_backend`), `vgx_autoq.cpp` (the Vita clock) and one static initialiser. Those functions are
compiled without counts, as if there were no profile. Keeping the harness equal to the console build was the work that made
this fit: the console's libstdc++ headers, GL header (mask-guard macros; the stub's functions with a result are external
symbols, as the driver's are, since a result the compiler can see is folded into the caller's branches before a profile is
instrumented), `__VGX_VITA__`, the quiet runtime defines, the console's release environment, and the stub answering the
uniform queries from the shaders' source instead of a shortcut.

Re-checked on 18 Nov 2026 (pgotool31) at integ-3.1 601336ad: the harness had drifted from the console build in three ways that each
change the control flow a profile is keyed on, and a fresh harness profile still lost the counts of `EmitDraw`, `ClippingActorHolder::movement`,
`KCollisionServer::checkSphereWalk`, `LiveActor::calcAnim`, `ModelManager::calcView` and so on: the console compiles with
`-Xclang -no-enable-noundef-analysis` (InstCombine folds an i1 select into an and without it), the game units' port code now uses `std::`
templates whose GCC 13 (cross) and GCC 15 (Vita SDK) forms differ (`std::find`), and the Vita's integer-type macros. `ARMHF_VITA_MATCH`
(default ON with `ARMHF_VITA_RUNTIME_FLAGS`) adds them; the game and `platform/src/gx` units stay at -O2 (their Vita commands carry
`-O3 ... -O2`, the last wins; at -O3 the pre-inliner decides differently). Measured with `scan_units.py` on 64 hot units (the 35 renderer
units and 29 game units) with an opening-route profile: 192 hash mismatches before the option, 28 with it (26 renderer, 2 game), 1,846 of
the 1,874 functions with counts matching. What is left is code that only one target compiles: `vgx_galaxy.cpp` (as above), the back
end's `NowMs()` (`#ifdef __vita__`: `sceKernelGetProcessTimeWide`, inlined into `ExecuteFrame`, `StageNote`, `StageFrameBegin`),
`BeginVertices` (`#if defined(__vita__)`), and the bounded waits of `port/wait_bound.h` (`AsyncBatch::Ensure`, `JpaPoolLock`), which
nap with `std::this_thread` in the harness.

## Record on the console (instrumented eboot)

The profile above is stale (about 54 % of the renderer weight still matches) and cannot be re-recorded where there is no
armhf toolchain, qemu or the game data. The other way is to build the console eboot itself with `-fprofile-generate`, play a
tour on the console and bring the counters back. Nothing changes without the opt-in: the compile commands of every unit
are identical (checked on all 1835 units of the source stage and on the link stage).

Build (environment variable, read by CMake, `build.py` and `vita_runtime_gen_vgx.py`; it cannot be combined with
`--pgo-profile` / `VITA_PGO_PROFILE`, which it ignores or refuses):

    VITA_PGO_GENERATE=1 python3 -I tools/vgx_build/build.py all --build-dir <separate dir> --jobs 4

Use a build directory of its own: every unit is compiled again, and the release build in `/build` needs a full rebuild after
it otherwise. The link adds `vita/pgo/` (the profile runtime of LLVM 18.1.3 from `vita/pgo/compiler-rt/`, a platform layer
for the section start/stop symbols, the writer thread) and `-DGXV_PGO_GENERATE=1`. Eboot about 11.9 MB (7.5), about 12 MB
more RAM, about 2 to 3 times slower (estimates, to be confirmed by the first run).

What it writes (ux0:data/smg/): `pgo/default-<n>.profraw` (last 3 kept, temp name then rename), `pgo.profraw` (the same
bytes, the file to fetch), `pgo/last.txt`. Dump triggers: every `GXV_PGO_DUMP_S` seconds (default 120, 0 = never), when the
file `ux0:data/smg/pgo.dump` exists (removed afterwards), and at exit / abort. The thread `smg_pgodump` has the lowest priority.
The log (log.txt) has the lines `pgo: INSTRUMENTED build` and `pgo: dump`.

Procedure, env files and the pad script: `agents/pgovita/jobs/README.txt` (stages per run: `env.run-a..f.txt`; pad script
`input-wander.txt`, made by `tools/pgo/make_console_route.py`).

Merge and use:

    tools/pgo/merge_console.sh <dir> run-a.profraw run-b.profraw ...     # <dir>/smg.profdata.gz, paths as /source/...
    python3 -I tools/pgo/scan_units.py <rewritten profile> scan.jsonl && python3 -I tools/pgo/stale_report.py scan.jsonl
    cp <dir>/smg.profdata.gz tools/pgo/smg.profdata.gz    # then the usual docs/SOURCE-MANIFEST.json refresh

Expect at least 95 % of the weight to match. Then build as usual with `--pgo-profile tools/pgo/smg.profdata.gz`.

Build paths: the game units are compiled through the CMake cache path (`/source/source`), the runtime units through the
resolved checkout, and clang keys static functions by `<source path>;<name>`. `build.py` now rewrites the game's profile
for the cache path (`cmake_source_root`) and the runtime's for the resolved root; before, the first applied to both and the
static functions of the other never matched (696 functions). Any script that compiles the runtime stage on its own
(a script outside the tree that takes `build/pgo/bebd4ee0ec00.profdata`, the unrewritten one) has to pass the profile rewritten
for the resolved root too.

## Prove a profile build (strk31)

The emulator's determinism check cannot judge a PGO build alone (pacing follows its wall clock). Galaxy's route is step-locked (game turns) and `GXV_UNDRAWN_LEAN_HASH` logs a state hash by turn:
run plain, plain2 and PGO on the same route and read them with `tools/release_gates.py twin` (PASS / FAIL / DET INCONCLUSIVE, exit 2: not a pass). The other final-build gates, the PGO eboot's own, and the
loop-unswitching note (clang, not GCC) are in `docs/RELEASE-3.0-CHECKLIST.md` sections 4b and 4c.
