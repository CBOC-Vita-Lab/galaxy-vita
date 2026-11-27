#!/usr/bin/env python3
"""Runs one desktop game process for the MP test harness and watches it.

  supervise.py --run-dir D --env-file E --timeout S [--gdb] -- <command...>

- stdout+stderr go to D/console.log, each line prefixed with the wall clock in epoch milliseconds ("<ms> <text>"),
  so lines of several processes can be merged (merge_timeline.py).
- every 0.5 s: RSS / peak RSS (VmHWM) / CPU time of the process (and its children, e.g. under gdb) -> D/proc.csv.
- a hard timeout: SIGTERM to the process group (the game stops cleanly on SIGTERM and flushes), SIGKILL 10 s later.
- D/result.json: start/end ms, wall seconds, exit code or signal, timed_out, peak RSS, CPU seconds, crash line.
Only the standard library: it runs next to two game processes on a 2-core box, so it sleeps most of the time.
"""
import argparse, json, os, re, signal, subprocess, sys, threading, time


def now_ms():
    return int(time.time() * 1000)


def read_env_file(path):
    env = {}
    if not path:
        return env
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.lstrip().startswith('#') or '=' not in line:
                continue
            k, v = line.split('=', 1)
            env[k.strip()] = v
    return env


def proc_tree(root):
    """pids of root and its descendants (gdb mode: the game is a child of gdb)."""
    kids = {}
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open(f'/proc/{d}/stat') as f:
                st = f.read()
            ppid = int(st[st.rfind(')') + 2:].split()[1])
            kids.setdefault(ppid, []).append(int(d))
        except (OSError, ValueError, IndexError):
            pass
    out, todo = [], [root]
    while todo:
        p = todo.pop()
        out.append(p)
        todo.extend(kids.get(p, []))
    return out


def sample(pid):
    """(rss_kb, hwm_kb, cpu_s, threads, vm_peak_kb) summed over the tree; None when gone."""
    tick = os.sysconf('SC_CLK_TCK')
    rss = hwm = thr = vmp = 0
    cpu = 0.0
    alive = False
    for p in proc_tree(pid):
        try:
            with open(f'/proc/{p}/status') as f:
                for line in f:
                    if line.startswith('VmRSS:'):
                        rss += int(line.split()[1])
                    elif line.startswith('VmHWM:'):
                        hwm += int(line.split()[1])
                    elif line.startswith('VmPeak:'):
                        vmp += int(line.split()[1])
                    elif line.startswith('Threads:'):
                        thr += int(line.split()[1])
            with open(f'/proc/{p}/stat') as f:
                st = f.read()
            fields = st[st.rfind(')') + 2:].split()
            cpu += (int(fields[11]) + int(fields[12])) / tick
            alive = True
        except (OSError, ValueError, IndexError):
            pass
    return (rss, hwm, cpu, thr, vmp) if alive else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run-dir', required=True)
    ap.add_argument('--env-file')
    ap.add_argument('--timeout', type=float, required=True)
    ap.add_argument('--name', default='proc')
    ap.add_argument('cmd', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == '--' else a.cmd
    if not cmd:
        ap.error('no command')
    env = dict(os.environ)
    env.update(read_env_file(a.env_file))
    con = open(os.path.join(a.run_dir, 'console.log'), 'w', encoding='utf-8', errors='replace')
    pcsv = open(os.path.join(a.run_dir, 'proc.csv'), 'w')
    pcsv.write('t_ms,rss_kb,hwm_kb,cpu_s,threads\n')
    start = now_ms()
    # Memory ceilings (Vita limits mode, see README): HARNESS_AS_MIB=<n> = RLIMIT_AS for the process (an allocation or mapping beyond it FAILS in
    # the process, like the console's heap running dry; the whole desktop process counts: Mesa, SDL and the two memory banks too, so pick it
    # from a measured peak_vm_kb, not from the console's figure); HARNESS_RSS_CEILING_MIB=<n> = the harness kills the process (SIGKILL,
    # "memory_ceiling_exceeded" in result.json, the run FAILS) when its resident size passes it.
    as_mib = int(env['HARNESS_AS_MIB']) if env.get('HARNESS_AS_MIB') else 0
    rss_ceiling_kb = int(env['HARNESS_RSS_CEILING_MIB']) * 1024 if env.get('HARNESS_RSS_CEILING_MIB') else 0

    def limits():
        os.setsid()
        if as_mib:
            import resource
            resource.setrlimit(resource.RLIMIT_AS, (as_mib << 20, as_mib << 20))
    p = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         preexec_fn=limits, cwd=a.run_dir)
    with open(os.path.join(a.run_dir, 'pid'), 'w') as f:
        f.write(f'{p.pid}\n')

    # HARNESS_KILL="<regex>|<delay s>|<signal>" (env file): <delay> seconds after the first output line matching <regex>,
    # send <signal> (default 9) to the process group: a crash / power cut / kill -9 at a chosen point of the session.
    kill_rx = kill_delay = kill_sig = None
    kill_spec = env.get('HARNESS_KILL')
    if kill_spec:
        parts = kill_spec.split('|')
        kill_rx = re.compile(parts[0])
        kill_delay = float(parts[1]) if len(parts) > 1 and parts[1] else 0.0
        kill_sig = int(parts[2]) if len(parts) > 2 and parts[2] else 9
    kill_at = [None]
    # SMG_MP_AUTO_GO_FILE=<path> (run_one.sh sets it for an autopilot process): create that file when the console is in play, i.e. the first
    # "[OS] port: player at <x y z>" line with a real (non-zero) position: after the file select, in the first scene. The game's autopilot
    # (mp_core.cpp mp_tick_game) opens its session only then. HARNESS_GO_RX overrides the line.
    go_path = env.get('SMG_MP_AUTO_GO_FILE')
    go_rx = re.compile(env.get('HARNESS_GO_RX') or r'\[OS\] port: player at (?!0\.0 0\.0 0\.0)-?\d')
    go_done = [False]
    # HARNESS_END="<regex>|<delay s>": <delay> s (wall) after the first output line matching <regex> the process is asked to stop (SIGTERM, the game
    # leaves its session with a BYE and exits 0): the run ends as soon as its last rule is decided instead of at the timeline's end.
    end_rx = end_delay = None
    if env.get('HARNESS_END'):
        ep = env['HARNESS_END'].split('|')
        end_rx = re.compile(ep[0]); end_delay = float(ep[1]) if len(ep) > 1 and ep[1] else 0.0
    end_at = [None]
    # HARNESS_MARKS=<marks.json> (run_one.sh, from an input file's `mark NAME /REGEX/` lines) + PETARI_INPUT_FILE=<input.live>: when the game prints a
    # line matching a mark's regex, "!NAME" is appended to the live script (atomic rename); the game's input thread (platform/desktop/main.cpp,
    # resolveInputMarks) reads it within 100 ms and runs the entries "NAME+<t>:..." <t> game seconds after that.
    mark_rx, mark_seen = {}, []
    live_path = env.get('PETARI_INPUT_FILE')
    if env.get('HARNESS_MARKS') and live_path and os.path.exists(env['HARNESS_MARKS']):
        mark_rx = {n: re.compile(r) for n, r in json.load(open(env['HARNESS_MARKS'])).items()}
        base_spec = open(live_path).read().strip()

    def feed_marks(text):
        hit = [n for n, rx in mark_rx.items() if n not in mark_seen and rx.search(text)]
        for n in hit:
            mark_seen.append(n)
            tmp = live_path + '.tmp'
            with open(tmp, 'w') as f:
                f.write(','.join([base_spec] + [f'!{x}' for x in mark_seen]) + '\n')
            os.replace(tmp, live_path)
            con.write(f'{now_ms()} harness: input mark {n} fed\n')
            con.flush()
    # HARNESS_STOP="<regex>|<delay s>|<silence s>": <delay> s after the first matching line the process group gets SIGSTOP and,
    # <silence> s later, SIGCONT: a console that goes silent (frozen, no packets, no heartbeat) for a while and comes back.
    stop_rx = stop_delay = stop_secs = None
    stop_spec = env.get('HARNESS_STOP')
    if stop_spec:
        sp = stop_spec.split('|')
        stop_rx = re.compile(sp[0])
        stop_delay = float(sp[1]) if len(sp) > 1 and sp[1] else 0.0
        stop_secs = float(sp[2]) if len(sp) > 2 and sp[2] else 2.0
    stop_at = [None]
    cont_at = [None]

    def pump():
        for raw in iter(p.stdout.readline, b''):
            text = raw.decode("utf-8", "replace").rstrip()
            con.write(f'{now_ms()} {text}\n')
            con.flush()
            if go_path and not go_done[0] and go_rx.search(text):
                go_done[0] = True
                open(go_path, 'w').close()
                con.write(f'{now_ms()} harness: autopilot go file created ({text[:60]})\n')
                con.flush()
            if mark_rx:
                feed_marks(text)
            if end_rx is not None and end_at[0] is None and end_rx.search(text):
                end_at[0] = time.time() + end_delay
            if kill_rx is not None and kill_at[0] is None and kill_rx.search(text):
                kill_at[0] = time.time() + kill_delay
            if stop_rx is not None and stop_at[0] is None and stop_rx.search(text):
                stop_at[0] = time.time() + stop_delay
    reader = threading.Thread(target=pump, daemon=True)
    reader.start()

    peak_rss = peak_hwm = peak_vm = 0
    ceiling_hit = False
    cpu = 0.0
    timed_out = False
    term_at = None
    deadline = time.time() + a.timeout
    while p.poll() is None:
        s = sample(p.pid)
        if s:
            rss, hwm, c, thr, vmp = s
            peak_rss, peak_hwm, cpu, peak_vm = max(peak_rss, rss), max(peak_hwm, hwm), max(cpu, c), max(peak_vm, vmp)
            if rss_ceiling_kb and rss > rss_ceiling_kb and not ceiling_hit:
                ceiling_hit = True
                con.write(f'{now_ms()} harness: memory ceiling exceeded: resident {rss // 1024} MiB > HARNESS_RSS_CEILING_MIB {rss_ceiling_kb // 1024}\n')
                con.flush()
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
            pcsv.write(f'{now_ms() - start},{rss},{hwm},{c:.2f},{thr}\n')
            pcsv.flush()
        t = time.time()
        if stop_at[0] is not None and stop_at[0] > 0 and t >= stop_at[0]:
            con.write(f'{now_ms()} harness: SIGSTOP for {stop_secs} s (HARNESS_STOP {stop_spec})\n')
            con.flush()
            try:
                os.killpg(p.pid, signal.SIGSTOP)
            except OSError:
                pass
            cont_at[0] = t + stop_secs
            stop_at[0] = -1.0
        if cont_at[0] is not None and t >= cont_at[0]:
            try:
                os.killpg(p.pid, signal.SIGCONT)
            except OSError:
                pass
            con.write(f'{now_ms()} harness: SIGCONT\n')
            con.flush()
            cont_at[0] = None
        if kill_at[0] is not None and t >= kill_at[0] and kill_at[0] > 0:
            con.write(f'{now_ms()} harness: kill signal={kill_sig} (HARNESS_KILL {kill_spec})\n')
            con.flush()
            try:
                os.killpg(p.pid, kill_sig)
            except OSError:
                pass
            kill_at[0] = -1.0
        if end_at[0] is not None and end_at[0] > 0 and t >= end_at[0] and term_at is None:
            con.write(f'{now_ms()} harness: end requested ({env.get("HARNESS_END")}): SIGTERM\n')
            con.flush()
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
            end_at[0] = -1.0
            term_at = t
        if t > deadline and term_at is None:
            timed_out = True
            term_at = t
            con.write(f'{now_ms()} harness: timeout after {a.timeout:.0f} s: SIGTERM\n')
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
        elif term_at is not None and t > term_at + 10:
            con.write(f'{now_ms()} harness: still running 10 s after SIGTERM: SIGKILL\n')
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            term_at = float('inf')
        time.sleep(0.5)
    end = now_ms()
    reader.join(timeout=5)
    # leftovers of the group (gdb children, a detached helper)
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except OSError:
        pass
    rc = p.returncode
    crash = None
    con.flush()
    with open(os.path.join(a.run_dir, 'console.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            if re.search(r'desktop crash signal=|received signal SIG(SEGV|BUS|ILL|FPE|ABRT|TRAP)|Program terminated with signal', line):
                crash = line.split(' ', 1)[1].strip()
                break
    res = {
        'name': a.name, 'pid': p.pid, 'cmd': cmd, 'start_ms': start, 'end_ms': end, 'wall_s': round((end - start) / 1000, 2),
        'exit_code': rc if rc is not None and rc >= 0 else None, 'signal': -rc if rc is not None and rc < 0 else None,
        'timed_out': timed_out, 'killed_by_harness': kill_at[0] == -1.0, 'peak_rss_kb': peak_rss, 'peak_hwm_kb': peak_hwm, 'peak_vm_kb': peak_vm, 'memory_ceiling_exceeded': ceiling_hit, 'as_limit_mib': as_mib or None, 'rss_ceiling_mib': rss_ceiling_kb // 1024 or None, 'cpu_s': round(cpu, 2), 'crash': crash,
    }
    with open(os.path.join(a.run_dir, 'result.json'), 'w') as f:
        json.dump(res, f, indent=1)
    con.close()
    pcsv.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
