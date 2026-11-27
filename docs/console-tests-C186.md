# C186.2: one physical Vita session

> **For the C186.1 / C186.2 VPKs only (c186merge note).** Their Performance mode sets the C182 warp rows
> (`GXV_HRW41_DEPTH/BOXKEY/BOX_ROUND/BUDGET`) and this card's base env sets `GXV_LA88_MIPS=1`. An integ-3.1 build
> (owner, 17 Nov) has all of these OFF until the console A/B: its mode table no longer sets the warp rows, so check 4's
> "baseline should resolve" line does not hold there, and `GXV_LA88_MIPS=1` is an A/B arm, not the base. For an integ-3.1
> build use docs/RELEASE-3.0-CHECKLIST.md section 0b for those items; the backup, online-account and log rules below apply unchanged.

Operator's run card, **not a completed test**. Match the proposed VPK to
its own handoff SHA-256 and use the same hash on every arm. No deployment,
shader collection, clock change,
file deletion or online session is authorized by this document alone. When
approved, use the existing bubble and the owner's own room/test account.

**Build gate.** The original C186 VPK (`5b95929e...6f67e64`) lacks the
native Settings functions in its linked executable. Its pause-menu restart
check is blocked; source or host menu tests do not change that result. For
this diagnostic sequence use the actual `3.0-C186.2` boot and VPK SHA-256
`67a65943dbee14bab0a27a19e13e8c6dc1f50dbb0f14bab2de8a052424b1cb1e`.
C186.2 includes the C186.1 settings link fix. Do not rename or substitute
an earlier VPK. Back up everything below **before any launch**, then check
the native menu on the working copy as described under Common retail baseline.
An executable link check is not physical acceptance.

## Before any launch or install

1. Fully close the title. Copy **the entire** `ux0:data/smg/saves/`,
   `ux0:data/smg/player-settings.cfg`, `ux0:data/smg/env.txt` and
   `ux0:data/smg/shadercache/` to a new dated off-device backup. Include any
   `settings.ini` (camera zoom, pointer and controls), all `player-settings*`
   companions/backups, stage-presets file, input script,
   `log.txt`, `log.txt.prev`, `ioplay.txt` and `loadtimes.txt` in
   `ux0:data/smg/`. The last two are writable ledgers outside the shader
   cache. Record files that do not exist. Keep the
   previous VPK. Verify file counts, sizes and SHA-256s against a second
   read of the device copy. **Stop if a backup cannot be read back.**
2. Keep that master immutable. For each comparison restore a *working copy*
   of the same save and shader-cache snapshot with the title closed. Preserve
   the current copy first; reset the test settings and the two ledger copies
   to the same starting state. Swap directories or names, do not merge trees or
   clear a player's cache. Quarantine any existing test input, stage presets or
   external graphics overrides for the session by reversible rename. Record
   original names. Leave the packaged seed and game files alone.
3. Record handheld model/clocks, VPK hash, save slot, camera/zoom, graphics
   mode, Assist, cache snapshot ID and stage/scenario. Keep the camera fixed;
   judge double images on the display as well as in unblended video. Use a
   short exposure and preferably 120 fps or faster for adjacent 60 Hz pictures.
   A phone frame blended across two refreshes is not evidence of game ghosting.
   Keep a local copy of each run's `log.txt` (and
   `log.txt.prev` after restart) before the next launch; do not publish IDs,
   tokens, invites or account data. Use the same firmware/plugins/clocks for
   all arms. For online checks use normal TLS, never a verifier bypass.

## Common retail baseline

With the game closed, put the following in the entire temporary
`ux0:data/smg/env.txt` **before** selecting a mode: an old env override
can take precedence over the saved menu setting.

```text
GXV_VSOUT=1
GXV_LA88_MIPS=1
```

There must be **no** forced shader/user packs, cache tag, stage/picture
override, scripted input or capture flag. Leave the depth-edge experiment
off. Other than the lines explicitly named in a test, do not edit the base.
Start each arm from a clean launch; many flags are read only at startup.
For timing, label results **physical Vita / retail shaders (VSOUT=1,
no forced packs)**, then the one test override, save, and cache state.
No host/emulator timing is a console result.

Launch into single-player, pause, and open native Settings (Triangle at
the Settings hint). Confirm **Picture: Smooth Motion** (Performance 60 in
the log) and **60 Assist: On**; save and
let the game relaunch if necessary. Check the corresponding fresh
`settings: picture mode Performance (60)` and `modekeys: Performance mode:`
(`GXV_HRW=5`) lines, including their `+ ` continuations. Reject a later
`GXV_HRW = 0` override or a message that HRW is off. If the screen is
unavailable or the effective settings
cannot be confirmed, stop before comparisons; do not hand-edit a player's
settings to manufacture a pass. Back up this *test* settings copy separately
from the original, then restore the baseline working state for step 1.

The separate console QA zip contains complete temporary files at
`env/<arm>/env.txt`. Use `baseline`, `mips-off`, `round-off`, `plant-full`,
`pose-full` or `assist-off` as named below; replace the whole working file,
never append one arm to another. From the extracted kit, optionally run
`python3 c186_console_arms.py check <arm> <copied-env.txt>` on the copy read
back from the Vita. For the backup and final restore, use
`python3 c186_console_arms.py compare <backup-dir> <second-read-dir>` on
two complete, independently obtained **local copies** with the same layout.
This checks names, directories and bytes, not device identity. Neither
command reads or writes the handheld. Record the source device and paths.

The short checks precede the longer journeys. Reuse an already captured
matching baseline traversal, but never reuse a baseline with different
settings, cache or save. Assign each check PASS, FAIL or NOT EXERCISED
(missing save, effect, counter or log). The third outcome is not a pass.
For numbers, keep the same in-play log windows at the same landmarks. From
`fps:` record shown FPS, game turns/s, picture resolution, count over
100 ms and longest hitch; from `pace:` keep the in-play hold histogram.
Shown FPS includes assisted pictures: it is not the real render rate or
a CPU/GPU timing. A cumulative run p99 is not a route-specific p99.
Without a matched older-build run, these are observations, not savings.

## Ordered checks

1. **Title planet (15 seconds after title appears).** Baseline only. Film one
   rotation behind Press Start, with a text-free strip of the globe in view.
   PASS: the broad rectangular grid does not crawl across the cloud and text
   stays sharp. Record visible seams or shimmer as FAIL, not an FPS result.
   Where available confirm `C183: title Cloud01k base-level linear sampler`;
   an unmatched-sampler log is not evidence that the treatment ran.
2. **Two-channel texture mips (one short replay).** During a baseline load,
   locate the exact affirmative line `GXV_LA88_MIPS=1: mipmapped I/IA
   textures go as LA88 (the transfer unit's U8U8 downscale is exact)` in
   log.txt. Then use `mips-off`, replacing **only** `GXV_LA88_MIPS=1` with `GXV_LA88_MIPS=0`
   and repeat the same load and fixed near/far views of the same I/IA
   effect or textured surface. Require `compact mips:` with nonzero accepted
   uploads in the enabled run; the probe alone proves a capability, not an
   upload. PASS: affirmative probe for 1, accepted upload, and no added
   banding, missing alpha, or bad coarse levels vs 0. A negative probe means
   the fallback worked but LA88 is **not accepted**; no probe means NOT
   EXERCISED. Never use 2, which bypasses the probe. Restore base.
3. **Pause-menu restart (one relaunch).** In single-player Performance,
   pause the *running game*, open Settings, change Picture to **Full Detail**
   (Quality 30 in the log),
   Back, choose Save and Start Again and answer **Yes**. Do not exit manually
   or tap the bubble. PASS: the game itself restarts; previous log has
   `settings: restart asked (pause)` / `settings: restart now`, and fresh
   log.txt says `settings: picture mode Quality (30) (GXV_PICTURE_MODE=30)`.
   A `settings: restart returned 0x...` line records the call's return code,
   not the outcome. Vita3K logged zero before it processed the queued restart;
   use the observed relaunch and fresh boot log, not that line, to decide.
   A settings value on disk without a new boot is FAIL. Restore Performance
   via Settings and verify the corresponding new Performance (60) log and
   Assist On before continuing.
4. **Mario silhouette, second dome (about two minutes from the relevant
   save).** On the first Observatory entrance, keep the same pad, camera and
   background point. Jump, double jump, then sharply reverse; repeat three
   times. Baseline should resolve `GXV_HRW=5`, `GXV_HRW_WARP_DEPTH=4`,
   `GXV_HRW41_DEPTH=1`, `GXV_HRW41_BOXKEY=1`,
   `GXV_HRW41_BUDGET=1`, `GXV_HRW41_BOX_ROUND=1` in Performance;
   **do not add these to env.txt**. Confirm the `hrw: hrw41` counters show
   positive `drawn so` and `keyed boxes drawn`, not only requests or refused
   masks. `drawn so` also counts exactly depth-independent projective draws;
   subtract the same-window `exactly depth-independent scene reads` count
   before claiming a picture really sampled scene depth.
   Watch feet, cap and surrounding ground, a near occluder and HUD. Compare
   one `round-off` replay with **only** `GXV_HRW41_BOX_ROUND=0` appended. Round vs
   rectangular affects the unkeyed fallback, not a successfully keyed box;
   use the log (drawn player boxes vs keyed boxes from corresponding windows)
   plus a visible fallback. If neither run exercises it, record round-box
   comparison NOT EXERCISED; don't disable keying to manufacture a pass.
   PASS: stable single silhouette, no rectangular fringe at head/feet, no
   drifting HUD or ghost ground; round no worse than rectangular on the
   matched fallback. Restore `baseline` **before** the independent map test.
   Check `hrw: c182 | detail budget:` when present: fallback under
   pressure is permitted, never force 60 or claim recovery after a per-stage
   give-up. From a second-dome unlock save, on *first* map open point at two
   icon centers and note the highlighted names without entering a level.
   Close and reopen, repeat, then select one. PASS: shown positions, highlights
   and picked level agree. A save already past the unlock is not the first-open case.
5. **Online idle room, Friends and FTP (at least 75 seconds idle).** Baseline
   env. Score four checks separately; one cannot substitute for another.
   Use an approved test account. Before going online, make a verified,
   private backup of any existing `ux0:data/smg/online/` and
   `ux0:data/smg/mp/` directories; record absence rather than creating
   them. A sign-in can renew credentials outside the single-player save.
   Do not reset an account, include credentials in the evidence, or restore
   an old token over a live renewal. Skip this arm if a suitable account
   or a private backup is unavailable.

   - **Held polls.** Use a private **forming** Internet room. Mark entry
     and exit times; remain in the lobby without starting or using menus
     for at least 75 s and two completed holds. PASS: still connected,
     two idle-window GET completions consistent with the room's ~25 s holds
     each show transport `-> OK` and `status=200`, and no
     `no data for too long`. A partial body can have status 200 but a
     transport failure; do not count it. Record elapsed ms.
     A settled host's ~55 s hold is a different case. The HTTPS summary
     prints method, host and port, not the endpoint: concurrent/ambiguous
     requests do not establish a poll; record NOT EXERCISED if ambiguous.
   - **Friends priority.** The C186 native Internet-room lobby has no
     Friends command: Square does not open Friends there, and confirming
     Back leaves/closes the room. Mark **in-room priority NOT EXERCISED**.
     After leaving, open Multiplayer > Square (Friends) and time from the
     press to a populated list or confirmed empty answer, not the animation.
     Record actual ms against ~1 s; this is an **out-of-room** usability
     check, not proof that Friends interrupted a live poll.
   - **Sockets.** In the same process after leaving, archive the first
     legitimate post-session `[net-https] sockets:` line. PASS: `0 open`
     or `1 open`, `refused 0`. A successful request reusing a connection
     need not emit it: the logger samples after 20 more opens or on certain
     failures. No fresh line means NOT EXERCISED, not zero. Do not force
     failures or flood requests to provoke a sample.
   - **FTP.** Use the owner's usual local connection to list a folder and
     fetch a harmless existing file without writing. PASS: both complete;
     distinguish an unavailable service/untested route (NOT EXERCISED)
     from a failed connection on a confirmed working local FTP route.
6. **Honeyhive entry, Queen and first-use pause (one mission).** Baseline,
   same Bee Mario mission/route. Record the opening camera before control
   returns (including the distant waterfall through the zoom); climb around
   the Queen at a fixed distance and then approach and retreat. PASS: no
   invalid/missing waterfall or abrupt disappearance, no repeated coat detail
   switching at nearly constant distance. At the first Bee/Queen approach
   and the first Observatory entrance record any stall: elapsed time from
   control, stage, frame, first-use/compile/link/texture/target/present and
   game-thread log lines. Repeat from the same save but with the naturally
   learned cache; label that run **warm return**, not a matched cold A/B.
   On the optional C186.2 diagnostic build, keep both the `hitch:` and
   `stutter:` lines for the same frame. Record the total picture gap,
   `present` duration, and the display-queue time and call count (or `n/a`).
   These can overlap; do not add them to each other or to a game-thread
   wait. A longer display call does not by itself identify GPU work or a
   shader compile. The Mac emulator's host-cache changes do not run on Vita.
   If not a genuine first visit for the captured cache, label it as such.
   PASS for the reviewed route: no perceptible blocking after control; a
   repeatable pause without a matching compiler event remains unexplained.
7. **Vibration isolation (longest; stop for inconclusive baseline).**
   Compare the same two *single-player* spots: hover in the Bee suit without
   touching the same lilies/flowers, then pan past the same Luma at the first
   Observatory entrance. Use the same saved slot/checkpoint per spot, witness
   object, camera distance, angle, pan direction and duration. Keep a fixed
   piece of background and Mario in view. Reproduce visibly twice in the
   baseline before blaming any subsystem. If it occurs only during actual
   contact or natural swaying, note that distinction. Record this grid for
   each spot (restarts required):

   | Arm | Only change to baseline env.txt | What distinguishes it |
   | --- | --- | --- |
   | A (`baseline`) | none | Establish the tremor in a retail Performance picture. |
   | B1 (`plant-full`) | `GXV_HR_PLANTMOVE=0` | Resting group members only: Wait for over 30 turns, no pointer/contact, player more than 1500 world units beyond the group's touch radius. A nearby plant, a lily that isn't a PlantGroup, or a Luma does not establish coverage. |
   | B2 (`pose-full`) | `GXV_DIAG_FULL_FAR_POSE=1` | Only turns normally skipped by the level-3 far-logic character pose rule are restored. Movement and other pose budgets stay as shipped. Examine `farlogic: level` and `far-pose-test: restored first level-3 half-rate pose:` (once per launch, with model). The preset's `cpulod=3` is not farlogic level 3. No level 3/no eligible call, or only evidence for an unrelated actor, is not proof for this object. Do not force a stage to level 3 or disable all far logic. |
   | B3 (`assist-off`) | `GXV_HRW=0` | 60 Assist off, still Performance, not Quality 30. Only real pictures; check the fresh `modekeys: Performance mode:` line reports `GXV_HRW=0`, not merely UI text. |
   | A again (`baseline`) | none | Original tremor must return in the same scene; otherwise report inconclusive. |

   Start B1/B2/B3 separately from A, **never cumulatively**. Score each spot
   independently: present / absent / reduced / not reproduced. A cause is
   supported only when the tested path was exercised, the change repeats
   and matched A brings the symptom back. Re-run a promising B on its own
   after the returning A before assigning a cause. If B1/B2 remove it in their eligible
   objects, report that result; don't treat a different Luma result as the
   same cause. B3 also changes presentation cadence: a B3-only result implicates
   the assisted path, not a specific shader. If multiple switches change it, report an interaction, not
   a sole cause. If B3 doesn't remove it, **stop; no warp work or depth dumps**.
8. **Conditional colour/depth evidence.** Only for a spot where B3 removed
   the defect and the A repeat restored it. Outside the timing A/B, film the
   exact same short physical route once on baseline (colour), then replay
   with only `GXV_HRW41_DEBUG=1` added to baseline (Assist back on). The
   actual D41 second picture displays its mask; the HUD/player can remain
   composed over it. Record physical display output fast enough to distinguish
   adjacent pictures, correlate to `hrw: hrw41` with nonzero depth draws and
   `THE SECOND PICTURES SHOW THE MASK`. Preserve original frames and run logs.
   PASS for evidence only: same object and geometry identifiable in colour
   and in a nonuniform grayscale depth view, near/far boundaries visible
   *outside* the HUD; record frame indices and whether adjacent or across
   replays. Prefer the original colour picture immediately preceding the
   mask in the same debug clip; a matching route on another replay is not
   a same-pair measurement. All-black, flat, wrong scene, a blended camera
   frame, or a coloured HUD over black is an
   invalid capture. A screen debug view is not raw depth; if raw same-pair
   capture is needed or valid depth isn't available, mark evidence incomplete.
   No forced packs, shader harvesting, or shader-code changes in this session.

## Restore and verify

Exit the room and game, stop test transfers, and save the evidence. With the
game closed, move the working copies aside and restore **the exact original
save tree, settings file and companions, env.txt, shader cache, both original
logs, both ledgers and any quarantined files** from the verified off-device
master. Restore the recorded
absence of files that did not originally exist (archive test-created copies).
Compare original filenames, counts, sizes and hashes with device readback.
Do not relaunch before that comparison or overwrite the backup with test
data. Leave the previous VPK available; reverting the installed program or
sharing logs is a separate owner decision.
