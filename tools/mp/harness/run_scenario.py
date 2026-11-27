#!/usr/bin/env python3
"""Runs harness scenarios (YAML) and judges them with assert_log.py.

  run_scenario.py scenarios/a_boot_observatory.yaml [more.yaml ...] [-o DIR] [--dry-run] [--judge-only FOLDER]

Scenario file:
  name: boot_observatory            # folder name prefix
  description: ...
  seconds: 150                      # default run length (game seconds under PETARI_GAME_CLOCK=1)
  env: {KEY: value}                 # every game process
  link: true                        # automatic SMG_LINK_* per process (run_pair.sh); default true
  link: {profile: bad_wifi, ...}    # a mapping = bad-network relay between the consoles (link_relay.py; keys in its --help); also
                                    #   `link: bad_wifi` (profile name). --link PROFILE / HARNESS_LINK_PROFILE does it for any scenario.
  requires:                         # skip (not fail) when the game binary lacks one of these strings (features not merged yet)
    strings: [SMG_MP_AUTOHOST]
    files: [/path/to/mp_bot]        # ... or one of these files is missing
  processes:                        # game processes, in order (index -> link port)
    - name: host
      input: "@file1_observatory"   # PETARI_INPUT; "@name" = inputs/name.txt
      env: {KEY: value}
      delay: 0                      # seconds after the others
      seconds: 150
  bots:                             # optional extra processes (mp_bot)
    - name: bot1
      cmd: "/path/to/mp_bot --flag"
  rules: [ "expect host /.../ within 60s", ... ]   # assert_log.py rules

Exit 0 when every scenario that ran passed (skipped ones do not fail), 1 otherwise.
"""
import argparse, datetime, json, os, shlex, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))


def sh_default(var, default):
    return os.environ.get(var, default)


def requires_ok(sc, binary):
    req = (sc.get('requires') or {}).get('strings') or []
    files = (sc.get('requires') or {}).get('files') or []
    gone = [f for f in (x.replace('{HARNESS}', HERE) for x in files) if not os.path.exists(f)]   # ({HARNESS} = this folder, like in a bot's cmd)
    if gone:
        return False, 'missing ' + ', '.join(gone)
    if not req:
        return True, ''
    if not os.path.exists(binary):
        return False, f'no binary {binary}'
    data = open(binary, 'rb').read()
    missing = [s for s in req if s.encode() not in data]
    return (not missing), ('binary lacks ' + ', '.join(missing)) if missing else ''


def under_lock():
    """True when an ancestor already holds the game-run lock (someone ran gamerun.sh run_scenario.py ...): taking it
    again from here would wait for ourselves forever."""
    pid = os.getppid()
    for _ in range(32):
        if pid <= 1:
            return False
        try:
            if b'game-run.lock' in open(f'/proc/{pid}/cmdline', 'rb').read():
                return True
            st = open(f'/proc/{pid}/stat').read()
            pid = int(st[st.rfind(')') + 2:].split()[1])
        except (OSError, ValueError, IndexError):
            return False
    return False


def build_cmd(sc, out_parent, folder):
    cmd = [os.path.join(HERE, 'run_pair.sh'), '-n', folder, '-o', out_parent, '-s', str(sc.get('seconds', 120))]
    lk = sc.get('link', True)
    if lk is False:
        cmd.append('-L')
    elif os.environ.get('HARNESS_LINK_PROFILE'):
        pass   # (--link / the environment wins over the scenario's own block; run_pair.sh reads it)
    elif isinstance(lk, dict):
        cmd += ['-l', json.dumps(lk)]
    elif isinstance(lk, str):
        cmd += ['-l', lk]
    if sc.get('gdb'):
        cmd.append('-g')
    for k, v in (sc.get('env') or {}).items():
        cmd += ['-e', f'{k}={v}']
    if sc.get('timeout'):   # hard wall-clock limit per process (a slow scenario, e.g. four processes on two cores)
        cmd += ['-t', str(sc['timeout'])]
    if sc.get('shot_size'):   # e.g. 960x544: captures at the console's size (GXV_CAP_PRESENT); HARNESS_SHOT_SIZE does the same for every scenario
        cmd += ['-e', f"GXV_CAP_PRESENT={sc['shot_size']}"]
    for p in sc['processes']:
        spec = f"{p['name']}::{p.get('delay', 0)}:{p.get('seconds', sc.get('seconds', 120))}"
        cmd += ['-p', spec]
        env = dict(p.get('env') or {})
        if p.get('input'):
            env['PETARI_INPUT'] = p['input']
        for k, v in env.items():
            cmd += ['-P', f"{p['name']}={k}={v}"]
    for b in sc.get('bots') or []:
        cmd += ['-b', f"{b['name']} {b['cmd'].replace('{HARNESS}', HERE)}"]   # ({HARNESS} = this folder)
    return cmd


def judge(sc_path, folder, sc=None):
    cmd = [sys.executable, os.path.join(HERE, 'assert_log.py'), folder, '--scenario', sc_path, '--json', os.path.join(folder, 'verdict.json')]
    if sc and sc.get('min_speed') is not None:   # a scenario with more processes than cores says how slow the game may run (default 0.6)
        cmd += ['--min-speed', str(sc['min_speed'])]
    return subprocess.call(cmd)


def load_scenario(path):
    """yaml.safe_load, but a bare `on:` / `off:` / `yes:` / `no:` KEY (YAML 1.1 reads them as booleans) is turned back into its word:
    link events are written {on: "host /RE/", ...}; as True the event never fired and the scenario passed on a clean link."""
    import yaml

    def fix(v):
        if isinstance(v, dict):
            return {({True: 'on', False: 'off'}.get(k, k) if isinstance(k, bool) else k): fix(x) for k, x in v.items()}
        if isinstance(v, list):
            return [fix(x) for x in v]
        return v
    with open(path, encoding='utf-8') as f:
        return fix(yaml.safe_load(f))


def main():
    import yaml
    ap = argparse.ArgumentParser()
    ap.add_argument('scenarios', nargs='+')
    ap.add_argument('-o', '--out', default=os.path.join(sh_default('HARNESS_OUT', '/home/claude/g3/hruns'),
                                                        datetime.date.today().strftime('%Y%m%d')))
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--shots544', action='store_true', help='captures at the console\'s 960x544 (sets HARNESS_SHOT_SIZE=960x544)')
    ap.add_argument('--link', metavar='PROFILE', help='run every scenario through the bad-network relay (profile name, file or JSON; HARNESS_LINK_PROFILE); '
                    'a scenario\'s own link: block is replaced')
    ap.add_argument('--judge-only', help='judge an existing pair folder with the (single) scenario\'s rules')
    a = ap.parse_args()
    if a.link:
        os.environ['HARNESS_LINK_PROFILE'] = a.link
    if a.shots544:
        os.environ['HARNESS_SHOT_SIZE'] = '960x544'
    binary = sh_default('HARNESS_BIN', '/home/claude/g3/desk/build/game')
    if a.judge_only:
        sc1 = load_scenario(a.scenarios[0])
        return preflight_verdict(sc1, a.judge_only, judge(a.scenarios[0], a.judge_only, sc1))
    rows, bad = [], 0
    for path in a.scenarios:
        sc = load_scenario(path)
        name = sc.get('name') or os.path.splitext(os.path.basename(path))[0]
        ok, why = requires_ok(sc, binary)
        if not ok:
            rows.append((name, 'SKIP', why, ''))
            print(f'run_scenario {name}: SKIP ({why})')
            continue
        folder = f"{name}-{datetime.datetime.now().strftime('%H%M%S')}"
        cmd = build_cmd(sc, a.out, folder)
        print('run_scenario:', ' '.join(shlex.quote(c) for c in cmd))
        if a.dry_run:
            rows.append((name, 'DRY', '', ''))
            continue
        t = time.time()
        os.makedirs(a.out, exist_ok=True)
        # one run at a time on the machine (7 GB, no swap): each pair under the machine-wide game-run lock, so scenarios
        # of several agents interleave instead of one sweep holding the lock for an hour
        lock = sh_default('HARNESS_GAMERUN', '/home/claude/g3/tools/gamerun.sh')
        if lock and os.path.exists(lock) and not under_lock():
            cmd = [lock] + cmd
        run_rc = subprocess.call(cmd)
        pair = os.path.join(a.out, folder)
        jrc = preflight_verdict(sc, pair, judge(path, pair, sc))
        verdict = 'PASS' if jrc == 0 else 'INVALID' if jrc == 3 else 'FAIL'
        bad += verdict != 'PASS'
        rows.append((name, verdict, f'{time.time() - t:.0f} s wall, runs {"ok" if run_rc == 0 else "NOT ok"}' + soft_note(pair), pair))
    print('\n== scenarios')
    for name, v, why, pair in rows:
        print(f'{v:5} {name:28} {why}  {pair}')
    return 1 if bad else 0


KNOWN_KEYS = {'name', 'description', 'seconds', 'env', 'link', 'requires', 'processes', 'bots', 'rules', 'gdb', 'timeout', 'shot_size', 'min_speed'}


def _jload(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


import re   # (preflight, below)
DIRECT_JOIN_RE = re.compile(r'\[mp\] (?:hosting session=\S+ conn=|join start conn=|joined .* conn=)(direct|online)\b')


def unimpaired_paths(sc, folder):
    """Scenario / run uses a path that does NOT go through link_relay.py: the direct (play by address) UDP socket (np_udp_posix,
    SMG_MP_DIRECT_PORT, join_addr:) and the online path (STUN / TURN / service). Under a bad-network profile those would run on a clean link."""
    why = []
    for p in sc.get('processes') or []:
        env = dict(sc.get('env') or {}); env.update(p.get('env') or {})
        env = {k: str(v) for k, v in env.items()}
        if env.get('SMG_MP_AUTO_CONN') in ('direct', 'online') or 'SMG_MP_DIRECT_PORT' in env or env.get('SMG_MP_AUTO', '').startswith('join_addr'):
            why.append(f"{p['name']} sets the direct/online path in its env")
        elif env.get('SMG_MP_STUN') not in (None, '', '0') or 'SMG_MP_SERVICE' in env or 'SMG_MP_ONLINE' in env:
            why.append(f"{p['name']} enables STUN / the online service in its env")
        try:
            with open(os.path.join(folder, p['name'], 'game.log'), encoding='utf-8', errors='replace') as f:
                m = next((DIRECT_JOIN_RE.search(l) for l in f if DIRECT_JOIN_RE.search(l)), None)
        except OSError:
            m = None
        if m:
            why.append(f"{p['name']} logged a {m.group(1)} connection")
    return why


def preflight(sc, folder):
    """Reasons the RUN cannot count as a PASS whatever its rules say (-> INVALID). The judge only reads logs; these look at the run itself."""
    why = []
    rules = sc.get('rules')
    if not isinstance(rules, list) or not [r for r in rules if isinstance(r, str) and not r.strip().startswith(('#', '?'))]:
        why.append('scenario has no hard rules (a mistyped `rules:` key judges nothing and passes)')
    for k in sc:
        if k not in KNOWN_KEYS:
            why.append(f'unknown scenario key {k!r} (typo? its content is ignored)')
    pair = _jload(os.path.join(folder, 'pair.json')) or {}
    for p in sc.get('processes') or []:
        r = _jload(os.path.join(folder, p['name'], 'result.json'))
        if r is None:
            why.append(f"declared process {p['name']} has no result.json (it never ran or the supervisor died): `*` rules silently skip it")
            continue
        secs = float(p.get('seconds', sc.get('seconds', 120)))
        if r.get('exit_code') == 0 and not r.get('timed_out') and (r.get('wall_s') or 0) < 0.8 * secs and not p.get('may_end_early'):
            why.append(f"{p['name']} exited 0 after {r.get('wall_s')} s of a {secs:g} s run (an early clean exit; may_end_early: true if intended)")
    # a requested bad-network profile must have been APPLIED and carried traffic on every console pair, both directions
    want = os.environ.get('HARNESS_LINK_PROFILE') or (sc.get('link') if sc.get('link') not in (True, None, False) else None)
    if want:
        if sc.get('link') is False:
            why.append(f'link profile {want!r} requested but the scenario says `link: false` (no relay: the run was a clean link)')
        else:
            st = _jload(os.path.join(folder, 'link-stats.json'))
            names = [p['name'] for p in sc.get('processes') or []] + [b['name'] for b in sc.get('bots') or []]
            if st is None:
                why.append('link profile requested but no link-stats.json (the relay did not run or died before writing it)')
            else:
                paths = st.get('paths', {})
                for a in names:
                    for b in names:
                        if a != b and not (paths.get(f'{a}>{b}') or {}).get('out'):
                            why.append(f'relay carried no packet {a}>{b}')
                if st.get('stray'):
                    why.append(f"{st['stray']} stray packet(s) reached the relay from outside the consoles' own ports (traffic that bypassed it?)")
                wall = pair.get('wall_s')
                if wall and (st.get('runtime_s') or 0) < wall - max(3.0, 0.1 * wall):
                    why.append(f"relay ran {st.get('runtime_s')} s of a {wall} s run (it died mid-run)")
        up = unimpaired_paths(sc, folder)
        if up:
            why.append('path not impaired: a bad-network profile was requested but ' + '; '.join(up[:3]) + ' (that socket bypasses link_relay.py)')
    if os.environ.get('HARNESS_VITA_LIMITS') == '1':
        for p in sc.get('processes') or []:
            sj = _jload(os.path.join(folder, p['name'], 'summary.json')) or {}
            ml = sj.get('mem_line') or ''
            if not ml or 'IGNORED' in ml or 'banks not logged' in ml:
                why.append(f"{p['name']}: HARNESS_VITA_LIMITS=1 but the limits were not applied ({ml[:70] or 'no mem line'})")
    return why


def soft_note(folder):
    """', N SOFT-FAILED' when `?` rules failed in this run (they never fail it, so the summary table must show them)."""
    v = _jload(os.path.join(folder, 'verdict.json')) or {}
    return f", {v['warned']} SOFT-FAILED" if v.get('warned') else ''


def preflight_verdict(sc, folder, rc):
    """rc from the judge (0 pass, 1 fail, 2 bad rule, 3 invalid) -> the run's rc: a pass on a run that was not what it claims is 3 (INVALID)."""
    if not sc:
        return rc
    why = preflight(sc, folder)
    for w in why:
        print('run_scenario: INVALID:', w)
    if why:
        try:
            with open(os.path.join(folder, 'preflight.json'), 'w') as f:
                json.dump(why, f, indent=1)
        except OSError:
            pass
    return 3 if (why and rc in (0, 3)) else rc   # (a failing rule stays FAIL)


if __name__ == '__main__':
    sys.exit(main())
