# Version 3.0 — November 2026

This version includes everything in 2.3 (listed under Version 2.3 below), adds multiplayer, upgrades both **Performance (60 fps)** and **Quality (30 fps, hardware 2× MSAA)** modes with major GPU/shader/CPU speed-ups, ships refreshed Home Screen bubble, LiveArea and launch splash artwork (`content-rev 5`), and packs a complete **5,401-shader native PS Vita seed pack** so the game boots and plays cleanly with zero cold shader compiles across galaxies. **Hardware-verified on a real PS Vita (498 MHz, 4 cores) on 26–27 November 2026**, including live two-player Wi-Fi Play Together sessions (menus, gathering screen, fly-in, play, bubble rescue, checkpoints and leaving at 5–16 ms ping). **Two physical Vitas together, Nearby (ad hoc) play and far-away codes on real routers have not been tried on consoles yet.** [Known Issues](KNOWN-ISSUES.md) lists what is open and what has not been seen on a console yet.

### Multiplayer
- **Play Together** (2 to 4 players) shares the galaxy, switches, purple coins and stars. **Star Race** runs everyone's own copy of a scenario from a shared countdown, with a timer on screen, the standings while you wait and a results card. See [Multiplayer](docs/MULTIPLAYER.md).
- **One door to a game.** After the title, **Multiplayer**, then **Play Together** or **Star Race**: a gathering screen shows a face for each player it finds on the Wi-Fi (or nearby, with no router), and the leader presses **START**. There is no Host or Join to choose. For a friend far away the leader chooses **Invite from Far Away** and reads out a code; the friend chooses **Enter a Friend's Code**. A game that has already started shows as **Play with <name>**. Every button name and picture in the menus is the Vita's, and touch works on lists, faces and buttons. **How to Play** opens a short page with the controls of a game.
- **A new first screen.** After the title, the game asks **Single Player** or **Multiplayer** and remembers your choice. The round two-player button on the file select is gone. (`SMG_MP_MENU=0` in `env.txt` switches the multiplayer menus off.)
- **Back from the save files.** On the file select (the heads), **Circle** or the round Back button goes back to the Single Player / Multiplayer screen, with the game's own fade. Nothing is opened or saved.
- **Settings are in the game; the LiveArea tile is gone.** Choose **Settings** on the Single Player / Multiplayer screen, or press **△ Settings** in the pause menu (which now has two lines: camera zoom and Settings). Clean **2-mode Picture choice** (**Performance 60** and **Quality 30**); the user-facing 50 fps toggle is retired (`GXV_HRW_ALLOW50=1` remains available in `env.txt` for testers). Rows that added no value were removed and now set themselves, and **Safe saves is always on**. Testers keep `env.txt` lines for each: [Tester switches](docs/TESTER-SWITCHES.md).
- **Upgraded bubble, LiveArea and launch splash artwork (`content-rev 5`):** refreshed 8-bit palette PNGs for the Home Screen bubble, LiveArea background framed around the Start gate, transparent 3D title emblem, and full-screen launch splash (`vita/vgx-game/sce_sys/ARTWORK.md`).
- **Multiplayer console polish (hardware verified on PS Vita, 27 November):**
  - **Even 60 / even 30 pacing in multiplayer:** busy 2P scenes no longer stutter in an uneven 45–52 fps band; compact galaxies lock **60.0 fps** (Flipswitch: 300/300 1-vblank frames, 0 late) and heavier open galaxies snap cleanly into **even 30 fps** (Good Egg: 150/150 2-vblank frames, 0 long holds).
  - **Smoother remote player movement:** hardened pose interpolation, velocity-matched glide, animation blending, and 30 fps near/mid puppet cadence cut remote motion jitter dramatically (raw packet `p99=149.2 u` smoothed to `mean=5.68 u`, `p99=49.3 u` rendered).
  - **Zero fly-in pushing:** player-to-player collision pushing is suppressed cleanly through galaxy fly-in and landing so teammates never bump each other off the start spot.
  - **Full Luigi body on face-first fly-in:** fixed self-puppet / model visibility so Luigi renders completely during face-first launch-ring fly-ins.
  - **Bubble follow camera after death:** after falling into a bubble, the camera smoothly follows your bubbled character across the galaxy until a teammate pops you free (never overriding cutscenes or event cameras), and clears leftover death-area hits on pop.
  - **Group-restart & reconnect hardening:** epoch guards prevent late defeat/coin races during group restarts, and reconnect probing supports unicast + broadcast fallback with direct rejoin addressing.
- **Picture, 60 Assist and Language changes apply without quitting by hand.** They are read when the game starts, so leaving Settings offers **Save and Start Again** and the game starts itself again. From the pause menu it asks first, because you leave the level (progress since your last save is lost; your save file is untouched). In 2.3 the plain Save kept these changes for the next launch, so a mode switch seemed not to work until you quit the game. If you choose not to start again now, the toast says the change comes the next time the game starts. If a save is still being written, a small window says **Finishing saving... the game starts again as soon as it's done.** and the game starts again by itself when it is; **Later** (or Circle) keeps the change for the next start. Only if saving stops for 30 seconds or reports an error does it offer **Start Again Now** (your game save is protected by Safe Saves) or **Later**.
- **60 Assist** (Settings, under Picture, in Smooth Motion): **On** (the default, as in 2.3) shows 60 pictures a second by estimating every other one from the one before; thin moving edges can shimmer. **Off** shows only real pictures, so busy places look less smooth. It applies when the game starts: leaving Settings offers **Save and Start Again** (see below).
- **Safe Saves is now always on.** Your single-player save always goes through a checked temporary file and keeps the previous save as `GameData.bin.mpbak`. It uses about 100 KB more card space (the `.mpbak` is about 48 KiB, and a `.mptmp` exists while saving).
- **The game now starts in your console's language.** Language defaults to the Vita's system language when the disc has it. A console set to French will start the game in French even if 2.x ran it in English. Change it in **Settings > More**.
- **No multiplayer settings screen.** A game always has room for four, bumping is on, ordinary enemies are shared and other racers are ghosts. The only rule you are asked is what a fall costs (**In a Bubble** or **Try Again**), once.
- **Star Race results card.** After a race, a card on the star screen shows the places and a Series column of points that add up from race to race. The last winner starts the next race a few seconds late (3, 6 or 9), so the others can catch up. The leader chooses **Race Again**, **Next Star** or **Leave Race**; every other player can press **Leave Race** to leave on their own. The card shows no countdown, but if nobody touches the leader's card for 90 seconds the game starts **Race Again** (twice at most; the third time it chooses **Leave Race**). Guests can ask for a rematch with **Race Again?**; the leader sees "<name> wants to race again" and still decides. A race waits 3 minutes after the first player finishes.
- **Faces under the star banner.** In Play Together the players who have a Power Star stand under the game's own star banner; there is no separate Team Star card.
- **Boss fights are per console.** Each Vita runs its own boss. The first star ends the fight for everyone. This is said in the guide on purpose; sharing boss damage is possible later work.
- **Six characters:** Mario, Luigi, Toad, Yellow Toad, Cosmic Mario and Cosmic Luigi.
- **Bubbles:** a fallen player is carried in a bubble to a teammate. A friend who is still loading counts as not caught for up to 40 seconds, so a group is not restarted under them.
- **Star rules.** A second star won while you are already in a star celebration is recorded and announced instead of lost. A star that the game itself does not record is not shared. A star handed to you while you are on a Launch Star or Pull Star waits up to 12 seconds. On your own single-player file, another player's star counts only once it has appeared in your copy of the galaxy; hidden, boss and grand stars, which only the other console can make, count when they belong to the scenario you are playing and that player is in the same scene. A star that is finally not saved is never silent: both players see "<name>'s Power Star couldn't be saved this time. Sorry!".
- **Quick messages** (twelve phrases) and typed messages (up to 24 characters), on in every kind of game. The game does not filter typed words (names are filtered with a list shared with the online service). **Hide Messages** hides one player's messages and balloons on your console, and follows that player if they come back.
- **Waiting is explained.** When another console holds yours behind a closed screen for more than about 2 seconds (after a Power Star, until the leader chooses the next galaxy), a small line says **Waiting for <name>...** (also for a player in a bubble while a friend is still loading) and goes away by itself; it never takes a button. **Friends** refreshes by itself while it is on screen (every 30 seconds at most, it backs off after errors and stops after 3 minutes without a button press or 20 requests); **START** refreshes it by hand. The lists of consoles on the Wi-Fi or nearby already update live.
- **Reconnecting.** A Wi-Fi drop, a sleep or a quiet link no longer ends the game at once: the game shows **Reconnecting...** and puts you back in the same place for 30 to 75 seconds. If it cannot, **Continue <name>'s Game** is at the top of the Multiplayer menu for two minutes, and the messages say plainly what happened.
- **If the leader leaves,** the player in the lowest remaining place takes over and the game goes on. Seen with three copies on one computer; not tried on consoles.
- **Add Friend, Block and Played With** work in every kind of game. **Played With** keeps the last 20 players.
- **Languages:** English, French and Spanish, plus German and Italian (not yet reviewed by native speakers).
- **Update notice:** "A newer version of the game is available." is shown once per start when the online service says so (only in a version connected to it).
- **Who Can I Play With?** replaces the old Connection Test. It is offered after a connection fails and answers three questions (people in this room, a friend far away, anyone online).

### Online
- **The online service is live on `https://galaxy.psvita.dev`** (protected by Cloudflare WAF and rate limiting, with Cloudflare TURN relay active and `workers.dev` disabled). **Play with Anyone**, **Friends**, and **Star Race Leaderboards** (`https://galaxy.psvita.dev/times`) are active for Version `3.0.0`.
- **No sign-up form.** The online account is made quietly the first time something needs it, without a password or e-mail address: your console sends a display name (names do not have to be different) and solves a small puzzle first, so mass sign-ups cost time. The friend code is your identity.
- **The service dictates how often a game asks it anything,** so a game costs about 60 requests per player-hour instead of 354, and the daily allowance covers about 1,000 player-hours instead of 169 (computer tests, not a live service). Wrong room codes are limited per network address (ten in 10 minutes, 40 a day), and an invite gives out no address until the leader's console has accepted the joiner.
- **The service says when it is busy.** It keeps a daily allowance, stops taking new games at about 80 percent of it while games already going continue, and the game shows "Online play is busy right now" and stops asking for up to 10 minutes. New accounts are limited to 10,000 a day in launch week and 2,000 afterwards. Wi-Fi, nearby and code games never use the service.
- **Leaderboards check races.** A race time shows to other players only when a second player of the same game, on another network, with an account at least two days old, sent a matching time. A time that looks too good is hidden until an administrator looks at it. The service has a public web page of checked times.
- **A far-away code:** the leader chooses **Invite from Far Away** and the game shows a short code for the friend. This is being reworked, so how it reaches the leader's console can change. Tried only against pretend routers on a computer.
- **`server =` and `stun =`** settings, so you can use your own service or STUN server.
- **Wi-Fi** games and **nearby** (ad hoc, no router) games. Nearby has never been tried on real consoles.

### Sessions you can trust more
- **Sealed sessions.** In every kind of game, what the consoles send each other after the join is encrypted and checked, console by console. A message that does not come from the player it claims is thrown away. Joining uses a cookie step so a stranger who sends a join request leaves no trace on the host, and a console that dropped out takes its place back with a secret token that only it knows. A console that has been removed is told so and is refused if it tries again, even after the leader changes, while it keeps the identity it joined with. A returning player's place is held for that player alone, and a flood of fake or wrong join requests no longer keeps honest players out. Someone on the same Wi-Fi can still jam a game, list fake games, or answer a join before the real game does; there is no password and the gathering screen puts together every console on the Wi-Fi that is on it; a removed player can come back as a new console (removal is not a permanent ban). See [Playing with strangers](docs/MULTIPLAYER.md#playing-with-strangers), [Privacy](docs/PRIVACY.md) and [the session security notes](docs/MP-SESSION-SECURITY.md). Only tested on computers.

### Saves
- Multiplayer progress lives in its own Team Files. The game is built so that your single-player file is not changed unless you choose **Play This File**, which makes a backup first. In computer test runs, a guest's file and a leader's file were never changed. **Play This File** has only been tested by unit tests and computer runs. Back up `ux0:data/smg/saves` before your first multiplayer game.
- **Your ordinary single-player save is now written safely (Safe saves, always on).** It goes through a temporary file, is read back and checked, and keeps the previous save as `GameData.bin.mpbak` (about 48 KiB) beside it; a `.mptmp` exists only while saving. A power cut leaves the old save or the new one. In a computer test that cuts the power inside the write, 22 of 300 random sequences ended with a damaged file before, and none now. Cost on a computer: 4.5 to 6 ms a save instead of 1 to 2 (not measured on a Vita). `SMG_ATOMIC_SAVES=0` in `env.txt` turns it off (testers). A missing `GameData.bin` is put back at the next start only when a `.mptmp` is beside it; a lone `.mpbak` is not restored, so a save you delete by hand stays deleted.
- **Recovery checkpoints are checked.** A blank checkpoint is never offered; one with fewer stars than your file now has (saved later) is set aside; putting one into a file with more stars or a newer save asks first. This follows a random computer test where the recovery step left a single-player file blank; its causes were found and fixed.
- **Save-safety fixes.** Outside reviews found ways multiplayer could change your single-player file or lose a backup: a file select that showed multiplayer files while the single-player file was written, a game that ended at the wrong moment, backups that could be removed straight away when the clock was behind, and another player's progress that could be written after the leader left. These are fixed and tested on a computer. A multiplayer file the game cannot read is kept aside instead of being written over, and a file written by a newer game is left alone.
- **Safe Copies.** The first time a new version starts, the game makes one checked copy of your Team Files, settings and online sign-in in `ux0:data/smg/mp-backups/`. **Safe Copies** (**Make a Copy**, **Make a Copy to Share** with no Friend Code, and **Bring Back a Copy**) is under **How to Play** (**Triangle** on the Multiplayer menu, **OK**, **Yes**), and with **Triangle** on **Which File?**; **Bring Back a Copy** is also offered where a file cannot be read. A save that is waiting on the card for more than 15 seconds no longer holds the game: it stops waiting and says to try again. Tried only on a computer.
- While you play, the game writes recovery checkpoints, and is meant to offer to keep them after a crash or a lost connection ("Keep its progress?"). The checkpoints are written in computer test runs; the question itself has only been tried in unit tests.
- Team Files, copies, backups, recovery checkpoints and your single-player file are all written through a temporary file and renamed, so a cut does not leave half a file.

### An even 60 or an even 30 (Performance mode)
- **No more in-between 50 fps.** Both single player and multiplayer now target an even 60 fps and fall back cleanly to a locked even 30 fps only after sustained misses (two 5-second quiet windows below 52 fps), never snapping down on brief spikes and retrying 60 fps automatically (`10 -> 20 -> 30 s` backoff, with fast 3-second recovery when headroom returns). The user-facing 50 fps toggle is retired; `GXV_HRW_ALLOW50=1` in `env.txt` remains as a tester escape hatch.
- **Fresh CPU PGO profile + shader & GPU speed-ups (hardware verified on PS Vita at 498 MHz, 4 cores):**
  - **Shader FPS Boost & vitaGL vp34:** split branchless vertex shaders, half-precision fog, projective texture-coord optimization, early alpha-test + register-pressure cuts, exact 1-byte `TF_I8` (`GL_INTENSITY`) texture storage, fast ARMv7 display-list vertex welding, 2×2 tile-packed `SwizzleU8`, and vitaGL fast detach/write-copy/intensity paths.
  - **Fresh CPU PGO profile** (`tools/pgo/smg.profdata.gz`) re-recorded across 8 gameplay routes for zero profile mismatch warnings and extra CPU headroom on heavy stages.
  - **Measured real-Vita Performance 60 (`960×544`):** Honeyhive holds **59.9 fps native** (`600/600` frames, `100/100` cadence, 12.8 ms GPU); Melty Molten runs **55–59.6 fps** (`90–100%` ideal cadence); Dreadnought holds **55–57 fps** in normal play and steps cleanly to locked even 30 on its heaviest overlook.

### Quality mode (30 fps, hardware 2× MSAA)
- **Hardware 2× MSAA + crisp filtering at native 960×544** (`GXV_Q30_MSAA=1`, `GXV_Q30_MSAA_STENCIL=1`), trilinear + hardware anisotropic auto-mipmaps, sharp character LOD (`GXV_PLAYER_LODBIAS=-50`, `GXV_PLAYER_ANISO=3`), and seven exact Performance GPU speed-ups enabled in Quality too.
- **Measured real-Vita Quality 30:** Dreadnought and Melty Molten hold a locked **30.0 fps** (`300/300` exact 2-vblank frames, `98–100/100` cadence) at `960×544 2× MSAA`; Beach Bowl holds **28.6–30.2 fps** with native HUD; automatic rung step-down (`2× MSAA` -> `1×` native) protects the 30 fps lock under heavy load (`GXV_Q30_MSAA=0` in `env.txt` pins 1×).

### Complete 5,401-Shader Native PS Vita Seed Pack & Visual/Boot Fixes
- **Instant cold-boot shader loads (~0.4 s, zero cold compiles):** all **5,401** Wave 6 shaders were harvested natively on retail PS Vita hardware across all **134** Galaxy:Scenario stops, **18** boss fights, all **7** power-ups, multiplayer menus/puppets/bubbles, and in-game Settings (both Performance 60 and Quality 30), packed into `stages.pack` + `v3/seed.c182` (`15,996` stage-warm records across all 46 tour stage keys, `0` open closure). Cold-boot verification from an empty card cache preloaded all 5,401 shaders in **`404 ms`** with **0 cold shader compiles** across Title, Comet Observatory, Good Egg, Honeyhive, Beach Bowl, Melty Molten, and Dreadnought.
- **Black-hole visual fix:** fixed white flickering and crosshair artifact when Mario falls into a black hole (MIRROR wrap coordinate fix).
- **Faster start-up & cleaner boot feedback:** trimmed boot-time overhead and suppressed false "Taking longer than usual" stall warnings while shader compilation is actively making progress.
- **Fixed (console rounds, 25–27 November):**
  - **20-second periodic hitch:** spread thread checks across frames (well under 1 ms each).
  - **Galaxy map pointer:** planets whose center is slightly off-screen (Bowser's) can be targeted wherever their disc is visible.
  - **Multiplayer pause menu:** restored camera zoom row (`L`/`R`) with "The game keeps running." indicator.
  - **OLED-safe keep-awake:** console display stays awake cleanly without screen dimming during long sessions and shader runs.

### Smoother play (Performance mode)
Measured on a PS Vita at 498 MHz with four cores, Performance mode, scripted routes, each change against the same build without it.
- **No more getting stuck at 48 fps.** A stage that could not quite hold 60 used to lock at an even 48 because of a pacing rule. Beach Bowl now spends most of its time at 60 (median 48.0 -> 59.9 fps; visible stutters about a half to two thirds fewer). Good Egg has about two thirds fewer stutters, and Dreadnought gains 1 to 2 fps (48-49 -> 50-51) with fewer stutters. Hitches and input latency are unchanged. `GXV_PENDING_HOLD25=0` in `env.txt` brings the old rule back.
- **Less GPU work at Dreadnought.** One fewer GPU pass per picture where the game captures the screen for its heat-haze effects (`GXV_EFBSWAP2_BACKOFF`), and a cheaper pass for the corner effect (`GXV_CORNERPASS_ALT`). Same pixels; up to about 1.5 ms less GPU time a picture.
- **The picture goes straight to the screen more often.** While 60 Assist is on but not in use, the finished picture is now shown directly instead of being copied first (`GXV_DD_DYN`). Same pixels; about 0.3 to 0.5 ms less GPU work a picture. Dreadnought 50.4 -> 51.4 fps and Melty Molten 51.5 -> 53.1 fps on average, with about an eighth fewer stutters at Melty Molten.
- **More free video memory at Honeyhive** (8 to 10 MiB at stage load): the pieces 60 Assist needs are made only when it is about to start (`GXV_S68_PAIR_LAZY`).
- Dreadnought, Melty Molten and the final Bowser fight are still limited by the GPU's pixel work (about 18 ms a picture, 16.7 ms is needed for 60). They now run at an even 30 where 60 does not hold and at 60 where it does (with **Allow in-between rates** on: about 50-53 fps, as in the test builds); a smaller picture size was measured and did not help there.

### Under the hood
- **A quieter log.** `ux0:data/smg/log.txt` now keeps what helps with a bug report (start-up, settings, stage loads, hitches, first-use shader builds, errors) and leaves out the developer's periodic statistics, which saves a memory-card write every 100 ms. `GXV_LOG_LEVEL=2` in env.txt brings the full log back.
- A bridge (`mp_bridge.h`) joins the game side and the port side. Sessions are sealed with XChaCha20-Poly1305 and X25519 keys (Monocypher), and the connection to the service uses TLS (Mbed TLS).
- A link that goes silent is retried for up to 75 seconds (the leader keeps the place for 70). Smaller memory use for queued messages and the world tables, in-play panels redrawn only when they change, and a quality governor whose ceiling is level 0 by default.
- A support-report writer (`mp_report.cpp`, addresses, codes and names removed) exists, but no screen asks for it in this build.
- Unit tests for the update check and the report scrubber (`tests/mp/release3_test.cpp`), and a large set of tests for sealed sessions, saves and copies.
- On a link in a game, each player is sent a small note at least every 50 ms, meant to stop the Wi-Fi going to sleep. Tried only in a simulation, never on a real Wi-Fi network.
- Documentation: [Multiplayer](docs/MULTIPLAYER.md), [Privacy](docs/PRIVACY.md), [hardware test list](docs/MULTIPLAYER-HARDWARE-TEST.md).
- **Far things a star depends on are always drawn.** In Performance mode the far-speck cut (tiny models beyond 25 m, `GXV_FARDRAW`) could skip a far bunny, Toad, Rosalina, Luma, a switch or a boss, as well as anything with a stage switch or camera of its own. Those are never taken now (`GXV_FARDRAW_OBJECTIVE=0` in `env.txt` brings the old rule back). Host test only; no console run.

# Version 2.3 — November 2026

The crash-fix update. The Bee Mushroom and Comet Observatory crashes are fixed, 2.2's smoother play is back, and Performance mode gets lighter.

### Fixed
- **A power-up that can't be used is no longer lost.** If a form's models are not loaded (so Mario can't change into it), the Bee Mushroom, Ice Flower, Spring Mushroom, Boo Mushroom or Rainbow Star stays where it is, with no sound and no change, instead of being used up. For the Bee there is also a late retry that prepares the models once more in case they finished loading late; it is off until it has been tried on a console (`GXV_POWERUP_RETRY=1` in `env.txt` turns it on).
- **Surfing and the Star Ball with a dead motion sensor.** Some Vitas report a motion sensor that gives no readings, which left these levels without steering unless you found Settings > Controls > Tilt with > Left stick. Now, if the sensor's readings stay completely frozen while you move the left stick in one of these levels, or it doesn't answer a fully pushed left stick for a moment, the left stick steers instead. Your saved setting is not changed, and `log.txt` says when this happened. `GXV_TILT_AUTOSTICK=0` in `env.txt` turns it off.
- **A crash when Mario changes form.** Grabbing a Bee Mushroom could crash the game, and so could the other moments Mario switches bodies: an Ice Flower, a Spring Mushroom, a Rainbow Star, or a shock from an electric enemy. The game was finishing a graphics update for the body Mario had just left; now that update is skipped once he's in another body.
- **The Comet Observatory crash from 2.2.** Graphics memory was handed back while the Vita's graphics chip was still drawing into it. It's now handed back a few frames later, once the chip is done. The same fault is the likely cause of the crash reported in the **Kitchen dome**.
- **A black screen that never ended while a galaxy loaded in Performance mode.** At 60 FPS the loading work could wait forever for the graphics chip. It now waits for its own work only.
- **Fog and textures on some objects.** Since 2.0, the fog value inside the shaders had no fixed place, so, depending on the order your Vita first built its shaders, fog could go missing or a texture could pick up the wrong values. It now has a fixed place (all fog shaders are precompiled in the 3.0 seed pack).
- **Objects just off screen with the camera zoomed in.** At the default 1.5× zoom, coins, Star Bits, Launch Stars and Pull Stars a little outside the picture were treated as far away and frozen, so you couldn't collect them or point at them. They now act as they do at 1.0× and are only left undrawn while off screen. While a Pull Star is in reach, the zoom eases back to 1.0× so you can aim at it.
- **Fixed camera shots** (the dome rooms and other still, composed shots) ease the zoom back to 1.0×, so they're framed as the game intends.
- **Mario's ground shadow no longer blinks** on small planets and on the Dreadnought's start disc, where the set of ground triangles under him changed from one frame to the next. It now keeps the larger set steady (never less shadow than before). `GXV_COLSHADOW_STABLE=0` in `env.txt` goes back to the old selection.
- **A picture-size change right after a galaxy loads** no longer trusts what it learned before the load (it resamples instead of skipping), which closes a path to the 2.2 crash. `GXV_RUNG_SKIPRS_SETTLE=0` turns this off.
- **Settings could show "Controller disconnected" and not answer** when the Vita's pad query failed once; the menu now falls back to the plain read of the Vita's own controls. `GXV_PAD_RAWFALLBACK=0` turns this off.
- **If the game ever closes by itself,** `log.txt` now says why, and the Vita saves a crash report (`ux0:data/psp2core-….psp2dmp`) you can attach to an issue.

### Smoother play
- **The memory card can no longer hold up the music.** A long read (an archive of several MiB) is done in pieces of 512 KiB and a music-stream read goes in between, so a stream no longer queues behind a 300 ms archive read; each recently read file keeps its own read-ahead window. `log.txt` gets one `io:` line per stage (what the card did while it played, and the verdict `CLEAN` or what waited). `GXV_DVD_CHUNK_KIB=0` / `GXV_DVD_RA_SLOTS=1` / `GXV_IO_LINE=0` turn these off.
- **2.2's smoother play is back,** now that the crash that made 2.2.1 roll it back is fixed: galaxies prepare their graphics during the loading screen, picture-size changes are prepared ahead (about a millisecond instead of a visible skip), menus are ready at start-up, and Performance mode steps down within a fraction of a second when a scene gets heavy.
- **Shaders load from memory.** The package's ready-made shaders are read into memory once at start-up, so loading screens no longer open hundreds of small files on the memory card, and a change of picture size no longer waits for the graphics chip in the middle of play.
- **Fewer graphics passes for shadows and water.** Two screen copies no longer split the picture into extra passes on the graphics chip: in Good Egg that's about 2 to 3 ms less graphics work per picture.
- **Less game work every frame.** Hit checks between objects only compare the pairs that can actually touch (the same results, in the same order), and the busiest update loops fetch their data ahead.

### Performance mode
- **Fewer far-away details, for speed.** To get closer to 60 FPS, Performance mode switches distant objects to their simpler models a little sooner, thins far-away particles, flower patches and planet fur, leaves out shadows and plants in the far distance, and animates distant scenery every other frame. Flower patches outside the picture aren't animated or drawn at all. Nothing near Mario changes, and gameplay is the same. Quality and Locked 30 look exactly as before. `GXV_PERF_BUDGET=0` in `env.txt` turns it off.

### Under the hood
- More speed-ups are in the source but **off** until they've been checked on more consoles. [Technical notes](docs/TECHNICAL.md#version-23-switches) lists them and how to try them.

**Updating?** Install the new VPK over the old one. Your saves and settings stay. If the LiveArea still shows the old look or no Settings button, use VitaShell's Refresh LiveArea (Triangle on its home page), or delete the game and install fresh (your saves stay).

# Version 2.2.1 — November 7, 2026

A quick fix for 2.2.

### Fixed
- **2.2 could crash on entering the Comet Observatory.** 2.2.1 goes back to the proven game code of 2.1.1 while the cause is found; the smoother loading and picture-size changes of 2.2 will return in a later update once they're fixed.
- Keeps 2.2's new artwork and the larger Settings tile.

**Updating?** Install the new VPK over the old one. Your saves and settings stay.

# Version 2.2 — November 7, 2026

The smooth-play update, part two. Fewer hitches everywhere, quicker menus and a new look.

### Smoother play
- **Galaxies start ready.** Each galaxy finishes preparing its graphics during the loading screen (a few seconds longer the first time at most), so the first steps in a galaxy no longer stutter. On a 3-galaxy test, graphics prepared mid-play went from 25 to 0.
- **No more hitch when the picture size changes.** The next size is prepared ahead of time, so a change takes about a millisecond instead of a visible skip.
- **Menus prepare their graphics at start-up**, so the pause menu and star select open smoothly.
- **Performance mode reacts faster.** When a scene gets heavy it steps down within a fraction of a second instead of several seconds. Good Egg at the widest camera now holds 60 FPS.
- **Every packaged shader is compiled on a real Vita.**

### New look
- **New original artwork** for the bubble and LiveArea: a starry night sky over a glowing ocean.
- **The Settings button on the LiveArea** has the size the Vita expects. If you still don't see it after updating, use VitaShell's Refresh LiveArea (Triangle on its home page) or delete the game and install fresh (your saves stay).

### Fixed
- A guard against a crash if Bee Mario's model isn't loaded yet when you grab the Bee Mushroom.

**Updating?** Install the new VPK over the old one. Your saves and settings stay.

# Version 2.1.1 — November 7, 2026

A quick fix for 2.1.

### Fixed
- **Objects out of place and a crash on getting the Bee Mushroom.** A small number of the ready-made graphics added in 2.1 were built wrong, which could put things in the wrong place or crash the game the first time it needed them. Those have been taken out; the Vita now prepares them itself, correctly, the first time they're needed (mostly while a galaxy loads). Speed in play is the same as 2.1.

**Updating?** Install the new VPK over the old one. Your saves and settings stay. If you're on 2.1, please update.

# Version 2.1 — November 7, 2026

The smooth-play update. Far fewer pauses while the game prepares new graphics.

### Smoother play
- **Many more graphics ready out of the box.** The download now carries ready-made shaders gathered from every mission of every galaxy, not just the first, so the game almost never has to build a shader mid-play anymore. In our test of eight galaxies on a fresh install, only 4 shaders had to be built on the Vita, and those are in the download too. A short hitch can still appear in the first moments of a galaxy while its graphics are linked.
- **Galaxies warm up while they load.** Each galaxy prepares the graphics it's known to need during its loading screen, adding at most a second or so, so your first steps in a galaxy are as smooth as the rest.
- **The list of what each galaxy needs now ships with the game,** so even a fresh install gets that warm-up on its very first visit.

### Under the hood
- An optional background shader compiler is included for testing (`GXV_ASYNC_SHADERS=1` in `env.txt`). It's off by default while it's being tuned.
- The package no longer carries a stray macOS file.

**Updating?** Install the new VPK over the old one. Your saves and settings stay.

# Version 2.0 — November 7, 2026

The full-resolution update. Super Mario Galaxy now runs at the Vita's native 960×544 at a steady 30 FPS, with a new Performance mode that reaches for 60.

### Pick your picture
- **Quality (the default):** the sharpest picture at a steady 30 FPS, at up to the full 960×544. In the busiest moments it softens edges first, then sharpness a step at a time, and brings everything back as soon as there's room.
- **Performance:** as close to 60 FPS as each scene allows. When 60 isn't possible it settles on a smooth 30 rather than a choppy 40.
- **Locked 30:** always 960×544, always 30 FPS when the Vita can manage it.

### Smoother play
- **Honeyhive, Beach Bowl and the Bowser fights hold 30 FPS** in Quality mode. They used to drop into the teens and 20s.
- **All four CPU cores.** With the CapUnlocker plug-in, sound, animation and part of the graphics work move to the Vita's fourth core.
- **The game and the graphics work side by side,** so the game never waits on the renderer between frames.
- **Leaner graphics:** lighter shaders, cheaper water and glow effects, and nothing drawn that ends up off screen.
- **Even frame pacing,** with each frame shown right on time.

### Better picture
- **Widescreen picture sizes** and texture sharpness that follows the resolution.
- **Edge smoothing** across the whole picture, with the HUD kept crisp.
- **No more stray lines:** a 1-pixel line in Beach Bowl and under menu buttons, and a line under the star counter, are gone.
- **The unpowered Comet Observatory,** early in a new game, now has dim but visible floors.

### Controls and gameplay
- **New pause-menu options:** use the right stick as the Star Bit pointer (Square), and set Star Bit reach (Select) to collect from farther away.
- **Enemies that grab and release Mario hurt him again,** just like on the Wii.
- **Hidden-star missions** work properly.
- **Rear touch works right after the Settings menu.**
- **Spins come out every time,** even when you press Square right after a jump.

### Under the hood
- Shaders are prepared again after an update, graphics memory stays tidy in long sessions, and a 500 MHz PSVshell profile is left alone.
- The HD texture pack builder matches the game's colours exactly.
- Full details are in [Technical notes](docs/TECHNICAL.md) and [Source record](docs/SOURCE.md).

### Thanks
Several fixes and ideas come from Producdevity's fork of version 1.1: Star Bit reach, the right-stick pointer, the enemy-damage, hidden-star and rear-touch fixes, and some speed-ups. See [Credits](CREDITS.md).

**Updating?** Install the new VPK over the old one. Your saves and settings stay. Measured frame rates are in [Performance](docs/PERFORMANCE.md).

# Version 1.2 — October 31, 2026

The picture-perfect update: a sharper, correct picture, a 30 FPS floor in most of the game, and a real Settings menu.

### Better picture
- **Bowser no longer twists and spins** in his battle introductions (also fixed: the Bowser Jr. ending, three Polta scenes and a Dino Piranha scene).
- **Lumas have their colours back** instead of showing up as white blobs.
- **Ground-pound and black-hole trails are back,** with the right colours on about 85 other effects.
- **Matter Splatter's walkways appear as they should** when the light reaches them, instead of flashing black.
- **Mario's eyes in HD** with the optional HD textures, and **Queen Bee's fur** looks right.
- **Sparkles and rain at full size,** and textures as sharp or soft as the game asks for.
- **A sharper picture by default.** Most galaxies now stay at 640×456 or better, where 1.0 often stepped down.

### Smoother play
- **A 30 FPS floor.** The quality system now puts 30 FPS first: in a heavy scene it lowers the sharpness and the effects that matter least, keeps Mario at full quality, and brings everything back within seconds when there's room.
- **Locked 30.** Prefer an even, steady picture? Settings > Picture rate > **Locked 30** holds a smooth 30 every frame.
- **No first-visit freezes.** Every galaxy's graphics come ready-made in the package, so entering a galaxy for the first time no longer stops the game for seconds.
- **Much faster start.** The title screen comes up in about 20 seconds (1.0 took about 40), and the first launch after installing is just as quick.
- **Rock solid in long sessions.** A memory problem that could crash the game after many galaxies in one sitting is fixed (tested through all 45 galaxies in a row).
- **Movies on the Vita's video hardware,** and sound mixing on its own core.

### Settings
- **A full Settings menu,** from the game's LiveArea screen: picture presets, sharpness, anti-aliasing, effects, HD textures, depth of field, reduced motion, language, and a Controls page that shows what every button does. It's drawn in the game's own font, over your own title screen.
- **Tilt your way.** Tilt sensitivity on a simple 1–10 scale, and steer the Star Ball and Manta Ray with the left stick instead of tilting, if you prefer.
- **Mouse-style aiming** for the Star Bit pointer, as an option.
- **Reduced motion** now really works in play: much less camera shake and blur.

### Controls and gameplay
- **Music keeps playing** after sleep or the Home menu.
- **Dialogue no longer skips to the end** when a finger rests on the screen.
- **Spins answer at once,** even right after a jump.
- **PS TV with a PS3 or PS4 controller:** R2 shoots Star Bits, the right stick moves the pointer, the D-pad keeps the camera.
- **Tidier hints.** Rewritten prompts for the Vita's controls wrap at whole words.

### Your own title screen
- **No game artwork in the download.** The first time you reach the title screen, the game saves it on your memory card and uses it for the loading screen and the Settings background. The CBOC Vita Installer builds the home-screen bubble from your own game; a manual install keeps a plain star (the console doesn't let a game change its own bubble).

### Steadier start
- **The loading screen can't hang at 99%,** and a real stall says what it's waiting for.
- **Clearer errors** if the shader compiler or the graphics fail to start, and running out of graphics memory skips a frame instead of freezing the Vita.

Updating? Install the new VPK over the old one. Your saves and settings are kept. The full engineering list is in [docs/SOURCE.md](docs/SOURCE.md#version-12).

# Version 1.1 — October 26, 2026

Mario's eyes, a Settings menu, and a steadier start.

### New
- **Settings menu.** Tap **Settings** on the game's LiveArea screen, before launching or while playing. It is drawn in the game's own fonts. Choose:
  - **Controls.** Remap every button for the Vita or for a PlayStation controller on a PS TV, with a picture of the controller.
  - **Picture rate.** Up to 60 FPS, a fixed 30, or **Steady 30**: an even 30 FPS that lowers the resolution and effects quality when needed. For players who prefer a steady picture to the highest frame rate.
  - **Dynamic quality** and **reduced motion.**
  - **Game language,** from the languages your game files include.
  
  Choices are saved in `ux0:data/smg/player-settings.cfg`. Some apply after a restart, which the menu offers. Your saves are never touched.
- **Title-screen loading picture,** with a loading bar that shows real progress.

### Fixed
- **Mario's eyes.** Players with the optional HD textures saw his eyes white, or his eyelids closed all the time. Eyes now keep the game's own pictures, so they show and blink normally. The rest of the HD pack still applies.
- **Blue eyes without the HD pack too.** A colour calculation that the Wii wraps around was being clipped, which could leave his irises white. It now matches the Wii.
- **Start-up crash guarded.** A rare crash in the first seconds of a launch came from a file seek on a handle that was not open. That seek now fails safely.

### Faster
- **Quicker sound mixing.** The game's audio is now mixed with the Vita's vector unit, which frees a little time on the game's busiest thread. The sound is bit-for-bit the same.

### Easier to help with
- **A slow start tells you.** If loading takes more than 90 seconds, the screen says so and points you to the game files folder.
- **A fuller log.** `log.txt` (in `ux0:data/smg`) records each loading stage and when the title was reached. Each 5-second line shows how many shaders were prepared.

Updating? Install the new VPK over the old one. Your saves are kept. The first start after updating takes a little longer while the game prepares its updated shaders.

# Version 1.0 — October 2026

Super Mario Galaxy, running natively on the PS Vita.

### Measured on a PS Vita
- **51 fps** on average in Good Egg Galaxy, **43 fps** in the Comet Observatory (the busiest place in the game), a full 60 in quieter moments.
- **0** stutters longer than a tenth of a second while playing, and **0** picture-size changes in the middle of play, in every test run.

### Touch controls and gameplay
- **Rear touch Star Bit aiming.** Slide on the rear pad to move the Star Bit pointer, like a trackpad, with your thumbs still on the sticks. Resting fingers are ignored. Turn it on in the pause menu with Triangle.
- **Settings panel in the pause menu,** drawn in the game's own fonts and window style: camera zoom with L and R (1.0× to 2.0×, 1.5× by default) and rear touch aiming with Triangle, with the game's own menu sounds.
- **Touch everywhere you used to point.** Tap to collect and choose, and hold still to grab Pull Stars. On menus, the left stick and Cross work too.
- **Tilt steering** for the Star Ball and the Manta ray, levelled to how you hold the Vita. Select resets it.
- **Vita button prompts** throughout the game's text and pictures.

### Smoother play
- **No resolution hitches.** The picture saves its sharpness changes for pauses and screen fades, and each galaxy remembers what works best for it.
- **Three cores at work.** The game, the graphics preparation and the GPU feed each run on their own Vita core, so busy scenes like the Comet Observatory keep moving.
- **Faster graphics engine.** Repeated graphics state is remembered instead of re-read every frame, and big scenes need fewer GPU passes.
- **Effects ready ahead of time.** 1,131 graphics effects ship already compiled for the Vita, so new ones load instantly instead of pausing the game.

### Better picture
- **Sharper than the Wii.** The game renders at up to the Vita's full 960×544 when there's room, with anti-aliased edges on top, and a sharpened upscale when it renders smaller.
- **Optional HD characters and items.** Sharper textures for Mario, Luigi, Peach, Rosalina, the Lumas, Toads, Bowser, Bowser Jr., the Power Stars, launch stars and power-ups, made on your computer from your own game files. [How](tools/hdtex/README.md).
- **Full-screen 16:9** by default.

### Under the hood
- Shares its graphics engine work with our Mario Strikers Charged port: safe texture clean-up while a scene is drawing, a large shader-program cache, and careful video memory use.
- A short log in `ux0:data/smg/log.txt` (frame rate, stutters, freezes, picture changes, errors) to attach to bug reports.

The full engineering list is in [docs/TECHNICAL.md](docs/TECHNICAL.md#whats-in-version-10).

