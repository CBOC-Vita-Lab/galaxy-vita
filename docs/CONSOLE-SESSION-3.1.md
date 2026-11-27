# Console session 3.1: one ordered script for the Vita

The owner gives us the Vita once, at the end. Everything the 3.1 line still needs from a console is in this file, in one order: the
shortest and most important checks first, backups first, a restore last. Stop at any "cut line" (end of section 4) and the checks above
it are complete.

It gathers the console checks of the branch reports (netport31, setnat31, setapply31, setfix31, setfix31b, c183fix, c185merge, popin31,
ratecut31, seedrun31b, linkfix31, s1117b, dma31, fix31, emu31b, the integ-3.1 review) and section 0b of
[RELEASE-3.0-CHECKLIST.md](RELEASE-3.0-CHECKLIST.md). The six CPU switches come from the performance inventory (its row C-off18);
the emulator verification report of those switches (emuverify31) did not exist when this was written, so if it exists by the time of the
session, switches it already proved exact need only their `=1` A/B (section 3.8).

## Rules and how to read this file

Hard rules for the whole session:

- **Test build only.** Install the console test VPK of the integ-3.1 line (its eboot's SHA-256 is in the build's `BUILD-INFO.txt`). Never
  the release package. Never a system plug-in. **Never edit `ur0:tai/config.txt`.** Leave the Vita's clock and plug-ins as they are and
  only write down what was on (battery charger, Wi-Fi, PSVshell clock, any plug-in) so runs can be compared.
- **Backups first (section 0), restore last (section 5).** The relay writes only `ux0:data/smg/*.txt` files (env.txt, input.txt, the
  popin31 presets file) and, if asked, the eboot. It never touches saves.
- **Nothing identifying goes in a log we keep.** Before a log leaves the owner's hands, search it for the online name and the Vita's
  address (the game hides codes and addresses, but check).
- **One change per run.** Every arm below changes one thing against a control, and each run's env file replaces the whole `env.txt`.

Markers on every item:

| Marker | Meaning |
|---|---|
| **H** | By hand, on the Vita, the owner holding it. |
| **R** | Relay job, unattended. The relay launches the game, waits, closes it, pulls the logs. Claude reads the logs afterwards. |
| **R+eyes** | The relay puts the env file in and launches (a `-start` job); **the owner plays or watches**; a second job (`-end`) closes the game and pulls the logs. Queue the `-end` job only when the owner says done. |
| `Nm` | Estimated minutes. The relay figures come from `docs/console31/JOBS.txt` (75 s boot, 45 s a stage load, 25 s to close and pull). The probe run (section 2) measures the real boot and load; scale all later figures by it. |

All log lines named here are in `ux0:data/smg/log.txt` (the run before it in `log.txt.prev`). A restart rolls the log, so pull after every restart.
Pull with a `pull-b1-N` relay job, or copy the file away over FTP/VitaShell. **Before every run write down: build, mode, the arm.**

How to read a run (applies to every arm):

1. The first lines say which picture mode ran: `settings: picture mode Performance (60)` (or Quality (30)). A run in the wrong mode is void: the
   settings file beats env.txt (two 30/60 runs of 2.3 were void for this).
2. The `modekeys:` line counts the keys that "came from env.txt": it must equal the number of the arm's mode keys.
3. `fps:` lines (every 5 s) give `shown N fps; game N turns/s; N hitches over 100 ms (longest N ms); picture WxH`. Use the **median of settled
   windows** (skip the first 20 s of a stage). **A difference under about 3 fps in one run is undecided.** The controls K0a (start) and K0b
   (end) measure drift (heat, battery).
4. No `FATAL`, no fault, no crash dump (`ls` of `ux0:data` shows `*.psp2dmp`).
5. **Reading an A/B pair (strk1118, from Strikers' note of 18 Nov).** One run cannot resolve 1-2 ms: run the arm and its control in alternation (on, off, on, off) and
   compare matched windows (same stage, same time into it) with `python3 tools/console31/abpair.py --on a.log b.log --off c.log d.log [--metric frame-p95|frame-p50|fps|hitches|pair-p95]`.
   It prints the mean difference with a bootstrap 95 % interval and says "on better", "off better" or "no difference" (interval contains 0). It reads the `fps:` lines
   (frame time = 1000 / shown fps of each 5 s window, 30 s windows after the first 20 s of a stage) and, for Quality, the q30lock `game pair ... p95` line. Pair the logs in the order the runs were made.
   A "no difference" from one pair of pairs means undecided, not equal: add pairs. Tested on synthetic logs (`tests/abpair_test.py`) and on the 2.3 console logs.

## 0. Backups and env.txt (10 min)

| Step | What | |
|---|---|---|
| 0.1 | Plug in the charger, Wi-Fi on, same brightness all session. Note the build's eboot SHA-256 (relay job `8108-c31-eboot-hash` returns it in `result.json`). Install the test VPK. | H 3m |
| 0.2 | **Saves.** With VitaShell or FTP copy `ux0:data/smg/saves` (and `ux0:data/smg/mp`, `ux0:data/smg/mp-backups` if they exist) to the computer. `shasum -a 256 GameData.bin` of `saves/title/00010000/524d4750/data/GameData.bin`: write it down as **S0**. | H 4m |
| 0.3 | **Settings.** Copy `ux0:data/smg/player-settings.cfg` (relay job `8101-c31-backup-settings`). List the folder and write down which of `player-settings.untested`, `player-settings.before-3.0*.cfg` already exist (relay job `8100`). | R 1m |
| 0.4 | **env.txt.** Relay job `8100-c31-backup-list` pulls it (`outbox/8100-c31-backup-list/env.txt`). Copy it to `relay/staging/env-user.txt`. **If the owner had no env.txt**, make `env-user.txt` with one line `# none`, and at the restore (section 6) delete the Vita's env.txt. Keep a second copy of the original on the computer. | R+H 2m |
| 0.5 | **Install the jobs.** `python3 tools/console31/make_jobs.py install --relay <relay folder>` copies the env and route files to `relay/staging` (never `env-user.txt`) and the jobs to `relay/claude-jobs-console31`. Queue with `... queue --relay <relay> <job name>...`. The relay window must be running. | H |

From here on every relay run ends by putting the owner's `env.txt` back, so an aborted session leaves the owner's file in place.
The scripted arms never write the save: they start with `PETARI_BOOT` (the game reads the first save file and "continues without saving").

## 1. Block 1: no env.txt change (by hand, 81 min)

Leave the owner's env.txt as it is (or delete it for this block; step 0.4 has its copy). The order is by importance: the settings
restart is the owner's own bug report. Pull the logs at the six checkpoints (CP1-CP6, jobs `8102` to `8107`).

**1.0 Preflight (3m).** Entry screen, Settings, Picture. The A/B arms need **Smooth Motion with 60 Assist On**. If it is not, change it now
(Circle, **Save and Start Again**): that restart is test 1.4's entry-screen path. Write down the owner's own Picture and language first (section 5 puts them back).

**1.1 Boot, pause menu, Observatory first entry (8m, H).** Title, Single Player, the file, into the Observatory. This is the first entry of a
stage on this build, so do it before any arm. Pause (Start).
- The panel shows the hint **"Triangle: Settings"** (linkfix31: the native Settings code was never linked on the Vita before; this proves the fix).
- Triangle opens Settings over the pause. Controls: Tilt Console/Left Stick, Tilt Strength 5 to 7; More: Less Motion Off/On. Each changes at once
  with no restart and no question (log `settings: tilt -> stick`, `tilt_strength -> 6`, `less_motion -> on`). Put each back. A Tilt change should remake the hold-the-controller pictures.
- Open and close Settings from pause five times: no crash, no slowdown (setnat31: pause-menu memory was never run on a console).
- **Log, default env (no C182 set): `prewarm-res: stage AstroGalaxy: screen open, mode 2: plan N [... targeted mask 0] ...` is expected here**: the mask
  prewarm has a consumer only with `GXV_HRW41_DEPTH=1` (arm A1). Pass: the line is there, no `NOT taken ... MaskSnap` line, no 240x136 target made in play.
  `VRAM unknown N` tells whether the console gives a free-VRAM reading (arm A3 needs it unknown to act).
- `cover: stage=4903d024 ... compiles=0` and a `first-visit: stage 4903d024: 6 nonresident expensive variants moved to the front` line, **no** `WARNING: the package's ... has no stagetags stamp`
  line (seedrun31b / fix31: the packaged list must be tagged; if the warning shows, the VPK skipped the tagging step, a release blocker).
- **Clocks (strk31, from Strikers' console data: their 500 MHz request is refused with `0x802B0000` on every run and the core stays at 444).** The log must hold the request and its result: `clocks: CPU request=500 ... arm_rc=... bus_rc ... gpu_rc ... xbar_rc ... reported CPU=...`, `clocks: the ARM core measures N MHz before the requests and M MHz after them` and (default `GXV_CLOCK_DEFER=1`, the CPU request is made at 1 s) `clocks: 1 s into the process the ARM core measures N MHz (asked for 500 | a profile is in place: no request; arm_rc=0x........ fallback_rc=0x........); now M MHz`. Run `python3 -I tools/console31/check_boot_log.py <log.txt>`: exit 0 prints the facts to write down (accepted / REFUSED `0x802B0000` with fallback 444 / a profile held 496-500), exit 1 means a line is missing and the run's clock is unknown. A refused 500 is a note, not a failure: but every A/B in this session must say which CPU clock it ran at.
- One start line for each of: `profile: word=...`, `modekeys: Performance mode: ... (N written, 0 kept from env.txt ...)` as one whole line (fmt31/F10: no cut census), `[autoq] GXV_AUTOQ_GPUGATE` (s1117b), and the `[settings]` census with `GXV_AUTOQ_MIN` 6 (the size floors: 960x544 pinned in both modes; confirm).

**1.2 Honeyhive first load (6m, H).** Dome, galaxy map, Honeyhive. Pass: `cover: stage=17bab031 ... compiles=0` for the visit, `first-visit: stage 17bab031: 3 nonresident
expensive variants moved to the front`, no stall over about 1 s on the first entry (look and count). Honeyhive's plant reveal is **on** by the owner's decision: look at the far grass and blue
flowers while the camera flies in; one picture of about half a far group is held 1/30 s (a softening, not a fault). **CP1: pull the logs.**

**1.3 Settings restart from pause (30m, H).** The owner's bug: a saved Picture change did not apply without a manual quit. One level, then four restarts. Before each, **Circle** after the change.
Do them in this order so there are only four restarts (pull CP2 to CP5 after each):

| Restart | Do | Must see |
|---|---|---|
| R1 (Smooth Motion to Quality), first try | Pause, Triangle, Picture Right (**Full Detail**). Band: "The game starts again to use this; you're asked first." **Circle**: leave question **Save and Start Again / Don't Save / Keep Editing**. **Cross**: Yes/No "Start the game again now? You leave this level. Progress since your last save is lost." **Test the pad:** press **Cross at once**. | It must answer **No** (back to the leave question; setfix31: the pad used to be ignored). Then choose **Don't Save**: the game goes on, `player-settings.cfg` unchanged (P2). |
| R1, second try | Picture Right again, Circle, Save and Start Again, Cross, then **Right, Cross** (Yes). On another try tap **Yes** with a finger once (it answers at once). | The game restarts by itself in a few seconds (title, then entry). **No** "Saved. Please close the game and start it again." window. Log: `settings: restart requested`, `restart now`, no `restart returned`; next log `settings: picture mode Quality (30)`, `GXV_FPS_LIMIT=30`. Play 1 minute: the locked 30. |
| R2 (Quality to Smooth Motion) | Same route, Picture Left. | Restart; log `Performance (60)` and `modekeys ... GXV_HRW=5`. |
| Keep Editing path (in R2's level, no restart) | Picture Right, Circle, Circle (Keep Editing), Circle again; then Picture Left, Circle (a plain Save). | First: menu closes, toast "Settings saved. Picture and language change next time.", no restart, no second question (P3). Second: plain **Save**, no restart (the value is what is running). |
| R3 (60 Assist Off) | In Smooth Motion the **60 Assist** row sits under Picture (only then). Set **Off**, Circle, Save and Start Again, Yes. | Restart; log `GXV_HRW = 0 (Settings: 60 Assist off)`. |
| R4 (60 Assist On, and the buttons) | **Before leaving Settings:** Controls, Change Buttons, Jump to **Circle** (swap), 60 Assist **On**, Circle, Save and Start Again, Yes. | After the restart: log has **no** `active mapping rejected` line; Jump is on **Circle** in play and the on-screen button pictures say Circle (setfix31: the saved buttons used to be lost at every start). Then swap Jump back to Cross and save (live, no restart). |

Also check on R1-R4: once the entry screen shows, `ux0:data/smg/player-settings.untested` is **gone** (setfix31, the recovery marker). **CP2-CP5.**

**1.4 Entry-screen path (4m, H).** From the entry screen change any restart row, Circle, **Save and Start Again**: it restarts at once, no Yes/No. Then again with **Keep Editing**, Circle: it saves and restarts by itself (setapply31 P4). (Fold this into 1.0's change if 1.0 needed one.)

**1.5 "Finishing saving" (6m, H).** The hardest to stage (setfix31b). In a level take a star or reach a save point and accept the save, then open the pause menu at once and repeat the R1 route (Picture Right, Circle, Save and Start Again, Yes), several times if needed.
- Quiet card (the normal case): the game starts again at once, no window; log `settings: restart now`.
- A save in flight: a small window **"Finishing saving... the game starts again as soon as it's done."** with one choice, **Later**. It restarts by itself the moment the card is quiet (log `settings: restart waits for a save to finish`, then `restart now`). **Later** (or Circle): menu closes, toast "Settings saved. Picture and language change next time.", no restart.
- The stuck-choice window (Start Again Now / Later after 30 s with no progress) cannot be staged on a console without holding the card busy: it stays an emulator check. If it ever shows by itself: focus is on **Later**; Start Again Now must leave a whole save (old or new) and no `GameData.bin.mptmp` after the next start. Put Picture back to Smooth Motion afterwards (it ends block 2 as Smooth Motion with 60 Assist On). **CP6 comes after 1.7.**

**1.6 Stamp, strays (9m, H).** The settings file's version stamp (setfix31):
1. In VitaShell **rename** `player-settings.cfg` to `player-settings.cfg.mine` (never delete it). Start the game with no settings file; change anything in Settings (a language is fine), Save, restart.
2. Pass: the log has **no** `settings guard: settings file from 2.2.1 ...` line; the file's first line is `# player-settings written by Galaxy 3.0.0`; **no `player-settings.before-3.0*.cfg` files exist** (also none after steps 1.3 to 1.5). Compare the folder list with section 0.3: any new `.before-3.0` file is a failure.
3. Rename the new file to `player-settings.cfg.test`, the owner's file back to `player-settings.cfg`. (Section 5 checks it.)
4. Fresh-first-launch note (emu31b): the first launch with no file prints no guard line.

**1.7 Online room, Friends, sockets, FTP (12m, H, the owner's Wi-Fi and the live service).** netport31: a held room poll used to be cut after 10 s, so a room left forming failed after about 4 minutes; the fix gives held polls their hold plus 10 s and lets a menu request go first. **This talks to the owner's online service from the owner's Vita and the game may make an anonymous account (Friends triggers a sign-up try, emu31b). Skip the step if the owner does not want that on the test day; everything else here is independent.**
1. Multiplayer, Play Together, **Invite from Far Away** (a private code; do not use Play with Anyone, a stranger could join). Get the code. Leave the gathering screen **waiting more than one minute** (two is better). Nobody joins.
2. Then **Friends**. On the Multiplayer menu it is **Square**; if the gathering screen offers it, use that, otherwise Circle out of the room and open it at once. Pass: the list answers in **about a second or two**, not after a held poll runs out.
3. **Sockets and FTP.** Pull the logs (CP6). Pass: no `[net-https] ... TIMEOUT (no data for too long)` line (before the fix: one every 10 s); `GET /v1/rooms/...` lines of about 25 s each with status 200; the last `[net-https] sockets:` line shows `open` 0 or 1, `refused 0`, opened equals closed (or one more); and **FTP still works after the long online run** (the pull job itself succeeding is the proof; Strikers' FTP wedge never showed in Galaxy, this closes the question).
4. A settled host for several minutes (55 s polls) needs a second console: not testable alone, not asked.

**1.8 LiveArea and language (3m, H).** The bubble shows the five contributor pictures; starting shows the gate picture (startup.png, with transparency) without a visual fault; there is **no Settings tile** (owner's decision). The game language follows the console's system language (setnat31).

Block 1 total about 81 min (3 + 8 + 6 + 30 + 4 + 6 + 9 + 12 + 3).

## 2. The relay gate and the control (10 min)

| Job | Run | |
|---|---|---|
| `8120-c31-probe` | Observatory 60 s, Performance, nothing else, one screenshot. **Gate**: the log says `boot: PETARI_BOOT` and `boot: play began`, `picture mode Performance (60)`, and the owner's save was read, not written. If `PETARI_BOOT` does nothing on this build, stop the unattended arms and tell the coordinator. Measure real boot and load times here. | R 3.4m |
| `8121-c31-K0a` | **Control K0a**: Observatory 60 s then Honeyhive 120 s, scripted route (`input-c31-120s.txt`), **no switch**. Its env file is only the harness lines (`PETARI_BOOT=1`, `PETARI_STAGE=AstroGalaxy:1,HoneyBeeKingdomGalaxy:1`, `PETARI_STAGE_HOLD=3600`, `PETARI_FLOW_LOG=1`, `PETARI_SCRIPT_ANCHOR=play`, `PETARI_SCRIPT_LOG=1`). | R 6.2m |

**Texture-upload census (free, from K0a and every later run; no extra run).** `texup:` lines are on by default. Write down per stage:
uploads, KiB and ms **behind the wipe** and **in play**, the split (names, swizzle, gl, mips, soft), `hitch pictures`, and the worst picture's ms. The 2.x reference is Observatory: 259 uploads, about
7 MiB, 691 ms of upload time (the per-texture fixed cost, perf-inventory D18, still unexplained). No pass rule: the numbers say where the next work goes. (If the texup31 work adds a switch before the session, add its line to a copy of K0a's env file and rerun K0a.)

K0a is also the baseline for every default-card arm and reads `cover:` (`hold_ms`), `fps:` and the autoq lines. **K0b (item 13 of section 4) repeats it at the end** to size the drift.

## 3. Block 2: the A/B arms (each: env lines, route, look at, pass rule)

Each arm file in `docs/console31/staging/env-<job>.txt` is the whole env.txt of its run: the harness lines of K0a plus the arm's lines below.
Order inside block 2 is by value for the least time. Arms 4.1 to 4.4 and 4.7 compare against K0a on the owner's card; 4.5 and 4.6 use a **fresh tagged shader cache**
(`GXV_SHADER_CACHE_TAG`, its own `stagewarm`/`shaderlist` files) so the owner's cache and lists are neither read nor written.

### 3.1 C182 smoothing set (the Smooth Motion warp; the owner's 60 estimated pictures) 21m

Checklist 0b, first item. Off by default since fix31; this arm tests the set as one change.
```
GXV_HRW41_DEPTH=1
GXV_HRW41_BOXKEY=1
GXV_HRW41_BOX_ROUND=1
GXV_HRW41_BUDGET=1
GXV_HRW_STAGE_AUTO_RETRY=3
```
- **R `8122-c31-A1-c182`** (6.2m): Observatory 60 s, Honeyhive 120 s, the K0a route. Numbers.
- **R+eyes `8123a/b-c31-A1o-on` then `8124a/b-c31-A1o-off`** (7.2m each): the owner plays Observatory 2 min then Honeyhive 2 min, the **same route both times**: jump around, spin, turn the camera hard left and right over the Lumas and flowers. Verdict: **halo around Mario on jumps** and **doubled scenery on turns** (the two reports C182 answers): better / same / worse. Also the soft box around Mario's jump (c183fix F10: investigated, unchanged; a still or video compare is owed).
- Log: `hrw: c182 | detail budget: N pairs on the cheaper c181 warp ...`; a `given up for this stage: switched off N times` line in a heavy spot (c183fix F2: expected sometimes; note the stage); `prewarm-res: stage AstroGalaxy ... targeted mask 1` (**here it should be 1**) and no 240x136 target made in play; the new "inside a pass" line on bloom stages and fewer `hrw41 ... plans` on MSAA or overscan rungs than K0a (c183fix F6); a line `compiles` over 0 in play means the package seed lacks the D41 programs (a hitch; note it, it does not fail the idea).
- **Pass:** no crash; median shown fps not below K0a by more than 1 in either stage (1 to 3 down is undecided: rerun; more than 3 down is a fail); the owner finds halo and doubling **not worse** than the control, ideally better.
- **Decision:** pass: the five rows go back into the Performance column of `mode_keys.h`; fail: the set stays off, record why. If it fails, bisect: one line per run (an extra run each, 6m).

### 3.2 Pose phase V0-V5 (the lily, flower and Luma vibration) 15m for V0 and V4, up to 53m

ratecut31's A/B. `GXV_POSE_PHASE_CENSUS=1` is in every arm's file (log only). The owner **plays or watches** (R+eyes; start job, play 150 s of Honeyhive in the bee suit, then 90 s of the Observatory, end job).
The game boots into Honeyhive with the bee suit given at turn 240 (`PETARI_POWERUP=4`); fly over and around the lily pond and the flower fields, from far, then near; after 150 s it jumps to the Observatory:
turn the camera over the Lumas and the flowers on the way in. Note per place: **does a lily, flower or Luma vibrate? yes / no / less.**

| Arm | Lines (plus census) | Isolates | Job (R+eyes) |
|---|---|---|---|
| V0 | none | Baseline; today's pose ages, especially `faranim [..]` ages 2 and 3 | `8125a/b-c31-V0` |
| V4 | `GXV_POSE_PHASE=1` | The new switch: census must say `pose age at draw max 0` apart from broken pairs | `8126a/b-c31-V4` |
| V3 | `GXV_HRW=0` | No second picture: plain 60 target | `8127a/b-c31-V3` |
| V1 | `GXV_HR_PLANTMOVE=0` | Flower-group members move every turn; expected: no change (frame-exact already) | `8128a/b-c31-V1` |
| V2 | `GXV_FARLOGIC_DRY=1` | Far logic off (`GXV_FARLOGIC=0` does not work) | `8129a/b-c31-V2` |
| V5 | `GXV_POSE_PHASE=2` | only if V4 fixes it: whether every-4th cuts were the cause | `8130a/b-c31-V5` |
| V6 | `GXV_HRW41_DEPTH=1`, `GXV_HRW41_DEPTH_EDGE=1` | only after V3 fixes it: the depth-edge option (c185 diagnostic; not an established fix) | `8131a/b-c31-V6` |
| V0q, V4q | `GXV_PICTURE_MODE=30` baseline, and with `GXV_POSE_PHASE=1` | only if the owner saw the vibration in **Quality** (the warp cannot explain that mode, the pose cuts can) | `8132a/b`, `8133a/b` |

Order and stopping: **V0, V4 first** (15m). Then follow the decision:
- **V4 fixes it, fps and pairs hold against V0:** make `GXV_POSE_PHASE=1` a Performance default (a `mode_keys.h` row). Run V5 to learn whether mode 2 suffices.
- **V4 fixes it but costs frames:** V5 says whether mode 2 suffices; otherwise wait for popin31's floor so the every-4th cuts keep their rate under it.
- **Only V3 fixes it:** the cause is the 60 Assist warp; run V6, then capture real non-black colour and depth frames (review F4). Keep `GXV_HRW41_DEPTH_EDGE` off until then.
- **Only V1 or V2 fixes it:** the ratecut31 model is missing something; Claude needs the log.
- If V0 shows **no** vibration for the owner on this route, stop: say so (the report may be a Quality or other-stage case: V0q).

### 3.3 Compact generated mipmaps (`GXV_LA88_MIPS`) 6m

```
GXV_LA88_MIPS=1
```
**R `8134-c31-A2-la88`**, same route as K0a (owner may glance at the screen: text, cloud and alpha textures at a distance must look as in K0a).
- Log: `[vgx-vita] GXV_LA88_MIPS: exact from 64x64 through 2x2 including padded rows and transfer witness` (the self-test verdict). Any other `GXV_LA88_MIPS:` line ("RGBA8 as before", "differs in N texels", "refused") is the verdict **fail**: the arm turns itself off.
- `compact mips: N accepted uploads, M fewer level-zero bytes submitted vs RGBA8 (cumulative)`: N over 0.
- **Pass:** verdict exact, N over 0, `texup:` upload ms behind the wipe not higher than K0a, no visible change. Then `entry.cpp` `GXV_LA88_MIPS` may go back to 1 (record it).

### 3.4 Pop-in: flights first (P0, P2), then the floor by kind (P7-P9), then all kinds (P1, P3, P4) 16m first, 39m more for the rest

popin31's B0 to B2 and popin31b's floor by kind. **R, scripted**: Honeyhive 200 s (the fly-in first), then Beach Bowl 80 s; route `input-c31-200s.txt`; `GXV_POPIN_CENSUS=1` in every arm.
Read per 5 s window the `popin25:` + `popin31:` pair labelled `play` (or `play, release after a flight`), and the flight-labelled pair; per flight `popin25-run:` + `popin31:`; at each stage load
`popin31: GXV_PB_MINPX=...` and `popin31b: GXV_PB_MINPX_SHADOW=... _DECOR=...` (each kind's floor). With any floor on, each window also has a **`popin31b:` price line**: per kind its floor, the
object-turns it kept a turn and, for shadows, plants and fur, the CPU of that work measured on a sample of the kept things (shadows: also how many of their ground probes the shadow memo answered).
The owner may watch the fly-in's end and the shadows and grass (optional eyes).

**First, as its own step right after K0a (section 4, step 3b): P0 and P2.** Flights exact (`GXV_PB_DEMO=2`) is the cheapest arm with the most to show: the budget is off only while the player has no
control (the opening fly-in, a Launch Star flight, a demo), it answers the intro grass, the Honeyhive waterfall's thinning and part of the enemy report, and its only cost is the fly-in's own frame time.
If P2 passes, it is the first Performance row to switch on (a `mode_keys.h` row), before any floor.

| Arm | Lines (plus `GXV_POPIN_CENSUS=1`) | Job |
|---|---|---|
| P0 | none (defaults): the control | `8135-c31-P0` |
| **P2** | `GXV_PB_DEMO=2` (flights exact; **run first**) | `8137-c31-P2` |
| P7 | `GXV_PB_DEMO=2`, `GXV_PB_MINPX_SHADOW=3` (the shadows' floor only) | `8170-c31-P7` |
| P8 | `GXV_PB_DEMO=2`, `GXV_PB_MINPX_DECOR=3` (the plants' floor only) | `8171-c31-P8` |
| P9 | P8 + `GXV_STATIC_INST=1` (a plant group's members drawn as one GL draw, exact; the plants' cheap give-back) | `8172-c31-P9` |
| P1 | the game's own pops, the full price of the budget: `GXV_PERF_BUDGET=0`, `GXV_CPULOD=0`, `GXV_FE_GOV=0`, `GXV_GAME_GOV=0`, `GXV_CPUGOV=0`, `GXV_STAGE_PRESETS_FILE=ux0:data/smg/popin31-p1-presets.txt` | `8136-c31-P1` (puts the file) |
| P3 | `GXV_PB_DEMO=2`, `GXV_PB_MINPX=3` (every kind) | `8138-c31-P3` |
| P4 | `GXV_PB_DEMO=2`, `GXV_PB_MINPX=6` | `8139-c31-P4` |
| P5 (optional) | `GXV_PB_DEMO=2`, `GXV_PB_MINPX=3`, `GXV_POPIN_LEAD=6` | `8140-c31-P5` |
| P6 (c185merge's plant-reveal A/B) | `GXV_HONEY_PLANT_REVEAL=0` | `8141-c31-P6` |
| P10 (optional, verify) | `GXV_PB_DEMO=2`, `GXV_PB_MINPX_SHADOW=3`, `GXV_PB_MINPX_DECOR=3`, `GXV_SHADOWMEMO_VERIFY=1`, `GXV_MO_GRAVMEMO=2` | `8173-c31-P10` |

The presets file for **P1** (`docs/console31/staging/popin31-p1-presets.txt`, put by the P1 job at `ux0:data/smg/popin31-p1-presets.txt`):
```
# popin31 P1: the base class without the budget's drawn cuts and without cpulod (sizes, fx and clip_skipempty as shipped)
@class base start=6 floor=6 ceil=6 size=fixed fx=down clip_skipempty=1
AstroGalaxy class=base
AstroDome class=base
```
Env lines alone do not turn the budget off: the built-in stage table's `base` class sets the cuts as stage keys, which beat env.txt, and the governors re-enable far-draw cuts. So **P1 must show** `perf-budget: off lod=1.00 decor=0 ptcl=1.00 shadow=0 ... fur=0` at each stage load; if it still says ON, the run is void.
- **Pass numbers:** P2: flights port-made pops **0** and its fly-in frame rate against P0. P7, P8: in the `popin31:` line, "port-made pops at or above it (one element)" **0** for that kind, the other kinds as in P2;
  their price is their frame time against P2, and the `popin31b:` line says how much of it is CPU (shadows: "us each" and the memo's share; plants: the members' movement). P9 against P8: the plants' price with
  their draws joined (`static-inst:` line: runs and joins over 0). P3 and P4: "port-made pops at or above the floor" **0**, their price against P2. Expect P3 (3 px) to switch off practically all visible cuts
  in play, so its frame time sits near P1's for decor, shadow, fur and LOD; P4 gives back little more. P10: `shadowmemo:` lines **mismatches 0**, `grav-memo:` lines **mismatches 0** (the kept shadows' cached
  ground probes and gravity are exact); its frame rate is not a price (verify casts everything).
- **Decision rule (owner):** switch on `GXV_PB_DEMO=2` after P2. Then the kinds one by one, cheapest and most visible first (shadows, plants: the tester's report), each only where its arm holds 60 in
  Honeyhive 1 and Beach Bowl; the rest stay on the budget (or the named-exception rule). Then mode_keys rows for `GXV_PB_DEMO=2` / `GXV_PB_MINPX_<KIND>`, and whether `port/honey_shadow.h` goes.
- P6: the plant reveal stays **on** unless the owner objects; this A/B gives its price (pops counted as `plant-reveal`) and the owner's look at far Honeyhive grass. Under the plants' floor the reveal
  stands aside by itself (it never staggers a group the floor gave back).

### 3.5 C185 load hold with an unknown free-VRAM reading 12m (cold card)

```
GXV_C185_HOLD_UNKNOWN_VRAM=1
```
**R** `8145-c31-K1-cold` (control, tag `c31k`) and `8146-c31-A3-hold` (the arm, tag `c31h`): Observatory 60 s then Honeyhive 120 s on an empty tagged cache (the package's seed and pack are used as always).
Their env files also hold `GXV_SHADER_CACHE_TAG`, `GXV_STAGEWARM_FILE` and `GXV_SHADERLIST` of the run's own, so nothing of the owner's is touched. Also read in K1-cold (c183fix F5): `menu-tier ... appended` on a first stage soon after boot.
- **Is the arm exercised?** `prewarm-res: ... VRAM unknown N`: if N is 0 at every stage in K1-cold the console gives a free-VRAM reading and the switch has nothing to do: record **not exercised**, keep it 0.
- **Pass (when exercised):** `cover: ... hold_ms` shorter or equal in A3 than K1-cold at each first visit, **pictures the same**, no new compile in play (`compiles=0`), no longer stall. Only the hold's length may differ.

### 3.6 Scenario order (`GXV_STAGEQ_SCENARIO`) 28m (cold tagged caches, two pairs)

```
GXV_STAGEQ_SCENARIO=1
```
Needs two visits on a card (the first writes the hints, the second uses them). Hints on: tag `c31s`; hints off (the control): tag `c31t`. **R**, four runs of 6.9m:
`8147-c31-S1-learn`, `8148-c31-S1-measure`, `8149-c31-S0-learn`, `8150-c31-S0-measure`. The learn run visits AstroGalaxy:1, AstroGalaxy:3, Honeyhive:1 for 60 s each; the measure run enters scenario **3 first**.
- Read at start `tags (seedrun31): N slow, M with a scenario hint, K can-draw`: M over 0 in the measure run of S1 (hints recorded), M = 0 in S0.
- **Pass:** in S1-measure the first visit of AstroGalaxy:3 has `compiles=0` and `links`, `link_ms`, `hold_ms` not worse than S0-measure, and the Observatory's historical records still lead (`first-visit: stage 4903d024: 6 ...`).
- If there is no difference, keep the default 0 (nothing to gain, and the hint record doubles list records, fix31 F5).

### 3.7 The 30 cap rule and the picture-size gate 4m + 10m (conditional)

- **R `8142-c31-F30`** (3.9m): `GXV_FPS_LIMIT=30` in env.txt under Smooth Motion. setnat31's fix: the rule "env.txt `GXV_FPS_LIMIT=30` under Performance keeps the fixed 30 cap" never acted on a console before. Honeyhive 90 s. Pass: the log shows the cap in force (`fps:` near 30, no estimated pictures, `GXV_FPS_LIMIT=30` among the keys kept from env.txt). **Owner: confirm you want this fix** (setnat31's "Decide").
- **Picture-size gate** (s1117b, default on; fable's second review asks for the A/B, so it is no longer only conditional): K0a already prints `[autoq] GXV_AUTOQ_GPUGATE` at start. **R `8143-c31-G-ctl`** (Beach Bowl 150 s) reads the size changes in a heavy stage; run **`8144-c31-G-off`** (`GXV_AUTOQ_GPUGATE=0`) **only if** G-ctl shows a held size while pictures take over 30 ms ("no step down while pictures are 30 ms"). Pass: G-ctl is never worse than G-off in size or fps. `GXV_AUTOQ_GPUGATE=0` is the kill switch. The review's arm is in section 3.11 (`8185` and `8186`: the default against `=0` on a GPU-heavy route, comparing shed steps and shown fps).

### 3.8 The six CPU switches (perf-inventory C-off18) 41m

**fixnow31, after emuverify31 (Vita3K, `reports/emuverify31.md`):** REPLAY25 (992,610 shapes compared, 0 differed), RT_INST (886,350 compared, 0), STARBATCH (about 18,300 stretches, 0 failed) and PAR25 (about 1.31 million jobs, 0 mismatches; a guard, not a saving by itself) are **ready for the console**. **IDLE25 is held**: its verify showed 12,025 of 251,372 mismatches (4.8 %) and 9,189 of 140,443 (6.5 %), the signature of the ALOD half-speed turns (coins mismatch on every second check in Honeyhive): do not send `GXV_IDLE25=2` or `=1` until that is resolved. **NOOP_FILTER is held**: its verify prints only to stderr, which the game log does not carry, so there is nothing to read. The verify job `8151` therefore carries the four ready switches and the workload probe, and `8153` (IDLE25) and `8156` (NOOP_FILTER) are marked HELD: do not queue them. Exact lines, below.

IDLE25, REPLAY25, PAR25, NOOP_FILTER, RT_INST, STARBATCH: exact by design, built, never verified on a console (the best CPU candidates on the shelf). Quality mode (`GXV_PICTURE_MODE=30`) at Beach Bowl, the heavy CPU stretch. `GXV_TURN_PROF=1` prints a `turn-prof:` line every 5 s splitting the game thread's turn.
1. **R `8151-c31-CPU-verify`** (7.2m): all six verify at once, Honeyhive 120 s then Beach Bowl 120 s: `GXV_IDLE25=2`, `GXV_REPLAY25=2`, `GXV_PAR25=2`, `GXV_NOOP_FILTER=2`, `GXV_RETAINED=2`, `GXV_RT_INST=2`, `GXV_STARBATCH=2`, `GXV_TURN_PROF=1`. If it crashes or a line is missing, split it (one switch a run).
   **Pass = 0 mismatches:** no `idle25: VERIFY MISMATCH` and the per-family census `VERIFY N checks, 0 mismatches` (verifyfix31: under ALOD half-speed the compare no longer takes a left-out turn as the reference; the old build showed 5-20 % on coins, star bits, petit piranhas); `replay25:` says `0 differed` and `0 post-state wrong`; `par25:` lines (mode 2 is "observe", verdicts logged as "would", `mismatches 0`); `noopfilter: N checked, 0 mismatching` (read it in the game log as the `verify: noopfilter: GXV_NOOP_FILTER requested 2, effective mode 2, N checked, 0 mismatching` line, printed every 300 pictures; **"effective mode" must be 2: a build without diagnostics runs `=2` as `=1`, the line then says so, and NOOP_FILTER is judged by its A/B and a picture check only**); `verify: ... retained verify (GXV_RT_INST=2 / GXV_RETAINED=2): N checked, 0 mismatching` (same line, needs no `GXV_WORKLOAD_PROBE`); `starbatch:` census says `0 differed` and `plan checks failed 0`.
2. **R `8152-c31-CPU-ctl`** then one `=1` run each (4.9m each; Beach Bowl 150 s, scripted, Quality): `8153` `GXV_IDLE25=1`, `8154` `GXV_REPLAY25=1`, `8155` `GXV_PAR25=1`, `8156` `GXV_NOOP_FILTER=1`, `8157` `GXV_RT_INST=1`, `8158` `GXV_STARBATCH=1`. Run only the switches whose verify was clean.
   Estimated gains (not measured): IDLE25 0.3-0.7 ms a turn, REPLAY25 0.8-1.3, NOOP_FILTER 0.1-0.3, RT_INST 0.3-0.9, STARBATCH up to about 0.6. PAR25 is a guard over the parallel workers: pass is "not worse".
   **Pass:** the game thread's `turn-prof:` total is lower by at least 0.3 ms a turn or the shown fps median is up by 1, hitches over 100 ms no more than the control, the pictures unchanged. Then it may become a default (record it), otherwise drop it.
3. PAR_DL stays **off** (6 of 283,174 differed in verify).

### 3.9 Clocks, DMA, ties (dma31, perf-inventory D5)

**GXV_DMA_LOAD** (14.4m, R: `8159` A `=3`, `8160` B `=2`, `8161` C `=1`, each Observatory then Honeyhive, hold 2400 turns, cold game start each):

| Run | Lines | Gives |
|---|---|---|
| A | `GXV_DMA_LOAD=3` | census: the CPU copies, timed |
| B | `GXV_DMA_LOAD=2` | verify: must log `0 differed` and no `dma-load: VERIFY` line |
| C | `GXV_DMA_LOAD=1` | the engine |

Compare the last `dma-load:` line of each run (ms and MB/s for the same bytes) and the stage's upload time in the `texup:` lines. **Pass:** B clean and C's copy ms lower than A's; then decide by the load-time difference (expected at most 12 ms at the Observatory).
Does `sceDmacMemcpy` read a cached source coherently? B's verify is our only guard; the Strikers chat has been asked.

**GXV_CLOCK_CENSUS** (8.5m, R `8162-c31-CLOCK`): `GXV_CLOCK_CENSUS=1`; Observatory 120 s, then Honeyhive (the fly-in is the cut scene, plus 120 s). Collect the `clock-census:` lines (reads a picture, by thread, top sites). Resolve the top sites with `arm-vita-eabi-addr2line -f -C -e galaxy_native <addr>` on the build machine; first check the line's "Report is at" address equals `nm -C galaxy_native | grep clockcensus::Report`. No pass rule: it says which threads and sites are worth the next exact merges (the console measured 0.87 microseconds a read).

**HSR ties census at Dreadnought** (6.4m, R `8163-c31-HSR-census`): standing still 240 s (CannonFleetGalaxy:1):
```
GXV_HSR_ORDER=2
GXV_HSR_ORDER_TIES=4
GXV_HSR_GEO_BUDGET=30000
GXV_HSR_GEO_KEEP_MB=24
```
Census only, the picture does not change. **Wait until the `geometry:` line's pending reaches 0, then read** the `ties 4: same-transform pairs per picture N, freed by: ...` line, the `geometry:` line (ready / pending / too big) and the first 8 `pin:` lines (reason coplanar / contact / near / dynamic / pending / nodata). It answers "how many of the 7 pinned alpha-tested draws are freed" (0 to 7; best guess 2 to 4). **Only if it frees at least one**: `8164-c31-HSR-gain` (ties 4 on, `GXV_HSR_ORDER=1` and the same three lines) against `8165-c31-HSR-ctl` (nothing): expected GPU 3D time between 30.5 ms (nothing freed) and 23.6 ms (all freed), 240 s each (6.4m each).

**Uniforms** (optional, 4.4m, R `8166-c31-BEPROF`): `GXV_BE_PROF=4`, Beach Bowl 120 s; read `be-prof:` uniform microseconds per draw and `be-prof vitaGL` ring copies to refresh the per-draw figures (1.17 uniform calls and 5.5 microseconds a draw in the c35 data).
The clock-read merges (render thread 16 to 13, front end 37 checks to one) show only in the census counts, never in frame time.

### 3.10 keepdepth (K0, K2, K1): the second picture keeps the first's depth (keepdepth31) 3 x 4.7m, owner watches

`GXV_HRW_KEEPDEPTH` is **on by default** and is not exact for the pair's second picture (fable's second review, D1): Mario is depth-tested against the first picture's 3D, whose depth is not warped, so on a pan an occluder's depth edge sits up to the pan distance from its warped colour. No console has seen it. Honeyhive, Performance with 60 Assist on; every arm adds `GXV_HRW_FLIPBOOK=240` and `GXV_HRW_FLIPBOOK_FROM=600` (half-size PPMs of every picture in `ux0:data/smg/hrw31/`; clear the folder between arms). Route, the same in every arm, about 10 s inside the capture: (1) run past the tall grass and the flowers so Mario passes behind a blade; (2) run along a fence post or the wooden posts near the slide with a post between the camera and Mario; (3) stand still, then a fast camera pan with the right stick (full deflection, about 1 s), then run under the pan; (4) jump once next to a wall (the old "force field"). Three screenshots in a row behind the grass if no flip-book.
Run **after keepwatch31's counter fix is in the build** (fixnow31: the violation counter now counts only what the plan forbids, so a depth-test ALWAYS draw without a write no longer reads as a violation).

| Arm | Job (R+eyes) | Lines | What it shows |
|---|---|---|---|
| K0 | `8180a/b-c31-KD0` | `GXV_HRW_KEEPDEPTH=0` | c181 as it was: in B, Mario never hidden (behind grass he shows in every B and is hidden in every A) |
| K2 | `8181a/b-c31-KD2` | `GXV_HRW_KEEPDEPTH=2` | census only, pixels as K0: read the `hrw: keepdepth31` line (how many first pictures would be kept, why others are refused) |
| K1 | `8182a/b-c31-KD1` | none (the default) | the clear left out: in B Mario is hidden behind grass and posts as in A. Line: kept > 0, **violations 0**, "cleared for" near 0, "fresh player" second pictures > 0 |

(The job ids KD0 to KD2 keep keepdepth31's K-numbers apart from K0a, K0b and K1-cold of this file.) Per arm note the `hrw: keepdepth31` lines, shown fps and evenness (`hrw:` and `stutter:`), GPU ms a picture, and in the flip-book the B pictures next to grass and posts, the box area on the jump, and the pan (does anything new flicker at occluder edges).
**Decision:** K1 shows violations 0 and B agrees with A behind grass: KEEPDEPTH stays on. Violations above 0 or a visible regression: `GXV_HRW_KEEPDEPTH=0` (a `mode_keys.h` row) and send the census line back. `GXV_HRW_BOXDEPTH=1` (and its control with KEEPDEPTH=0) stays an optional arm of keepdepth31's report, not a job here.

### 3.11 Fur, the picture-size gate and the blend fix (fable's second review, D2 to D4) 4 x 6.4m + 2 x 4.9m + 2 x 6.2m

- **Fur** (fur31, `GXV_PB_FUR_SURFACE`, default on; it keeps a coat in view at its old shell count, so the governor can no longer thin it: Performance draws more shells, never fewer). Honeyhive, Performance; both arms `GXV_POPIN_LOG=1`, `GXV_FURCENSUS=1`. **R+eyes `8183a/b-c31-FUR-A`** the default, **`8184a/b-c31-FUR-C`** `GXV_PB_FUR_SURFACE=0` (the old origin rule, the baseline). Climb the Queen from the ground to her head and back (about 2 minutes, camera free), then orbit her at 2000 to 3000 units; then look at a fur planet from a distance. Compare: `popin25:` fur events while she is in view (A target 0, C many), `furcensus:` shells drawn a picture and **GPU ms and the governor level** with her in view, fps, and by eye a shell-count shimmer (thinner or paler pelt). Pass: A holds her coat when the governor changes level and its extra GPU cost is small; otherwise lower `pb_fur` for those stages or send the numbers.
- **GPU gate** (s1117b, `GXV_AUTOQ_GPUGATE`, default on; a picture without a GPU timer never counts as busy, so in a real overload the gate can stay closed where c181 would step down). **U `8185-c31-GATE-ctl`** (default) and **`8186-c31-GATE-off`** (`GXV_AUTOQ_GPUGATE=0`), Beach Bowl 150 s, Performance, scripted route (a GPU-heavy stretch; swap in another stage in the env file if the log shows the GPU quiet). Compare the shed steps (`autoq` lines) and the shown fps. Pass: the default is never worse than `=0` in fps or shed steps; if it holds a size while pictures take over 30 ms, `=0` is the answer.
- **Blend fix timing** (blendfix31, `GXV_POSE_BLENDFIX`, default on, exact against its reference; timing only). **U `8187-c31-BF-0`** (`GXV_POSE_BLENDFIX=0`) against **`8188-c31-BF-1`** (the default), census off in both, Beach Bowl (the penguins) then the Observatory (the Lumas), scripted. Compare the `fps:` lines and the `q30lock` and pair p95 values (`relay/tools/abcompare.py`). Expected: no difference beyond noise. (blendfix31's full route also names Good Egg; the relay's stop list has no Good Egg entry, so add one to `STOP` in `tools/console31/make_jobs.py` if it is wanted.) Its measure and fix runs (`GXV_POSE_BLENDFIX=2` and `=1` with `GXV_POSE_BLENDFIX_CENSUS=1`) are in blendfix31's report, section 6.

### 3.12 Sharpness arms (crisp31, `reports/crisp31.md`): W0, W1 and Q0 to Q4 (owner watches) 9 x about 5m

Pictures are compared paused at the same spot, or in `GXV_HRW_FLIPBOOK` pairs. W arms add the flip-book lines of 3.10.

| Arm | Job (R+eyes) | Lines | Where / what to compare |
|---|---|---|---|
| W0 | `8189a/b-c31-W0` | none | Performance, 60 Assist on. Honeyhive pan over fine ground texture, and the Observatory's railings. Flip-book pairs |
| W1 | `8190a/b-c31-W1` | `GXV_HRW_WARP_NEAREST=1` | same route. In each pair, is the second picture as sharp as the first? Half-pixel shimmer on thin edges? Log: `WARP_NEAREST 1: warps with point reads` > 0, and the pass time on the `hrw:` lines (no cost expected) |
| Q0 | `8191a/b-c31-Q0` | none | Quality (`GXV_PICTURE_MODE=30`) at Dreadnought far zoom (or Kingfin), the GPU-heavy bloom: fps and late pictures |
| Q1 | `8192a/b-c31-Q1` | `GXV_Q30_FULLFX=1` | same place. Log: no `half-size effects and blur on` line, and the `[crisp31] GXV_Q30_FULLFX=1` line at start. Q1 may lose some pictures where Q0 shed the effects (the owner's call against the "never cheapened" rule) |
| Q2 | `8193a/b-c31-Q2` | `GXV_EDGE_DEPTH=1` | Quality at 1:1: screenshots of Mario's eyes, emblems, sign text, a fence or leaf silhouette; detail sharper with the staircase still smoothed; `[aa25]` line |
| Q3 | `8194a/b-c31-Q3` | `GXV_PASSHALF=0` | Quality, Beach Bowl under water or Kingfin (normal bloom): is the glow less blocky? Check the GPU ms |
| Q4 | `8195a/b-c31-Q4` | `GXV_Q30_MID=0` | Quality in a stage that took step 6 (the `q30lock: ... 896x512` line in Q0): 960x544 kept, and fps against Q0 |


**Quality's odd stutter, the three candidates apart** (q30cush31, R `8200`..`8204`, each Quality, Honeyhive 120 s then the Beach Bowl heavy stretch 180 s, the same scripted route):
QS0 baseline with the censuses, QS1 `GXV_POSE_PHASE=1`, QS2 `GXV_Q30_CUSHION=1`, QS3 both, QS4 (optional) `GXV_Q30_DRAIN=0`. Read, per 5 s window, the `q30sep:` line:
(a) `late pictures` (ready after their slot: the pipeline's own long holds), `a long hold ... with the picture on time` (the pacer left a turn out: not the display's margin);
(c) `drains`, `drain holds (1 vblank)`, `late within 1 s of a drain`, `near (margin < guard)` and the `margin ms` histogram (a thin low tail means one spike from a 50 ms hold; a margin that
stays under 8 ms after a hold means the chain does not heal and the cushion will give up); (b) `pose:` decisions at pose age 1+ (zero under `GXV_POSE_PHASE=1`). With the cushion
on: `engaged / let go / gave up / holds avoided / late in spite of it`; it costs one vblank of latency while engaged (read `latency:` pad to flip, which should rise by at most
17 ms in QS2 against QS0). Pass: QS2's 50 ms holds (`smooth:` 50 and 67+) fall against QS0 with `holds avoided` above 0 and no `gave up`; QS1's pose age 1+ falls to 0 and the owner sees the
far plants and enemies step evenly. The three causes are separate if each arm moves only its own line. Report: reports/q30cush31.md.

### 3.13 Mid-pass split and sceGxm ring sizes (midflush31, `reports/midflush31.md`): MF-0 to MF-3 and RING-1, 6 x 6.2m

Both ideas come from sh3vita's notes (GPLv3, ideas only; nothing copied). The **split**: sceGxm starts a scene's vertex work only when the scene ends (or a ring fills), so the Observatory's one big EFB scene
(about 15 ms of CPU recording) runs its vertex phase after the last draw; `GXV_MIDFLUSH` hands the GPU the vertex work recorded so far every so often (`sceGxmMidSceneFlush`), so GPU and CPU overlap inside the scene.
Nothing is drawn differently; the host test proves the same state calls, programs, uniforms and draws in the same order (`tools/test_midflush31_backend.sh`, 8 modes, digest equal). The **rings**: sceGxm's
VDM ring (default 128 KiB), vertex (2048), fragment (512) and fragment USSE ring (16 KiB); sh3vita enlarged VDM to 1 MiB, fragment to 1 MiB and USSE to 2 MiB. The keys exist (`GXV_VITA_VDM_KIB`, `_FRAGMENT_KIB`, `_USSE_KIB`, USSE clamped at 1024).
All six arms: Performance (default), **Observatory 90 s then Beach Bowl 90 s**, the scripted route, and the **same census lines** so controls and arms read alike: `GXV_RING_CENSUS=1 GXV_BE_SPLIT=2 GXV_GPU_SCENES=1`.
Run **off, on, off** (MF-0, MF-1, MF-0b) and compare with `abpair.py` (section 0 item 5: `--on` the arm, `--off` MF-0 and MF-0b, the logs in the order made).

| Arm | Job (U, 6.2m) | Lines added to the census | What it is |
|---|---|---|---|
| MF-0 | `8210-c31-MF-0` | none | control |
| MF-1 | `8211-c31-MF-1` | `GXV_MIDFLUSH=64` | a flush after every 64th EFB draw, wherever that is (sh3vita's N = 60 to 90; their draws are PC-game small) |
| MF-0b | `8212-c31-MF-0b` | none | second control (drift) |
| MF-2 | `8213-c31-MF-2` | `GXV_MIDFLUSH=10064` | after 64 draws, **at the next change of program** (that draw gives its program again anyway: nothing extra to re-issue); at most 32 draws later the flush is forced |
| MF-3 | `8214-c31-MF-3` | `GXV_MIDFLUSH=30003` | adaptive: **3 flushes in the picture's largest EFB scene**, chunk = that scene's draws in the picture before / 4 (at least 24), at a change of program; a small scene is never flushed |
| RING-1 | `8215-c31-RING-1` | `GXV_VITA_VDM_KIB=1024 GXV_VITA_FRAGMENT_KIB=1024 GXV_VITA_USSE_KIB=1024` | the three rings at sh3vita's sizes (USSE 1 MiB, our clamp; sh3vita 2 MiB). +896 +512 +1008 KiB = **2.4 MiB** more, taken from vitaGL's pools (CDRAM first, which is full at stage start: the textures that no longer fit go to RAM/phycont as they already do) |

Other values for later (`GXV_MIDFLUSH`): `n` fixed; `10000+n` boundary; `20000+n` boundary with a chunk of n x 1000 indices (the vertex work, not the draw count); `30000+k` adaptive, k = 1..8 flushes.

**Order:** MF-0, MF-1, MF-0b first (19m). If MF-1 is not better than both controls (below), run MF-2 and MF-3 only to record "no" (they cannot beat a switch that does nothing); if MF-1 is better, run MF-2 and MF-3 and keep the best. RING-1 is independent (run it after MF-0b so MF-0 and MF-0b are its controls).
No restart is needed between arms beyond the relay's own. Never keep an arm's env.txt on the console: the relay puts the owner's back.

**Read, every arm** (`ux0:data/smg/log.txt`):
- First lines: `GXV_MIDFLUSH=<v>: mode M ...` (an arm), `GXV_RING_CENSUS=1` (all arms), and for RING-1 `[vgx-vita] sceGxm rings: vertex 0 KiB, fragment 1024 KiB, USSE 1024 KiB` and the buffers line `VDM ring 1024 KiB`. Memory: the `memory ...: free main, cdram, phycont` lines at start and after the first stage load, against MF-0 (RING-1 should show about 2.4 MiB less CDRAM or main free, no more).
- `midflush:` (every 5 s, only with the switch on): `flushes N a picture (at a boundary, forced, every-n, failed)`, `flush call X ms mean` (cost of a split: want under 0.2 ms), `EFB scene D draws T ms`, `draws left after the last flush` (the tail the GPU still has after EndScene: want it small against D), **`flushes followed by a draw that gave its program A of B`**: A must equal B in modes 1 to 3 (the back end's own exactness check; mode 0 may fall short by a flush after a scene's last draw), `failed 0`.
- `ring-5s:` (all arms): per picture draws mean/max, scenes, **uniform bytes copied** (vitaGL's own 2 MiB uniform pool, not a sceGxm ring), indices; and the **slow calls**: `reserve N (ms, longest)` and `draw N (ms, longest)`: calls of 0.3 ms or more, which only a full ring (or the GPU not yet done) makes. No call reads a ring's fill, so this is the only ring evidence the console can give. `draw` is timed because `GXV_BE_SPLIT=2`.
- `be-split-5s:` `scene begin / end` and `waits`, `render-thread cpu` per picture; `gpu-scenes:` (how many scenes had all their vertex work done before EndScene: should rise in the MF arms); `fps:`, `present:` (ready after), `hitches over 100 ms`.

**Pass rules.**
1. **Exact (every MF arm, first):** no `FATAL`/crash dump, no `sceGxmMidSceneFlush failed` line, `failed 0`, A = B in modes 1 to 3, and **the draw counts equal the control's**: `ring-5s` draws mean within 2 % of MF-0/MF-0b at the same stage (a dropped-draw failure, sh3vita's "late draws lost after each flush", would show as fewer draws and as missing actors or a flash: look at one picture of the Observatory's Lumas and Beach Bowl's penguins; `shots=0` here, so use a hand run with `GXV_MIDFLUSH=<the arm>` if a line is off). The draw-stream hash (`GXV_MIDFLUSH_VERIFY=1`) is for replays of one input, not for these runs (the game is not deterministic between runs).
2. **Better:** `abpair.py ... --metric frame-p95` (and `fps`) says "on better" against MF-0 and MF-0b, **or** the shown fps median is up 3 or more, **and** hitches over 100 ms and the late-picture count are no worse, **and** `flush call` is under 0.2 ms. The GPU ms of `floor:`/`autoq` lines falls by the vertex work that moved before EndScene (comment at the switch): judge by fps and `present:` ready-after, not by GPU ms.
3. **Decision:** the best passing arm becomes the candidate for a default (Observatory and Beach Bowl both at least not worse; record the value). **No arm better: GXV_MIDFLUSH stays 0 and the idea is closed**, with the `midflush:` tail and `flush call` figures in the report so the question is not asked again. A split costs ring and kick time; a stage that is GPU-bound, not CPU-bound, gains nothing from it (the Observatory's recording is the case that should).
4. **RING-1:** in the **control** `ring-5s:` read `draw N (ms)` and `reserve N (ms)`. If the control shows **no slow call** (0) and RING-1's fps matches (abpair: "no difference"), the rings are not a stall source for Galaxy: close the idea, keep vitaGL's defaults, no memory spent. If the control shows slow calls (a few ms a picture) and RING-1 removes them with fps up, keep RING-1's three values as a default candidate (2.4 MiB of the CDRAM-bound texture pool is the price: check no new `out of memory`/texture fallback lines) and bisect (VDM alone first: it is the ring that makes sceGxm start vertex work by itself when it fills, so MF-style splits may already be happening unseen).

### 3.14 Arms added by armaudit31 (18 Nov): every switch that waits for a console A/B (`reports/armaudit31.md`) 58 arms (60 relay jobs), 8220 to 8293

Each env file is `docs/console31/staging/env-<job>.txt`; the job ids are in section 6. **Mode** says where the arm must run: **Perf** = Performance with 60 Assist on (the session's Performance block, build console5 b7d1ed9f); **Quality** = the 30 card
(these can run in the Quality block); **any** = the mode the session is in. Unattended arms (U) judge by log lines; owner arms need eyes (R+eyes). Run an arm and its control in alternation (off, on, off) and read the pair with `abpair.py` (section 0 item 5).

**Warp steadiness (warpsteady31), Perf.** Honeyhive 120 s then Beach Bowl 90 s, scripted (`input-c31-120s.txt`); every arm has `GXV_HRW36_PLANE=1 GXV_HRW36_PLANE_R=500` (the plane exists at all). Read the 10 s `hrw:` lines (`pictures`, `warp edge`, `warp36`, `warpsteady31`): pairs shown as 60, lost second pictures a second, holds started, pairs held, plane pairs below full weight, 30-runs shorter than 0.5 s; frame time must not rise (the hold only skips warps).

| Job | Lines | Pass / question |
|---|---|---|
| `8220-c31-S0` | none | control for S1 to S5, E1 to E3, KD-B1, KD-2D |
| `8221-c31-S1`, `8222-c31-S2` | `GXV_HRW_PLANE_RAMP=4`, `=6` | plane weight changes by at most 0.2 a pair (no flip over 0.5); no new lean after a jump |
| `8223-c31-S3`, `8224-c31-S4` | `GXV_HRW_HOLD30_MS=500`, `=300` | every 30-run lasts the hold or longer; lost second pictures a second not higher than S0 plus the hold's share |
| `8225-c31-S5` | ramp 4 and hold 500 | both together |
| `8226-c31-E1`, `8227-c31-E2` | `GXV_HRW_EDGE_PX=24`, `=64` | E1: more 30 on pans, less smear; E2: fewer drops, more smear (the owner's 15 Nov trial value) |
| `8228-c31-E3` | `GXV_HRW_EDGE_MIRROR=1` | edge smear against mirrored texels |

Decision (warpsteady31): ship S1 or S2 if the ground-band figure falls without a new lean; the hold only if the rhythm reads steady. A pass becomes a Performance row in `mode_keys.h`; the eye part is optional for the first cut.

**Box depth and 2D writes (keepdepth31, emufix31), Perf,** same route as S0. `8229-c31-KD-B1` `GXV_HRW_BOXDEPTH=1` (the box keyed on the full-size depth: `hrw: keepdepth31 B` shows "boxes keyed on the full-size depth" close to the box count and "no depth texture" 0);
`8230-c31-KD-B1off` `GXV_HRW_BOXDEPTH=1 GXV_HRW_KEEPDEPTH=0` (control: "no kept depth" equals pairs; pictures as KEEPDEPTH=0); `8231-c31-KD-2D` `GXV_HRW_KEEPDEPTH_2D=1` (not exact; run it only after K2's census shows "refused only by partial draw writes" over 0; **pass: `cleared for N (inexact, 2D writes not restored: M)` with M = 0**, violations 0).

**Pop-in, the kinds and settings not run above (popin31b, fable-tester31), Perf** (flights exact like P7 to P9; control **P2**; Honeyhive 200 s then Beach Bowl 80 s; `GXV_POPIN_CENSUS=1`): `8232-c31-P11` `GXV_PB_MINPX_FUR=3`, `8233-c31-P12` `_LOD=3`, `8234-c31-P13` `_SPECK=3`,
`8235-c31-P14` `_STARBIT=3`, `8236-c31-P15` `_PARTICLE=3` (their price is read as P3 minus the one-kind arms: the untimed kinds have counts only), `8237-c31-P16` `GXV_POPIN_ZOOM=2` (the game's own distances grow with the handheld zoom, enemies; it changes the original distances: read its price),
`8238-c31-P17` `GXV_POPIN_ZOOM=1` (the port's distances only), `8239-c31-P18` `GXV_POPIN_HYST=8` (costs almost nothing; stops flipping at the limit). **Pass:** "port-made pops at or above it" 0 for that kind and its price against P2 as in 3.4; P18: flips at the limit fall at no cost.

**Pose made only where drawn (bowl31, ondraw31), Quality.** `GXV_TURN_PROF=1 GXV_POSE_PHASE_CENSUS=1` in every arm. Beach Bowl bay 180 s (`8240` to `8243`, bowl31); Beach Bowl 120 s then Honeyhive 120 s for enemies and NPCs (`8244` to `8247`, ondraw31; `GXV_BOWL_POSE` stays 0 there). Verify first:
`8240-c31-BP-V` `GXV_BOWL_POSE=2` (fish "would leave out" over 0, mismatches 0/0/0/0, "stale at draw" 0, no `bowl-pose: MISMATCH`), `8244-c31-OD-V` `GXV_POSE_ONDRAW=2` (several kinds over 0, 0/0/0/0, no `pose-ondraw: MISMATCH`; the NPC-goods false alarm was fixed by emufix31). **Any mismatch: keep the log and do not take mode 1.**
Then off, on, off: `8241 BP-0`, `8242 BP-1`, `8243 BP-0b` and `8245 OD-0`, `8246 OD-1`, `8247 OD-0b`. Pass: `LATE reads` 0 (or near it), `drift` 0, `stale at draw` 0, left out close to asked minus refusals, and `abpair.py --metric pair-p95` (the `q30lock: ... game pair p95` line) "on better", or the turn profile's game-thread ms lower by 0.3 or more. ondraw31's switch supersedes bowl31's.

**Particle worker priority and pause (parprio31), Quality.** Honeyhive 150 s then Beach Bowl 150 s, `GXV_TURN_PROF=1`. `8250-c31-PP-A` (`GXV_PAR_PRIO=0 GXV_PAR_PAUSE=0`, today), `8251-c31-PP-B` (`=1` and `=1`), `8252-c31-PP-C` (`=1` and `=2`, only if PP-B still stalls), `8253-c31-PP-Ab` (control again).
Read `par-prio:` and `par-ptcl:`: joins max (A 2.4 to 4.1 ms; expected under about 0.5 in B and C), stalls (A 2 to 3 a 5 s; expected 0), paused turns (A 247 to 295 of 300; expected under 30), `cores-5s:` core 3 busy (about +0.3 ms a picture) and `threads: core 3:` catch-up (must stay under 0.5 ms a picture). Mode 2 only where B's stalls are not 0.

**Texture upload (texup31), any,** Observatory then Honeyhive, stage loads, no route: `8255-c31-TU-0` `GXV_TEXUP_CENSUS=1` (the split and the `soft census` field), `8256-c31-TU-1` plus `GXV_TEXUP_LAZYFENCE=1 GXV_TEXUP_SCRATCH=1`
(wait and alloc per upload, the stage's ms an upload against TU-0; LAZYFENCE needs `GXV_VITA_UPLOAD_SYNC=2`, which is the game's default), `8257-c31-TU-V` plus `GXV_TEXUP_VERIFY=1` (**`differing 0`** is the exactness proof; judge no timing). Pass: TU-1's ms an upload lower, TU-V differing 0, no new `out of memory`.

**Shader compile worker (asyncsh31), any,** each run on its own fresh tagged cache (tags `c31a`, `c31b1` to `c31b3`, `c31m2`; Observatory 60 s then Honeyhive 120 s); `GXV_PACK_MISSLOG=1 GXV_HITCH25_LOG=1 GXV_UBERSHADER=1 GXV_LOG_LEVEL=2` in all.
`8260-c31-AS-A` baseline and census (worker off; `first-use: ubershader census:` is **census only: `GXV_UBERSHADER` stays 0**); `8261`, `8262`, `8263` `GXV_ASYNC_SHADERS=1` (three runs, a new tag each); `8264-c31-AS-M2` adds `GXV_MISS_STANDIN=2`. **Pass (B):** no crash or power-off; `threads:` `smg_shaders` mask `0x60000`; `async-shaders-5s:` 0 refused allocations (or each shows as a render-thread compile), 0 pooled reallocs passed on, longest wait well under 15 s;
no skippable-draw compile on the render thread in play; no new picture over 100 ms that the census does not attribute to a must-block compile; report the `hitch33-standin:` counts (those pictures are look-alikes). Stand-in level 1 is the default and only chooses what a waiting draw shows: no arm for it.

**Array bases (`GXV_ARRAYSKIP`, lighter25, exact), Quality,** Honeyhive 120 s then Beach Bowl 120 s, `GXV_TURN_PROF=1`: `8270-c31-AK-V` `=2` (the front end's **`arrayskip verify:` must say 0 differing**; no timing), `8271 AK-0`, `8272 AK-1`, `8273 AK-0b` (off, on, off; the `arrayskip:` line gives writes and writes left out a picture). Pass: verify 0 differing and game-thread ms a turn lower by 0.3 or more, or pair p95 "on better". This is gxdedup31's cheaper first A/B.

**Fclose lock hitch check (`GXV_FCLOSE_LOCK`, fable-review-integ31c S3), any,** cold tagged cache so the card is written to, `GXV_HITCH25_LOG=1`: `8275 FL-1` (the default, on), `8276 FL-0` (`GXV_FCLOSE_LOCK=0`), `8277 FL-1b`, `8278 FL-0b` (tags `c31f1`, `c31f0`, `c31g1`, `c31g0`). Pass: hitches over 100 ms with the lock on not more than off (pairs). The lock is a bug fix and stays on unless it shows a game-thread wait.

**Owner arms (R+eyes), Perf, Honeyhive:** `8280a/b-c31-FUR-B` `GXV_FUR25=1 GXV_POPIN_LOG=1 GXV_FURCENSUS=1` (the Queen climb as FUR-A; `furcensus:` shells a picture lower than FUR-A, and is the pelt thinner or paler?), `8281a/b-c31-V7` `GXV_DIAG_FULL_FAR_POSE=1` with `GXV_POSE_PHASE_CENSUS=1` and the bee-suit route of V0 (log `far-pose-test: restored ...`; does the lily, flower or Luma vibration go?).

**Censuses to read (log only, not arms; each alone because their timers nest), any:** `8285-c31-CENS-FP` `GXV_FEPLAN_CENSUS=1` (read "a plan could take N (S state cmds, D draws, T ms)"; go on to `GXV_FEPLAN=1` only if T is 0.3 ms or more with S/N about 20 or more),
`8286-c31-CENS-TL` `GXV_TINYLIST_CENSUS=1` ("could replay N (ms)": go on to `GXV_TINYLIST=1` only if 0.5 ms or more at Honeyhive), `8287-c31-CENS-PJ` palette join and stream census (`GXV_PALJOIN_CENSUS=1 GXV_FE_CENSUS=1 GXV_STREAM_CENSUS=60 GXV_BE_PROF=4 GXV_PAR_ANIM_WHY=1`; judge no ms),
`8288-c31-CENS-GPU` `GXV_GPU_SCENES=2 GXV_GPU_CENSUS=1` (Quality; Dreadnought far zoom then Beach Bowl; GPU figures only). **`GXV_FEPLAN=2` and `GXV_TINYLIST=2` (verify) need a diagnostics build (`PORT_RUNTIME_DIAGNOSTICS`); the console build takes them as 0, so there is no console verify arm, and the `=1` A/B waits for the census.**
`GXV_GX_DEDUP` needs a `-DPORT_GX_DEDUP_BUILD=1` build and its estimate is a net loss (gxdedup31: "no console slot for this now"): no arm. `GXV_BE_LEAN` and `GXV_PALMODEL` are plans, not code.

**vitaGL test archives (vglup31), Perf, build arms.** Not env switches: `out/vglA` (eboot aed669ae...: micro-opts and glFinish without an empty scene) and `out/vglB` (563dc274...: A plus `NO_TILE_CLIPPER`, less CPU and more GPU) are builds of console4 (e76e06fc), whose eboot is the control. Put each eboot (as for console2 and 4), then run the same job:
`8290-c31-VGL-0` (console4), `8291-c31-VGL-A`, `8292-c31-VGL-B`, `8293-c31-VGL-0b` (console4 again, drift). Honeyhive 120 s then Beach Bowl 120 s, scripted, `GXV_TURN_PROF=1`. Pass: A not worse, and better only if frame or pair p95 says "on better" (micro gains expected; "no difference" closes it); B only if the GPU ms does not rise by more than the CPU ms falls. `GXV_VGL_ID` stays as it is: the shader seed stays valid.

## 4. The order, the minutes, the cut lines

All relay figures are estimates (see the legend in section 0). Totals are console time, owner included.

| # | Step | Min | Who | Cumulative |
|---|---|---|---|---|
| 1 | Section 0: backups, relay install | 10 | H and R | 10 |
| 2 | Block 1 (1.0 to 1.8): settings restart, first visits, stamp, online | 81 | H | 91 |
| | **CUT LINE A: single-player defaults and the settings fixes are checked (1 h 31)** | | | |
| 3 | Probe, K0a (section 2) | 10 | R | 101 |
| 3b | **P0 and P2: flights exact, the cheapest high-value arm (section 3.4), first** | 16 | R | 117 |
| 4 | A1 C182 set: unattended run, owner's on/off pair | 21 | R, R+eyes | 138 |
| 5 | V0 and V4 (pose phase) | 15 | R+eyes | 153 |
| 6 | A2 LA88 mips | 6 | R | 159 |
| | **CUT LINE B: flights exact, the three non-exact picture items and the vibration have a verdict (2 h 39)** | | | |
| 7 | P7, P8, P9 (the floor by kind: shadows, plants, plants with one draw) | 23 | R | 182 |
| 7b | P1, P3, P4 (the budget's full price, every kind at 3 and 6 px) | 23 | R | 205 |
| 8 | F30 cap rule | 4 | R | 209 |
| 9 | A3 C185 hold (K1-cold, A3-hold) | 12 | R | 221 |
| | **CUT LINE C: pop-in prices by kind and the hold (3 h 41)** | | | |
| 10 | Scenario order (S1, S0 pairs) | 28 | R | 249 |
| 11 | CPU switches: verify, control, six A/B | 41 | R | 290 |
| 12 | DMA A/B/C, clock census, HSR census | 29 | R | 319 |
| 13 | K0b (drift control) | 6 | R | 325 |
| 14 | Section 5: restore and cleanup | 8 | H and R | 333 |
| | **CUT LINE D: the complete list (5 h 33)** | | | |
| | Conditional and optional, added to D only when named | | | |
| | V3, V1, V2, V5, V6 (when the decision tree asks) | 38 | R+eyes | |
| | V0q, V4q (only if the owner saw it in Quality) | 15 | R+eyes | |
| | P5, P6, P10 | 24 | R | |
| | G-ctl and G-off (only if the gate holds a size) | 10 | R | |
| | Fable's second-review arms, section 3.10 to 3.12: K0, K2, K1 (14), fur A and C (13), GATE ctl and off (10), BF-0 and BF-1 (12), CPU-all4 (5), W0 and W1 (13), Q0 to Q4 (26): added to D when named | about 93 | R, R+eyes | |
| | HSR-gain and HSR-ctl (only if census frees a draw) | 13 | R | |
| | Mid-pass split and rings, section 3.13: MF-0, MF-1, MF-0b first (19), then MF-2, MF-3 and RING-1 (19), added to D when named | 37 | R | |
| | BE_PROF | 4 | R | |
| | armaudit31 arms (section 3.14), **Quality-now**: bowl31 and ondraw31 (8240 to 8247, 8 x 5 to 7), parprio31 (8250 to 8253, 33), arrayskip (8270 to 8273, 29), in this order | about 100 | R | |
| | armaudit31 arms, any mode: texup31 (8255 to 8257, 14), shader worker (8260 to 8264, 31), fclose lock (8275 to 8278, 25), censuses (8285 to 8288, 28) | about 100 | R | |
| | armaudit31 arms, **Performance** (60 Assist on): warp steadiness S0 to S5, E1 to E3 (60), box depth and 2D (20), pop-in kinds P11 to P18 (62), FUR-B and V7 (owner, 14), vitaGL archives (29) | about 185 | R, R+eyes | |
| | A1 bisect (one line a run, only if the set failed) | up to 30 | R | |
| | **Everything: 8 h 24** | **504** | | |

**Never cut section 5.** If the session ends early run `8990-c31-restore` and do section 5 by hand. K0b should be run even when later arms are cut if any fps numbers are to be trusted.
Heat and battery: the relay's runs keep the screen on (`nosleep on`); leave the Vita on the charger and let it cool two minutes between hand block and relay block.

## 5. Restore and cleanup (8 min, H and R)

1. Queue **`8990-c31-restore`**: closes the game, removes `input.txt`, **puts the owner's `env.txt` back** from `relay/staging/env-user.txt`, pulls the logs. If the owner had no env.txt, delete the Vita's env.txt in VitaShell.
2. **Settings:** if step 1.6 left `player-settings.cfg.mine`, delete the test `player-settings.cfg` and rename `.mine` back. Otherwise, copy the backup of step 0.3 over `player-settings.cfg` (the tests changed Picture, 60 Assist, buttons and language). Delete only files this session made: `player-settings.untested`, any `player-settings.before-3.0*.cfg` (there should be none: that is test 1.6).
3. **Saves:** compare `GameData.bin` with S0. Keep the new save if the owner wants the progress; copy the section 0.2 backup back only if a save is damaged or the owner wants the pre-test state. Look for `GameData.bin.mptmp` / `.mpbad` leftovers (none expected).
4. **Our test files** (VitaShell): `ux0:data/smg/c31k-*`, `c31h-*`, `c31s-*`, `c31t-*`, the armaudit31 tags `c31a-*`, `c31b1-*` to `c31b3-*`, `c31m2-*`, `c31f1-*`, `c31f0-*`, `c31g1-*`, `c31g0-*` (stagewarm, shaderlist, journals), the folders `ux0:data/smg/shadercache/*-c31k`, `-c31h`, `-c31s`, `-c31t` (and the folders of the armaudit31 tags above), and `popin31-p1-presets.txt`. The owner's own shader cache and lists were never written by the tagged runs.
5. **Final check against the section 0 listing:** `env.txt` is the owner's, `input.txt` is absent, no `ur0:tai/config.txt` change, no plug-in added, the VPK is the one installed. Keep `relay/outbox/` (the logs); scrub names and addresses before sharing any of it.

## 6. The relay jobs

Generated by `python3 tools/console31/make_jobs.py all` (`docs/console31/jobs/*.json`, `staging/*.txt`, `JOBS.txt`); the file format is the relay's own (a `batch` of `put`, `launch`, `vc`, `shot`, `sleep`, `close`, `pull_logs`, `clear_input`, `ls`, `get`, `status`), the same as `tools/pack24/make_jobs.py`. Job numbers 81xx: no other job of the last weeks uses them. A job that `put`s writes only `ux0:data/smg/*.txt`; the generated files were checked against the relay's path rules and with `tools/check_env_file.py` (all 0 errors, 0 warnings).

| Jobs | What | Eyes |
|---|---|---|
| 8100, 8101, 8108 | backups: list and pull env/settings/logs, copy the settings file, eboot hash | no |
| 8102 to 8107 | block 1 checkpoints CP1 to CP6: pull the logs, list the folder | no |
| 8120, 8121 | probe, K0a | no |
| 8122 | A1 C182 set, scripted | no |
| 8123a/b, 8124a/b | A1o on and off: the owner plays | **yes** |
| 8125a/b to 8133a/b | V0, V4, V3, V1, V2, V5, V6, V0q, V4q: the owner plays | **yes** |
| 8134 | A2 LA88 | optional glance |
| 8135 to 8141 | P0 to P6 pop-in, scripted (P0 and P2 first, step 3b) | optional (fly-in end, shadows, grass) |
| 8142 to 8144 | F30, G-ctl, G-off | no |
| 8145, 8146 | K1-cold, A3-hold | no |
| 8147 to 8150 | scenario order S1 and S0, learn and measure | no |
| 8151 to 8158 | CPU verify, control, six A/B | no |
| 8159 to 8166 | DMA A/B/C, clock census, HSR census/gain/control, BE_PROF | no |
| 8167 | K0b | no |
| 8170 to 8173 | P7 to P10 pop-in by kind (popin31b), scripted | optional |
| 8180a/b to 8182a/b | keepdepth K0, K2, K1 (flip-book): the owner watches | **yes** |
| 8183a/b, 8184a/b | fur A and C (the Queen climb): the owner climbs | **yes** |
| 8185, 8186 | GPU gate default and `=0` | no |
| 8187, 8188 | blendfix timing `=0` and default | no |
| 8189a/b to 8195a/b | crisp31 W0, W1, Q0 to Q4: the owner watches | **yes** |
| 8196 | CPU-all4: REPLAY25, RT_INST, STARBATCH, PAR25 at `=1` together | no |

| 8200 to 8204 | QS0 to QS4 Quality's odd stutter, three candidates apart (q30cush31, below), scripted | optional |
| 8220 to 8231 | warp steadiness S0 to S5, E1 to E3, box depth KD-B1, KD-B1off, KD-2D (Perf; armaudit31, 3.14) | optional |
| 8232 to 8239 | pop-in P11 to P18: fur, LOD, speck, star-bit, particle floors, handheld zoom 2 and 1, hysteresis (Perf) | optional |
| 8240 to 8247 | bowl31 BP-V, BP-0, BP-1, BP-0b; ondraw31 OD-V, OD-0, OD-1, OD-0b (Quality) | optional |
| 8250 to 8253 | parprio31 PP-A, PP-B, PP-C, PP-Ab (Quality) | no |
| 8255 to 8257 | texup31 TU-0, TU-1, TU-V | no |
| 8260 to 8264 | shader worker AS-A, AS-B1 to AS-B3, AS-M2 (fresh tagged caches) | no |
| 8270 to 8273 | `GXV_ARRAYSKIP` AK-V, AK-0, AK-1, AK-0b (Quality) | no |
| 8275 to 8278 | fclose lock FL-1, FL-0, FL-1b, FL-0b (cold tagged caches) | no |
| 8280a/b, 8281a/b | FUR-B (`GXV_FUR25=1`), V7 (`GXV_DIAG_FULL_FAR_POSE=1`): the owner plays | **yes** |
| 8285 to 8288 | censuses: FEPLAN, TINYLIST, palette join + stream, GPU (log only) | no |
| 8290 to 8293 | vitaGL archives VGL-0, VGL-A, VGL-B, VGL-0b (a different eboot per run) | no |
| 8985 | shader-cache pull (strk31, section 8): close the game, copy `ux0:data/smg/shadercache` back, read-only | no |
| 8990 | restore | no |

To run: `make_jobs.py install --relay <relay>`; put the owner's env in `relay/staging/env-user.txt`; then `make_jobs.py queue --relay <relay> 8120-c31-probe 8121-c31-K0a ...`.
The relay takes the inbox in name order and snapshots it at the start of a pass; a job renamed `*.hold` waits. **Queue a `...b-...-end` job only when the owner says done**, or the relay closes the game under the player.
Every job's results land in `relay/outbox/<job>/` (`result.json`, the logs, shots).

Unattended runs never write the save (`PETARI_BOOT`), but they do read it: the first save file that exists is loaded. A scripted route (`input-c31-120s.txt`, `-150s`, `-200s`) is the seedwalk route, so every arm plays the same inputs.

## 7. What each check came from

| Check | Source |
|---|---|
| Held room poll, Friends in a room, `sockets:` line, FTP after a long run | netport31 ("Needs the console") |
| Pause hint, Triangle, Less Motion, Tilt, Tilt Strength live | linkfix31, emu31b |
| Restart from pause, Save and Start Again, Yes/No by pad, No / Keep Editing / entry screen, 60 Assist from pause | setapply31 P1-P5, setfix31, setnat31 |
| "Finishing saving", Later, stuck choice | setfix31b (console checks 1-4) |
| Saved buttons after a restart, stamp, `.before-3.0` files, `.untested` marker | setfix31 |
| `GXV_FPS_LIMIT=30` rule | setnat31 section 4 |
| Observatory first entry (targeted mask), Honeyhive first load, plant reveal A/B, depth-edge vibration | c185merge, seedrun31b, fix31 (`cover:` line) |
| C182 set, detail budget, bloom "inside a pass", menu-tier append, round box | c183fix, fix31 (decision 1), checklist 0b |
| LA88 mips, scenario order, C185 hold | checklist 0b, fix31, seedrun31b |
| Pose phase V0-V5 | ratecut31 section 5 |
| Pop-in P0-P5 and the P1 presets file | popin31 |
| Pop-in P2 first, P7-P10 (the floor by kind, plants' one draw, the memo verify) | popin31b |
| Picture-size gate, floors census line | s1117b |
| DMA, clock census, per-draw uniforms | dma31 |
| Six CPU switches, ties census at Dreadnought, texture-upload census | perf-inventory (C-off18, D5, D18); ties25 report section 6 |
| Verify and A/B lines of REPLAY25, RT_INST, STARBATCH, PAR25; IDLE25 and NOOP_FILTER held | emuverify31 |
| keepdepth K0/K2/K1, fur, GPU gate, blendfix timing | fable's second review of integ-3.1 (fix-now item 10), keepdepth31, fur31, s1117b, blendfix31 |
| W0, W1, Q0 to Q4 | crisp31 |
| Mid-pass split MF-0 to MF-3, sceGxm rings RING-1, the `midflush:` and `ring-5s:` lines | midflush31 (ports31 items 2 and 3; sh3vita's SH3_SPLIT and ring sizes as ideas) |
| Boot of the whole line, packaged list tag, LiveArea files | the integ-3.1 review (release-blocker table), fix31 F3-F4 |
| Clocks line check (step 1.1), shader-cache pull (section 8) | strk31 (Strikers' notes of 13-18 Nov: `pull_cache`, the refused 500 MHz request) |

## 8. Pull the console's own shader cache (strk31; do it at the END, after every arm and before or after the restore job)

Why. A `.gxp` is written only when the console really compiles a program. The emulator walk (`tools/seedwalk`) never sees a program that only the console's draws reach: Strikers' read-only pull of their test copy's cache brought back 21 programs their emulator collection had missed. Console files are the reference for the seed: `refill_pack.py` already byte-compares every emulator file against them (COMPILER-IDENTITY.txt) and never replaces a console file by an emulator one.

**Job `8985-c31-pull-shadercache`** (R, 1 min, `make_jobs.py install` puts it in `claude-jobs-console31`): `close`, `ls ux0:data/smg/shadercache`, `pull_tree ux0:data/smg/shadercache` (64 MB cap), `ls ux0:data/smg`. **Read-only:** no `put`, no `launch`, no delete, nothing is written on the Vita (tests/`tools/test_strk31.sh` asserts the job has only those four actions). It closes the game first so no `.gxp` is half-written. The result is `outbox/8985-c31-pull-shadercache/` (a `shadercache/` tree: the key folder `<key>/v3/{v,f}/HH/HASH.gxp`, `vgx_shaderlist.bin`, `vgx_stagewarm.bin` with its `.jnl` and `.ply`, and the tagged `<key>-c31k|h|s|t` folders of this session). If the relay answers "too big", pull the key folder alone by hand with VitaShell/FTP (read only) and put it in a folder shaped `shadercache/<key>/v3/...`.
Do **not** ask for the pull while an arm is running, and never delete or move anything in that folder: the owner's own cache is there.

**Merge into the seed** (host, `tools/pack24/refill_pack.py`, run with `python3 -I`; the pulled folder or its `.tgz` goes in `--pulled`, the emulator walk's harvest in `--harvest`; the console files are the reference):

```
python3 -I tools/pack24/refill_pack.py --package <2.x .vpk | shadercache/> --pulled <outbox/8985-.../> --harvest <seedwalk harvest> --out <new folder> --source <tree> --log <a log with "compiled-shader cache: ...">
```
Read in the output: `pulled[0]: N .gxp`, `console-only files (compiled on the console, in no emulator harvest): K of N pulled -> CONSOLE-ONLY.txt` (K is the gain of the pull; Strikers' K was 21), `compiler identity ... PROVEN: M shared texts, 0 differ` (a console file and an emulator file of the same name must be byte-identical; anything else is BROKEN and the emulator's files stay out), and the closure line (`0 open`). `NEW-FROM-CONSOLE.txt` lists every console file the package seed lacked. The tagged `-c31*` folders are test caches (other shader keys): `refill_pack.py` reads only the key folder it is asked for and says `other key folders ignored`. Pull the card's folder once with the tour's own env (no tag) if the point is the seed; the A/B arms with a tag compile nothing the default key needs.

**Reading the late-picture split (no new log line was needed, item 5 of the Strikers notes).** Strikers added a 30 s `[head]` line (game thread with and without a simulation step, render thread, GPU, pictures over the limit by cause). Galaxy has the parts: `turn-prof:` (`GXV_TURN_PROF=1`, every 5 s: the game thread's turn, a drawn turn = the picture's turn with its step, an undrawn turn = a step with no picture, `busy` and buckets), `workload: fe-split` (front end per picture), `floor:` (every 5 s: late pictures and the time in them by the largest stage: GPU, game CPU, renderer CPU, none, unknown), `pace23:` / `pace60:` (GPU busy ms a picture, TA / 3D / display), `rate:` (lanes at a decision: game, front end, render, GPU ms), `cpugov:` (every 30 s in Performance with the governor on: late by GPU / game / renderer / front end), `smooth:` and `q30sep:` (Quality: late pictures, drain holds, pose beats). Run the arm with `GXV_TURN_PROF=1` and read those lines side by side before deciding that 60 is GPU work.
