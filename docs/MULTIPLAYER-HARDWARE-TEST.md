# Multiplayer on a real Vita: the first-run checklist

For one Vita and the practice partner program on a Mac. Nobody has run multiplayer on a console yet, so every "Must be seen" is a
guess until you do it. Steps go from the quickest and riskiest to the slowest. **Stop at the first step that fails and send the files.**
Steps that need a second Vita (nearby play, a far-away code, real four-player play) are not here: see part 4.

Button names are the Vita's: **Cross**, **Circle**, **Square**, **Triangle**, **L**, **R**, **START**, **SELECT**, the D-pad, the sticks and touch.

## Before you start

1. **Back up.** With VitaShell (USB or FTP) copy `ux0:data/smg/saves` (and `ux0:data/smg/mp` and `ux0:data/smg/mp-backups` if they exist) to the Mac.
2. **Fingerprint.** On the Mac, `shasum -a 256 GameData.bin` for the copy of `ux0:data/smg/saves/title/00010000/524d4750/data/GameData.bin`: **S0**.
3. **Same build everywhere.** Install the VPK. Build the practice partner from the same source (`tools/mp/partner`, `./build.sh`, then `./partner`; see its `README.md`). Its build and
   protocol must match the line `[mp] core ready build=... proto=...` in the Vita's `log.txt`.
4. **`ux0:data/smg/env.txt`:** `SMG_MP_PERFLOG=1` on one line, `GXV_THREAD_CPU=1` on the next, `SMG_MP_CRYPTO_LOG=1` on a third. Add `SMG_ATOMIC_SAVES=1` on a fourth line (the safe single-player save is OFF by default; steps 2 and 14 test it, so it must be on; or turn on Settings, Display, Safe saves).
5. **Before every step** copy `log.txt` away and delete it. Vita and Mac on the **same Wi-Fi** (not a guest network).
6. **Never choose "Play This File".** Always choose **New Team File**. "Send back" always includes that step's `log.txt`.
7. **A habit for every file check:** in VitaShell, open `ux0:data/smg/saves/title/00010000/524d4750/data/` and write down which of `GameData.bin`, `GameData.bin.mpbak`, `GameData.bin.mptmp` and `GameData.bin.mpbad` exist, their sizes, and copy them to the Mac for fingerprints.

## The steps

**1. Baseline and the first start.** Single player, 5 minutes, save the usual way (the Observatory save point or a star), quit with the PS button's Close. Fingerprint `GameData.bin`: **S1**. Then start the game, **Multiplayer**, and back out with **Circle**.
Must be seen: the game loads and saves normally. `log.txt` has a `keepsafe:` line and `ux0:data/smg/mp-backups/` has a new folder named after the version (the first start of a new version makes one checked copy; nobody knows what that takes on a console). Send back: S0, S1, the log, the list of `mp-backups`.

**2. The safe single-player save: the extra files appear.** Single player again. Save once at the Observatory save point; quit. Look in the data folder (habit 7). Save a second time in a new session and quit.
Must be seen: after the first save `GameData.bin` has a new fingerprint (**S2**) and `GameData.bin.mpbak` (about 48 KiB) exists and its fingerprint equals **S1** (it is the previous save); after the second save `GameData.bin` is **S2b** and `.mpbak` equals **S2**. No `GameData.bin.mptmp` is left over. The save is not visibly slower; note any pause while "saving" shows. Send back: S1, S2, S2b, both `.mpbak` files, the folder listing, the log. (Cost on a Vita is unknown: the computer needs 4.5 to 6 ms instead of 1 to 2. If the save pause is clearly longer, say so and turn **Safe saves** off in Settings, or put `SMG_ATOMIC_SAVES=0` in `env.txt`, once for comparison.)

**3. The menus, with Wi-Fi off.** Wi-Fi off. After the title, the **Single Player / Multiplayer** screen: press **Cross** on **Multiplayer**. The Multiplayer menu: look at the buttons; press **Triangle** (**How to Play**), then **Cross** to close. Choose **Enter a Friend's Code**. Then choose **Play Together**: choose your character the first time (**Cross**), watch the gathering screen alone for a minute, then **Circle**.
Must be seen: the buttons are **Play Together**, **Star Race**, **Enter a Friend's Code**, and **Friends** (**Square**) in a build made with the default address (a build made with `NP_ONLINE_URL=none` has no **Friends** button); **How to Play** names Vita buttons only. **Enter a Friend's Code** with Wi-Fi off says "Couldn't connect to Wi-Fi. Check your Wi-Fi settings." and does not open the keyboard. With Wi-Fi off, **Play Together** looks for consoles nearby by itself and the line says "Waiting for friends..." Alone, **Circle** leaves with no question. No hang. Now Wi-Fi on: **Enter a Friend's Code**, type `ABCDE-FGHJK-MNPQR` (wrong on purpose), **Cross**.
Must be seen: the Vita's keyboard opens and closes; the window "That code doesn't look right. Please check it and try again."; **Cross** (OK) and the keyboard opens again with your text in it.
Picture to take (hardware only; the desktop build cannot open the system keyboard): the Vita's own keyboard while it is open from **Enter a Friend's Code**, with its title showing and the typed `ABCDE-FGHJK-MNPQR` in the field (PS + START for a screenshot; if the system keyboard hides the game, photograph the screen). Take a second one after **Cross** on the wrong-code window, where the keyboard has opened again with your text in it. Write down: the longest text it let you type (the code is 17 characters, so 17 must fit and nothing past the limit), and whether **Circle** or the keyboard's own cancel closes it with no window and no stuck screen behind it.

**4. Vita buttons and touch on every screen.** Walk the entry screen, Multiplayer menu, character select, the gathering screen, the falls question, **Which File?**, **Safe Copies** (from **How to Play**: **Triangle** on the Multiplayer menu, **OK**, then **Yes** on the second page; or **Triangle** on **Which File?**), the rules card, the session menu (**START** in play), Players (**Triangle**), a player's page, Messages (**Square**) and **How to Play**. Look at every button picture. Then use only touch (tap a row once to point at it, tap again to choose, tap each button of the bottom strip), then only D-pad / stick with **Cross** and **Circle**. (Do this with the partner in the game from step 5; come back to it afterwards.)
Must be seen: every picture is a Vita button, never a Wii box (A, B, 1, +, -, Z); no word names the Wii Remote; a tap points and a second tap chooses, once; lists show three rows at a time; the guide band at the bottom is easy to hit with a thumb; on the Messages page the first tap points and the second sends. **Look first at** the screens that computer walks reached least: Safe Copies, the rules card, the recovery question. Note any picture that shows as a box, and its screen.

**5. Meet the partner on the gathering screen.** Wi-Fi on. On the Mac: `./partner` (it looks for a room). On the Vita: **Multiplayer**, **Play Together**. Wait up to 30 seconds. If the partner never appears, stop the partner, run `./partner --host` instead and wait on the Vita's gathering screen again. (Nobody has run the partner against a gathering screen yet; say which way worked.)
Must be seen: a second round face appears (Luigi, the partner), you have a crown or "You" under your face; the line says **Press START to start!** (or "Waiting for ... to start..." if the partner is the leader).

**6. Start, save safety, encryption.** **START**. The first time: the falls question, **In a Bubble** (**Cross**); **Which File?**, **New Team File** (**Cross**). If a rules card appears, press **Cross**. In the Observatory pick Good Egg, walk 2 minutes, jump, spin, take a Power Star. **START**, **Leave Multiplayer**, **Cross** to confirm; the question "Save this progress to a Team File?" **Yes**, **New Team File**. Quit. Fingerprint `GameData.bin` again: **S3**. Then start single player, load your file, play 5 minutes, save.
Must be seen: the partner appears a few steps behind you, with name tag and face; the star plays on your screen. **S3 equals S2b**: multiplayer did not touch your file. After the new single-player save the file changes and `.mpbak` holds the earlier one. `ux0:data/smg/mp/saves` holds a Team File. `log.txt` has one `crypto cost: pair key N us ...` line (an estimate says 3 to 6 ms for the pair key, under 0.2 ms for the datagram). Send back: S3, the data folder after the last save, both logs, the Mac's `partner.log`.

**7. Messages, typing and Hide Messages.** Start a new game with the partner (`./partner --chat-every 10`). Send three quick phrases (hold **SELECT**, press a direction). Then **START**, **Square** (**Messages**), **Write a Message**, type a message of 24 characters or fewer. Then **START**, **Triangle** (**Players**), choose the partner, **Cross** on **Hide Messages**; wait 30 seconds; **Cross** on **Show Messages**.
Must be seen: balloons appear; the keyboard works in a game and stops you at 24 characters; there is no switch for typed messages (they are on); while hidden, the partner's phrases and balloons do not show; after **Show Messages** they do.

**8. Shaders for other players.** Start a galaxy you have not visited since the game started. Watch the 5 seconds after the partner appears. Then `./partner --bubble-me`: watch its first bubble and its name tag.
Must be seen: no stop longer than about 0.35 second. Write down which one stopped, if any. Look for `puppet slot=... appear` and shader build lines.

**9. Wi-Fi keep-awake.** Partner idle (`./partner --idle`). Stand still 2 minutes with **Settings > Power Save Settings > Use Wi-Fi in Power Save Mode** on, then 2 minutes off. Keep both logs, named on and off.
Must be seen: ping in `[mp] status` lines within 30 ms of the off run; no "Weak signal". Jumps to 100 ms or more every few seconds only with power save on means the keep-awake fails on this Vita.

**10. Frame rate, memory and the logs.** With the partner, visit 10 galaxies in a row through the Observatory (take the star or leave by the dome). Then, if you can, add two more partners (`./partner --name P3`, `./partner --name P4`) and play Honeyhive. Before you quit, **do not** delete the log. Copy `log.txt` and `log.txt.prev` to the Mac.
Must be seen: no drop under 25 fps for more than a few seconds; the tenth galaxy loads about as fast as the first; no `out of memory`. Pull these lines out of the log and put them in your report: `[mp] net memory: free min N, free now N` (free min must stay well above 0), the `[mp] puppet heap init` line and every `[mp] puppet heap play serial=...` line (they show main and video memory per galaxy), and the `[mp] perf` lines (every 10 seconds). Note the galaxy and the player count next to each.

**11. A 10-second Wi-Fi drop and the way back.** In a game with the partner (idle, in a galaxy), cut the link for 10 seconds and restore it. Easiest: switch the router off and on; or switch the Mac's Wi-Fi off and on (the partner drops); or, on the Vita, **PS button > Settings > Network > Wi-Fi Settings** off and on, then return to the game. Count 10 seconds. Then do it again for 40 seconds.
Must be seen (10 seconds): no crash or black screen; **Reconnecting...** shows at the bottom of the screen; when the link returns the line goes away by itself within about 30 seconds and you are in the same game, in the same place, with the partner back (if the partner does not come back by itself, restart it with `./partner` and write that down). `log.txt` has `ui: reconnecting`, then `ui: reconnected`.
Must be seen (40 seconds): after 10 seconds the line says **Still trying to reconnect...**; the game tries for up to 30 seconds (Wi-Fi switched off) or 75 seconds (link silent). If it cannot come back you see "The connection was lost. You can go back in from the Multiplayer menu for two minutes." and the Multiplayer menu has **Continue <name>'s Game** at the top; choose it within two minutes. Send back: the logs and what you did to cut the link.

**12. Suspend and resume.** In a game with the partner: press the **PS button**, wait 10 seconds, come back. Then press the power button briefly (sleep), wait 30 seconds, wake the Vita.
Must be seen: no crash or black screen. After each return you see **Reconnecting...** and then you are in the game again, or the "connection was lost" window with **Continue <name>'s Game**. Look in `log.txt` for `suspend: left the session (rejoin possible)`, `resume: rejoining conn=...`, `ui: reconnecting` and `ui: reconnected`.

**12b. Forced resume, then the real one (new aids).** First add `SMG_MP_SIM_RESUME=30` to `env.txt` on the console that will suspend, start the game, join the partner and wait 30 s in play: the game behaves as if the console had slept (no real sleep happens), so the same code runs as after a real wake. Then remove the line, join again, press the power button, wait 20 s and wake the Vita.
Must be seen (forced): `[mp] sim resume: on, 30 s ...`, `sim resume: pretending the console slept (1 of 1)`, `resume detected: netctl call 0x00000000 state 3 (connected) ip ...`, `mem after resume: ...`, then `session ended reason=...`, `connection lost`, `reconnecting`, `ui: reconnected`; `resume +3 s` and `resume +15 s` lines follow with the network state and memory. Must be seen (real): the same, but the first line is `watch: the console slept (N s of real time passed)` or `watch: resume reported by the system`. If the real sleep shows no `watch:` line and no reconnect, report it: that is the one part the forced run cannot test. Look at `mem ... resume +15 s`: free user memory should be close to its value before the sleep.

**13. A crash and the recovery question.** With the partner, take a Power Star (this writes a checkpoint). Press the PS button and close the game from the LiveArea. Start again, open **Multiplayer**.
Must be seen: "Your last game ended suddenly. Keep its progress?" Choose **Yes** and a Team File (**not** your own file). The single-player `GameData.bin` still equals the last fingerprint you took after step 6 (the one after your single-player save). Start it a second time: the question does not come back. Send back: second log, `ux0:data/smg/mp`.

**14. A cut in the middle (only after step 6 passed).** (a) Multiplayer: with the partner, take a Power Star and hold the power button until the Vita turns off just after it. Start again. (b) Single player: save at the Observatory save point and, as soon as the save is written, hold the power button until the Vita turns off. Do (b) three times; a cut that really lands inside the write is luck.
Must be seen: the game starts; your single-player file loads and is either the new save or the one before it (compare fingerprints with `.mpbak`); no `GameData.bin` is missing; leftover `.mptmp` or `.mpbak` files, if any, are finished or removed after one normal start; a Team File loads or the recovery question appears. Send back: the fingerprints, the folder listing before and after, the log, `ux0:data/smg/mp`.

**15. A far-away code on a real router (host side only).** Wi-Fi on. **Multiplayer**, **Play Together**, wait for the gathering screen, then choose **Invite from Far Away**. Wait for the code.
Must be seen: the line "Getting your code..." changes to a short code, big on the screen, with "Tell your friend this code."; `log.txt` shows the router lines (`natpmp:` / UPnP) and an address with the digits hidden. Look at the router's port list: "Super Mario Galaxy" is listed, and it is gone (or expires) after you leave with **Circle**. The practice partner cannot join with a code (it does not read them), so a friend joining is not tested here: that needs a second Vita. Send back: the log, the router's make and model.

**16. Boss fight, honestly.** In Play Together with the partner, enter a boss galaxy (for example Dino Piranha in Good Egg) and play the fight on the Vita while the partner follows.
Must be seen: the boss on your screen is your own; the partner's puppet appears but never hurts it. When you win, the star plays as usual. (Nothing is shared about the boss itself; this is how it is meant to be.)

## Parts of the old list that are no longer there

- **Save a report** (old step 13): the Connection Test has no **Save Report** button in this build and the **Who Can I Play With?** screen is only offered after a failed online try, so it cannot be reached unless an online try fails. Skip it and write "not run".
- **Play by Address on a real router** (old step 12): replaced by step 15.
- **What multiplayer costs single player** (old step 14): do it as a second pass of step 2: single player, Good Egg, 2 minutes; add `SMG_MP_MENU=0` to `env.txt`, repeat, remove the line. Send back both logs. (Nobody has measured this on a Vita.)

## Results

### 2.3 Memory headroom after 10 galaxies with 4 players (needs hardware; use Part 4 for four, or the partner program with bots if it can make more)
- Steps: start a Play Together room with 4 players if you can (otherwise 2 and note it). Visit 10 different galaxies in a row through the Observatory (no restarts), taking the host's star in each or leaving by the dome.
- Watch: any slowdown, texture glitches, loading that takes much longer, a "Preparing graphics" screen that does not end.
- Pass: the tenth galaxy loads in about the same time as the first and the game keeps the frame rate.
- Proof: `[mp] net memory: free min <N>, free now <N>` (should stay well above 0; note the minimum), `[mp] puppet heap play serial=... napa=... gddr=... cache=...`, no `out of memory`.

### 2.4 Quick messages, pointer, hello
- Steps: send each quick phrase from the menu and with **SELECT** + direction. Hold the pointer button 10 s. Press D-pad down.
- Pass: the partner sees balloons, the pointer in your colour, the hello. One phrase every 2 seconds is enforced.
- Proof: `[mp] ui: chat sent id=<n>`, `[mp] ui: chat shown from=<slot> kind=phrase`.

### 2.5 Suspend and resume mid-session (needs hardware)
- Steps: during a session, press the PS button, wait 10 s, then come back to the game. Repeat with the Vita asleep (power button) for 30 s.
- Watch: the game must not hang. The session may end for this console and offer a rejoin.
- Pass: no crash, no black screen; either the session continues or you land in a menu and can rejoin the same room.
- Proof: `[mp] suspend: left the session (rejoin possible)`, then `[mp] resume: rejoining conn=<kind>`, then `[mp] joined slot=... session=...`.

### 2.6 Connection loss and recovery
- Steps: while hosting with the partner joined, turn the Vita's Wi-Fi off for 20 s, then on. Then do it on the partner's side.
- Pass: no crash. After the Wi-Fi returns the session continues or the game offers to keep the progress (**Your last game ended suddenly. Keep its progress?**).
- Proof: `[mp] slot 1 left reason=...`, `[mp] ui: recovery offered stars=<n>`.

### 2.7 Router port opening on real routers (a far-away code) (needs hardware, and a second Vita to join)
Goal: UPnP and NAT-PMP / PCP work on routers people really own. Try at least three different routers or networks if you can (home, a friend's, a mobile hotspot: a hotspot should not let a friend in).
- Steps: Vita A: **Multiplayer**, **Play Together**, **Invite from Far Away**; read the code. Vita B, on another network (a phone hotspot is one): **Multiplayer**, **Enter a Friend's Code**, type it. With one Vita only, do the host side and read the log (step 15).
- Watch: the code appears; B reaches A ("We couldn't reach your friend's game." if the router does not let it in).
- Pass: on a router with UPnP or NAT-PMP on, the code appears and B joins from outside. On a router with both off, B cannot reach A; after a manual forward of the UDP port, B joins. After leaving, log into the router and check that the "Super Mario Galaxy" mapping is gone (or expires).
- Proof: `[mp] natpmp: asking the router to forward UDP <port>`, `[mp] direct: address <hidden> source=<kind>`, `[mp] natpmp: ...` / UPnP mapping lines; on leaving, a delete-mapping line.
- Note the router make and model next to the result.

### 2.8 Internet through the service (when it is deployed)
- Steps: **Multiplayer**, **Play Together**, **Play with Anyone** on the gathering screen (the account is made by itself, "Connecting..."); the partner cannot join this, so use a second Vita or a second copy. Then **Star Race** online, finish, **Leaderboards** on the Star Race gathering screen.
- Pass: joined; a Star Race finish posts a time; **Leaderboards** shows it.
- Proof: `[mp] service register ok code=<hidden>`, `[mp] service signin ok`, `[mp] online: room code=<hidden>`, `[mp] service time queued ...`.
- If the build has no service address: **Play with Anyone** and **Friends** are not on the screen; this is expected, mark "not run".

### 2.9 Update notice
- Steps: only with a test service whose `LATEST_APP_VERSION` is higher than the build's. Open the Multiplayer menu.
- Pass: "A newer version of the game is available." shows once, and not again until the game is restarted.
- Proof: `[mp] update: a newer version is available (this build <v>, service says <v>)` once.

### 2.10 Save a report
- No screen writes the report in this build (the Connection Test has no **Save Report** button). Mark "not run". If a later build brings it back: after a session, choose **Save Report**, open `ux0:data/smg/report.txt` on a computer, and check it is under about 30 kB, shows the build, protocol, settings and `[mp]` lines, and contains no addresses, friend codes, room codes, player names or folder names.

## 3. With a computer partner

*Placeholder: an agent ("macbot") is preparing a practice partner program that runs on a computer so one Vita can try multiplayer. When it exists, write here:
how to start it, which connection it speaks (Wi-Fi), how to see what it does, and which items above it can stand in for. Until then, items in Parts 1 and 2 need a second Vita.*

## 4. Two or more Vitas

### 4.1 Join, play, leave with real consoles
- Steps: A and B both choose **Play Together** on the same Wi-Fi; A (the leader) presses **START**. Play 5 minutes in Good Egg. Get a star together. Leave from B.
- Pass: both consoles play the star-get and move together; B's leaving shows a toast on A.
- Proof (both logs): `[mp] joined slot=1`, `[mp] star delivered`, `[mp] slot 1 left reason=...`.

### 4.2 Wi-Fi keep-awake, idle between two Vitas (needs hardware)
- As item 1.6, but with two Vitas, power save ON on both. Both stand still in the Observatory for 2 minutes, then move.
- Pass and proof: as 1.6, in both logs.

### 4.3 Nearby (ad hoc), no router
- Steps: Wi-Fi off on both Vitas. **Multiplayer**, **Play Together** on both (the gathering screen looks nearby by itself); A presses **START**. Play 5 minutes at 1 m and at 5 m.
- Pass: connects in under 30 s; moves are smooth at 1 m.
- Proof: `[mp] net ad hoc: rx late max <ms> ms, ring peak <n>` (see the appendix).
- If the two consoles do not pair, climb this ladder. The switches come from branch `radiostub` (they exist only in a build that has it). Change ONE thing in `env.txt` per step, on both consoles, delete `log.txt`, try for at most 2 minutes, and stop at the first step that pairs (about 10 minutes in all):
  1. Defaults (nothing added).
  2. `SMG_LAN_GROUP=0` on both (the system dialog without the group name).
  3. Host: `SMG_LAN_DIALOG=create` and `SMG_LAN_TIMEOUT_MS=0`; joiner: `SMG_LAN_DIALOG=join` (the host waits for the joiner for as long as it takes).
  4. `SMG_LAN_GROUP=0` on both, with the default dialog mode (`conn`).
  In each log look for `Nearby starts: mode ..., group ..., timeout ... ms`, `NetCheck dialog result 0x...` and `first datagram from <MAC>: ...`. Send back the lines of every step you tried, and say which step paired.

### 4.4 Host migration by pulling the host's Wi-Fi (needs hardware)
- Steps: three Vitas if possible (A is the leader, B and C join), otherwise two. Play. On A, switch Wi-Fi off in the Control Panel (swipe from the screen edge on a Vita:
  press PS button > Settings > Network > Wi-Fi Settings > turn off, or enable Airplane Mode). Wait up to 30 s.
- Watch: B (the lowest slot) becomes the host; a toast says "<name> left. <name> chooses the galaxy now." Play goes on without a reload.
- Pass: the game continues for B and C; B's star still counts; when A's Wi-Fi returns A is out of the room (or can rejoin).
- Proof: on B: `[mp] following new host` or `[mp] flow host changed to=<slot> epoch=1`; `[mp] world host change epoch=1 host=<slot> me_host=1`.
- Also check: after the migration, B is asked **Save this progress to a Team File?** at the end (only the original host saves).

### 4.5 Four Vitas
- Steps: A, B, C, D all choose Play Together on the same Wi-Fi. Play 5 minutes in a heavy galaxy (Honeyhive).
- Watch: frame rate, quality level, all three puppets, name tags, the face row.
- Pass: no console drops under 25 fps for more than a few seconds; no crash.
- Proof: `[mp] perf ...` lines on every console, `[mp] quality level <n>`.

### 4.6 Star Race
- Steps: **Star Race** with 2, then 4 players, a scenario in Good Egg. Finish first on one console, last on another, and leave one player behind (do not finish).
- Pass: countdown starts together; standings show; the 3-minute wait after the first finisher ends the race for the one left behind; results appear everywhere, with **Race Again**, **Next Star** and **Leave Race** for the leader, and **Race Again?** and **Leave Race** for the others (if nobody touches the leader's card for 90 seconds the game chooses **Race Again**, twice at most, and the third time **Leave Race**). Try a guest who wants a rematch: choose **Race Again?** on the guest; the leader should see "<name> wants to race again" and the guest's card **Asked! Waiting for <name>...**. Write down what you saw.
- Proof: `[mp] race armed serial=...`, `[mp] race finish slot=...`, `[mp] race over serial=...`.

### 4.7 2-hour soak (needs hardware)
- Steps: 4 Vitas (or as many as you have, minimum 2) on chargers, Play Together, hop between galaxies for 2 hours, with one player leaving and rejoining every 15 minutes. Do not touch the pause menu settings.
- Watch every 10 minutes: frame rate, the lag icons, memory line, battery.
- Pass: no crash, no desync you can see (everyone in the same galaxy and star), memory minimum not shrinking over time, frame time not rising over time.
- Proof: `[mp] perf` every 10 s throughout; `[mp] net memory: free min` stays steady; no `out of memory`. Send every log.

## 5. Memory (section to be added)

*An agent ("memfix") will append its memory checklist here: the numbers to read from the log and the limits to pass.*

## 6. Results table

| Item | A (Vita) | B (second) | Result | Notes |
|---|---|---|---|---|
| 1.1 .. 1.8 | | | | |
| 2.1 .. 2.10 | | | | |
| 4.1 .. 4.7 | | | | |

---

# Appendix: with and without the fourth core

Most players do not have the CapUnlocker plug-in, so the game cannot use the Vita's fourth core (core 3, mask 0x80000). Multiplayer
has to stay smooth on three cores. This checklist covers two console setups, each over Nearby (ad hoc) and over Same Wi-Fi:

- **A, four cores:** CapUnlocker installed, and env.txt has no `GXV_CORE3` line.
- **B, three cores:** either CapUnlocker is not installed, or it is installed and env.txt has `GXV_CORE3=0`.

`GXV_CORE3=0` gives every thread the place it would have on a console without the plug-in, so you do not need to uninstall it
(`tests/mp/nocap_test.cpp` checks this against the 1.3 defaults). The only difference is the probe line. **Do not use Settings >
Core plan: Off for B:** it also moves vitaGL's collector to core 2 and the DSP mixer to cores 1-2 (the 1.2 layout), so you would be
testing something else.

## env.txt (`ux0:data/smg/env.txt`) on both Vitas

```
SMG_MP_PERFLOG=1      # "[mp] perf" every 10 s, plus the session's thread census once, 5 s into the session
GXV_THREAD_CPU=1      # "threads: 10.0 s, CPU ms by core mask: ..." every 10 s
GXV_CORE3=0           # only for setup B on a console that has CapUnlocker
```

Use the same build and the same settings (Picture mode) on both Vitas. Delete `log.txt` before each run.

### First-console aids (all default off; none is a plug-in)

```
SMG_LOG_UDP=192.168.1.20:9999   # copy every log line to this address (your Mac), by UDP
SMG_MP_SIM_RESUME=30            # act as if the console slept, 30 s into every session (SMG_MP_SIM_RESUME_COUNT=n: how many times, default 1)
SMG_MP_STACK_CANARY=1           # measure the multiplayer helper thread's stack use (adds a "helper thread stack peak" line)
```

- **Log mirror.** On the Mac: `nc -ul 9999 | tee mirror.txt` (use the port you wrote; macOS `nc` needs `-u -l 9999`). Only private addresses are accepted (10.x, 172.16-31.x, 192.168.x, 127.x); a public address or a broadcast address is refused with a line in `log.txt` and the mirror stays off. Nothing arrives before the network library is up, which for single player and the first minutes is not until **Multiplayer** is first opened, and nothing arrives in **Nearby** play (no router Wi-Fi). Lines logged while nothing could be sent wait in a 16 KiB ring (the oldest are dropped and counted: `[mirror: N lines lost]`). The game never waits for it. `log.txt` on the card stays the complete record; pull it anyway. The mirror sends your log, which can contain your console's IP address, only to the address you set.
- **Simulated resume.** See step 12b.
- **Stack canary.** Off unless `SMG_MP_STACK_CANARY=1`. It paints the unused part of the helper thread's 64 KiB stack when the thread starts and prints `[mp] mem 30 s: helper thread stack peak X of 64 KiB` every 30 s in a session. Report the highest X; `(OVER 75%)` in the line means the stack is too small for comfort.
- **Computer rehearsal of the console's memory** (desktop game, not the console): `PETARI_VITA_LIMITS=1` gives the game the console's 24 + 64 MiB banks (a heap that runs out on the console runs out there too), and `SMG_VITA_BUDGET_MIB=96` makes every `[mp] mem` line say what percentage of a 96 MiB C heap is in use (`OVER` at 100; the desktop's 64-bit objects are larger than the console's, so treat it as an upper bound). Scenario `mp_mem4_EggStarGalaxy` with both switches is the 4-player rehearsal.

### Net timing knobs and the choppiness numbers (`[mp] net:` lines)

Every timing value that decides how smooth the other players look can be changed in `env.txt`, with no rebuild. They are read once when a
session (or a scan) begins, clamped to the range shown, and written to `log.txt` in one `[mp] net: tune ...` line (the effective values, and
`changed=` listing the keys you gave; a key that is not a number is ignored and named in a `net: tune notes:` line). With no key set, nothing
differs from the build's own values. Times are milliseconds.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `SMG_MP_NETLOG_SEC` | 5 | 0..300 | seconds between the `[mp] net:` statistics lines (0 = off) |
| `SMG_MP_NET_SEND_MS` | 22 | 10..100 | shortest time between two pose (STATE) sends; 22 = every second 60 Hz turn, 30 a second |
| `SMG_MP_NET_DELAY_MIN_MS` | 75 | 34..333 | the least a remote player is drawn behind the newest pose it sent (jitter buffer floor) |
| `SMG_MP_NET_DELAY_MAX_MS` | 333.3 | 75..333 | the most (jitter buffer ceiling); a value under the floor is raised to it |
| `SMG_MP_NET_DELAY_EXTRA_MS` | 25 | 0..100 | added to the measured lateness when the buffer picks its delay |
| `SMG_MP_NET_DELAY_PCT` | 95 | 50..99 | the delay covers this percentage of the packets; the late rest is guessed over (extrapolated) |
| `SMG_MP_NET_EXTRAP_MS` | 200 | 0..300 | how long a puppet is guessed ahead past its newest pose before it stands still |
| `SMG_MP_NET_KEEPALIVE_MS` | 50 | 20..200 | a quiet peer link carries a datagram at least this often (Wi-Fi power save) |
| `SMG_MP_NET_BUNDLE_MS` | 10 | 0..50 | the host's poses wait at most this long for company in one datagram |
| `SMG_MP_NET_HEARTBEAT_MS` | 500 | 100..2000 | while other traffic flows, the ping heartbeat rides inside a datagram this often |
| `SMG_MP_NET_POLL_MS` | 5 | 1..20 | the network thread's sleep between passes (how soon received datagrams are looked at) |

Two kinds of line, every `SMG_MP_NETLOG_SEC` seconds in a session:

- `[mp] net: slot=1 rx 29.8/s gap 33.5/71 ms (mean/max) buf 82/96 ms (mean/max, target 83) late=0 dup=0 stall100=0 interp=299 extrap=3 hold=0 snap=1`
  one per remote player. `rx` = poses received a second; `gap` = time between two poses (mean/max; measured when the game takes them in, so a game hitch
  shows here too); `buf` = how far behind the newest received pose the player is drawn, the interpolation delay in use, with the buffer's target;
  `late` = poses that arrived after a newer one; `dup` = poses refused (twice, or too old); `stall100` = gaps over 100 ms; `interp` / `extrap` / `hold` =
  draws inside the buffer / guessed ahead of the newest pose / standing still (`extrap` and `hold` are the visible stutter); `snap` = jumps the smoothing could not glide.
- `[mp] net: link adhoc tx 61.2/s 14.9 kB/s fail=0 rx 60.8/s 15.1 kB/s gapmax=64 ms ring_full=0 radio ok=306 blocked=0 (dropped, not retried) err=0`
  one per link, with the transport (`adhoc`, `udp` or `relay`). `kB/s` = bytes out / in; `fail` = sends the transport reported as errors; `gapmax` = longest silence on the link;
  `ring_full` = pose sends the hand-over ring to the network thread turned away; on Nearby the `radio` part is the radio's own count: `blocked` =
  sends it refused (would block / time-out), which are **not** retried (the reliable layer resends what mattered). The 10 s `net ad hoc:` line also gives
  the running totals (`sends blocked N, send errors N`).
- `[mp] net link slot=2: in 30.1/s 8120 B/s, out 31.0/s, pose delay 83 ms, rx 301 drop 0 snap 0` every 10 s while traffic flows, one per remote player in
  the scene: datagrams and bytes received from that player's link and datagrams sent to it (the wire level; the `net: slot=` line above is the pose
  level), the pose buffer's delay now, and its poses received / dropped / snaps since the last line. On a guest with 3 or more players the other guests
  read `in 0.0/s` by design: the link is a star, all traffic goes through the host.

After a two-Vita session, look at `buf` and `extrap`/`hold` per player first (a high `gap` max or `stall100` with a low `blocked`/`fail` says the
other console or the air was slow; `blocked` or `fail` above 0 says this console's radio turned sends away), then try `SMG_MP_NET_DELAY_MIN_MS` higher.

### While a session lasts the screen stays on

A player who waits (in a bubble, spectating, in a lobby) touches nothing; the console used to be free to dim and sleep, which ends the network link. While
a lobby or session exists the network thread calls `sceKernelPowerTick(DEFAULT)` once a second. `log.txt` has `[mp] watch: keep-awake on` when it begins
and `keep-awake off` when the session is over. The screen is kept lit too (the 1 Hz tick is `DEFAULT`, which restarts the dimming and screen-off timers). `SMG_MP_KEEP_SCREEN=0` in `env.txt` holds
off auto-suspend only, so the screen may dim (battery test); the default is `1`. Test: leave one Vita untouched in a lobby for longer than its auto-dim time (Settings > Power Save Settings).

## What to read in log.txt

| Line | A (four cores) | B (three cores) |
|---|---|---|
| `clocks/cores: fourth core ...` (once, at start-up) | `available (CPU 3, mask 0x80000, NN% ..., probe 1 of 6); used by: disc read-ahead and movie decoder 3; audio output and DSP mixer; front-end finishing thread;` | Without the plug-in: `not available (the system refused the mask; ...): nothing moves`, with one probe. With `GXV_CORE3=0`: `not available (not probed: GXV_CORE3=0 ...)` |
| `threads: core 3: the ... runs on the fourth core` | audio output thread, DSP mixer thread, front-end finishing thread | none |
| `par-view async: worker started (...; core N, priority 0xP)` / `par-anim: ...` | core 3 | core 1 |
| `[mp] helper thread smg_mp_net: core 2 priority 0x70 (mask 0x40000 asked, 0x40000 now)` | the same | the same |
| `[mp] cores: fourth core ...; N threads (masks: ...)` | `available (CapUnlocker)`, and `fourth core` count 3 or more | `not available ...`, and `fourth core 0` |
| `threads: 10.0 s, CPU ms by core mask: ... 3 X ...` | X > 0 | X = 0 |

### The thread census (`[mp] threads: "name" priority 0xP, core mask 0xM`)

These lines come once per session, 5 s into it. In the first session after launch they come right after the first perf line,
which turns the measurement on. Threads made through pthreads show the library's name, so find them by their mask and priority.

| Thread | Mask in A | Mask in B | Priority |
|---|---|---|---|
| game thread | 0x10000 | 0x10000 | its class-3 priority (the one the renderer threads share) |
| renderer back end, SceGxmDisplayQueue, vitaGL "Garbage Collector" | 0x20000 | 0x20000 | |
| front end's player | 0x40000 | 0x40000 | |
| front-end finishing thread | 0x80000 | 0x20000 | one step above the renderer's |
| `galaxy_audio` (audio output) | 0x80000 | 0x60000 | 0x40 |
| DSP mixer | 0x80000 | 0x20000 | 0x50 |
| JAudio thread (GXV_AUDIO_CPU1) | 0x20000 | 0x20000 | 0x4F |
| AI clock, OS clocks, pad thread | 0x60000 | 0x60000 | 0x40 / 0x50 |
| disc read-ahead | 0x80000 | 0x60000 | 0x70 |
| `smg_view_async` (par-view) | 0x80000 | 0x20000 | one step above the game thread's |
| `smg_spec` (par-anim) | 0x80000 | 0x20000 | one step below the game thread's |
| **`smg_mp_net`** (session helper) | **0x40000** | **0x40000** | **0x70** |
| **`smg_mp_rx`** (Nearby only: ad hoc receive) | **0x40000** | **0x40000** | the process default (`SMG_MP_RX_PRIO` changes it) |
| **`np_https`** (Internet only) | **0x40000** | **0x40000** | the default + 20 |
| `smg_loading` (loading screens) | 0x40000 | 0x40000 | the default |

Fail: any thread with mask 0x80000 in B, or a `smg_mp_*` / `np_*` thread with mask 0x80000 or 0x10000 in either setup.

### The perf line (`[mp] perf 10.0 s: ...`, every 10 s)

`mp A/B ms a turn`, `frame p95 X ms (max, target T, ...)`, `quality level L`, `helper H ms/s (W wakes, max M ms)`, `sends S/s
(refused R, wait avg a max m ms)`. The first perf line after launch shows helper and sends as 0, because it starts the
measurement.

| Field | Expected | Investigate if |
|---|---|---|
| `sends S/s` | about 30 standing in a scene; up to about 45 while B (the pointer) is held | below 25 for a whole line with the scene running |
| `refused R` | 0 | more than 0 in steady play (the helper fell 4 sends behind) |
| `wait avg / max` | about 2.5-3 ms average (half the helper's 5 ms sleep); max under 10 ms in A, under 15 ms in B | max over 33 ms (a whole picture), more than once |
| `helper H ms/s` | low and about the same in A and B | B more than about 1.5 times A |
| `mp A ms a turn` | the same in A and B, within noise (game-thread work does not depend on the cores) | B clearly higher |
| `frame p95` vs `target` | at or under the target in A. B can be a little higher | p95 over 1.12 times the target in most lines |
| `quality level` | 0 in A. B may step to 1 or 2 in heavy places (Good Egg, Honeyhive, the lit Observatory) | 3 or 4 with only two players, or levels going up and down all the time |

The level changes are logged once each: `[mp] quality level N (frame p95 X ms)`.

### Nearby only: `[mp] net ad hoc: rx late max X ms, ring peak N` (every 10 s)

- `rx late max` is the longest time the ad hoc receive worker waited for core 2. It runs below the front end's player there.
  - Good: under about 5 ms.
  - 10-30 ms: it waits for the player, which adds that much delay to the other console's moves. Repeat the run with
    `SMG_MP_RX_PRIO=0x70` in env.txt and compare the remote player's smoothness and this number. Report both runs.
- `ring peak` is the most datagrams waiting for the helper. Good: 1-4. Over 32 points at the helper, not the radio.

## The four sessions

Run each one with two Vitas, both in the same setup. Play about 5 minutes in a heavy galaxy (Good Egg or Honeyhive) and return to
the lit Observatory. During the run, do the following:

1. Both Vitas choose **Play Together**; the first is the leader and presses **START**.
2. Walk and jump next to each other for 1 minute. Remote Mario/Luigi should move smoothly, with no freezing or rubber-banding.
3. Hold **Circle** (the pointer button) for 10 s. The other Vita shows the pointer and drops it when you let go.
4. Get a star together. Both play the star-get and move on together.
5. Make a galaxy move (the loading zones follow the host).
6. Open the session menu, then leave from the guest.

| Session | Setup | What to send back |
|---|---|---|
| 1. Nearby | A on both | both log.txt files |
| 2. Nearby | B on both | both log.txt files, and `rx late max` against session 1 |
| 3. Same Wi-Fi | A on both | both log.txt files |
| 4. Same Wi-Fi | B on both | both log.txt files |

Optional: a mixed pair (host A, guest B) over Nearby. Nothing on the wire depends on the cores, so it should play the same.

For each session, note:

- frame p95 and the quality level from the last three perf lines;
- the `sends`, `refused` and `wait` numbers;
- any hitch you saw, and when.

- Not seen even on the desktop (the timeline could not reach them; the host tests cover their words and touch boxes): Leaderboards, Friends, How to Play (the three-line controls page), More Options, the host-rules card, the recovery prompt, the Connection Test screen. On the Vita, open each and check the glyphs next to the words and that a finger on each row and guide button does its action.

<!-- memfix: the "Memory" section below is a separate block at the end of the file on purpose (release3 rewrites the rest). -->

## Memory

What to read in `log.txt` to check the multiplayer memory budget (the numbers come from the audit in `share/memory-budget.md`). No switch is
needed: these lines are written once per boot / scene / session. `GXV_PERF=1` and `SMG_MP_PERFLOG=1` add the port's own memory lines
(`[vgx-vita] ... memory now: free main ...`, `vitaGL pools now ...`) and the `[mp] puppet heap init/play` lines already existed.
Compare an MP scene against the same galaxy with multiplayer off: Egg Planet first (the file cache is tight there), then the heaviest
galaxies, with 4 players (Luigi + Cosmic + Cosmic Luigi as remotes), and 20+ scene changes in one session.

| Line (stable format) | When | What to check |
|---|---|---|
| `[mp] mem ui boot stationed-napa-free=A->B layouts=0 eager=0` | once at boot | A == B: the multiplayer UI takes **nothing** from the stationed Napa heap at boot (it took 0.5-1.5 MiB before memfix). `SMG_MP_UI_EAGER=1` in env.txt restores the old boot-time creation (eager=1, layouts=39): only for measuring A-B on the console. |
| `[mp] mem ui layouts kind=menu\|session why=scene\|open\|need count=N bytes=B napa-free=X->Y ms=T` | when a scene makes its UI layouts | `kind=menu` (file select): at the first need (Multiplayer menu opens), never when it is not opened. `kind=session`: at the scene's load, only when a session is active. B = scene Napa bytes taken (all die with the scene). T = milliseconds (a one-off hitch when the menu opens). Single player: **no such line at all**. |
| `[mp] mem ui layouts REFUSED kind=.. napa-free=X need=N reserve=3145728: no multiplayer UI in this scene` | only if the scene heap is too full | must never appear. If it does, report the galaxy. |
| `[mp] mem scene init serial=S napa=N gddr=G cache=C` | session scenes, after the scene's own init, before any multiplayer object | free bytes of the scene Napa / scene GDDR / file cache |
| `[mp] mem scene mp serial=S napa=N gddr=G cache=C` | the same scene, after puppets, spares, tags, balloons, world | `init` minus `mp` = what multiplayer's objects cost in that scene |
| `[mp] mem archive <arc> heap=cache\|gddr cache-free=C gddr-free=G` | when a character archive is added to the scene's list | Our archives go to the scene GDDR while it has 4 MiB to spare (`heap=gddr`), so they never take the file cache; `heap=cache` appears only when the scene GDDR is that tight |
| `host heap mp session start: arena A KiB, in use U KiB, free F KiB in P pieces; ...` | when the session starts | the C heap's use at the start |
| `host heap mp first tls: ...` | after the first TLS handshake of the process (Internet) | what a live TLS connection holds (about 50-70 KB estimated) |
| `host heap mp session end: ...` | when the session ends | back near the start value (growth over repeated sessions = a leak or fragmentation: report it) |
| `[mp] mem <when>: free user U cdram C phycont P KiB (lowest seen u / c / p); C heap used X of 98304 KiB, arena ..., free ... in N pieces` (hwaids) | `<when>` = `mp session start`, `mp session end`, `scene start serial N`, `30 s` (every 30 s in a session), `after resume`, `resume +3 s`, `resume +15 s` | Report the lowest free user, cdram and phycont of the whole run and the line they came from. Free user under about 20000 KiB, or C heap used above 80% of its size, is a warning; any `OVER` is a failure. A value after `resume +15 s` far below the one before the sleep means a leak across a resume. |
| `[mp] mem scene start serial N heaps: scene napa free A KiB, scene gddr free G KiB, file cache free C KiB` (hwaids) | each scene of a session, after the puppets | Egg Planet's file cache is tight (about 1 MB free before the 4-player archives): report C for it. |
| `[mp] mem scene start serial N gl: ...` / `gl (cont.): ...` (hwaids) | the same line, the renderer's pools | vitaGL's free and total KiB for main, video and phycont. |
| `[mp] mem 30 s: helper thread stack peak X of 64 KiB` (only with `SMG_MP_STACK_CANARY=1`) | every 30 s in a session, and `resume +3 s` | Report the highest X. Over 48 KiB is `(OVER 75%)` and means the 64 KiB stack needs to grow. |
| `[mp] puppet spares wanted=N freeSeats=F present=P max=M dropin=D` | session scene init | spares are capped to the free seats: N <= F (0 when the room is full or closed to drop-ins) |
| `[mp] puppet slot=N hidden character=X reason=memory (until the next scene)` / `puppet slot=N self NOT created reason=memory` | only if the scene heap cannot hold a puppet with its 3 MiB reserve | that player is shown by name tag and party strip only for this scene; must never appear on a real galaxy |

Test switch (desktop only, not for the console): `SMG_MP_TEST_NO_PUPPET_MEM=1` makes every puppet instance fail that check
(scenario `tools/mp/harness/scenarios/mp_nopuppet_mem.yaml`).
| Step | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Pass / fail / not run | | | | | | | | | | | | | | |
| Notes | | | | | | | | | | | | | | |
