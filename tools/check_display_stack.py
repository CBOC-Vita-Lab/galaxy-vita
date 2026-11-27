#!/usr/bin/env python3
"""check_display_stack.py: the display-queue callback thread's stack gate (stackgate31).

Why: sceGxm's display queue runs vitaGL's display callback (vglSetDisplayCallback: PaceDisplay or SyncDisplay in platform/vgx/vita/vgx_host_galaxy_vita.cpp) on a
SYSTEM thread whose stack is 4 KiB. Twice a branch put a few hundred bytes of buffers into that call chain and the console died with a stack overflow in newlib's
__ssputs_r / memmove (c116 smoke: PATCH2; 18 Nov console1 build 94dfcc77: q30cush31's tail[320] + pose[200] + line[1024] in PaceDisplay, 2160 bytes, then logToFile's own
1100-byte line and vsnprintf below it). Nothing in the build noticed. This gate does.

What it checks (clang -fstack-usage, the build's own Vita flags; static frames only):
  1. every listed function on that thread has a static frame within its own budget (a dynamic frame, alloca / VLA, is a failure: the budget cannot be known);
  2. the worst CHAIN (the callback, what it calls, the log path) summed over our code stays within OUR_BUDGET (2560 bytes);
  3. that sum plus newlib's printf path (NEWLIB_PRINTF, 800: vsnprintf 32 + _vsnprintf_r 12 + _svfprintf_r 312 + _dtoa_r 112 + __pow5mult 24 + __multiply 56 + _Balloc / malloc,
     read off the linked eboot's disassembly) plus SYS_RESERVE (512: display_queue_callback's own frame and the kernel's entry) stays within the 4096-byte stack.
MAINTENANCE (fable31fix): the tables are a HAND-WRITTEN list: any new function called from the display callbacks (PaceDisplay, SyncDisplay, EvenShown, SmoothShown, PaceGate,
LatencyBlock, LatencyShown, or anything they call) must be added to FUNCS and CHAINS here, or its frame is not counted. A missing function (rename) is exit 2; a missing ADDITION is not
seen by this tool: tools/check_display_callees.py lists the calls in those bodies that are not in the tables (best effort), and tools/test_stackgate31.sh runs it.
A chain function that is missing from the .su files is an error (renamed or removed: the table below must follow the code) unless it is marked optional (it may be inlined).

Usage:
  check_display_stack.py --root <repo> --build-dir <build>      compile the listed units with -fstack-usage (the real command of each unit from
                                                                <build>/vita-vgx-runtime/build.ninja, /source/source -> <repo>, CPATH <build>/case-shim) into a
                                                                temp dir; the objects are deleted with it. Same shape as release_gates.py wformat.
  check_display_stack.py --su-dir DIR                           read the *.su files in DIR instead (host tests, a build made with -fstack-usage)
  options: --our-budget N (2560)  --hard-only (just chain + newlib + system <= 4096, no margin policy)  --jobs N (default 1)  --nice N (default 10)  --keep DIR (keep the .su files there)  --quiet
Exit: 0 pass, 1 a budget is breached, 2 INCONCLUSIVE (a unit did not compile, a listed function is missing, no input): not a pass.
Pure Python 3 + clang + c++filt (or llvm-cxxfilt); no network, no game build, no console.
"""
import argparse, concurrent.futures, os, re, shlex, shutil, subprocess, sys, tempfile

STACK = 4096          # the display queue's callback thread (vitaGL source/gxm.c display_queue_callback runs on the system's 4 KiB stack)
OUR_BUDGET = 2560     # our code, summed over the worst chain (2.5 KiB)
NEWLIB_PRINTF = 800   # vsnprintf's worst frames in newlib (the disassembly of the linked eboot: 796)
SYS_RESERVE = 512     # display_queue_callback (vitaGL) and the kernel's thread entry

# The units that hold the display-thread functions (relative to the repo root; all in vita-vgx-runtime/build.ninja).
UNITS = [
    "platform/vgx/vita/vgx_host_galaxy_vita.cpp",   # PaceDisplay, SyncDisplay, EvenShown, PaceGate, SmoothShown, LatencyBlock, the formatters inlined from the port/*.h models
    "platform/src/os/os_alarm.cpp",                  # port_log_summary_cb, logToFileBuf: the log path
    "platform/src/port/perf.cpp",                    # port_pace_even_info, port_pose_phase_window_take, port_q30lock_shown, port_pace_steady30_*
    "platform/src/gx/vgx_galaxy.cpp",                # port_vgx_fif_retire, port_vgx_even_take, port_vgx_gate_wait_take, port_vgx_pace_*, port_hrw_still
]

# id -> (demangled-name regex, per-function budget in bytes, optional[, fallback regex used only when the first matches nothing: the code before stackgate31])
FUNCS = {   # (MAINTENANCE: add every new function called from the display callbacks here and in CHAINS; tools/check_display_callees.py lists candidates)
    "PaceDisplay":      (r"(^|::)PaceDisplay\(void\*\)$", 448, False),
    "SyncDisplay":      (r"(^|::)SyncDisplay\(void\*\)$", 768, False),
    "SyncInit":         (r"SyncDisplay\(void\*\)::\$_\d+::operator\(\)", 192, True),
    "EvenShown":        (r"(^|::)EvenShown\(", 768, False),
    "SmoothShown":      (r"(^|::)SmoothShown\(", 256, False),
    "PaceGate":         (r"(^|::)PaceGate\(", 192, False),
    "LatencyBlock":     (r"(^|::)LatencyBlock\([^)]*\)$", 256, True),
    "LatencyShown":     (r"(^|::)LatencyShown\(", 192, True),
    "SepFormat":        (r"SepWindow::Format\(", 384, False),
    "PaceMetricFormat": (r"PaceMetric::Format\(", 256, False),
    "EvenFormat":       (r"EvenMeter::Format\(", 256, True),
    "EvenCompute":      (r"EvenMeter::Compute\(", 128, True),
    "SmoothFormat":     (r"SmoothCensus::Format\(", 256, True),
    "LogCb":            (r"^port_log_summary_cb$", 64, False, r"^port_log_summary$"),   # (fallback: before stackgate31 the callback called port_log_summary itself)
    "LogBuf":           (r"^logToFileBuf\(", 320, False, r"^logToFile\("),           # (fallback: the version with its own 1100-byte line, 1304 bytes: over budget on purpose)
    "LogPeriodic":      (r"^logPeriodicDropped\(", 96, True),                        # (fable31fix: logToFileBuf's filter call: 40 bytes, may be inlined)
    "LogPut":           (r"^logToFileBuf\(.*::operator\(\)", 96, True),             # (fable31fix: the put / emit lambdas inside logToFileBuf: 40 bytes)
    "EvenInfo":         (r"^port_pace_even_info$", 128, False),
    "PoseTake":         (r"^port_pose_phase_window_take$", 96, False),
    "Q30Shown":         (r"^port_q30lock_shown$", 128, False),
    "FifRetire":        (r"^port_vgx_fif_retire$", 96, False),
    "EvenTake":         (r"^port_vgx_even_take$", 96, True),
    "GateWaitTake":     (r"^port_vgx_gate_wait_take$", 96, True),
}

# chains: callback -> ... -> leaf. 'libc' = the leaf calls newlib's printf family (snprintf / vsnprintf), so NEWLIB_PRINTF is added.
LOG = ["LogCb", "LogBuf", "LogPut", "LogPeriodic"]
CHAINS = {
    "Pace > pace line":              (["PaceDisplay"] + LOG, True),
    "Pace > q30sep line":            (["PaceDisplay", "SepFormat"], True),           # std::snprintf into the static buffers
    "Pace > PaceMetric::Format":     (["PaceDisplay", "PaceMetricFormat"], True),
    "Pace > EvenShown > log":        (["PaceDisplay", "EvenShown"] + LOG, True),
    "Pace > EvenShown > even info":  (["PaceDisplay", "EvenShown", "EvenInfo"], True),
    "Pace > SmoothShown > log":      (["PaceDisplay", "SmoothShown"] + LOG, True),
    "Pace > PaceGate > log":         (["PaceDisplay", "PaceGate"] + LOG, True),
    "Pace > pose take":              (["PaceDisplay", "PoseTake"], False),
    "Pace > q30lock shown":          (["PaceDisplay", "Q30Shown"], False),
    "Sync > present line":           (["SyncDisplay"] + LOG, True),
    "Sync > init lambda > log":      (["SyncDisplay", "SyncInit"] + LOG, True),
    "Sync > EvenShown > log":        (["SyncDisplay", "EvenShown"] + LOG, True),
    "Sync > EvenShown > even info":  (["SyncDisplay", "EvenShown", "EvenInfo"], True),
    "Sync > SmoothShown > log":      (["SyncDisplay", "SmoothShown"] + LOG, True),
    "Sync > PaceGate > log":         (["SyncDisplay", "PaceGate"] + LOG, True),
    "Sync > LatencyBlock > log":     (["SyncDisplay", "LatencyBlock"] + LOG, True),
    "Sync > LatencyShown":           (["SyncDisplay", "LatencyShown"], False),
    "Sync > fif retire":             (["SyncDisplay", "FifRetire"], False),
}

# Other small stacks of the game (not on this thread; listed so the table is complete and a smaller one is noticed): thread name -> bytes, from the sources.
OTHER_STACKS = {"smg_freezebutton (vita/vgx-game/entry.cpp)": 0x2000}   # the only game-created thread under 16 KiB (the rest: 16 KiB and up)


def demangle(names):
    tool = shutil.which("c++filt") or shutil.which("llvm-cxxfilt")
    if not tool:
        return list(names)
    r = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True, text=True)
    out = r.stdout.splitlines()
    return out if len(out) == len(names) else list(names)


def parse_su(paths):
    """[(demangled name, bytes, kind)] from clang -fstack-usage files: 'file:line:name<TAB>bytes<TAB>static|dynamic[,bounded]'"""
    rows = []
    for p in paths:
        try:
            lines = open(p, errors="replace").read().splitlines()
        except OSError:
            continue
        for l in lines:
            f = l.split("\t")
            if len(f) < 3 or not f[1].isdigit():
                continue
            head = f[0]
            m = re.match(r"^(.*?):(\d+):(?:(\d+):)?(.*)$", head)   # the file name may itself hold ':' only on Windows: not here
            name = m.group(4) if m else head
            rows.append((name, int(f[1]), f[2]))
    names = demangle([r[0] for r in rows])
    return [(n, r[1], r[2]) for n, r in zip(names, rows)]


def evaluate(rows, our_budget=OUR_BUDGET, hard_only=False):
    """(per-function results, chain results, problems). results: id -> (bytes or None, kind)."""
    problems, found = [], {}
    for fid, spec in FUNCS.items():
        rx, budget, optional = spec[:3]
        hit = [(b, k) for (n, b, k) in rows if re.search(rx, n)]
        if not hit and len(spec) > 3:
            hit = [(b, k) for (n, b, k) in rows if re.search(spec[3], n)]
        if not hit:
            found[fid] = None
            if not optional:
                problems.append(("missing", f"{fid}: no frame in the .su files for /{rx}/ (renamed or removed? this table follows the code: tools/check_display_stack.py FUNCS)"))
            continue
        b, k = max(hit)
        found[fid] = (b, k)
    fails = []
    for fid, v in found.items():
        if v is None: continue
        b, k = v
        budget = FUNCS[fid][1]
        if hard_only:
            continue
        if not k.startswith("static"):
            fails.append(f"{fid}: DYNAMIC frame ({k}, at least {b} bytes): a VLA / alloca on the 4 KiB display thread has no budget")
        elif b > budget:
            fails.append(f"{fid}: frame {b} bytes > budget {budget}")
    chains = {}
    for cname, (ids, libc) in CHAINS.items():
        total = 0
        for i in ids:
            v = found.get(i)
            if v: total += v[0]
        full = total + (NEWLIB_PRINTF if libc else 0) + SYS_RESERVE
        chains[cname] = (total, full)
        if total > our_budget and not hard_only:
            fails.append(f"chain '{cname}': our frames sum to {total} bytes > {our_budget}")
        if full > STACK:
            fails.append(f"chain '{cname}': {total} + newlib {NEWLIB_PRINTF if libc else 0} + system {SYS_RESERVE} = {full} bytes > the {STACK}-byte stack")
    return found, chains, problems, fails


def report(found, chains, quiet=False):
    lines = []
    lines.append("per function (static frame / budget):")
    for fid, spec in FUNCS.items():
        rx, budget, optional = spec[:3]
        v = found.get(fid)
        lines.append(f"  {fid:18s} " + (f"{v[0]:5d} / {budget:<5d} {v[1]}" if v else "  (not in the .su: inlined)" if optional else "  MISSING"))
    lines.append(f"chains (ours / ours + newlib printf {NEWLIB_PRINTF} + system {SYS_RESERVE}; our budget {OUR_BUDGET}, the stack {STACK}):")
    worst = max(chains.items(), key=lambda kv: kv[1][1]) if chains else None
    for cname, (t, full) in chains.items():
        lines.append(f"  {cname:30s} {t:5d}  {full:5d}  margin {STACK - full}")
    if worst:
        lines.append(f"worst chain: '{worst[0]}': {worst[1][1]} of {STACK} bytes, margin {STACK - worst[1][1]}")
    return lines


def ninja_entries(build_dir):
    p = os.path.join(build_dir, "vita-vgx-runtime", "build.ninja")
    t = open(p).read()
    ents = {}
    for m in re.finditer(r"^build (\S+): cc (\S+)\n  cmd = (.*)\n  extra = (.*)$", t, re.M):
        ents[m.group(2)] = (m.group(3), m.group(4))
    return ents


def compile_unit(args):
    unit, ents, root, build_dir, tmp, nice = args
    key = next((k for k in ents if k.endswith("/" + unit)), None)
    if key is None:
        return unit, None, f"{unit}: not in {build_dir}/vita-vgx-runtime/build.ninja"
    cmd, extra = ents[key]
    prefix = key[: -len(unit) - 1]   # '/source/source'
    # the ninja file names the build tree it was made in (/build-c183-clean-final): map it onto this build dir
    s = (cmd + " " + extra).replace(prefix, root)
    m = re.search(r"/(\S+?)/vita-vgx-runtime/", open(os.path.join(build_dir, "vita-vgx-runtime", "build.ninja")).read())
    if m:
        s = s.replace("/" + m.group(1), build_dir)
    a = shlex.split(s)
    a = [x for x in a if not x.startswith("-fprofile-use")] if not any(os.path.exists(x.split("=", 1)[1]) for x in a if x.startswith("-fprofile-use=")) else a
    obj = os.path.join(tmp, unit.replace("/", "_") + ".o")
    a += ["-fstack-usage", "-c", os.path.join(root, unit), "-o", obj]
    env = dict(os.environ)
    shim = os.path.join(build_dir, "case-shim")
    if os.path.isdir(shim) and not env.get("CPATH"):
        env["CPATH"] = shim
    r = subprocess.run((["nice", "-n", str(nice)] if nice else []) + a, env=env, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(obj[:-2] + ".su"):
        return unit, None, f"{unit}: compile failed (exit {r.returncode}): {r.stderr[-400:]}"
    try: os.remove(obj)
    except OSError: pass
    return unit, obj[:-2] + ".su", ""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", help="the repository (default: this file's repo)")
    ap.add_argument("--build-dir")
    ap.add_argument("--su-dir")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--nice", type=int, default=10)
    ap.add_argument("--keep")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--our-budget", type=int, default=OUR_BUDGET, help="the summed budget of our frames per chain (default 2560)")
    ap.add_argument("--hard-only", action="store_true", help="only the hard limit: every chain + newlib + system must fit the 4096 bytes (no per-function or margin budgets)")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root or os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    tmp = None
    problems = []
    if a.su_dir:
        su = [os.path.join(a.su_dir, f) for f in sorted(os.listdir(a.su_dir)) if f.endswith(".su")]
        if not su:
            print(f"stack INCONCLUSIVE: no .su files in {a.su_dir}"); return 2
    elif a.build_dir:
        build_dir = os.path.abspath(a.build_dir)
        try:
            ents = ninja_entries(build_dir)
        except OSError as e:
            print(f"stack INCONCLUSIVE: cannot read the build's ninja file: {e}"); return 2
        tmp = a.keep or tempfile.mkdtemp(prefix="dispstack.")
        os.makedirs(tmp, exist_ok=True)
        jobs = [(u, ents, root, build_dir, tmp, a.nice) for u in UNITS]
        su = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
            for unit, path, err in ex.map(compile_unit, jobs):
                if err: problems.append(err)
                elif path: su.append(path)
    else:
        print("stack: give --build-dir (compile) or --su-dir (read)"); return 2
    try:
        rows = parse_su(su)
    finally:
        if tmp and not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    found, chains, miss, fails = evaluate(rows, a.our_budget, a.hard_only)
    problems += [m for _, m in miss]
    out = report(found, chains)
    if not a.quiet or fails or problems:
        print("\n".join(out))
    if fails:
        for f in fails: print("stack FAIL: " + f)
        print("stack FAIL: the display-queue callback thread has a 4 KiB stack: make the buffers static (thread-confined, as q30sepfix / PATCH2 did) or hand the work to another thread")
        return 1
    if problems:
        for p in problems: print("stack INCONCLUSIVE: " + p)
        return 2
    worst = max(chains.values(), key=lambda v: v[1])[1]
    print(f"stack ok: worst display-thread chain {worst} of {STACK} bytes (margin {STACK - worst})" + ("" if a.hard_only else "; every listed frame within its budget"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
