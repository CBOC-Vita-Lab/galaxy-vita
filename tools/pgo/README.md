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
| `opening.txt` (135 s) | boot, strap screens, title, file select, storybook, Peach's letter, castle path |
| `tour.txt` | AstroGalaxy (Comet Observatory), EggStarGalaxy (Good Egg) scenarios 1 and 2, HeavenlyBeachGalaxy (water), KoopaBattleVs3Galaxy |
| `tour2.txt` | OceanRingGalaxy (water), SurfingLv1Galaxy, HoneyBeeKingdomGalaxy, IceVolcanoGalaxy, BattleShipGalaxy, FactoryGalaxy |
| `tour3.txt`, `tour4.txt` | KoopaBattleVs1Galaxy; TamakoroExLv1Galaxy, StarDustGalaxy, FlipPanelExGalaxy, TriLegLv1Galaxy |

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
3. Run the routes under qemu-arm with the console's release environment (`tools/pgo/vita_env.sh`); the game data is read in
   place, never copied. About 3 s of wall time per game second (qemu without the counting plugin):
   ```sh
   . tools/pgo/vita_env.sh; export EXTRA_ENV="$VITA_ENV" QEMU_ARM=<qemu-arm>
   tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/opening.txt <runs>/opening 135
   tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/tour.txt    <runs>/tour    480
   ```
   (`tour2.txt` 800, `tour3.txt` 520, `tour4.txt` 420.) The game runs on a virtual clock, so the counts do not depend on how
   fast qemu is. `make_tour_route.py` writes the tour routes.
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
