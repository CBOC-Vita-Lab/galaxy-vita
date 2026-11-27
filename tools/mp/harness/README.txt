Galaxy 3.0 multiplayer test harness (tools/mp/harness)
========================================================

Runs real desktop game processes (platform/desktop/main.cpp, the "game" binary) headless on one machine with the owner's
cooked disc, alone or several at once talking over 127.0.0.1, plus optional mp_bot clients, and judges the runs from
their logs, screenshots, exit status and the single-player save guard. Python 3 + PyYAML + bash only.

Files
-----
  common.sh          defaults (binary, data, saves, output folder) and the headless environment
  run_one.sh         one game process in a fresh run folder (setup, run, save guard, crash symbolization, summary)
  run_pair.sh        N game processes (host, guest, ...) + bots started together; per-process env; merged timeline
  supervise.py       launches and watches one process: epoch-ms stamped console log, RSS/CPU samples, hard timeout
  save_guard.py      sha256 of every save file before/after; verdict on the single-player GameData.bin
  hlog.py            log loading (game.log + console.log, deduplicated, on one clock)
  merge_timeline.py  merged timeline of several processes
  assert_log.py      the rules language (below); exit 0 pass / 1 fail / 2 bad rule
  run_scenario.py    runs scenario YAML files (run_pair.sh + assert_log.py); skips scenarios whose features are missing
  scenarios/         a_*, b_*, c_* = baseline (run now); mp_* = multiplayer (prepared)
  inputs/            named PETARI_INPUT timelines ("@name" in -i / scenario input)
Self-tests (no game build needed): tests/mp/harness_test.py, tests/mp/harness_mp_golden.py (see the end).

Defaults (override from the environment)
---------------------------------------
  HARNESS_BIN=/home/claude/g3/desk/build/game   snapshotted per run into $HARNESS_CACHE/bin/game-<sha12> (a relink during
                                                a run cannot change it; crash addresses are symbolized against the snapshot)
  HARNESS_DATA=/data/cooked   HARNESS_SAVES0=/data/saves0/saves (copied into every run, never written)
  HARNESS_OUT=/home/claude/g3/hruns (<date>/<name>/...)   HARNESS_CACHE=$HARNESS_OUT/.cache (binaries, shader cache)
  HARNESS_LP_THREADS=1 (llvmpipe threads per process)   HARNESS_NICE=5   HARNESS_LINK_BASE=41110   HARNESS_XVFB=0

Requirements of the desktop game on x86-64 (found while bringing the harness up, 8 Nov 2026)
  * GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 is REQUIRED (common.sh sets it). Without it the C heap (brk) grows from the
    image (base 0x48000000) into the emulated Wii window and the boot aborts: "mmap at 80000000 (+8000000) failed: File
    exists" (SIGABRT). With it every host malloc is an mmap far above the window.
  * The build needs -fnew-alignment=8 -fno-aligned-allocation (CMakeLists.txt desktop branch, harness commit 6abed2e /
    mp-int 99370dd): the game's operator new returns 4-byte aligned JKRHeap blocks, clang on x86-64 assumes 16 and zeroes
    new objects with movaps -> #GP at boot in StarPointerOnOffController() (reported as SIGSEGV address=0).
    A crash with address=0 is not always a null pointer: crash.txt shows the faulting instruction.
  * Image base 0x48000000 (-DGALAXY_NATIVE_IMAGE_BASE=0x48000000; 0x98000000 does not link on x86-64): fine at run time.

Headless environment given to every game process (common.sh; a scenario or -e overrides any key)
  EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=1 SDL_AUDIODRIVER=dummy
  GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 (as tools/run_native.sh) PETARI_SHOT_MS=5000 PETARI_STALL_S=30
  PETARI_SHADER_CACHE=<run>/shader-cache (seeded from and merged back into $HARNESS_CACHE/shaders.bin under flock)
  MESA_SHADER_CACHE_DIR=$HARNESS_CACHE/mesa

Commands
--------
  Shared box (7 GB, no swap, ~0.9 GB per game): wrap every direct run_one.sh / run_pair.sh in /home/claude/g3/tools/gamerun.sh
  (machine-wide flock). run_scenario.py does it per pair by itself (HARNESS_GAMERUN= to turn it off).
  # one run, 120 s, file 1 into the Observatory, screenshots every 5 s
  tools/mp/harness/run_one.sh -n obs -s 120 -i @file1_observatory -e PETARI_GAME_CLOCK=1
  # two processes, per-process environment, merged timeline in <pair>/timeline.txt
  tools/mp/harness/run_pair.sh -n duo -s 150 -e PETARI_GAME_CLOCK=1 -e PETARI_INPUT=@file1_observatory \
      -P host=SMG_MP_AUTO=host -P guest=SMG_MP_AUTO=join
  # judge any run or pair folder
  tools/mp/harness/assert_log.py <folder> -e 'nocrash *' -e 'save_guard *' -r rules.txt
  # scenarios (baseline now; the mp_* ones say SKIP until their features are in the binary)
  tools/mp/harness/run_scenario.py tools/mp/harness/scenarios/a_boot_observatory.yaml ...
  tools/mp/harness/run_scenario.py --judge-only <pair-folder> scenarios/x.yaml   # re-judge without re-running
  tools/mp/harness/run_scenario.py --dry-run scenarios/mp_*.yaml                 # print the commands
  tools/mp/harness/merge_timeline.py <pair-folder> [--all] [-g REGEX]

run_one.sh options: -n name -o parent -s seconds -e KEY=VAL (repeat) -f envfile -i input -t timeout -S savesdir|none
  -g (under gdb: every thread's backtrace on a crash) -x (xvfb-run) -w waitfile. Exit 0 = ended by itself with status 0,
  no crash, single-player save unchanged.
run_pair.sh options: -n -o -s -e as above; -p LABEL[:ENVFILE[:DELAY_S[:SECONDS]]] (repeat; default host, guest);
  -P LABEL=KEY=VAL; -b 'LABEL CMD...' (a bot); -L (no link env); -g.

A run folder: console.log ("<epoch ms> line", everything the process printed), game.log (the port log, own HH:MM:SS.mmm
stamps), shots/frame_NNNN.png, saves/ (copy of the test save), save-before/after/guard.json, mp-saves.json (the MP save
root <run>/mp/saves, listed only), proc.csv (RSS/peak/CPU/threads every 0.5 s), result.json, summary.json, crash.txt
(crash line with every pc/lr symbolized by llvm-addr2line against the binary snapshot, plus gdb backtraces with -g),
env.txt, cmd.txt, binary.txt. A pair folder: one run folder per process, <label>.out, timeline.txt (filtered: [mp],
port:, tour:, harness:, crash...), timeline-all.txt, pair.json (wall time, peak RSS of each).

Link environment per process (run_pair.sh, process i = 0,1,...; games first, then bots)
  SMG_LINK_PORT / SMG_MP_UDP_PORT = 41110+i
  SMG_LINK_BCAST / SMG_MP_UDP_BCAST = 127.0.0.1:41110,127.0.0.1:41111,...  (every port of the set; the backend skips its own)
  SMG_LINK_VIP = 10.77.0.<i+1>, SMG_MP_UDP_VIP = 0x0a4d00<i+1> (core parses a number)
  SMG_LINK_NAME / SMG_MP_NAME = <label>
  (net/src/vlink_udp.cpp reads SMG_LINK_*, core's mp_transport_vlink.cpp reads SMG_MP_UDP_*: both are set until one wins.)

Autopilot environment for MP scenarios (core's mp_core.cpp, as merged/being merged)
  SMG_MP_AUTO=host|join   SMG_MP_AUTO_MODE=together|race   SMG_MP_AUTO_CONN=wifi|nearby|loopback   SMG_MP_AUTO_START=<n>
  SMG_MP_NAME=<name>      SMG_MP_LOOK=0..3 (Mario, Luigi, alt colours)
  Galaxy moves without menus: PETARI_STAGE on the host (GameScene: a direct requestGalaxyMove = MOVE type 0 after 120
  gameplay turns; a list "A:1,B:1" + PETARI_STAGE_HOLD=<turns> is a tour).
  Requested from other owners (scenarios that need them SKIP until the binary contains the name):
    SMG_MP_TEST_STAR=<s>   (flow) the local player gets the current scenario's star <s> s after the scene barrier
    SMG_MP_TEST_SWITCH=<s>:<id>|<zone>/<id>  (world) turn a switch on through StageSwitchFunction (PETARI_SWITCH sets the
                           container bit directly, so the bridge's edge hook never sees it)
    mp_bot CLI             (core) --join --name N --seconds S --pose circle|still; reads SMG_MP_UDP_*; exit 0 when done

Rules language (assert_log.py; one rule per line or per YAML list item; '#' comments; leading '?' = soft, only WARN)
  expect  P /RE/ [within T]                            a P line matches (within T of P's start)
  expect  P /RE/ [within T] after Q /RE2/              ... at/after the first Q match of RE2, within T of it
  absent  P /RE/                                       no P line matches
  count   P /RE/ OP N                                  OP: == != >= <= > <
  order   P /RE/ before Q /RE2/ [within T] [each|first]  the first (each: every) P match is followed by a Q match within
                                                       T; $1..$9 in RE2 = groups of the P match (same serial, crc, ...)
  order   P /RE/ near Q /RE2/ within T [each]          a Q match within T either side of the P match (same event on two
                                                       consoles, either may come first)
  exit P OP N | nocrash P | nostall P | save_guard P | mp_save P | shots P OP N
  metric P wall_s|cpu_s|rss_mib|lines|p50_ms|p95_ms|p99_ms|hitches|fps OP N
  distinct P /RE(group)/ OP N    absent P /RE/ before Q /RE2/
Process control keys (in a process's env; read by the harness, harmless to the game):
  HARNESS_KILL="<regex>|<delay s>|<signal>"   kill the process <delay> s after its first output line matching <regex>
  HARNESS_AFTER=<label>                       start once process <label> has ended (+ DELAY_S)
  HARNESS_CONTINUE=<label>                    start from <label>'s saves/ and mp/ (same console restarting)
  P/Q: process name, comma list, '*' (each must pass), 'any' (one must). T: 500ms, 30s, 2m. RE: /python regex/ (\/ for /;
  trailing i = ignore case). Times: game.log stamps (in-process wall clock) or the supervisor's stamp for stderr-only
  lines; all processes run on one machine, so lines of different processes are ordered on one clock.
  In YAML, write each rule in single quotes ('...', a quote inside doubled): a rule may contain ": ", "#" or start with "?".

Self-tests
----------
  python3 tests/mp/harness_test.py        7 tests, ~20 s: run_pair with a fake game; pass on a good run; fail on a guest
                                          that does not follow a MOVE, a crash (crash.txt), a save write (guard), absent /
                                          count / missing lines; bad rule = exit 2; soft rule = WARN; scenario SKIP
  python3 tests/mp/harness_mp_golden.py   ~2 min: every prepared mp_* scenario passes on a golden fake log with the exact
                                          [mp] wording and fails when one key line is changed (rules are satisfiable and
                                          not vacuous)

harnessfix additions (11 Nov 2026)
----------------------------------
  Verdicts: PASS / FAIL / INVALID. assert_log.py exits 3 (INVALID) when a rule failed AND a game process ran below min speed (game seconds per wall
    second after the title screen, from the "gx: frame N presented" stamps; default 0.6 = 36 fps, a scenario says `min_speed:` or --min-speed / HARNESS_MIN_SPEED).
    Why 0.6: a healthy run measures 0.96-1.0; the network timers run on the wall clock, so below 0.6 a failed timing rule describes the machine. A PASS stays PASS.
    Every verdict (stdout, verdict.json "machine") carries load average (start -> end, run_pair.sh writes load.json) and per process wall s, CPU s, wall s to the title, speed.
  Times: `within 30s` is GAME time inside one process (frames / 59.94: a slow machine or cold disk cannot fail it); `30ws` is wall; between processes always wall.
harness-ticks additions (13 Nov 2026): game-time windows, frame stamps, no-draw
--------------------------------------------------------------------------------
  Frame stamps: with PETARI_LOG_FRAMES=1 (platform/src/os/os_alarm.cpp: compiled for every non-Vita target, inert where there is no renderer frame counter,
    default off; common.sh sets it for every run) each game.log line is "HH:MM:SS.mmm @f<game frame> text"; hlog.py strips the stamp and builds exact
    (wall ms, frame) points: Proc.game_s(wall) and Proc.wall_ms(game s). A binary without the switch logs unstamped lines; the judge then interpolates from
    the coarser "gx: frame N" / frametimes.txt points as before.
  Game-unit windows (OPT-IN): `30gs` game seconds, `1800f` game frames (1/59.94 s). Same console: its own frame counter. Between consoles (after Q, order
    within / tol, near): the sender's wall instant is mapped to the receiving console's game time and the window is measured there. Every game window
    (same console or between consoles, tol too) has a WALL CEILING of window / 0.25 (HARNESS_WALL_CEIL_SPEED / --wall-ceil-speed; 0 = off), so a console that
    makes no frames for 30 s cannot stretch it into a pass. `s` / `ms` / `m` / `ws` keep their old meaning exactly (s: game time inside one console when the
    log has frame points, wall between consoles). A gs / f rule on a console with no frame points FAILS ("no game-frame stamps"). The game's own network
    timers are wall-clock (mp_sys_now_ms = CLOCK_MONOTONIC, MP_LINKLOST_RECONNECT_MS 75 s), so rules about them (qa_guest_wifi_12s's reconnect rules) use `ws`.
  Verdict: UNCHANGED. A failing run with any console under min_speed is INVALID, otherwise FAIL; a PASS stays a PASS. verdict.json now also carries, as
    information only: results[].timing (bounded / cross / wall / untimed), all_game_time, wall_rules, slow, slowest_speed; a PASS below speed 0.8 prints "PASS, slow".
  Examples converted to gs: mp_smoke, qa_guest_wifi_12s (reconnect rules in ws), qa_race_tie. HARNESS_END delays are wall seconds (a game-time end was tried and removed).
  Cheap runs: LP_NUM_THREADS=1 (HARNESS_LP_THREADS) and SDL_AUDIODRIVER=dummy were already the defaults. HARNESS_NODRAW=1 sets GXV_SKIP_ALL=1
    (platform/src/gx/vgx_galaxy.cpp: non-Vita targets, inert elsewhere, default off): the render thread rasterises no frame except a captured one (screenshots,
    PETARI_SHOT_MS). A NODRAW run must NOT be compared with a baseline (HARNESS_BASELINE, vsbase) recorded with drawing on.
  vsbase P p50_ms|p95_ms|p99_ms|fps OP FACTOR: frame times against a single-player run of the same machine (--baseline / HARNESS_BASELINE); skipped (and said so)
    without a baseline. agree P,Q roster|stars: quiet-network invariant, added as a SOFT pair to every run of 2+ consoles (HARNESS_AGREE=off|soft|hard).
Rig-honesty rules (rigfix): `absent`, `count`, `distinct` FAIL on a console whose log has fewer than HARNESS_MIN_LINES lines (default 40;
the fake-game tests set 0): not printing a line proves nothing on a console that printed nothing. `order` without `each` judges the FIRST
anchor only and says so in its detail ("only the FIRST of N matches judged"; `first` spells the default out). A failed `?` rule is listed
as SOFT-FAILED in the assert_log summary. run_scenario.py's preflight turns a PASS into INVALID for runs that were not what they claim
(no hard rules, unknown scenario key, missing result.json, early clean exit, link profile on `link: false`, relay with no packet on a
pair/direction, relay died or stray packets, HARNESS_VITA_LIMITS ignored); the reasons are printed and written to <run>/preflight.json.

  New rules: same P,Q /RE(g)/ [first|last|each] (captured values equal on the consoles); order ... before ... tol D (Q may be D earlier);
    absent P /RE/ before shutdown; `absent ... before Q` also ends at the shutdown window (500 ms before the first "desktop: terminal" / SIGTERM of any process):
    the BYE of the process that stops first reaches the others before its own last log line (mp_join host-left, 2 ms).
  Input marks: input files declare `mark NAME /REGEX/ ~FALLBACK_S` and use `NAME+<t>[-<t1>]:items`; the supervisor appends !NAME to <run>/input.live (PETARI_INPUT_FILE,
    re-read every 100 ms by platform/desktop/main.cpp resolveInputMarks) when the game printed the line. Binaries without "desktop: input mark" get FALLBACK+t absolute times.
    inputs/file2_observatory.txt and entry_recover.txt use them (the old timeline: file2_observatory_timed.txt).
  Warm-up: the first run after a boot reads the game data once (stamp /dev/shm/harness-warm.stamp, HARNESS_WARM=0 skips).
  Screenshots: every run gets PETARI_HANDHELD_UI=1 + PETARI_BACKEND=vgx (HARNESS_HANDHELD=0 off). 960x544 (the Vita's screen, GXV_CAP_PRESENT): HARNESS_SHOT_SIZE=960x544,
    run_scenario.py --shots544, or `shot_size: 960x544` in a scenario.
  run_scenario keys: timeout (hard wall s per process), min_speed, shot_size. run_pair.sh -t. Each run folder: binary-commit.txt (HARNESS_BIN_COMMIT or $HARNESS_CACHE/bin-commits.tsv).
  Evidence: /home/claude/g3/tools/make-evidence.sh rebuilds /home/claude/g3/share/EVIDENCE.md (map: evidence-map.tsv).

QA hooks and the sweep (hooks branch, 12 Nov 2026)
--------------------------------------------------
Test hooks (desktop build only: `#if defined(TARGET_PC) && !defined(__vita__)`; the Vita objects do not contain the names; one static env read when unset):
  SMG_MP_TEST_RACE_FRAMES=<n>          MpRace.cpp onStarGet: this console's real FINISH carries exactly n frames (two consoles with the same n tie). Log "race test frames n".
  SMG_MP_TEST_RACE_LEAVE="<card>[:<s>]"  MpUiSystem.cpp: OK is pressed on the n-th results card with choices, <s> (default 6) after it came up: a guest's Leave Race,
                                       through resultsInput / takeResultsLeave / leaveByUs. Log "race test leave n".
  SMG_MP_TEST_KICK="<s>[:<slot>]"      MpFlow.cpp testMenuHostStep: the host calls mp_ui_kick(slot) (default 1) <s> after a scene start (the Remove button's call). Log "kick test: slot n".
  SMG_MP_TEST_DOME=<s>                 MpFlow.cpp: the host does "Return to the Observatory" (domeRequestStep, what the menu's confirmed request ends in). Log "dome test: serial=n".
  SMG_MP_TEST_WARP="<s>[:<slot>]"      (existing, MpBubble.cpp) "Go to a Friend": MpFlowHooks::requestWarpTo, the call the menu's request ends in.
  SMG_MP_TEST_CHAT="<s>@<serial>:mute:<slot>"  (existing, MpChat.cpp) hides one player's messages (MpChat::setMuted, the Mute button's call).
  SMG_MP_AUTO=join_code:<code>|join_code:wrong   mp_core.cpp AutoStep: join through mp_ui_join_direct_code (the "Join with a Code" keyboard's call); "wrong" = a
                                       well-formed code for 127.0.0.1:$SMG_MP_DIRECT_PORT (default 42611) with the wrong secret. Log "auto: join by code ...: started|refused".
Log lines the scenarios rely on (new): "warp slot=<own> target=<n>", "observatory slot=<own>", "kick slot=<own> target=<n>", "race leave slot=<own>",
  "chat hide slot=<own> target=<n> on=<0|1>", "save write file=<name> ok=<0|1>", "connection lost reason=<err>", "reconnected after=<n>ms",
  and "race results ... places=<a,b,..>" (existing lines: "reconnecting (try n)", "rejoin start conn=.. session=..").
Scenario files: a bot's `cmd` and `requires: files` may use {HARNESS} for this folder (bin/wait_then_bot.sh).
  tests/mp/scenario_regex_test.py checks, from the SOURCE, that every rule regex of the qa_* and glitch_* scenarios can match a line the game can print.
  sweep.sh [-b binary] [-l list] [-o status.tsv] [scenario...]   one scenario at a time through gamerun.sh, waits for the load (<= 2.5), reruns INVALID twice,
  deletes the folders of passes, writes the table (scenario, verdict, seconds, speed, failed rules); default list sweep_order.txt (the audit's 21 runs).
