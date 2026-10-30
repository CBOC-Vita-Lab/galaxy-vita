# Profile-guided optimisation of the game code

The game units (`decomp/`, 1,834 of them) are built by `tools/vgx_build/build.py` as ThinLTO bitcode. With a profile,
Clang compiles them with `-fprofile-use` (branch weights, hot/cold functions, call counts and indirect-call targets
for the inliner and the layout), and ThinLTO uses the same counts across units. The profile is recorded on 32-bit ARM
Linux, not on the Vita: the armhf harness (`armhf/`, run under qemu-arm) compiles the same game units with the same
code-generation flags as the Vita build. No Vita is needed to record it.

## Build with a profile

```sh
python3 -I tools/vgx_build/build.py all --build-dir /build --jobs 4 --pgo-profile tools/pgo/smg.profdata
```

`--pgo-profile` (default none: the plain build) takes an indexed `.profdata`. The driver copies it to
`<build>/pgo/<hash>.profdata` and passes `-DVITA_PGO_PROFILE=` to CMake for the source stage; the LTO stage reuses those
compile commands. The profile's name changes with its content, so a new profile rebuilds the game units. The runtime and
the renderer never see it. A profile recorded in another checkout is rewritten with `llvm-profdata` (needs the LLVM 18
tools) so that the functions with internal linkage, which a profile keys by source path, still match.

Without `--pgo-profile` nothing changes.

## Record a profile

1. Profile runtime for armhf (the Ubuntu clang package ships only x86 runtimes):
   `tools/pgo/build_profile_rt.sh <dir>` makes `<dir>/libclang_rt.profile-armhf.a` from the LLVM 18.1.3 sources.
2. Instrumented harness (same steps as the usual armhf build, plus the options):
   ```sh
   export CPATH=<case-shim>   # python3 -I tools/vgx_build/make_case_shim.py . <case-shim>
   cmake -S armhf -B <obj> -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/armhf/toolchain.cmake -DCMAKE_BUILD_TYPE= \
         -DARMHF_PGO_GEN=ON -DARMHF_PROFILE_RT=<dir>/libclang_rt.profile-armhf.a
   nice ninja -C <obj> -j2 game
   ```
   Only the game units are instrumented; the platform layer and the renderer are not.
3. Run the routes (`tools/pgo/routes/`; game data is read in place, never copied):
   ```sh
   QEMU_ARM=<qemu-arm> tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/opening.txt <runs>/opening 135
   QEMU_ARM=<qemu-arm> tools/pgo/run_route.sh <obj>/game <cooked-dir> tools/pgo/routes/tour.txt    <runs>/tour    <seconds>
   ```
   `opening.txt`: title, file select, storybook, Peach's letter, castle path, festival plaza. `tour.txt`
   (`make_tour_route.py`): the menus, then Comet Observatory, Good Egg (two scenarios), Beach Bowl (water), a Bowser
   battle, each for 45 s of gameplay with a wandering pad script. The game runs on a virtual clock, so the counts do
   not depend on how fast qemu is.
4. Merge. Functions that never ran in any route are dropped (`--sparse`) so that Clang treats them as unknown and
   not as cold, and the routes are weighted equally:
   ```sh
   llvm-profdata-18 merge --sparse -o tools/pgo/smg.profdata <runs>/*/route.profraw
   ```
5. Check how much of it applies to the Vita target, without running anything:
   ```sh
   python3 -I tools/pgo/match_report.py --vita-db <vita-source>/compile_commands.json \
       --armhf-db <obj>/compile_commands.json --profile tools/pgo/smg.profdata --units 120
   ```

A profile goes stale when the game code changes (a function whose control flow changed loses its counts, with a
`-Wprofile-instr-out-of-date` warning in `logs/source.log`; everything else keeps them). Re-record it for a release.

## Measure in the harness

`ARMHF_DECOMP_BITCODE=ON` builds the harness with the game units as ThinLTO bitcode linked by `ld.lld -r --lto-O2`, like
the Vita build; `ARMHF_PGO_USE=<profdata>` adds `-fprofile-use`. Run both with the harness's qemu runner (the instruction-counting
plugin) on the same route and compare the per-frame counts of the game thread.
