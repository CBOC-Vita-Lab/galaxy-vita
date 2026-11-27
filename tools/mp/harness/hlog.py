"""Log loading for the MP test harness (used by merge_timeline.py and assert_log.py).

A process run folder (made by run_one.sh or run_pair.sh) has:
  game.log     the port log: "HH:MM:SS.mmm text" (wall clock, local time, written by the game itself); with PETARI_LOG_FRAMES=1 every
               line is "HH:MM:SS.mmm @f<game frame> text" (the stamp is stripped from the text; Proc.game_s / wall_ms use it)
  console.log  every stdout/stderr line, stamped by supervise.py: "<epoch ms> text"
  result.json  start/end ms, exit code, peak RSS...
port_log() lines reach both files; port_log_summary() lines only game.log; plain stderr (the weak mp_log, the crash line,
input-script warnings) only console.log. events() returns the union without duplicates, as (t_ms, text) sorted by time:
game.log lines with their own stamps, then console lines that are not also in game.log.
"""
import bisect, collections, datetime, json, os, re

_GAME_TS = re.compile(r'^(\d\d):(\d\d):(\d\d)\.(\d\d\d) (?:@f(\d+) )?(.*)$')   # "@f<frame> " = PETARI_LOG_FRAMES=1 (the game's own frame counter)
_CON_TS = re.compile(r'^(\d{12,14}) (.*)$')


class Proc:
    def __init__(self, run_dir, name=None):
        self.dir = run_dir
        self.name = name or os.path.basename(os.path.normpath(run_dir))
        self.result = {}
        p = os.path.join(run_dir, 'result.json')
        if os.path.exists(p):
            self.result = json.load(open(p))
        self.start_ms = self.result.get('start_ms')
        self.end_ms = self.result.get('end_ms')
        self._events = None
        self._stamps = []   # (wall_ms, game frame) of every game.log line that carries an "@f<frame>" stamp (PETARI_LOG_FRAMES=1)

    def file(self, name):
        return os.path.join(self.dir, name)

    def json(self, name):
        p = self.file(name)
        return json.load(open(p)) if os.path.exists(p) else None

    def events(self):
        if self._events is not None:
            return self._events
        ev = []
        game_texts = collections.Counter()
        gl = self.file('game.log')
        if os.path.exists(gl):
            base = None
            if self.start_ms:
                base = datetime.datetime.fromtimestamp(self.start_ms / 1000)
            last = self.start_ms or 0
            for line in open(gl, encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                m = _GAME_TS.match(line)
                if not m:
                    if line:
                        ev.append((last, line, 'game'))
                        game_texts[line] += 1
                    continue
                h, mi, s, ms, fstamp, text = int(m[1]), int(m[2]), int(m[3]), int(m[4]), m[5], m[6]
                if base is not None:
                    t = base.replace(hour=h, minute=mi, second=s, microsecond=ms * 1000)
                    tms = int(t.timestamp() * 1000)
                    if tms < self.start_ms - 3600_000:  # the run went past midnight
                        tms += 86400_000
                else:
                    tms = ((h * 60 + mi) * 60 + s) * 1000 + ms
                last = tms
                if fstamp is not None:
                    self._stamps.append((tms, int(fstamp)))
                ev.append((tms, text, 'game'))
                game_texts[text] += 1
        cl = self.file('console.log')
        if os.path.exists(cl):
            for line in open(cl, encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                m = _CON_TS.match(line)
                if not m:
                    continue
                tms, text = int(m[1]), m[2]
                if game_texts[text] > 0:
                    game_texts[text] -= 1
                    continue
                ev.append((tms, text, 'console'))
        r = self.result
        if r:
            ev.append((r['start_ms'], 'harness: process start', 'harness'))
            how = (f"signal={r.get('signal')}" if r.get('signal') else f"exit={r.get('exit_code')}") + \
                  (' timeout' if r.get('timed_out') else '')
            ev.append((r['end_ms'], f'harness: process end {how}', 'harness'))
        ev.sort(key=lambda e: e[0])
        self._events = ev
        return ev


_FRAME_RE = re.compile(r'^gx: frame (\d+) presented')
_TITLE_RE = re.compile(r'^boot: the title screen has appeared')


def _frames(self):
    """(wall_ms, frame) points, wall_ms ascending and the frame never going back, with (start, 0) in front. EXACT when the log carries the
    game's own "@f<frame>" stamps (PETARI_LOG_FRAMES=1: one point per log line); otherwise every 300th game frame from the "gx: frame N
    presented" lines or every 120th line of frametimes.txt (the older, coarser way)."""
    if getattr(self, '_fr', None) is None:
        self.events()
        fr = [(self.start_ms, 0)] if self.start_ms else []
        self.stamped = len(self._stamps) >= 2
        if self.stamped:
            top = 0
            for t, f in sorted(self._stamps):
                top = max(top, f)
                if not fr or t > fr[-1][0] or top > fr[-1][1]:
                    fr.append((max(t, fr[-1][0]) if fr else t, top))
            self._fr = fr
            return fr
        for t, text, _ in self.events():
            m = _FRAME_RE.match(text)
            if m:
                fr.append((t, int(m[1])))
        ft = self.file('frametimes.txt')   # (the vgx renderer prints no "gx: frame N presented" lines; run_one.sh always sets PETARI_FRAMETIMES: "<frame> <ms since start> <delta>")
        if len(fr) < 3 and os.path.exists(ft) and self.start_ms:
            fr = [(self.start_ms, 0)]
            for i, line in enumerate(open(ft)):
                p = line.split()
                if len(p) >= 2 and i % 120 == 0:
                    fr.append((self.start_ms + int(p[1]), int(p[0])))
        self._fr = fr
    return self._fr


FPS = 59.94


def _game_s(self, t_ms):
    """Game seconds (frames / 59.94, the clock PETARI_GAME_CLOCK scripts and the game's own timers run on) at wall time t_ms, interpolated
    between the frame points; None when the log has no frame points (then callers use the wall clock)."""
    fr = self.frames()
    if len(fr) < 2:
        return None
    if t_ms <= fr[0][0]:
        return 0.0
    i = bisect.bisect_left(fr, (t_ms, -1))
    if i >= len(fr):
        t1, f1 = fr[-1]
        return (f1 + (t_ms - t1) * FPS / 1000.0) / FPS
    (t0, f0), (t1, f1) = fr[i - 1], fr[i]
    return (f0 + (f1 - f0) * (t_ms - t0) / max(1, t1 - t0)) / FPS


def _wall_ms(self, g_s):
    """The inverse of game_s: the wall time (ms) at which this process reached game second g_s; None without frame points. A game second
    before the start is the start; one past the last point is extrapolated at 1x."""
    fr = self.frames()
    if len(fr) < 2:
        return None
    f = g_s * FPS
    if f <= 0:
        return fr[0][0]
    keys = [p[1] for p in fr]
    i = bisect.bisect_left(keys, f)
    if i >= len(fr):
        t1, f1 = fr[-1]
        return t1 + (f - f1) * 1000.0 / FPS
    if i == 0:
        return fr[0][0]
    (t0, f0), (t1, f1) = fr[i - 1], fr[i]
    return t0 + (t1 - t0) * (f - f0) / max(1, f1 - f0)


def _speed(self):
    """Game speed after the title screen (game seconds per wall second; 1.0 = the console's 60 fps), the wall seconds to the title,
    and the span measured; (None, title_wall_s, 0) when there are too few frame stamps."""
    fr = self.frames()
    title = next((t for t, text, _ in self.events() if _TITLE_RE.match(text)), None)
    tw = (title - self.start_ms) / 1000.0 if title and self.start_ms else None
    pts = [p for p in fr if title is None or p[0] >= title]
    if len(pts) < 2:
        pts = fr
    if len(pts) < 2 or pts[-1][0] - pts[0][0] < 10000:
        return None, tw, 0.0
    wall = (pts[-1][0] - pts[0][0]) / 1000.0
    return (pts[-1][1] - pts[0][1]) / 59.94 / wall, tw, wall


Proc.frames = _frames
Proc.game_s = _game_s
Proc.wall_ms = _wall_ms
Proc.speed = _speed


def find_procs(paths):
    """Run folders from a list of paths: a run folder (has result.json) or a pair/scenario folder (its subfolders)."""
    procs = []
    for p in paths:
        if os.path.exists(os.path.join(p, 'result.json')):
            procs.append(Proc(p))
            continue
        for d in sorted(os.listdir(p)):
            q = os.path.join(p, d)
            if os.path.isdir(q) and os.path.exists(os.path.join(q, 'result.json')):
                procs.append(Proc(q))
    return procs


def timeline(procs):
    t0 = min((p.start_ms for p in procs if p.start_ms), default=0)
    rows = []
    for p in procs:
        for t, text, src in p.events():
            rows.append((t, p.name, src, text))
    rows.sort(key=lambda r: r[0])
    return t0, rows
