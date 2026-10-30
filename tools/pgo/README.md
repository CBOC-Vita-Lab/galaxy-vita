# Profile-guided optimisation of the game code

The game units (`decomp/`, 1,834 of them) are built by `tools/vgx_build/build.py` as ThinLTO bitcode. With a profile,
Clang compiles them with `-fprofile-use` (branch weights, hot and cold functions, call counts and indirect-call targets
for the inliner and the code layout), and ThinLTO uses the same counts across units. The profile is recorded on 32-bit
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
`-fprofile-use=...`; the LTO stage reuses those compile commands. The file name changes with the content, so a new
profile rebuilds the game units. The runtime and the renderer never see it.

A profile names functions with internal linkage (`static`, anonymous namespaces; about 6 % of the executed functions) as
`<source path>;<name>`. `smg.profdata.gz` was written with the path `/source` (the container mount of docs/BUILDING.md);
when the checkout is somewhere else, the driver rewrites the paths with `llvm-profdata` (LLVM 18 tools, `llvm-profdata` or
`llvm-profdata-18` on `PATH`) before use. Without that, those functions simply get no profile.

## What it covers

Recorded with the armhf harness at Clang 18.1.3 from these routes (all `-O2`, game thread only, the platform layer and the
renderer are not instrumented):

| route | what runs |
| --- | --- |
| `opening.txt` (135 s) | boot, strap screens, title, file select, storybook, Peach's letter, castle path |
| `tour.txt` | AstroGalaxy (Comet Observatory), EggStarGalaxy (Good Egg) scenarios 1 and 2, HeavenlyBeachGalaxy (water), KoopaBattleVs3Galaxy |
| `tour2.txt` | OceanRingGalaxy (water), SurfingLv1Galaxy, HoneyBeeKingdomGalaxy, IceVolcanoGalaxy, BattleShipGalaxy, FactoryGalaxy |
| `tour3.txt`, `tour4.txt` | KoopaBattleVs1Galaxy; TamakoroExLv1Galaxy, StarDustGalaxy, FlipPanelExGalaxy, TriLegLv1Galaxy |

Each tour stage is 45 s of gameplay with a wandering pad script (stick with turns, spin, jump, pointer sweeps and star-bit
shots). 13,658 of the 36,385 instrumented functions ran. The routes were merged with `--sparse`: a function that never ran
has no record, so Clang treats it as unknown (compiled normally) and not as cold. Areas that no route reached (most
galaxies, later boss phases, the ending) are therefore neither optimised for speed nor for size.

## Record a profile again

A profile goes stale when the game code changes: a function whose control flow changed loses its counts (a
`-Wprofile-instr-out-of-date` warning in `logs/source.log`), everything else keeps them. Re-record for a release.

1. Profile runtime for armhf (the Ubuntu clang package ships only the x86 runtimes):
   `tools/pgo/build_profile_rt.sh <dir>` makes `<dir>/libclang_rt.profile-armhf.a` from the pinned LLVM 18.1.3 sources.
2. Instrumented harness:
   ```sh
   python3 -I tools/vgx_build/make_case_shim.py . <case-shim> && export CPATH=<case-shim>
   cmake -S armhf -B <obj> -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/armhf/toolchain.cmake -DCMAKE_BUILD_TYPE= \
         -DARMHF_PGO_GEN=ON -DARMHF_PROFILE_RT=<dir>/libclang_rt.profile-armhf.a
   nice ninja -C <obj> -j2 game
   ```
   The build is the usual armhf one (about 40 min at `-j2`) with `-fprofile-generate` on the game units and
   `armhf/pgo/pgo_exit.cpp`, which writes the profile from `_exit` (the runner ends with it).
3. Run the routes under qemu-arm; the game data is read in place, never copied. About 3 s of wall time per game second
   (qemu without the counting plugin):
   ```sh
   QEMU_ARM=<qemu-arm> tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/opening.txt <runs>/opening 135
   QEMU_ARM=<qemu-arm> tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/tour.txt    <runs>/tour    480
   ```
   (`tour2.txt` 800, `tour3.txt` 520, `tour4.txt` 420.) The game runs on a virtual clock, so the counts do not depend on how
   fast qemu is. `make_tour_route.py` writes the tour routes.
4. Merge, make the source paths neutral, compress:
   ```sh
   llvm-profdata-18 merge --sparse -o all.profdata <runs>/*/route.profraw
   llvm-profdata-18 merge --text -o all.txt all.profdata
   sed -i 's|^<checkout>/decomp/|/source/decomp/|' all.txt       # the path the harness was built in
   llvm-profdata-18 merge -o smg.profdata all.txt && gzip -9 -n smg.profdata && mv smg.profdata.gz tools/pgo/
   ```
   The profile holds function names, CFG hashes and counts only; nothing of the game data.
5. Check how much of it applies to the Vita target, without running anything (compiles a random sample of units for both
   targets with `-fprofile-generate`, IR only, and compares names and hashes):
   ```sh
   python3 -I tools/pgo/match_report.py --vita-db <vita-source>/compile_commands.json \
       --armhf-db <obj>/compile_commands.json --profile all.profdata --units 150
   ```
   (`--profile` takes the profile with the checkout's own paths, here before step 4's `sed`.)

## Measure in the harness

`-DARMHF_DECOMP_BITCODE=ON` builds the harness with the game units as ThinLTO bitcode linked by `ld.lld -r --lto-O2`, like
the Vita build (about 40 min of compiling and a few minutes of link); `-DARMHF_PGO_USE=<profdata with this checkout's paths>`
adds `-fprofile-use`. Run both with the harness's qemu runner (the instruction-counting plugin) on the same route and
compare the per-frame instruction counts of the game thread (`game-main`). Counts, not time: qemu models neither cycles nor
caches.
