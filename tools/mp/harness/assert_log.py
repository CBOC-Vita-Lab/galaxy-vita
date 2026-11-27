#!/usr/bin/env python3
"""Judges harness runs from their logs. Exit 0 when every (hard) rule passes, 1 when one fails, 2 on a bad rule.

  assert_log.py <pair-folder | run-folder...> [-r rules.txt] [-e 'rule']... [--scenario s.yaml] [--json out.json] [-q]

Rules (one per line, '#' starts a comment; a leading '?' makes a rule soft: reported as WARN, never fails the run):

  expect  P /RE/ [within T]                      a P line matches RE (within T of P's start)
  expect  P /RE/ [within T] after Q /RE2/        ... at or after the first Q line matching RE2 (and within T of it)
  absent  P /RE/                                 no P line matches RE
  absent  P /RE/ before Q /RE2/                  no P match earlier than Q's first RE2 match (e.g. "host left" before the
                                                 host process ended)
  count   P /RE/ OP N                            number of matching P lines (OP: == != >= <= > <)
  distinct P /RE/ OP N                           number of distinct values of RE's group 1 (e.g. player positions)
  order   P /RE/ before Q /RE2/ [within T] [each]
                                                 the first (each: every) P line matching RE is followed by a Q line
                                                 matching RE2 within T; $1..$9 in RE2 are the groups of the P match
                                                 (e.g. host flow move serial=(\\d+) before guest flow move serial=$1)
  order   P /RE/ near Q /RE2/ within T [each]    ... a Q match within T either side (earlier or later) of the P match:
                                                 two consoles doing the same thing at about the same time, either first
  same    P,Q /RE(groups)/ [first|last|each]     the groups of RE are EQUAL on every listed process (first match, last match (default), or every
                                                 match in order): compares captured values between consoles instead of a range
  order   P /RE/ before Q /RE2/ within T tol D   Q may be up to D earlier than P (two processes log independently; a few ms of ordering noise)
  absent  P /RE/ before shutdown                 nothing earlier than the shutdown window (the BYE of the process that stops first reaches the others
                                                 a few ms before its own last line); `absent ... before Q` also stops at the shutdown window
  Times: `30s` = GAME seconds (frames / 59.94) inside one process (from its start or from an anchor in the same process), `30ws` = wall seconds;
  between processes `s`, `ms`, `m` are wall (the old rules keep their meaning).
  GAME-TIME units (load independent, also between processes): `30gs` = game seconds, `1800f` = game frames (1 f = 1/59.94 s). Inside one
  process they measure the process's own frame counter. Between processes (`after Q`, `order ... within/tol`, `near`) they are read on the
  clock of the process that has to REACT: the P-side event's wall instant is mapped to that process's game time and the window is measured
  there (how many frames did the receiver run between the sender's event and its own). A game second is exact when the log carries the
  game's frame stamps (PETARI_LOG_FRAMES=1, run_one.sh sets it), interpolated between coarser frame points otherwise; a process with no
  frame points at all fails a game-unit rule ("no frame stamps") instead of silently using wall time.
  A game on a loaded machine runs slower than real time: its rules are about game time.
  exit    P OP N                                 exit status of P (a process killed by a signal fails every exit rule)
  vsbase  P NAME OP FACTOR                       NAME (p50_ms p95_ms p99_ms fps) of P's frametimes.json against the SAME MACHINE's single-player baseline
                                                 (--baseline FOLDER|frametimes.json, or HARNESS_BASELINE): `vsbase * p95_ms <= 1.5` = at most 1.5 x the
                                                 baseline's p95 game-frame time; `vsbase * fps >= 0.7`. No baseline given: the rule is reported SKIPPED
                                                 (passes, counted in the summary), so it never fails a run that has nothing to compare with.
  agree   P,Q roster|stars                       quiet-network invariant: when traffic is quiet (before the shutdown window) every console that was still
                                                 running agrees on the roster size / on the set of stars credited (see agree_values). run_scenario adds
                                                 `?agree * roster` and `?agree * stars` (soft) to every run of 2+ consoles unless HARNESS_AGREE=off;
                                                 HARNESS_AGREE=hard makes them hard.
  nocrash P                                      no "desktop crash signal=" / gdb signal line, no signal exit
  nostall P                                      no "progress (stall)" report (no game frame for PETARI_STALL_S s)
  mp_save P                                      P wrote a GameData.bin in its multiplayer root (<run>/mp)
  save_guard P                                   P's single-player GameData.bin sha256 is the same after the run as before
  save_files P present|absent NAME...            after the run, the folder of P's single-player GameData.bin has (present) / lacks (absent) every NAME
                                                 (GameData.bin, GameData.bin.mpbak, .mptmp, .mpbad, .mpbak.old ...; from save-after.json's folder list)
  save_valid P                                   P's single-player GameData.bin is whole and valid after the run (save_slots.py)
  save_mine P N                                  only file N's blocks differ from the session's backup in <run>/mp/backups
  shots   P OP N                                 number of screenshots
  metric  P NAME OP N                            NAME: wall_s, cpu_s, rss_mib (peak), lines (log lines),
                                                 p50_ms p95_ms p99_ms hitches fps (frametimes.json, PETARI_FRAMETIMES)

P / Q: a process (run folder) name, a comma list (host,guest), '*' = every process (each must pass) or 'any' = at least one.
T: 30s, 1.5s, 500ms, 2m. RE: Python regex between slashes (\\/ for a slash), optional i flag after the closing slash.
"""
import argparse, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hlog

CRASH_RE = re.compile(r'desktop crash signal=|received signal SIG(SEGV|BUS|ILL|FPE|ABRT|TRAP)|Program terminated with signal')
SHUTDOWN_LEAD_MS = 500   # how long before the first "desktop: terminal" / SIGTERM the BYE of mp_port_shutdown can already be on the wire
OPS = {'==': lambda a, b: a == b, '!=': lambda a, b: a != b, '>=': lambda a, b: a >= b, '<=': lambda a, b: a <= b,
       '>': lambda a, b: a > b, '<': lambda a, b: a < b}


class RuleError(Exception):
    pass


def tokenize(s):
    toks, i = [], 0
    while i < len(s):
        c = s[i]
        if c.isspace():
            i += 1
            continue
        if c == '/':
            j, buf = i + 1, []
            while j < len(s) and s[j] != '/':
                if s[j] == '\\' and j + 1 < len(s) and s[j + 1] == '/':
                    buf.append('/')
                    j += 2
                    continue
                buf.append(s[j])
                j += 1
            if j >= len(s):
                raise RuleError(f'unterminated regex in: {s}')
            j += 1
            flags = 0
            while j < len(s) and s[j] == 'i':
                flags |= re.I
                j += 1
            toks.append(('re', ''.join(buf), flags))
            i = j
            continue
        j = i
        while j < len(s) and not s[j].isspace():
            j += 1
        toks.append(('w', s[i:j]))
        i = j
    return toks


def parse_dur(w):
    """(seconds, kind): kind 'wall' (ws), 'game' (gs, f: always game time) or 'legacy' (s, ms, m: game time inside one process when the log has
    frame points, wall between processes)."""
    m = re.fullmatch(r'([0-9.]+)(ms|ws|gs|s|m|f)?', w)
    if not m:
        raise RuleError(f'bad time {w!r}')
    v, u = float(m[1]), m[2]
    if u == 'f':
        return v / FPS_, 'game'
    if u == 'gs':
        return v, 'game'
    if u == 'ws':
        return v, 'wall'
    return (v / 1000 if u == 'ms' else v * 60 if u == 'm' else v), 'legacy'


FPS_ = hlog.FPS


def parse_time(w):
    """Seconds. A suffix `ws` means WALL seconds (the machine's clock); every other unit (s, ms, m) is GAME time where a rule measures inside one
    process (expect ... within T: from the process start or from an anchor in the same process): the game's own frames / 59.94, so a loaded machine
    or a cold disk cache (the game runs slower than real time) cannot fail a rule that is about what the game did. Between processes (order,
    near, an anchor in another process) every unit is wall time. parse_time returns the number; parse_within also the wall flag."""
    return parse_within(w)[0]


def parse_within(w):
    v, kind = parse_dur(w)
    return v, kind == 'wall'


def elapsed_s(proc, t_from, t_to, wall):
    """Seconds between two wall times of one process: game time when the process logged frame stamps, else wall."""
    if not wall:
        a, b = proc.game_s(t_from), proc.game_s(t_to)
        if a is not None and b is not None:
            return b - a
    return (t_to - t_from) / 1000.0


class Ctx:
    def __init__(self, procs):
        self.procs = {p.name: p for p in procs}
        self.baseline = None   # frametimes.json of a single-player run on this machine (assert_log --baseline / HARNESS_BASELINE)
        self.t0 = min((p.start_ms for p in procs if p.start_ms), default=0)
        # SHUTDOWN WINDOW: the game ends itself at its run length (or the harness sends SIGTERM): the first process to do so sends BYE to the
        # others a few ms BEFORE it logs "desktop: terminal", and the others log "host-left" and the like. Everything from SHUTDOWN_LEAD before the
        # earliest "desktop: terminal" / "harness: timeout" line of any process on is the end of the run, not a fault of the session.
        ends = []
        for p in procs:
            for t, text, _ in p.events():
                if text.startswith('desktop: terminal') or text.startswith('harness: timeout after') or text.startswith('harness: end requested'):
                    ends.append(t)
                    break
        self.shutdown_ms = (min(ends) - SHUTDOWN_LEAD_MS) if ends else None

    def select(self, sel):
        if sel in ('*', 'any'):
            return list(self.procs.values())
        out = []
        for n in sel.split(','):
            if n not in self.procs:
                raise RuleError(f'no process {n!r} (have: {", ".join(self.procs)})')
            out.append(self.procs[n])
        return out

    def rel(self, t):
        return f'+{(t - self.t0) / 1000:.3f}s'


def subst(pattern, groups):
    def rep(m):
        k = int(m[1])
        return re.escape(groups[k - 1]) if k <= len(groups) and groups[k - 1] is not None else m[0]
    return re.sub(r'\$(\d)', rep, pattern)


def matches(proc, rx, t_from=None, t_to=None):
    for t, text, _ in proc.events():
        if t_from is not None and t < t_from:
            continue
        if t_to is not None and t > t_to:
            break
        m = rx.search(text)
        if m:
            yield t, text, m


WALL_CEIL_SPEED = float(os.environ.get('HARNESS_WALL_CEIL_SPEED', '0.25'))
# A game-unit window (gs / f: `within 8gs`, `within 8gs after host ...`, `order ... within 3gs tol 15f`, same console or between consoles) is read on a
# frame counter, so a console that stops making frames (a 30 s main-thread hang) would stretch it for ever. The wall ceiling: the window can never be
# longer than window / WALL_CEIL_SPEED wall seconds (4 x by default). Same for `tol`. The old units (s, ms, m, ws) are not touched. The game's own network
# timers run on the wall clock (mp_sys_now_ms = CLOCK_MONOTONIC), so rules about THEM (a 75 s reconnect) belong in `ws`.


def edge_ms(q, ta, secs, sign, kind):
    """Wall ms of the edge of a window around the wall instant ta: wall units are added as they are; a GAME-time window is measured on the
    clock of the process q that has to react (its own frame counter): ta is mapped to q's game time, the window added there, mapped back."""
    if kind != 'game' or not secs:
        return ta + sign * secs * 1000
    g = q.game_s(ta)
    if g is None:
        raise RuleError(f'{q.name} has no game-frame stamps: a game-time window (gs, f) cannot be judged')
    w = q.wall_ms(max(0.0, g + sign * secs))
    if WALL_CEIL_SPEED > 0:
        cap = ta + sign * secs / WALL_CEIL_SPEED * 1000
        w = min(w, cap) if sign > 0 else max(w, cap)
    return w


def short(text, n=140):
    return text if len(text) <= n else text[:n] + '...'


def check_one(verb, toks, proc, ctx):
    """One rule against one process; returns (ok, detail). toks: the rule tokens after 'verb P'."""
    def need_re(k):
        if k >= len(toks) or toks[k][0] != 're':
            raise RuleError(f'{verb}: regex expected')
        return re.compile(toks[k][1], toks[k][2])
    words = [t[1] if t[0] == 'w' else None for t in toks]
    r = proc.result
    if verb == 'expect':
        rx = need_re(0)
        within, wall, after, kind = None, False, None, 'legacy'
        k = 1
        while k < len(toks):
            if words[k] == 'within':
                within, kind = parse_dur(words[k + 1]); wall = kind == 'wall'; k += 2
            elif words[k] == 'after':
                after = (words[k + 1], toks[k + 2]); k += 3
            else:
                raise RuleError(f'expect: unexpected {toks[k]}')
        if kind == 'game' and proc.game_s(proc.start_ms or 0) is None:
            return False, f'{proc.name} has no game-frame stamps (PETARI_LOG_FRAMES=1 / frametimes.txt): a game-time window (gs, f) cannot be judged'
        def first_hit(t_from, anchor_proc):
            """first match at/after t_from within T of it (game time when the anchor is in this process, wall otherwise)"""
            same_clock = anchor_proc is proc or t_from is None
            for t, text, m in matches(proc, rx, t_from, None):
                if within is not None:
                    el = elapsed_s(proc, t_from if t_from is not None else (proc.start_ms or 0), t, wall or (not same_clock and kind != 'game'))
                    if el > within:
                        return None
                    t_ref = t_from if t_from is not None else (proc.start_ms or 0)
                    if kind == 'game' and WALL_CEIL_SPEED > 0 and (t - t_ref) / 1000.0 > within / WALL_CEIL_SPEED:
                        return None   # (wall ceiling: a console that made no frames does not stretch the window)
                return t, text, m
            return None
        if after:
            qsel, qre = after
            anchors = []
            for q in ctx.select(qsel):
                anchors += [(t, text, q) for t, text, _ in matches(q, re.compile(qre[1], qre[2]))][:1]
            if not anchors:
                return False, f'anchor /{qre[1]}/ never seen in {qsel}'
            ta, atext, aproc = min(anchors, key=lambda x: x[0])
            hit = first_hit(ta, aproc)
            if hit:
                return True, f'{ctx.rel(hit[0])} ({(hit[0] - ta) / 1000:.2f}s after {aproc.name} {ctx.rel(ta)}): {short(hit[1])}'
            return False, f'no /{rx.pattern}/ {"within %gs " % within if within else ""}after {aproc.name} {ctx.rel(ta)} "{short(atext, 80)}"'
        start = proc.start_ms or 0
        hit = first_hit(None, proc)
        if hit:
            gs = proc.game_s(hit[0])
            return True, f'{ctx.rel(hit[0])} ({(hit[0] - start) / 1000:.1f}s wall' + (f', {gs:.1f}s game' if gs is not None else '') + f' after start): {short(hit[1])}'
        return False, f'no line matches /{rx.pattern}/' + (f' within {within:g}s{" wall" if wall else " (game time)"} of start' if within else '')
    if verb in ('absent', 'count', 'distinct'):
        # VACUITY GUARD: a negative / upper-bound rule on a process that logged next to nothing proves nothing (an `absent` on a console whose
        # log is empty, truncated or never started passed). Real game runs log hundreds of lines; HARNESS_MIN_LINES=0 turns this off.
        n_game = sum(1 for _, _, src in proc.events() if src in ('game', 'console'))
        if n_game < int(os.environ.get('HARNESS_MIN_LINES', '40')):
            return False, f'{proc.name} logged only {n_game} lines: a rule that passes on "nothing happened" is vacuous here (HARNESS_MIN_LINES)'
    if verb in ('absent', 'count'):
        rx = need_re(0)
        hits = list(matches(proc, rx))
        if verb == 'absent' and len(toks) >= 2 and words[1] == 'before' and words[2] == 'shutdown':
            # absent P /RE/ before shutdown: nothing earlier than the shutdown window (see Ctx.shutdown_ms)
            limit = ctx.shutdown_ms
            early = [h for h in hits if limit is None or h[0] < limit]
            if not early:
                return True, f'none before the shutdown window ({ctx.rel(limit) if limit else "no end seen: whole run"}; {len(hits)} in it)'
            return False, f'{len(early)} line(s) before the shutdown window ({ctx.rel(limit) if limit else "whole run"}), first {ctx.rel(early[0][0])}: {short(early[0][1])}'
        if verb == 'absent' and len(toks) >= 4 and words[1] == 'before':
            # absent P /RE/ before Q /RE2/: no P match earlier than the first Q match (e.g. "host left" only after the
            # host's process ended); Q never matching = the whole run
            anchors = [t for q in ctx.select(words[2]) for t, _, _ in matches(q, re.compile(toks[3][1], toks[3][2]))]
            limit = min(anchors) if anchors else None
            if ctx.shutdown_ms is not None and (limit is None or ctx.shutdown_ms < limit):
                limit = ctx.shutdown_ms   # (the shutdown window: the BYE of the process that stops first reaches the others before its own last log line)
            early = [h for h in hits if limit is None or h[0] < limit]
            if not early:
                return True, f'none before {ctx.rel(limit) if limit else "the end"} ({len(hits)} after)'
            return False, f'{len(early)} line(s) before {ctx.rel(limit) if limit else "the end"}, first {ctx.rel(early[0][0])}: {short(early[0][1])}'
        if verb == 'absent':
            if not hits:
                return True, 'none'
            return False, f'{len(hits)} line(s), first {ctx.rel(hits[0][0])}: {short(hits[0][1])}'
        if len(words) < 3 or words[1] not in OPS:
            raise RuleError('count: P /RE/ OP N')
        ok = OPS[words[1]](len(hits), float(words[2]))
        return ok, f'{len(hits)} {words[1]} {words[2]}' + (f' (first {ctx.rel(hits[0][0])}: {short(hits[0][1], 90)})' if hits else '')
    if verb == 'distinct':
        rx = need_re(0)
        if len(words) < 3 or words[1] not in OPS:
            raise RuleError('distinct: P /RE(group)/ OP N')
        vals = {m.group(1) if m.groups() else m.group(0) for _, _, m in matches(proc, rx)}
        ok = OPS[words[1]](len(vals), float(words[2]))
        return ok, f'{len(vals)} distinct {words[1]} {words[2]}' + (f' (e.g. {", ".join(sorted(vals)[:3])})' if vals else '')
    if verb == 'order':
        rx = need_re(0)
        if words[1] not in ('before', 'near'):
            raise RuleError('order: P /RE/ before|near Q /RE2/ ...')
        near = words[1] == 'near'
        qsel, qre = words[2], toks[3]
        within, each, tol, wk, tk = None, False, 0.0, 'legacy', 'legacy'
        k = 4
        while k < len(toks):
            if words[k] == 'within':
                within, wk = parse_dur(words[k + 1]); k += 2
            elif words[k] == 'tol':   # two processes log independently: Q may be up to TOL EARLIER than P and still count as "after"
                tol, tk = parse_dur(words[k + 1]); k += 2
            elif words[k] == 'each':
                each = True; k += 1
            elif words[k] == 'first':   # (the default, spelled out) only the FIRST P match is judged; `each` judges every one
                k += 1
            else:
                raise RuleError(f'order: unexpected {toks[k]}')
        if near and within is None:
            raise RuleError('order ... near ... needs within T')
        anchors = list(matches(proc, rx))
        if not anchors:
            return False, f'no {proc.name} line matches /{rx.pattern}/'
        n_anchors = len(anchors)
        if not each:
            anchors = anchors[:1]
        bad, good = [], []
        for ta, atext, am in anchors:
            pat = re.compile(subst(qre[1], am.groups()), qre[2])
            found = None
            for q in ctx.select(qsel):
                if q is proc and qsel in ('*', 'any'):
                    continue
                before_s, before_k = (within, wk) if near else (tol, tk)
                try:
                    lo = edge_ms(q, ta, before_s, -1, before_k)
                    hi = edge_ms(q, ta, within, 1, wk) if within else None
                except RuleError as e:
                    bad.append(str(e))
                    continue
                h = next(matches(q, pat, lo, hi), None)
                if h and (found is None or h[0] < found[0]):
                    found = (h[0], q.name, h[1])
            if found:
                good.append(f'{ctx.rel(ta)} -> {found[1]} {ctx.rel(found[0])} ({(found[0] - ta) / 1000:+.2f}s)')
            else:
                bad.append(f'{ctx.rel(ta)} "{short(atext, 80)}" not {"near" if near else "followed by"} {qsel} /{pat.pattern}/' +
                           (f' within {within:g}s' if within else ''))
        if bad:
            return False, '; '.join(bad[:3]) + (f' (+{len(bad) - 3} more)' if len(bad) > 3 else '')
        return True, f'{len(good)} ok: ' + '; '.join(good[:3]) + (' ...' if len(good) > 3 else '') + \
            (f' [only the FIRST of {n_anchors} matches judged: add `each` for all]' if n_anchors > len(anchors) else '')
    if verb == 'same':
        # same P,Q /RE/ [first|last|each]: the groups of RE (all of them, as a tuple) are equal on every listed process: of the first match,
        # of the last match (default), or of every match in order (the sequences are equal, same length). A value compared between consoles
        # instead of a range: "both show the same total" cannot pass by both being in a window.
        raise RuleError('same is handled in run_rule')
    if verb == 'exit':
        if len(words) < 2 or words[0] not in OPS:
            raise RuleError('exit: P OP N')
        code = r.get('exit_code')
        if code is None:
            return False, f'no exit code (signal={r.get("signal")}, timeout={r.get("timed_out")})'
        return OPS[words[0]](code, int(words[1])), f'exit {code}' + (' (timed out)' if r.get('timed_out') else '')
    if verb == 'nocrash':
        hit = next(((t, text) for t, text, _ in proc.events() if CRASH_RE.search(text)), None)
        if hit:
            return False, f'{ctx.rel(hit[0])}: {short(hit[1])}' + (' (see crash.txt)' if os.path.exists(proc.file('crash.txt')) else '')
        if r.get('signal') and not r.get('killed_by_harness'):
            return False, f'killed by signal {r["signal"]}'
        if r.get('killed_by_harness'):
            return True, f'no crash (killed on purpose: signal {r.get("signal")})'
        return True, 'no crash'
    if verb == 'nostall':
        hit = next(((t, text) for t, text, _ in proc.events() if text.startswith('progress (stall)')), None)
        if hit:
            return False, f'{ctx.rel(hit[0])}: no game frame for PETARI_STALL_S seconds' + \
                (' (see stall.txt)' if os.path.exists(proc.file('stall.txt')) else '')
        return True, 'no stall report'
    if verb == 'save_guard':
        g = proc.json('save-guard.json')
        if not g:
            return False, 'no save-guard.json'
        if g['unchanged']:
            return True, f'single-player GameData.bin unchanged ({g["after"][:16]})'
        return False, f'single-player GameData.bin {"MISSING" if not g["after"] or not g["before"] else "CHANGED"} ' \
                      f'{(g["before"] or "-")[:16]} -> {(g["after"] or "-")[:16]}'
    if verb == 'save_files':
        if len(words) < 2 or words[0] not in ('present', 'absent'):
            raise RuleError('save_files: P present|absent NAME...')
        a = proc.json('save-after.json')
        if not a or 'folder' not in a:
            return False, 'no save-after.json folder list'
        have = a['folder']
        want = words[0] == 'present'
        bad = [n for n in words[1:] if (n in have) != want]
        listing = ', '.join(sorted(have)) or '(empty)'
        if bad:
            return False, f'{", ".join(bad)} {"missing" if want else "still there"}; folder has: {listing}'
        return True, f'{words[0]}: {" ".join(words[1:])} (folder: {listing})'
    if verb in ('save_valid', 'save_mine'):
        import save_slots
        return save_slots.check(proc, verb, words)
    if verb == 'mp_save':
        m = proc.json('mp-saves.json') or {'files': {}}
        games = [k for k in m['files'] if k.endswith('GameData.bin')]
        if games:
            return True, 'multiplayer root has ' + ', '.join(games[:3])
        return False, 'no GameData.bin under <run>/mp (mp-saves.json)'
    if verb == 'shots':
        d = proc.file('shots')
        n = len([f for f in os.listdir(d) if f.endswith('.png')]) if os.path.isdir(d) else 0
        if len(words) < 2 or words[0] not in OPS:
            raise RuleError('shots: P OP N')
        return OPS[words[0]](n, float(words[1])), f'{n} {words[0]} {words[1]}'
    if verb == 'vsbase':
        if len(words) < 3 or words[1] not in OPS or words[0] not in ('p50_ms', 'p95_ms', 'p99_ms', 'fps'):
            raise RuleError('vsbase: P p50_ms|p95_ms|p99_ms|fps OP FACTOR')
        base = ctx.baseline
        if not isinstance(base, dict) or not isinstance(base.get(words[0]), (int, float)) or isinstance(base.get(words[0]), bool):
            return True, 'SKIPPED (no usable single-player baseline: --baseline FOLDER / HARNESS_BASELINE)'
        try:
            factor = float(words[2])
        except ValueError:
            return True, f'SKIPPED (vsbase factor {words[2]!r} is not a number)'
        ft = proc.json('frametimes.json') or {}
        v = ft.get({'p50_ms': 'p50_ms', 'p95_ms': 'p95_ms', 'p99_ms': 'p99_ms', 'fps': 'fps'}[words[0]])
        if v is None:
            return False, f'{words[0]}: not measured (frametimes.json missing)'
        if not isinstance(v, (int, float)):
            return True, f'SKIPPED ({words[0]} in frametimes.json is not a number)'
        return OPS[words[1]](v, factor * base[words[0]]), f'{words[0]}={v:g} {words[1]} {factor:g} x baseline {base[words[0]]:g} ({factor * base[words[0]]:g})'
    if verb == 'metric':
        if len(words) < 3 or words[1] not in OPS:
            raise RuleError('metric: P NAME OP N')
        name = words[0]
        vals = {'wall_s': r.get('wall_s'), 'cpu_s': r.get('cpu_s'),
                'rss_mib': (r.get('peak_hwm_kb') or 0) / 1024, 'lines': len(proc.events())}
        ft = proc.json('frametimes.json') or {}
        vals.update({'p50_ms': ft.get('p50_ms'), 'p95_ms': ft.get('p95_ms'), 'p99_ms': ft.get('p99_ms'),
                     'hitches': ft.get('hitches_over_50_ms'), 'fps': ft.get('fps')})
        if name not in vals:
            raise RuleError(f'metric: unknown {name}')
        v = vals[name]
        if v is None:
            return False, f'{name}: not measured (frametimes.json missing: a build without PETARI_FRAMETIMES?)'
        return OPS[words[1]](v, float(words[2])), f'{name}={v:.1f} {words[1]} {words[2]}'
    raise RuleError(f'unknown rule {verb!r}')


ROSTER_EV = re.compile(r'^\[mp\] (?:roster: )?slot (\d) (joined|left)\b(.*)$')
STAR_RE = re.compile(r'^\[mp\] flow star got id=(\d+) ')
WELCOME_RE = re.compile(r'^\[mp\] joined slot=\d+ session=\S+ host=\d+ epoch=\d+ players=(\d+)')
PLAYERS_RE = re.compile(r'players=(\d+)')


def agree_values(kind, proc, limit):
    """What one console believes at the end of the quiet stretch (lines before `limit`, wall ms; None = the whole run).
    roster: the number of players. Host: the players= of its last `slot N joined|left` line (or `session started players=`). Guest: the players= of
    its last welcome (`joined slot=.. players=N`) plus the `roster: slot N joined|left` lines after it (applyRoster does not log the members a
    welcome carries, so the count is rebuilt). stars: the set of ids of `flow star got id=` lines. Returns None when the console logged nothing of
    the kind (it never had a session)."""
    ev = [(t, x) for t, x, _ in proc.events() if limit is None or t < limit]
    if kind == 'stars':
        got = {m[1] for _, x in ev for m in [STAR_RE.match(x)] if m}
        return sorted(got, key=int) if any('[mp]' in x for _, x in ev) else None
    count = None
    for _, x in ev:
        m = WELCOME_RE.match(x)
        if m:
            count = int(m[1]); continue
        m = re.match(r'^\[mp\] session started players=(\d+)', x)
        if m and count is None:
            count = int(m[1]); continue
        m = ROSTER_EV.match(x)
        if not m:
            continue
        host_line = not x.startswith('[mp] roster:')
        pm = PLAYERS_RE.search(m[3]) if host_line else None
        if pm:
            count = int(pm[1])
        elif count is not None:
            count += 1 if m[2] == 'joined' else -1
    return count


def run_agree(line, toks, procs, ctx, soft):
    if len(procs) < 2 or len(toks) < 3 or toks[2][0] != 'w' or toks[2][1] not in ('roster', 'stars'):
        raise RuleError('agree: P,Q roster|stars (two or more processes)')
    kind = toks[2][1]
    vals, skipped = {}, []
    for p in procs:
        # a console that ended before the others' shutdown window left on purpose (or died: nocrash says so): it is not asked
        if ctx.shutdown_ms is not None and p.end_ms and p.end_ms < ctx.shutdown_ms - 2000:
            skipped.append(p.name); continue
        v = agree_values(kind, p, ctx.shutdown_ms)
        if v is not None:
            vals[p.name] = v
    if len(vals) < 2:
        return {'rule': line, 'ok': True, 'soft': soft, 'detail': f'SKIPPED (fewer than two consoles with a session to compare{", ended early: " + ",".join(skipped) if skipped else ""})',
                'timing': 'game', 'timing_why': '', 'skipped': True}
    ref = next(iter(vals.values()))
    ok = all(v == ref for v in vals.values())
    show = ' | '.join(f'{n}: {v}' for n, v in vals.items())
    return {'rule': line, 'ok': ok, 'soft': soft, 'detail': ('agree: ' if ok else 'DISAGREE: ') + f'{kind} ' + show, 'timing': 'game', 'timing_why': ''}


def run_same(line, toks, procs, soft):
    if len(procs) < 2 or len(toks) < 3 or toks[2][0] != 're':
        raise RuleError('same: P,Q /RE/ [first|last|each] (two or more processes)')
    rx = re.compile(toks[2][1], toks[2][2])
    mode = toks[3][1] if len(toks) > 3 else 'last'
    if mode not in ('first', 'last', 'each'):
        raise RuleError(f'same: unknown mode {mode}')
    seqs = {p.name: [m.groups() if m.groups() else (m.group(0),) for _, _, m in matches(p, rx)] for p in procs}
    empty = [n for n, s in seqs.items() if not s]
    if empty:
        return {'rule': line, 'ok': False, 'soft': soft, 'detail': 'no line matches /%s/ in %s' % (rx.pattern, ', '.join(empty))}
    pick = {n: (s if mode == 'each' else [s[0]] if mode == 'first' else [s[-1]]) for n, s in seqs.items()}
    ref = next(iter(pick.values()))
    ok = all(v == ref for v in pick.values())
    show = ' | '.join(f'{n}: {" ".join("/".join(g) for g in v[:6])}{" ..." if len(v) > 6 else ""}' for n, v in pick.items())
    return {'rule': line, 'ok': ok, 'soft': soft, 'detail': (f'equal ({mode}): ' if ok else f'DIFFER ({mode}): ') + show}


WALL_METRICS = {'wall_s', 'cpu_s', 'p50_ms', 'p95_ms', 'p99_ms', 'hitches', 'fps'}


def _one(sel):
    return sel not in ('*', 'any') and ',' not in sel


def rule_timing(verb, sel, toks, ctx):
    """How a rule's verdict depends on time. Returns (timing, why), timing:
      'bounded'  a time window in GAME units (gs, f; s / ms / m inside one console that has frame points) that is judged on ONE console's own
                 frame counter: the only kind a slow machine cannot fail (a failure of it on a starved run is a real FAIL, "slow but valid")
      'cross'    a game-unit window between consoles (after Q, order within / tol, near): read on the receiver's clock and capped in wall time
                 (see WALL_CEIL_SPEED), but it still depends on the other console's pace
      'wall'     `ws`, s / ms / m between consoles, wall-measured metrics (fps, wall_s, cpu_s, frame-time percentiles), vsbase
      'untimed'  no time quantity (absent, count, exit, nostall, same, agree, expect without within ...): a starved machine can make them fail
                 (a console that never got to the line, a stall report), so on a starved run they are INVALID, not FAIL."""
    words = [t[1] if t[0] == 'w' else None for t in toks]
    rest = words[2:]
    if verb == 'vsbase':
        return 'wall', 'frame times are wall-measured (a ratio to the same machine\'s baseline)'
    if verb == 'metric':
        return ('wall', f'metric {rest[0]}') if rest and rest[0] in WALL_METRICS else ('untimed', '')
    kinds = [parse_dur(rest[i + 1])[1] for i, w in enumerate(rest[:-1]) if w in ('within', 'tol') and rest[i + 1]]
    if 'wall' in kinds:
        return 'wall', 'a ws time'
    if verb == 'expect':
        asel = next((rest[i + 1] for i, w in enumerate(rest[:-1]) if w == 'after'), None)
        cross = asel is not None and not (_one(sel) and asel == sel)
        if 'legacy' in kinds:
            if cross:
                return 'wall', 'expect ... within in s after another console\'s line (use gs / f)'
            if any(p.game_s(p.start_ms or 0) is None for p in ctx.select(sel)):
                return 'wall', 'no frame points in the log: the s window is wall time'
        if not kinds:
            return 'untimed', ''
        return ('cross', 'after another console\'s line') if cross else ('bounded', '')
    if verb == 'order':
        if 'legacy' in kinds:
            return 'wall', 'order ... within/tol in s (between consoles; use gs / f)'
        if not kinds:
            return 'untimed', ''
        qsel = rest[2] if len(rest) > 2 else ''
        return ('bounded', '') if (_one(sel) and qsel == sel) else ('cross', 'order between consoles')
    return 'untimed', ''


def run_rule(line, ctx):
    soft = line.startswith('?')
    if soft:
        line = line[1:].strip()
    toks = tokenize(line)
    if len(toks) < 2 or toks[0][0] != 'w' or toks[1][0] != 'w':
        raise RuleError(f'rule must start with: VERB PROCESS: {line}')
    verb, sel = toks[0][1], toks[1][1]
    procs = ctx.select(sel)
    if verb == 'same':
        return run_same(line, toks, procs, soft)
    if verb == 'agree':
        return run_agree(line, toks, procs, ctx, soft)
    results = [(p.name,) + check_one(verb, toks[2:], p, ctx) for p in procs]
    timing, twhy = rule_timing(verb, sel, toks, ctx)
    if sel == 'any':
        ok = any(r[1] for r in results)
        shown = [r for r in results if r[1]][:1] or results
    else:
        ok = all(r[1] for r in results)
        shown = results
    detail = ' | '.join(f'{n}: {d}' for n, _, d in shown) if len(procs) > 1 else results[0][2]
    return {'rule': line, 'ok': ok, 'soft': soft, 'detail': detail, 'timing': timing, 'timing_why': twhy}


MIN_SPEED = 0.6
SLOW_SPEED = 0.8   # a process below this ran slower than real time: the run is reported "slow" (a PASS stays a PASS)
# Why 0.6: the game paces itself at 60 fps (59.94 game frames per second) and a healthy desktop run measured 0.98-1.0 after the title screen. Its
# network timers (resend, silence, 5 s ack stall) run on the wall clock, so below ~0.6 (36 fps: frames 28 ms apart, a 3 s capture stall and a
# starved peer look alike) a failed timing rule says more about the machine than about the game. Four-process runs on this 2-core box
# legitimately run slower: a scenario says `min_speed: 0.25` (run_scenario passes --min-speed).


def environment_notes(procs, paths, min_speed):
    """Load average (start / end of the pair, run_pair.sh writes load.json), and per game process: wall seconds, CPU seconds, the wall seconds the
    boot took to the title screen, and the game speed after it. starved = a process ran below min_speed."""
    load = None
    for p in paths:
        q = os.path.join(p, 'load.json')
        if os.path.exists(q):
            load = json.load(open(q))
    rows, starved, why = [], False, ''
    for p in procs:
        r = p.result
        sp, tw, span = p.speed()
        wall, cpu = r.get('wall_s'), r.get('cpu_s')
        rows.append({'name': p.name, 'wall_s': wall, 'cpu_s': cpu, 'title_wall_s': tw, 'speed': sp, 'speed_span_s': span})
        if sp is not None and sp < min_speed:
            starved = True
            why += f'{p.name} ran at {sp:.2f} x real time (< {min_speed:g}); '
    txt = ('load ' + ' -> '.join(' '.join(f'{x:.1f}' for x in l) for l in (load['start'], load['end'])) + '; ' if load else 'load n/a; ')
    txt += '; '.join(f"{x['name']}: wall {x['wall_s']} s cpu {x['cpu_s']} s" + (f", title after {x['title_wall_s']:.0f} s" if x['title_wall_s'] else '') +
                     (f", speed {x['speed']:.2f}" if x['speed'] is not None else ', speed n/a') for x in rows)
    return {'text': txt, 'starved': starved, 'why': why.rstrip('; '), 'min_speed': min_speed, 'load': load, 'procs': rows}


def load_rules(a):
    rules = []
    for path in a.rules or []:
        rules += open(path, encoding='utf-8').read().splitlines()
    if a.scenario:
        import yaml
        rules += yaml.safe_load(open(a.scenario, encoding='utf-8')).get('rules', [])
    rules += a.expr or []
    out = []
    for r in rules:
        r = r.strip()
        if r and not r.startswith('#'):
            out.append(r)
    return out


def main():
    global WALL_CEIL_SPEED
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+')
    ap.add_argument('-r', '--rules', action='append')
    ap.add_argument('-e', '--expr', action='append')
    ap.add_argument('--scenario')
    ap.add_argument('--json')
    ap.add_argument('-q', '--quiet', action='store_true')
    ap.add_argument('--baseline', default=os.environ.get('HARNESS_BASELINE'),
                    help='a single-player run folder (or its frametimes.json) of THIS machine: `vsbase` rules compare against it')
    ap.add_argument('--agree', choices=('off', 'soft', 'hard'), default=os.environ.get('HARNESS_AGREE', 'soft'),
                    help='add the quiet-network invariant (roster size and star set agree between consoles) to runs of 2+ consoles; default soft')
    ap.add_argument('--wall-ceil-speed', type=float, default=WALL_CEIL_SPEED,
                    help='a game-unit window (gs / f, tol too; same console or between consoles) is never longer than window / this in wall seconds; 0 = off (default %(default)s)')
    ap.add_argument('--min-speed', type=float, default=float(os.environ.get('HARNESS_MIN_SPEED', MIN_SPEED)),
                    help='game speed (game s per wall s after the title) below which a FAILED run is INVALID, not FAIL (default %(default)s)')
    a = ap.parse_args()
    procs = hlog.find_procs(a.paths)
    if not procs:
        print('assert_log: no run folders (result.json) found', file=sys.stderr)
        return 2
    WALL_CEIL_SPEED = a.wall_ceil_speed
    ctx = Ctx(procs)
    if a.baseline:
        bp = a.baseline
        bp = os.path.join(bp, 'frametimes.json') if os.path.isdir(bp) else bp
        try:
            ctx.baseline = json.load(open(bp))
            if not isinstance(ctx.baseline, dict):
                print(f'assert_log: baseline {a.baseline}: not a JSON object (vsbase rules are SKIPPED)', file=sys.stderr)
                ctx.baseline = None
        except (OSError, ValueError):
            print(f'assert_log: baseline {a.baseline}: no readable frametimes.json (vsbase rules are SKIPPED)', file=sys.stderr)
    try:
        rules = load_rules(a)
        auto_rules = []
        if a.agree != 'off' and len(procs) >= 2 and not any(r.lstrip('?').lstrip().startswith('agree ') for r in rules):
            pre = '' if a.agree == 'hard' else '?'
            auto_rules = [f'{pre}agree * roster', f'{pre}agree * stars']
        results = [run_rule(r, ctx) for r in rules]
        for r in auto_rules:   # (added by the harness, soft: whatever goes wrong inside them is a SKIPPED note, never an exit status)
            try:
                results.append(run_rule(r, ctx))
            except Exception as e:   # noqa: BLE001
                results.append({'rule': r, 'ok': True, 'soft': True, 'detail': f'SKIPPED ({type(e).__name__}: {e})', 'skipped': True, 'timing': 'untimed', 'timing_why': ''})
    except RuleError as e:
        print(f'assert_log: bad rule: {e}', file=sys.stderr)
        return 2
    notes = environment_notes(procs, a.paths, a.min_speed)
    fails = [r for r in results if not r['ok'] and not r['soft']]
    warns = [r for r in results if not r['ok'] and r['soft']]
    for r in results:
        tag = 'PASS' if r['ok'] else ('WARN' if r['soft'] else 'FAIL')
        if not a.quiet or not r['ok']:
            print(f'{tag}  {r["rule"]}\n        {r["detail"]}')
    hard = [r for r in results if not r['soft']]
    wall_rules = [r for r in hard if r.get('timing') == 'wall']
    all_game = not wall_rules
    speeds = [x['speed'] for x in notes['procs'] if x['speed'] is not None]
    slowest = min(speeds) if speeds else None
    slow = slowest is not None and slowest < SLOW_SPEED
    # INVALID = "a rule failed AND a console ran below min_speed" (the machine may be why): exactly the old rule, whatever the failed rule is. A PASS
    # stays a PASS (slow or not). The timing tags (verdict.json results[].timing) and `slow` are information only.
    if not fails:
        verdict = 'PASS'
    elif notes['starved']:
        verdict = 'INVALID'
    else:
        verdict = 'FAIL'
    print(f'assert_log: {len(results) - len(fails) - len(warns)} passed, {len(fails)} failed, {len(warns)} warned '
          f'({", ".join(p.name for p in procs)})')
    if warns:   # a `?` rule never fails the run, so make the ones that did fail visible in the summary
        print(f'assert_log: SOFT-FAILED {len(warns)} (not counted): ' + '; '.join(w['rule'][:70] for w in warns[:5]) + (' ...' if len(warns) > 5 else ''))
    nskip = sum(1 for r in results if r.get('skipped') or str(r['detail']).startswith('SKIPPED') or 'SKIPPED (' in str(r['detail']))
    if nskip:
        print(f'assert_log: {nskip} rule(s) SKIPPED (no baseline / nothing to compare): they passed vacuously')
    print('machine: ' + notes['text'])
    print(f'assert_log: timing: ' + ('every hard rule is judged in game time' if all_game else
          f'{len(wall_rules)} of {len(hard)} hard rules depend on wall time (first: {wall_rules[0]["rule"][:60]} [{wall_rules[0]["timing_why"]}])'))
    if slow and verdict == 'PASS':
        print(f'assert_log: PASS, slow ({slowest:.2f} x real time); {len(wall_rules)} of {len(hard)} hard rules depend on wall time (information only)')
    if verdict == 'INVALID':
        print(f'assert_log: INVALID, not FAIL: the game was starved ({notes["why"]}); {len(fails)} rule(s) failed but the run does not show the game at fault. Run it again when the machine is quiet.')
    if a.json:
        json.dump({'results': results, 'failed': len(fails), 'warned': len(warns), 'verdict': verdict, 'machine': notes,
                   'all_game_time': all_game, 'wall_rules': len(wall_rules), 'slow': slow, 'slowest_speed': slowest}, open(a.json, 'w'), indent=1)
    return 0 if verdict == 'PASS' else 3 if verdict == 'INVALID' else 1


if __name__ == '__main__':
    sys.exit(main())
