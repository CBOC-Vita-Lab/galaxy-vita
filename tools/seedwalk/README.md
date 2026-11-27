# seedwalk: the emulator harvest walk (plan-perf31 A.3, task A1 tooling half)

Walk every galaxy of the stage table in the headless Vita3K (the folder `$V3K` names: real shader compiler, retail profile), in both picture modes, and
collect what the game learned: the stage lists, the key list, the compiled programs and the `cover:` lines. Test tooling only: nothing here is
shipped, and no game data, firmware or compiled shader is ever copied into the repository (the output folder is refused when it is inside it).

| file | job |
|---|---|
| `plan.py` | splits the stage table into legs (every entry exactly once per pass), writes each leg's env file and scripted route, holds the env allow-list, reads a leg's log, makes follow-up legs, estimates wall time, `selftest` |
| `leg.sh` | runs ONE leg through `$V3K/run.sh` and copies its log and the card's delta into the leg's own folder |
| `collect.py` | gathers all legs into one harvest folder with a manifest and the gate's checks |
| `run_all.sh` | (harvprep31) the whole walk unattended: every leg one at a time, each in an empty network namespace, niced, resumable, with OUT/progress.txt |
| `test_run_all.sh` | (harvprep31) the dry run of the chain on the fixtures with a fake emulator (make -> run_all -> resume -> collect, and run_all's guards) |
| `fixtures/` | a fake log and a three-stop fake table for the dry run |

## Use

```
OUT=/path/outside/the/repo/sw31                      # its own folder (leg.sh refuses the repo, v3k/in, v3k/pref, v3k/home)
python3 -I tools/seedwalk/plan.py make --out $OUT    # 134 stops -> 71 legs; options below
python3 -I tools/seedwalk/plan.py list $OUT          # ids, kinds, estimates; DONE marks finished legs
export V3K=/path/to/v3k EBOOT=/path/to/candidate/eboot.bin   # V3K: the emulator setup (required); SEED=<shadercache folder> defaults to $V3K/pkgsc/shadercache
tools/seedwalk/leg.sh $OUT p60-menus-01              # FIRST: makes OUT/save-template (the PETARI_BOOT legs need a save)
tools/seedwalk/leg.sh $OUT p60-flow-01               # then any other leg, one at a time (run.lock)
python3 -I tools/seedwalk/plan.py resume $OUT        # follow-up legs for what an interrupted leg did not finish
python3 -I tools/seedwalk/collect.py collect $OUT    # -> $OUT/harvest/ ; exit 3 when a check fails
python3 -I tools/seedwalk/plan.py selftest           # host tests + the dry run (no emulator)
```
For a night: `run_all.sh OUT` (below). By hand: `for l in $(python3 -I plan.py list $OUT | awk '{print $1}'); do leg.sh $OUT $l; done` (finished legs are
skipped in a second) - but only inside a network namespace (`unshare -n`): run_all.sh does that for every leg.
`DRY=1 leg.sh ...` checks everything and prints the run.sh call without starting the emulator.

`plan.py make` options: `--stops-per-leg N` (default 4, at most 24: PETARI_FLOW's limit), `--passes p60:1.5,q30:1.0` (mode:zoom; Performance with the
handheld zoom 1.5, Quality with 1.0; use `p60:1.0,p60:1.5,q30:1.0,q30:1.5` for all four), `--hold TURNS` (default 5400 = 90 game s a stop, raised to
cover the survey hops), `--hop S --hop-max N` (survey hop interval 2 s, at most 40 hops), `--enum 0|1|2` (GXV_STAGE_ENUM census / compile in the same walk,
default 0), `--variant K` (the hold-out walk of A.5 item 2: rotated stop order, another leg split and a route turned a quarter), `--no-menus`,
`--menus-both`, `--table FILE` (default `tools/pack24/tour_legs.txt`), `--no-harvest-switch`.

## run_all.sh (harvprep31): the unattended night

```
V3K=<emulator setup> EBOOT=<candidate eboot.bin> SEED=<candidate shadercache> SAVE_TEMPLATE=<folder with saves/> tools/seedwalk/run_all.sh OUT
```
* One leg at a time (`leg.sh`), each under `nice -n $NICE` (10) and inside `unshare -n`: a new network namespace whose only interface is the loopback (left down,
  as the emu31b runs had it): no route, no DNS, so nothing of a leg reaches the live service. Checked from inside before leg.sh starts (a namespace with any other
  interface refuses the leg); the leg's environment is cleared (no proxy variables, no tokens). Needs root (unshare).
* Never beside a build or another emulator: a process matching `BUSY_PATTERN` (default `build31\.sh`) or a running Vita3K / Xvfb :77 holds the start of a leg
  (it waits, POLL s apart); a build that starts while a leg runs stops the leg (its whole session: run.sh's trap stops the emulator) and the leg runs again once
  the build ended (an interrupted run is not a try). The emulator takes 4-6 GB: a compile beside it can get it killed by the memory limit (seen once here).
* Low disk: below MIN_FREE_GB (2) free on OUT's file system it stops between legs, or stops the running leg, and exits 5 (`state: stopped: low disk`).
  `touch OUT/STOP` stops after the running leg; SIGTERM stops at once. Run it again with the same OUT to go on: DONE legs are skipped.
* After the pass: `plan.py resume` (one follow-up `<id>-r1` for the stops an incomplete leg did not finish; never a second one), another pass, at most
  RESUME_ROUNDS times; a leg is run at most MAX_TRIES (2) times. Then `collect.py collect` (a leg whose follow-up finished its stops counts as covered).
* `OUT/progress.txt`: state, the running leg and its time, legs done of all, the remaining time (the plan's estimate scaled by the walls seen so far), free disk,
  the last events, the last leg runs. `OUT/runall/events.log`, `legs.tsv`, `out/<leg>.try<N>.txt` (leg.sh's output), `collect.txt`.

## The harvest's own records and their evidence (harvprep31)

* With `GXV_SEEDWALK=1` a leg keeps up to 8,192 own records a stage (`kStageMaxRecsHarvest`; the release keeps 2,048): test19's menu list holds exactly 2,048,
  so before this no leg could add a menu record (`cap-full:`).
* Each leg leaves its draw evidence, `vgx_stagewarm.bin.drawn` (VGXSTD01, the player file's layout): every distinct draw of the leg, held package records
  included (the card's list cannot say which package records a leg drew: the package list is loaded into it at the start). collect.py copies it.
* `refill_pack.py --prune fill|all|off` (default fill) prunes a stage over 2,048 back by that evidence: every drawn record is kept (PRUNE-RANK.tsv ranks them by
  the legs that drew them), records no leg drew go, the latest in list order first, until the stage fits (fill) or all of them (all); list order kept; each
  dropped record in PRUNED-records.txt and a line per stage. A stage whose drawn records alone are over 2,048, or an over-full stage without evidence, fails.
* `sim_harvest.py` runs the whole 71-leg merge at test19's sizes on the host (`--old-cap`, `--no-evidence`, `--pkg-drawable`, `--prune` for the other cases).

## What a leg does

* Kinds. `flow`: `PETARI_BOOT=flow` + `PETARI_FLOW=<stops>` through the star select (hidden stars as the tour183 fix does); `stage`: `PETARI_BOOT=1` +
  `PETARI_STAGE=<stops>` for the five galaxies that have no star select (AstroDome, AstroGalaxy, LibraryRoom, PeachCastleFinal/Garden: `make_jobs.py`'s S0_G);
  `menus`: menu-fast input (title, file select, hub), `PETARI_FLOW_DOME=1` (dome, galaxy map, star select) and the harvest switch; `mpmenus`:
  `SMG_MP_DEMO=menus`: the game's own menu tour, which with `GXV_SEEDWALK=1` (harvprep31) also shows the Multiplayer menu's other screens that need no flow
  and opens the native Settings from the entry screen and shows each page once (MpHarvestTour.hpp; nothing chosen, changed, written or sent; the screens that
  sign in to the service are left out); `mpdemo`: `SMG_MP_DEMO=1` in EggStar, HoneyBeeKingdom, StarDust.
* In each flow/stage stop, at once: the pack24 action route (first use block, pointer + star-bit shots, one pause-menu open, crouch, ground pound,
  camera steps) then a form cycle every 300 turns (`mkroute.py` of `tools/pack24`, verified turn by turn); `PETARI_POWERUP` over bee, ice, spring,
  rainbow, boo, fire, flying (cure between); `PETARI_SURVEY_HOP` with `PETARI_SURVEY_GPOS=1` over every start entry and GeneralPos (up to `--hop-max`).
  The route starts at the first turn of play; it sends no button meant to skip the opening camera (A is a jump, and may skip one: unproven).
* The leg starts from the candidate seed (`SHADERPKG=$SEED`) and an empty card (`RESETCACHE=1`, `GXV_SHADER_CACHE_TAG=sw31`): the card's tagged folder is
  exactly what the seed lacked. The card's lists hold the package's records plus what the leg learned.
* Ended by `flow: done: all N entries` (or the stage tour's last `tour: jumping` + the hold) plus a wall cap of 3x the high estimate.
* Resumable: `legs/<id>/DONE` (plan fingerprint, eboot sha256, seed id) is checked first; an incomplete leg is run whole again, or `plan.py resume` makes
  `<id>-rN` for only the stops without a `flow: entry K of N (...) done` line. A crash costs one leg.

## The env allow-list (default deny)

Allowed: `PETARI_*`, `SMG_MP_DEMO(_SECS)` (1 | menus), keys ending `_LOG`, `GXV_PICTURE_MODE` (30 | 60), `GXV_STAGE_ENUM`, `GXV_SHADER_CACHE_TAG`, list paths
(under `ux0:data/smg/`), `GXV_SEEDWALK*`, and the emulator's non-text keys. **Refused with the reason**: every shader-text switch (`GXV_VSOUT`, `TEXVEC2`,
`TEVIN_CANON`, `FS_FINCLAMP`, ... the union of `refill_pack.py`'s `TEXT_SWITCHES` and a built-in copy), even at its retail value, and every forced pack
(`GXV_SHADERPACK`, `GXV_VITA_SEED`, `GXV_USERPACK`, `GXV_NO_SHADERLIST`). `plan.py make`, `leg.sh` and `collect.py` all check; `plan.py check-env FILE` alone.

## Harvest folder (collect.py) = the interface the merge gate (seedgate31) reads

```
harvest/MANIFEST.json      plan, per-leg status/eboot/seed/wall, profile words, program count, conflicts, coverage per pass, warnings, problems, ok
harvest/programs/<key>/v3/{v,f}/HH/HASH.gxp     union of the legs' compiled files (<key> = vitaGL cache key without the -sw31 tag)
harvest/lists/<leg>/{vgx_stagewarm.bin,.jnl,.ply,vgx_shaderlist.bin}   per leg, as the game wrote them (merge = the gate's job; the .jnl journal
                           holds what the leg learned after the last append, and a complete .jnl.tmp / .ply.tmp is copied too)
harvest/lines/<leg>.txt    cover:, profile, seedwalk:, pack:, pack-miss:, first-use:, flow entry/done, survey done lines
harvest/cover.tsv, cover-census.md                the A.5 census table
```
`collect.py` fails (exit 3) on: an env that breaks the allow-list, one program name with two different byte contents (compiler non-determinism or a text change),
different profile words / eboots / seeds between legs, an incomplete leg (`--allow-incomplete` to look anyway), a stage-table entry not finished in some pass.
It does not merge lists, check closure, or compare with the console-made seed: that is the gate (A2).

## Interface expected from the other agents (not built here)

From **seedrun31** (game side), all log-only or harvest-only:
1. `GXV_SEEDWALK=1`: at start-up build every `Ensure*` helper program once (so the port's own texts are compiled onto the card); (harvprep31) 8,192 own records a
   stage and the draw evidence file; on the `mpmenus` leg the menu tour shows each native Settings page and the other Multiplayer screens once. Off in the release. It logs one line `seedwalk: ...` (the legs' `lines/` keep it). The walk sets it in every leg; a build without it
   ignores the key.
2. A `cover:` log line per stage visit, key=value tokens (any order, extra keys are kept as columns). The game's line (seedgate31, fix31):
   `cover: stage=<key> why=left|periodic|exit profile=<word> play_s=<f> links=<n> link_ms=<f> link_worst_ms=<f> compiles=<n> compile_ms=<f> new_recs=<n>
   new_candraw=<n> uploads=<n> targets=<n> hold_ms=<f>`. new_recs / new_candraw: own and can-draw records LEARNED in the visit (visit totals; the start-up loads
   of the card's and the package's lists are not counted). collect.py reads `stage scenario links compiles misses new_recs new_candraw uploads link_worst_ms`
   (the draft names `newrec` and `worst_ms` are still read).
3. A profile line containing the word `profile` and the 64-bit stamp as 16 lower-case hex digits (`profile: word 0123456789abcdef ...`). Without it collect.py warns
   (`--strict` fails).
4. `GXV_PACK_MISSLOG=1` and `GXV_FIRSTUSE_LOG=1` as today (`pack:`, `pack-miss:`, `first-use:`).

From **seedgate31** (merge gate): read `harvest/` as above; its profile-word check compares `MANIFEST.json: profile_words` with the release tree's; the compiler-identity check
(emulator files vs console seed 1117) runs on `harvest/programs`. If it fails, only `harvest/lists` is used.

## Wall time (2 shared cores, nothing else running)

Measured (harvprep31, 17 Nov: integ31 34ee2865, test19 seed, one EggStar stop with a 1,200-turn hold): play 0.21 game s per wall s, a stop's load and opening
camera ~171 s, boot ~70 s; the leg took 5 min 31 s. With that model (`plan.py estimate`): a 4-stop leg ~40 min; the default plan (71 legs, both passes)
~48 h typical (31 fast, 84 slow): two to three nights (run_all.sh resumes). One pass with half the hold (`--passes p60:1.5 --hold 2700 --hop-max 20`): 37 legs,
~17 h. Never run beside a game build.

## Limits and known unknowns

* PETARI_BOOT needs a save. The `menus` leg's run (menu-fast input) is expected to leave one; it is copied to `OUT/save-template` once. If that does not
  hold, put a `saves/` folder there by hand (the file select's file made once in an emulator run).
* The hold of a stop is `PETARI_FLOW_HOLD` turns of play; hops or form changes cut by a short hold are not repeated.
* Survey hops are not play: bosses' later phases and story events are not reached (plan A.6).
* Whether the emulator's compiler output equals the console's is the gate's byte check, not decided here.
