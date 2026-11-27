#!/usr/bin/env python3
"""release_gates.py: the final-build gates that can be checked from files (strk31; the list is Strikers' final-build gate list, docs/RELEASE-3.0-CHECKLIST.md section 4b).
Pure Python 3, no network, no console. Exit codes: 0 pass, 1 fail, 2 INCONCLUSIVE / invalid input (read the line).

  buildid  --elf galaxy_native [--info BUILD-INFO.txt] --expect ID [--release]
        the multiplayer build id is compiled into the ELF (-DMP_BUILD_ID) and, when BUILD-INFO.txt is given, equals the one the build wrote down. --release also refuses
        the default ("dev") and every test id (ending "-dev", or containing "test"): consoles with different ids never play together, a release that says 3.1-dev is a test build.
  wformat  --db compile_commands.json [--jobs 1] [--filter REGEX] [--limit N] [--nice 10] [--cpath <build>/case-shim]
        -Wformat -Wformat-security over EVERY unit of the database (each command re-run with -fsyntax-only; no object is written). Any warning fails. The build logs only show the
        units a build recompiled, so a count from an incremental build's log is not a gate. The build finds the game's mixed-case headers through the CPATH
        environment variable (<build>/case-shim): without --cpath (or CPATH set) 33 audio / player units stop at a missing header, and the gate says so (exit 2, never a pass). (Vita C library: %z %t %j %hh are checked by tools/check_vita_format.py.)
  emu      --stage NAME:EXIT:LOG [--stage ...] [--min-stages 3]
        the emulator gate on three stages: the run's exit code is 0, its log has 0 faults, and every program was linked before play ('linked by a draw in play' = 0 and the
        first-use reports' 'programs linked by draws' sum 0). A log with no stage-warm-up / first-use line did not exercise the thing: INVALID (exit 2), never a pass.
  twin     --plain LOG --plain2 LOG --pgo LOG [--min-hashes 20]
        the PGO twin: three runs of the same step-locked script (PETARI_SCRIPT_ANCHOR=play counts the script in game TURNS) with GXV_UNDRAWN_LEAN_HASH=N, which logs
        'undrawn-lean: hash turn T HEX actors K'. Plain against plain2 is the noise floor (the emulator's pacing can change what the game does); plain against pgo is the question.
        pass: the hashes agree as far as both runs go. FAIL: pgo leaves plain at a turn where plain and plain2 still agree. INCONCLUSIVE: plain and plain2 differ at or before the
        turn where pgo differs (the check cannot judge: say so, do not call it a pass). An emulator check never proves a PGO build by itself: the console A/B is the proof.
  stack    --root <repo> --build-dir <build> [--jobs N]   |   --su-dir DIR   [--our-budget N] [--hard-only]   (stackgate31)
        the display-queue callback thread (SceGxmDisplayQueue, a 4 KiB system stack) runs PaceDisplay / SyncDisplay and the log path under them: tools/check_display_stack.py
        compiles the listed units with -fstack-usage (the build's own Vita commands) and checks each frame against its budget and the worst chain, with newlib's printf path and the
        system's share added, against the 4096 bytes. Fails (exit 1) on a breach; exit 2 when a listed function is missing or a unit does not compile.
"""
import argparse, concurrent.futures, os, re, shlex, subprocess, sys

# ---------------------------------------------------------------- buildid
def build_id_in(data, ident):
    """the id under its tag as a whole C string in the binary: NUL-delimited "MP_BUILD_ID=<id>" (platform/src/mp/mp_core.cpp mp_build_id_tag).
    The bare id is not proof: "3.0.0" is also SMG_APP_VERSION and is in every 3.x binary."""
    # The tag's own bytes, ended by its NUL. No NUL is required BEFORE it: the linker may place the array right after other data (the 3.0
    # release build, 25 Nov, had it after 8 bytes of a table), and the leading "MP_BUILD_ID=" already makes it the tag, not SMG_APP_VERSION.
    # The NUL after the id keeps a longer id ("3.0.0-dev") from matching "3.0.0".
    return (b"MP_BUILD_ID=" + ident.encode() + b"\0") in data


def is_test_id(ident):
    return ident == "dev" or ident.endswith("-dev") or "test" in ident.lower() or ident.startswith("dev")


def cmd_buildid(a):
    bad = []
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,40}", a.expect):
        bad.append(f"build id {a.expect!r} is not 1-40 characters of letters, digits and . _ + -")
    if a.release and not a.info:
        bad.append("--info BUILD-INFO.txt is required with --release (the id the build was asked for is the second witness)")
    if a.release and is_test_id(a.expect):
        bad.append(f"build id {a.expect!r} is a test id (dev / *-dev / test): a release sets its own")
    try:
        data = open(a.elf, "rb").read()
    except OSError as e:
        print(f"buildid: cannot read {a.elf}: {e}"); return 2
    if not build_id_in(data, a.expect):
        bad.append(f"{a.elf} does not contain the tag MP_BUILD_ID={a.expect} (the build id is compiled in with -DMP_BUILD_ID: the ELF was built with another id)")
    if a.info:
        try:
            kv = dict(l.split(" ", 1) for l in open(a.info).read().splitlines() if " " in l)
        except OSError as e:
            print(f"buildid: cannot read {a.info}: {e}"); return 2
        if kv.get("build_id", "").strip() != a.expect:
            bad.append(f"{a.info}: build_id {kv.get('build_id', '(none)')!r}, expected {a.expect!r}")
    for b in bad: print("buildid FAIL: " + b)
    if not bad: print(f"buildid ok: {a.expect!r} is in {a.elf}" + (f" and in {a.info}" if a.info else "") + (" (release id)" if a.release else ""))
    return 1 if bad else 0

# ---------------------------------------------------------------- wformat
DROP = {"-MD", "-MMD", "-MP", "-c"}
DROP_ARG = {"-o", "-MF", "-MT", "-MQ"}


def syntax_command(command):
    """the unit's own command, turned into a syntax-only run with the format warnings on; None when it is not a compile"""
    t = shlex.split(command)
    out, skip = [], False
    for x in t:
        if skip: skip = False; continue
        if x in DROP_ARG: skip = True; continue
        if x in DROP: continue
        if x.startswith(("-fprofile-use", "-fprofile-generate", "-flto")): continue   # (no profile / LTO needed to read a format string)
        out.append(x)
    if not out: return None
    return out + ["-fsyntax-only", "-Wformat", "-Wformat-security"]


FMT_WARN = re.compile(r"warning:.*(\[-Wformat[^\]]*\]|format)")


def one_unit(entry, nice, env=None):
    cmd = syntax_command(entry["command"])
    if cmd is None: return entry["file"], None, ""
    if nice: cmd = ["nice", "-n", str(nice)] + cmd
    r = subprocess.run(cmd, cwd=entry.get("directory") or None, capture_output=True, text=True, env=env)
    text = r.stdout + r.stderr
    warns = [l for l in text.splitlines() if FMT_WARN.search(l)]
    return entry["file"], (r.returncode, warns), text


def cmd_wformat(a):
    import json
    try:
        db = json.load(open(a.db))
    except (OSError, ValueError) as e:
        print(f"wformat: cannot read {a.db}: {e}"); return 2
    rx = re.compile(a.filter) if a.filter else None
    units = [e for e in db if "command" in e and (rx is None or rx.search(e["file"]))]
    if a.limit: units = units[:a.limit]
    if not units:
        print("wformat: no unit selected (an empty gate is not a pass)"); return 2
    total, failed, broken = 0, 0, 0
    env = dict(os.environ, CPATH=a.cpath) if a.cpath else None
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        for f, res, text in ex.map(lambda e: one_unit(e, a.nice, env), units):
            if res is None: continue
            rc, warns = res
            total += len(warns)
            if warns:
                failed += 1
                for w in warns: print("wformat WARN " + w)
            if rc != 0 and not warns:
                broken += 1
                print(f"wformat: {f}: the syntax-only run failed (exit {rc}) before the check could finish: " + (text.strip().splitlines() or ["(no output)"])[-1][:200])
    print(f"wformat: {len(units)} units, {total} format warnings in {failed} units, {broken} units that did not compile")
    if broken: return 2 if not total else 1
    return 1 if total else 0

# ---------------------------------------------------------------- emu
FAULT = re.compile(r"FATAL|SIGSEGV|SIGABRT|SIGBUS|SIGILL|Segmentation fault|data abort|prefetch abort|undefined instruction|crash dump|\.psp2dmp")
IN_PLAY = re.compile(r"linked by a draw in play")
FIRST_USE = re.compile(r"first-use: stage \S+ (?:left|periodic)[^;]*; programs linked by draws (\d+)")
WARM = re.compile(r"stage warm-up \d+ \([\d.]+ ms, (\d+) links\)")


def emu_stage(name, rc, text):
    """-> (problems, invalid)"""
    problems, invalid = [], []
    if rc != 0: problems.append(f"exit code {rc}")
    faults = [l for l in text.splitlines() if FAULT.search(l)]
    if faults: problems.append(f"{len(faults)} fault line(s), first: {faults[0].strip()[:120]}")
    inplay = len(IN_PLAY.findall(text))
    if inplay: problems.append(f"{inplay} 'linked by a draw in play' line(s): a program was linked after play began")
    fu = [int(x) for x in FIRST_USE.findall(text)]
    if sum(fu): problems.append(f"first-use reports: {sum(fu)} programs linked by draws")
    if not fu and not WARM.search(text):
        invalid.append("no first-use report and no stage warm-up line: the run did not exercise the shader warm-up (nothing is proven)")
    return problems, invalid


def cmd_emu(a):
    if len(a.stage) < a.min_stages:
        print(f"emu: {len(a.stage)} stage(s) given, the gate needs {a.min_stages}"); return 2
    fails = invalids = 0
    for spec in a.stage:
        m = re.fullmatch(r"([^:]+):(-?\d+):(.+)", spec)
        if not m: print(f"emu: bad --stage {spec!r} (NAME:EXIT:LOG)"); return 2
        name, rc, log = m.group(1), int(m.group(2)), m.group(3)
        try: text = open(log, errors="replace").read()
        except OSError as e: print(f"emu {name}: cannot read {log}: {e}"); return 2
        problems, invalid = emu_stage(name, rc, text)
        for p in problems: print(f"emu {name} FAIL: {p}")
        for i in invalid: print(f"emu {name} INVALID: {i}")
        if not problems and not invalid: print(f"emu {name} ok: exit 0, 0 faults, 0 programs linked in play")
        fails += bool(problems); invalids += bool(invalid)
    return 1 if fails else 2 if invalids else 0

# ---------------------------------------------------------------- twin
HASH = re.compile(r"undrawn-lean: hash turn (\d+) ([0-9a-f]{16}) actors (\d+)")


def read_hashes(path):
    """-> {(segment, turn): hash}: a new segment starts whenever the turn counter goes back (a new stage)"""
    out, seg, last = {}, 0, -1
    for l in open(path, errors="replace"):
        m = HASH.search(l)
        if not m: continue
        t = int(m.group(1))
        if t <= last: seg += 1
        last = t
        out[(seg, t)] = m.group(2)
    return out


def first_diff(x, y):
    """the first key (in order) both have with different values; the number of keys both have"""
    keys = sorted(set(x) & set(y))
    for k in keys:
        if x[k] != y[k]: return k, len(keys)
    return None, len(keys)


def twin_verdict(plain, plain2, pgo, min_hashes=20):
    """-> (exit code, text)"""
    d_pp, n_pp = first_diff(plain, plain2)
    d_pg, n_pg = first_diff(plain, pgo)
    if n_pp < min_hashes or n_pg < min_hashes:
        return 2, f"INCONCLUSIVE: too few hashes in common (plain/plain2 {n_pp}, plain/pgo {n_pg}, need {min_hashes}): the run did not reach far enough or GXV_UNDRAWN_LEAN_HASH was off"
    if d_pg is None:
        return 0, f"PASS: pgo and plain agree on all {n_pg} common hashes" + (f" (plain and plain2 first differ at segment {d_pp[0]} turn {d_pp[1]}: the noise floor, not reached by the common part)" if d_pp else "")
    if d_pp is not None and d_pp <= d_pg:
        return 2, (f"DET INCONCLUSIVE: pgo leaves plain at segment {d_pg[0]} turn {d_pg[1]}, but plain leaves plain2 at segment {d_pp[0]} turn {d_pp[1]} (not later): "
                   "the emulator's own pacing changes the run, so this check cannot judge the PGO build; the console A/B must")
    return 1, f"FAIL: pgo leaves plain at segment {d_pg[0]} turn {d_pg[1]} while plain and plain2 " + ("still agree there" if d_pp else "agree on all %d common hashes" % n_pp)


def cmd_twin(a):
    try:
        h = [read_hashes(p) for p in (a.plain, a.plain2, a.pgo)]
    except OSError as e:
        print(f"twin: cannot read a log: {e}"); return 2
    rc, text = twin_verdict(*h, min_hashes=a.min_hashes)
    print("twin " + text)
    return rc

# ----------------------------------------------------------------
def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv and argv[0] == "stack":   # (stackgate31: tools/check_display_stack.py has the same arguments: handed over whole)
        return subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_display_stack.py")] + argv[1:]).returncode
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("buildid"); b.add_argument("--elf", required=True); b.add_argument("--info"); b.add_argument("--expect", required=True); b.add_argument("--release", action="store_true")
    w = sub.add_parser("wformat"); w.add_argument("--db", required=True); w.add_argument("--jobs", type=int, default=1); w.add_argument("--filter"); w.add_argument("--limit", type=int, default=0)
    w.add_argument("--nice", type=int, default=10); w.add_argument("--cpath", help="the build's case-shim folder, set as CPATH for every unit")
    e = sub.add_parser("emu"); e.add_argument("--stage", action="append", default=[]); e.add_argument("--min-stages", type=int, default=3)
    t = sub.add_parser("twin"); t.add_argument("--plain", required=True); t.add_argument("--plain2", required=True); t.add_argument("--pgo", required=True); t.add_argument("--min-hashes", type=int, default=20)
    sub.add_parser("stack", help="the display-queue callback thread's stack gate: tools/check_display_stack.py (its own arguments follow)")
    a = ap.parse_args(argv)
    return {"buildid": cmd_buildid, "wformat": cmd_wformat, "emu": cmd_emu, "twin": cmd_twin}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
