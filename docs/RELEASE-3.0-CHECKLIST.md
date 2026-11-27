# Release 3.0 checklist (for the owner)

Do these in order. Nothing here has been run for you. Tick each box yourself.

## A. The release in order (18 Nov; the short list, details in the sections below)

The game ships as **3.0** (VPK 03.00, `SMG_APP_VERSION` 3.0.0, build id `3.0.0`, label `3.0`). "3.1" is the branch and the test builds' name only (build id `3.1-dev`, which no gate accepts for a release). One build slot, one build at a time.

1. [ ] **Console session** (`CONSOLE-SESSION-3.1.md`, with section 0b below): every A/B arm decided; each default that passed is turned on in the tree. Section 8 of that file pulls the console's own shader cache last (job `8985-c31-pull-shadercache`).
2. [ ] **Shader seed merge** (host): `python3 -I tools/pack24/refill_pack.py --package <the 2.x VPK or its shadercache/> --pulled <the pulled folder> --harvest <the seedwalk harvest> --out <new folder> --source <tree> --log <log>` (`CONSOLE-SESSION-3.1.md` section 8: `compiler identity ... PROVEN`, `0 open`). The result is the seed folder for step 7. Tag its stage list: `python3 -I tools/seedrun/stagetags.py check <its vgx_stagewarm.bin>` (refill_pack runs `legacy` itself).
3. [ ] **PGO re-record** (`reports/pgotool31.md`, `tools/pgo/record_all.sh`, writes `/home/claude/w/cache/pgorec31/smg.profdata.gz`; its log must say `0 line(s) naming /home`). Install it in both places the build reads, then check it:
   - `P=/home/claude/w/cache/pgorec31/smg.profdata.gz; gzip -t $P`
   - `cp $P tools/pgo/smg.profdata.gz` (the tree's copy, committed) and, keeping the old one beside it, `cp $P /home/claude/w/kit/c181kit/scripts/pgo/smg.profdata.gz`; put its new sha256 on the `scripts/pgo/smg.profdata.gz` line of `/home/claude/w/kit/c181kit/SHA256SUMS` (`build31.sh` stops on a kit checksum mismatch). `release30.sh`'s `pgo` gate fails unless both copies are the same file.
   - the scan (`tools/pgo/scan_units.py`, `reports/pgotool31.md` step 4): renderer units under about 12 hash mismatches, game units under 30 (`reports/pgo31.md` section 2).
   - `python3 -I tools/update-checksums.py`, commit.
4. [ ] **Final review**, then **freeze**: the frozen ref is one commit (tag it locally, for example `release-3.0-frozen`). Everything below is built and published from that commit only.
5. [ ] **Version and service strings** are 3.0.0 in the frozen tree (section 1): `python3 -I tools/release30_gates.py tree --root .`.
6. [ ] **Emulator gate** on three stages and the **PGO twin** (sections 4b and 4c) on the release eboot, once it exists (step 7); they need the build.
7. [ ] **Build, package, gates** (the one build slot, about 35 min cold plus the `wformat` gate, 45 min niced): `tools/release30.sh <frozen ref> /home/claude/w/out/release30 --seed <seed folder of step 2>`. It runs `build31.sh` with `LABEL=3.0 BUILD_ID=3.0.0 REPRO=1`, makes the compact VPK (`smg-vgx.vpk` target, then `stagetags.py package` and `checkvpk`: one `v3/seed.c182`, the stage list tagged), then the gates (build id with `--release`, link checks with `vita_must_define.txt`, the 15-symbol and TLS checks, display-queue stack, `-Wformat` on all three databases, the five contributor PNGs, the VPK's member list, no build path in the ELF, the profile) and writes `RELEASE-INFO.txt` (ref, sha256 of eboot and VPK, build id, gates). Exit 0 only when every gate ran and passed; `--skip-wformat` gives exit 3, which is not a release. Then step 6.
8. [ ] **Public snapshot** (no history; Codex's commits and the internal ones carry the owner's name): `tools/make_public_snapshot.sh <frozen ref> /home/claude/w/out/public30 --vpk /home/claude/w/out/release30/Galaxy-Vita.vpk`. One commit by the public identity, a tag `v3.0.0`, a bundle, `PUBLIC-SNAPSHOT-INFO.txt`; it scans the tree for private names, session links, secrets and private folders and stops if it finds any. It never pushes.
9. [ ] **Hand over to Codex**: `docs/CODEX-PUSH-3.0.md` is the prompt. Codex pushes the snapshot (we never do) and attaches the VPK to the GitHub release.
10. [ ] **Server package** (a separate Codex step, `out/svc1118/smg-online-server-20261118a.tgz`; its sha256 sits beside it): deploy as section 3 says, before the release is announced, with `workers_dev` off only after the hardware test passed on `galaxy.psvita.dev`. The server values (section 2) must name build `3.0.0` only. Do it before step 9's release goes public, so the first players do not meet "online is not ready".
11. [ ] Say only what was shown (section 0): the text of the release, README, CHANGELOG and KNOWN-ISSUES must match what the console session proved.

## 0. Say only what has been shown

- [ ] Multiplayer has never run on a real console. Do the steps in `docs/MULTIPLAYER-HARDWARE-TEST.md` first. For each step that passes, cut that item from the "Not yet verified on a real console" list in `KNOWN-ISSUES.md`, and from the matching sentence in `CHANGELOG.md` and `docs/MULTIPLAYER.md`. For each that fails, keep it and say what you saw.
- [ ] Re-read the status box at the top of `docs/MULTIPLAYER.md` and the first paragraph of the 3.0 section of `CHANGELOG.md`. They say "never run on a real PS Vita". Change them only when it is no longer true.
- [ ] The online service: say in `README.md`, `CHANGELOG.md`, `KNOWN-ISSUES.md`, `docs/MULTIPLAYER.md` and `docs/PRIVACY.md` whether the build you ship is connected to it. Today the client builds default to `https://galaxy.psvita.dev`, but online play is not open yet.
- [ ] The sentences about saves (how a save is written, "Play on a Copy", recovery) and about strangers must match the code you ship. Re-read them after every change to either.
- [ ] Boss fights: each console fights its own boss and a boss is not shared. `docs/MULTIPLAYER.md`, `KNOWN-ISSUES.md` and `CHANGELOG.md` say so; make sure no page, store text or trailer promises "fight bosses together".
- [ ] The safe single-player save is OFF by default and ON with Settings "Safe saves" or `SMG_ATOMIC_SAVES=1` (then `GameData.bin.mptmp` and `GameData.bin.mpbak` sit next to the save); it is always on in a multiplayer session. Check that `docs/MULTIPLAYER.md`, `docs/PRIVACY.md`, `docs/MP-DATA-FORMATS.md` and `KNOWN-ISSUES.md` agree, and that the cost per save (measured only on a computer, not on a Vita) is not given as a console result.
- [ ] The "Who Can I Play With?" test is reachable only after a failed online try, and no screen writes a report. Do not describe either as something a player can do, unless that changes.
- [ ] Reconnect after a Wi-Fi drop (about 30 s when the system reports it, about 75 s when the link goes silent) and "Continue <name>'s Game" (2 minutes) are described as built, not as tested. Keep it so until `docs/MULTIPLAYER-HARDWARE-TEST.md` steps 11 and 12 pass.

## 0b. Console A/B first: the non-exact defaults that are OFF until they pass (owner, 17 Nov)

Single player behaves like 2.3 (c181) until each item below passes on hardware. Each is OFF by default in this tree and is the
A/B arm, in this order, against the same build with an empty `env.txt` (the control). Add the arm's line to `ux0:data/smg/env.txt`,
play the same route, compare the logs and what you see. Turn an item on by default only after it passes, and record the decision.

- [ ] **C182 Performance warp** (Picture: Smooth Motion): `GXV_HRW41_DEPTH=1`, `GXV_HRW41_BOXKEY=1`, `GXV_HRW41_BOX_ROUND=1`,
      `GXV_HRW41_BUDGET=1`, `GXV_HRW_STAGE_AUTO_RETRY=3` (C182's values; the mode table no longer sets them). With
      `GXV_HRW41_BUDGET=1` the c183fix detail budget also acts (off under 90, gives up after 3), and the Observatory mask prewarm
      (`GXV_C185_ASTRO_MASK_WARM`, on) only has a consumer with `GXV_HRW41_DEPTH=1`: test them in this arm.
- [ ] **Compact generated mipmaps** (C183): `GXV_LA88_MIPS=1` (`docs/c183-compact-mips.md`; the log's `compact mips:` line, and
      the self-test's verdict).
- [ ] **Scenario hints** (seedrun31): `GXV_STAGEQ_SCENARIO=1` (stage records carry the scenario they were drawn in and a load
      queues its own scenario's first). With 0 the order is the order before seedrun31 and no hint is written.
- [ ] **C185 load hold with an unknown free-VRAM reading**: `GXV_C185_HOLD_UNKNOWN_VRAM=1` (C185's rule: the hold no longer waits
      for prewarm work that cannot run). With 0 (default) the hold is C184's. Pictures are the same; only the hold's length differs.

The whole console session, in order, with each arm's exact lines, route and pass rule, and its relay jobs, is in [CONSOLE-SESSION-3.1.md](CONSOLE-SESSION-3.1.md).

Stay ON by owner decision: `GXV_HONEY_PLANT_REVEAL` (C185's Honeyhive plant reveal) and `GXV_C185_ASTRO_MASK_WARM` (exact).

Owner decision (18 Nov): Quality may cut effects to keep a smooth, consistent 30. The severe-overload step that halves effects and blur
(`GXV_AUTOQ_OVERLOAD=2`, as c181) stays on in Quality; `GXV_Q30_FULLFX` (default 0) is only a measurement arm, not a candidate default.

## 1. Files that carry a version string

Change all of them together. Current values are in brackets.

**This release ships as 3.0** (owner, 17 Nov). Leave `SMG_APP_VERSION` (`"3.0.0"`), the VPK's `VERSION 03.00` and
`cboc-release.json` (`galaxy-v3.0`, `"3.0"`) as they are; the service's allow-lists stay on 3.0.0. The 3.1 name is the
integration branch only.

- [ ] `vita/vgx-game/CMakeLists.txt` (the `vita_create_vpk` line, `VERSION 03.00`): the Vita bubble version.
- [ ] `cboc-release.json`: `releaseId` (`galaxy-v3.0`), `version` (`"3.0"`) and the package sha256 (still `PENDING-VPK`).
- [ ] `docs/SOURCE.md`: the "Version 2.0" heading.
- [ ] `SHA256SUMS`: regenerate after the VPK is built.
- [ ] `START-HERE.txt`.
- [ ] `platform/src/mp/mp_service.cpp`: `SMG_APP_VERSION` (`"3.0.0"`).
- [ ] `net/src/online/np_app_version.h`: the same value (`"3.0.0"`). The Strikers game uses `1.3.0`; do not touch that.
- [ ] The settings version (the stamp the stale-settings guard writes into `player-settings.cfg`) follows `SMG_APP_VERSION` in `platform/src/mp/mp_service.cpp`: `vita/vgx-game/CMakeLists.txt` reads it from that source. A 2.3 release built from a tree that contains the guard must set its own version there, or not ship the guard. Never pass `GALAXY_SETTINGS_VERSION` by hand, and never override `SMG_APP_VERSION` with a compiler flag (the CMake reads the source, so a flag would make the stamp differ from the version the game reports). `tools/ccflags.json` (cc.sh checks, not a release file) carries a copy; keep it equal.
- [ ] `server/wrangler.smg.toml`: `MIN_APP_VERSION`, `LATEST_APP_VERSION`, `SMG_TIME_BUILDS`.
- [ ] `server/src/core/config.js`: defaults for the same values.
- [ ] `server/DEPLOY-SMG.txt` and `server/SMG-PROTOCOL.txt`: any version named in the text.
- [ ] `CHANGELOG.md`: change "Version 3.0 (in development)" to the release date.
- [ ] `MP_BUILD_ID` (`platform/src/mp/mp_core.h`): it defaults to `dev` and no build file sets it. Pass `-DMP_BUILD_ID=...` in the release build, or reports and logs say `dev`. Test builds of this line use `3.1-dev` (`build.py --build-id 3.1-dev`). **The release build sets a 3.0 release build id** (for example `build.py --build-id 3.0.0`): consoles with different ids never play together, so every console of a release test and every player must have the same one.
- [ ] `MP_PROTO_VERSION` (`platform/include/port/mp_proto.h`) is 3. Check that every console in a test used the same one.

## 2. Server values to choose

- [ ] `SMG_TIME_BUILDS`: the list of builds whose Star Race times are accepted for the board. Put only the release build in it. Leave out test builds, or their times will reach the public board.
- [ ] `MIN_APP_VERSION`: players below this get "update required" (HTTP 426). Default `3.0.0`. Raise it only when an old build cannot talk to the server any more.
- [ ] `LATEST_APP_VERSION`: this is what the "A newer version of the game is available." notice compares with. Set it to the release, and raise it when a newer one is out.
- [ ] Never set `ALLOW_TEST_CLOCK` or `RATE_LIMIT_SCALE` in a real deploy. `REGISTER_POW_BITS` is set on purpose in `wrangler.smg.toml` (15, about 0.2 s of work on a Vita); do not lower it.
- [ ] `REGISTER_MAX_PER_DAY`: new sign-ups a day. The file has 10000 for launch week. Change it to 2000 afterwards (edit and deploy again, or use the dashboard variable). Past that number new consoles read "Online is busy right now" until 00:00 UTC; consoles that already have an account are not affected.
- [ ] `DAILY_REQUEST_BUDGET` (45000) and `LIVE_RESERVE_PCT` (20): Galaxy takes at most 45,000 of the account's 100,000 requests a day. New sessions stop at 80% of it; rooms already running go on to the whole budget. The day resets at 00:00 UTC. Lower the budget when the Strikers service is busy, because the 100,000 is shared.
- [ ] `PRUNE_UNUSED_DAYS` (3) removes silent accounts that never did anything; `ACCOUNT_EXPIRE_DAYS` (365) removes accounts not seen for a year. `docs/PRIVACY.md` says both; keep them equal.
- [ ] What a player sees when the service is over budget: "Online play is busy right now. Please try again later." The game remembers that for up to 10 minutes and does not keep asking. Local play and the far-away code do not depend on it. Check this against `server/README.md` before you launch.

## 3. Server deploy (from `server/DEPLOY-SMG.txt`, in this order)

**A Codex step of its own** (section A, step 10): the package is `/home/claude/w/out/svc1118/smg-online-server-20261118a.tgz` (sha256 in the `.sha256` beside it; check it first). It is not part of the game push and not part of `release30.sh`; nothing here is run by us.

**Before you go live (do not skip).** A request that reaches the Worker counts against the account's 100,000 a day, whatever the answer is. Only a custom domain with a rate-limiting rule and a WAF custom rule can stop a flood before the Worker runs, and a `workers.dev` address cannot have either. So: the service is on the custom domain `galaxy.psvita.dev` (zone psvita.dev on Cloudflare DNS), add the rate-limiting rule and the WAF custom rule (`docs/ONLINE-DOMAIN-SETUP.md`; do NOT turn on Bot Fight Mode on that zone, it can challenge the game's API calls), set a usage notification, and turn `workers_dev` off at release once the hardware test has passed on the new address. Until that is done, say "online play is not ready" and do not open sign-ups. `KNOWN-ISSUES.md` lists this as open.

**Careful.** The server code is shared with the Strikers service (`msc-online`). Migration 4 also runs there on its next deploy. A deploy of the shared code can change that service too, so read its notes first.

1. [ ] `cd server && npm ci`
2. [ ] `npx wrangler login`
3. [ ] Optional: create a TURN key (for the relay).
4. [ ] `npm test` and `bash tests/mp/service_test.sh`. Both must pass.
5. [ ] `wrangler deploy --config wrangler.smg.toml --dry-run`
6. [ ] `wrangler deploy --config wrangler.smg.toml`
7. [ ] `wrangler secret put ADMIN_TOKEN --config wrangler.smg.toml` (and the TURN secrets, if used).
8. [ ] Smoke test: `curl <your service address>/v1/info`. Check `app.min` and `app.latest`.
9. [ ] The client builds default to `https://galaxy.psvita.dev` (`-DNP_ONLINE_URL=...` / `build.py --online-url` change it, `none` compiles no service in). There is no fallback to the workers.dev address.

There is also a helper, `server/scripts/deploy-smg-online.sh`; read it before you run it, and it follows the same order. Always pass `--config wrangler.smg.toml`. Rollback: `wrangler rollback --config wrangler.smg.toml`.

## 4. Build and shaders

- [ ] Build the VPK on the build machine (not on the shared server): `tools/release30.sh` (section A, step 7) does the build and the packaging below in one go; the lines here say what it does and what the by-hand way is.
- [ ] Make the release VPK with the stage-list tagging step: `python3 -I tools/vgx_build/build.py link --seed <seed VPK or shadercache folder>` writes `<build-dir>/Galaxy-Vita.vpk` (the CMake `smg-vgx.vpk` plus the seed's `shadercache/`, its `vgx_stagewarm.bin` passed through `tools/seedrun/stagetags.py legacy`). By hand: `python3 -I tools/seedrun/stagetags.py package <smg-vgx.vpk> <seed> <out.vpk>`. Never copy a seed's `shadercache/` into a VPK any other way.
- [ ] Hard check, on the very file you ship: `python3 -I tools/seedrun/stagetags.py checkvpk <the VPK>` must pass (its `shadercache/vgx_stagewarm.bin` carries the stagetags stamp and `legacy` would change nothing). An untagged list only logs a WARNING at start-up, and the game then runs without the Observatory and Honeyhive first-visit priorities and without the old player seed (a regression against C183). The current seed (test19) is untagged: it must go through this step.
- [ ] Shader pack (section A, steps 1 and 2: the console's cache is pulled and merged into the seed before the build): multiplayer sessions show puppets, bubbles and name tags that single player never draws. Play one session on a console (see `docs/MULTIPLAYER-HARDWARE-TEST.md`, step 8), then regenerate the shader pack, so players do not hitch the first time they see them.
- [ ] Check that the VPK holds no game data, no firmware and no HD pack. Its only Nintendo art is the contributor's bubble and LiveArea pictures (`vita/vgx-game/sce_sys/`: they include the game's key art and logo, shipped by the owner's decision); check that those are the approved files (`tests/test_player_settings_assets.py` pins them) and that nothing else from the game is in it.

## 4b. Final-build gates (strk31, from Strikers' final-build list of 13-18 Nov)

Run these on the very eboot / ELF you ship, in this order. Every one has an exit code; a gate that did not exercise its thing says INVALID or INCONCLUSIVE (exit 2) and is **not** a pass.
`tools/release_gates.py` holds the four that were missing (tests: `tools/test_strk31.sh`) and, since stackgate31, the `stack` gate (the display-queue callback's 4 KiB stack).

| Gate | Galaxy | How |
|---|---|---|
| Build id set | was a manual line in section 1 | [ ] `python3 -I tools/release_gates.py buildid --elf <build>/vita-vgx-game/galaxy_native --info <out>/BUILD-INFO.txt --expect <the release id> --release`. Looks for the tag `MP_BUILD_ID=<id>` (platform/src/mp/mp_core.cpp `mp_build_id_tag`; the bare id is not proof: `3.0.0` is also the app version), and needs `--info` with `--release`. Fails when the tag is not in the ELF, differs from BUILD-INFO.txt, or is a test id (`dev`, `*-dev` such as `3.1-dev`, anything with `test`). |
| ELF hooks | had it | [ ] `build.py link` (and `build31.sh`) run `tools/check_weak_links.py` with `tools/vita_must_define.txt` and `tools/weak_links.allow`; read `logs/link-check.log` (`RESULT` line, 0 weak-only references). The multiplayer 15-symbol and TLS checks of `build31.sh` too. |
| `-Wformat` over ALL units, 0 warnings | partly: `tools/check_vita_format.py` (`%z %t %j %hh` in literals) only; a build log shows the units it recompiled | [ ] `python3 -I tools/release_gates.py wformat --db <build>/vita-source/compile_commands.json --cpath <build>/case-shim` (the 1,886 game units, about 45 min on one niced job; **--cpath is needed**: the build finds the game's mixed-case headers through CPATH, and 33 units stop at a missing header without it; the gate then exits 2, not 0). At integ-3.1 `1a091299`: 1,887 units, 0 format warnings, then the same for `vita-vgx-game/compile_commands.json` and for the renderer (`ninja -C <build>/vita-vgx-runtime -t compdb > runtime-db.json`; the database holds the build's own paths: run it where they exist, as `build31.sh` does). Syntax-only, no object is written, `-Wformat -Wformat-security`. Plus `python3 tools/check_vita_format.py` (0 findings). |
| Emulator gate on three stages | emulator gates existed (pgo31 D, seedwalk) but no one verdict | [ ] Run the release eboot in Vita3K on three stages (default env, `tools/seedwalk/plan.py` legs or `PETARI_STAGE`), keep each run's exit code and log, then `python3 -I tools/release_gates.py emu --stage a:<exit>:<log> --stage b:<exit>:<log> --stage c:<exit>:<log>`: exit 0, 0 fault lines, 0 programs linked in play (`linked by a draw in play`, first-use `programs linked by draws`). Fewer than three stages, or a log with no warm-up line, is not a pass. |
| PGO twin, gated separately | not done: one gate for whichever build was made | [ ] The PGO eboot is a different file: it gets its own build id check, hooks check, emulator gate (above) and its own `-Wformat` is the plain build's (same sources). Then the twin proof, section 4c. Never accept PGO on the strength of the plain build's gates. |
| Display-queue callback stack (4 KiB) | did not exist: the console crashed twice (c116 smoke, 18 Nov Quality) | [ ] `python3 -I tools/release_gates.py stack --root <repo> --build-dir <build> --jobs 2` (compiles PaceDisplay / SyncDisplay's units and the log path with `-fstack-usage`, the build's own commands, nothing kept; tests: `tools/test_stackgate31.sh`). It checks every frame on that thread against its budget (PaceDisplay 448, SyncDisplay 768, EvenShown 768, ...) and the worst chain (ours at most 2560 bytes; plus newlib's printf path 800 and the system's 512 at most 4096). Exit 1 = a buffer is on the callback's stack: make it `static` (thread-confined) as q30sepfix / PATCH2 did, never raise a budget to pass. Exit 2 = a listed function is missing or a unit did not compile: update `FUNCS` in `tools/check_display_stack.py` with the code, not a pass. The thread's stack is the SDK's, so the real margin is only measured on the console: look for no `SceGxmDisplayQueue` crash in a 10-minute Quality run. Other small stacks: `smg_freezebutton` has 8 KiB (it logs once, then traps); every other game thread has 16 KiB or more. **The tables are a hand-written list** (fable31fix): any new function called from PaceDisplay / SyncDisplay / EvenShown / SmoothShown / PaceGate / LatencyBlock / LatencyShown (or anything they call) must be added to `FUNCS` and `CHAINS` in `tools/check_display_stack.py`, or its frame is not counted; `python3 -I tools/check_display_callees.py --strict` lists the calls in those bodies that the tables do not know (best effort, one level). Never write the memory card or take a blocking lock on that thread: the log path drops and counts a line when its buffer is full (`log: N display-thread lines dropped (buffer full)`) |
| Link order file regenerated from the final map | does not apply as Strikers do it | The Galaxy 3.1 build keeps no order file: `GAME_HOTORDER` is not set (kit README; `hotorder.py` builds a linker script from the profile and the ThinLTO object each time, never from an old map). If a release turns `GAME_HOTORDER` on, regenerate `hotgame.ld` from the release profile and the final `galaxy_lto.o` in that build, never reuse one. |

## 4c. PGO safety: what proves a profile build

An emulator run cannot prove PGO by itself: its inputs and pacing follow the emulator's wall clock (Strikers: plain against plain with the same seed first differs at step 26). Galaxy is better placed, but not immune:

- **Inputs are step-locked.** `PETARI_SCRIPT_ANCHOR=play` counts the scripted route in game turns (`platform/include/port/input_script.h`, `portScriptTurnMs`; the seedwalk route files), not in wall time. The renderer's speed can still change what the game does (frame slip, picture size, auto quality: `tools/pgo/README.md`), so the run is deterministic only up to the first such change.
- **The state is hashed by turn.** `GXV_UNDRAWN_LEAN_HASH=300` logs `undrawn-lean: hash turn T <hash> actors N` (positions, velocities, rotations, nerves, sensors, random seed).
- **Three runs, one verdict.** Run the same route (same input file, same env, `PETARI_GAME_CLOCK=1` where the harness has it) with the plain eboot twice (plain, plain2) and with the PGO eboot once, then `python3 -I tools/release_gates.py twin --plain L1 --plain2 L2 --pgo L3`. PASS: the PGO hashes equal the plain ones as far as both go. FAIL: PGO leaves plain at a turn where plain and plain2 still agree (a real behaviour difference: look for undefined behaviour exposed by the new counts; LESSONS 21 in `reports/pgo31.md`). **DET INCONCLUSIVE (exit 2): plain and plain2 differ at or before that turn; say so in the release notes. It is not a pass.**
- **The proof is the console.** The console A/B (old against new profile, same route, `fps:` and the turn hash lines) decides. The profile refresh of `reports/pgo31.md` section 2 also needs the mismatch counts (renderer under about 12, game cold build under 30).
- **Loop unswitching.** Strikers' note ("`-fprofile-use` enables `-funswitch-loops`, VitAdhoc's miscompile") is GCC behaviour; Galaxy compiles with clang 18, which ignores `-funswitch-loops` (`warning: optimization flag '-funswitch-loops' is not supported`) and, in its pass pipeline, `-fprofile-use` adds the profile reader and branch weights, not GCC's bundle of loop flags (not measured on a profile build here: there is no profile runtime on this machine to make one). What clang does: the ThinLTO back end runs at `-O3` (`GAME_LTO_OPT`, default 3, `tools/vgx_build/build.py`) and the renderer units at `-O3`: both run non-trivial `simple-loop-unswitch` in the plain and the PGO build alike (checked with `-mllvm -print-pipeline-passes`: `-O2` has `no-nontrivial`, `-O3` `nontrivial`); a profile changes which loops it treats as hot, not whether it runs. There is no `-fno-unswitch-loops` in clang, and `-mllvm -enable-nontrivial-unswitch=false` does not turn it off at `-O3`; the only switch is `GAME_LTO_OPT=2`, which changes much more. So no flag is added; the twin proof above (plain against PGO on the same route) is the guard, and a run that shows a hash difference points at the units named in the diff.

## 5. The anonymity scrub (decided 18 Nov: a fresh public snapshot, never the history)

- [ ] The public repository gets ONE commit of the frozen tree, made by `tools/make_public_snapshot.sh`, by the public identity (`CBOC-Vita`, the one the repository's earlier public commits use). The internal history (about 2,000 commits by the build identity, Claude and Codex lines, and the owner's own name on a few) is never pushed, so it is not rewritten.
- [ ] The snapshot script's scan (`tools/public_scan.py`) must say `passed`: no name or address of a private author of the history (it reads them from `git log --all` and prints counts and paths only), no link to the Claude web app and no session id, no private or secrets folder, no key, certificate or `.env` file, no token pattern, nothing over 20 MB. Warnings (home paths and LAN addresses in test fixtures and protocol docs) are read once by a person: `/Users/Bob`-style names in tests are fixtures, a real person's path would not be.
- [ ] Look inside the VPK and the zips for build paths: `release30.sh`'s `strings` gate does it on the ELF (no `/home/...` or `/Users/<name>`, no session id). The eboot carries the build's `/source/source` paths on purpose (Codex's C183 flow; they name no one).
- [ ] `docs/PRIVACY.md` and `docs/MULTIPLAYER.md` carry no file or line references any more. After any code change, re-read the statements about what is sent, kept and checked, and the "Playing with strangers" section.
- [ ] Commit trailers of the internal branches stay one line (`Co-Authored-By: Claude <noreply@anthropic.com>`); the snapshot has no trailer at all.

## 6. Licences and `THIRD_PARTY_NOTICES.md`

Check that each piece below is listed there with its licence and that the licence text ships in `licenses/`.

| Piece | Licence | Where |
|---|---|---|
| Mbed TLS 3.6.8 | Apache-2.0 | `net/deps/lock.json`, `tools/mp/build-mbedtls.py`, `licenses/Apache-2.0.txt` |
| Monocypher 4.0.3 | BSD-2-Clause or CC0 | `net/src/online/third_party/monocypher`, `licenses/Monocypher-LICENCE.md` |
| `np_hash.cpp` | CC0 (our own) | `net/src/online/` |
| `np_upnp.cpp`, `np_natpmp.cpp` | ours, written from the standards (UPnP IGD, RFC 6886 and RFC 6887) | `net/src/online/` |
| TLS root certificates (GTS Root R1/R4, ISRG X1/X2, SSL.com) in `np_https_roots.h` | public certificates, not code | `net/src/online/` |
| Public STUN server `stun.cloudflare.com` | a service we call, not shipped code | default in the settings |
| Petari decomp | CC0 | already listed |
| CAS | MIT | already listed |
| Inter font | OFL | already listed |
| DejaVu font | its licence | already listed |

- [ ] No Halo CE or other GPL code is in the tree (a check was made for this; run `grep -ri "halo" net platform/src/mp` again before release).
- [ ] The server's npm packages: run `cd server && npm ls --all` and check each licence is listed.
- [ ] Open question to settle: the Mbed TLS patch file must be shipped with the source offer.
