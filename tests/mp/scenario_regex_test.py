#!/usr/bin/env python3
"""A scenario rule whose regex can never match a line the game can print is the commonest way a guessed scenario goes wrong: an `expect` that
can never pass (the run FAILs for the wrong reason) or, worse, an `absent` that can never fail (a check that checks nothing).

This test reads the SOURCE, not a log. It extracts every format string the code logs:
  * mp corpus   every mp_log(...) / MP_LOG_LIMITED(...) format of platform/ net/ decomp/src/Game/MP ("[mp] " is the prefix mp_log adds);
  * all corpus  every string literal of those trees (C/C++) and of the harness (python f-strings): for the lines that are not "[mp] ..." (port:,
                [OS], harness: ...).
and checks, for every rule regex of the scenarios `qa_*.yaml` / `glitch_*.yaml` of tools/mp/harness/scenarios (and the HARNESS_END / START_ON /
KILL / STOP regexes of their process env), that some format can print a line the regex matches: each printf conversion is filled with sample
values (numbers, names, and the words the regex itself spells out), the regex is searched in the result. A word the regex needed that is not
somewhere in the code as a literal (e.g. a made-up "reason=...") fails too.

It also checks each such scenario's shape: it parses (an `on:` link key is the string 'on'), has at least one negative rule (absent, or a count
bound), has rules naming every console (a process name, a list containing it, or *), and every SMG_MP_TEST_* / SMG_MP_AUTO word it sets or
requires exists in the code.

  scenario_regex_test.py [--all] [-v] [scenario.yaml ...]       (--all: every scenario of the folder; default: the qa_ and glitch_ ones)
Exit 0 = every checked regex can match something. No game build needed (about 20 seconds).
"""
import ast, glob, itertools, os, random, re, sys

try:
    import re._parser as sre_parse   # python >= 3.11
except ImportError:                  # pragma: no cover
    import sre_parse
import warnings
warnings.filterwarnings('ignore')

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
HARNESS = os.path.join(ROOT, 'tools', 'mp', 'harness')
sys.path.insert(0, HARNESS)
import run_scenario as rs
import assert_log as al

# ---------------------------------------------------------------------------------------------------------------- corpus
_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')
_GAP = re.compile(r'\s*(?:/\*.*?\*/\s*|//[^\n]*\n\s*)*', re.S)
_ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', "'": "'", '\\': '\\', '0': '\0', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}
_CONV = re.compile(r'%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|t|j|L)?([diouxXeEfFgGaAcspn%])')


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'x':
                j = i + 2
                while j < len(s) and s[j] in '0123456789abcdefABCDEF':
                    j += 1
                out.append(chr(int(s[i + 2:j] or '0', 16) & 0x10FFFF) if j > i + 2 else 'x')
                i = j
                continue
            out.append(_ESC.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def literals_after(src, pos):
    """The adjacent string literals starting at pos (concatenated), and the end position; None when pos is not a literal."""
    parts, p = [], pos
    while True:
        m = _GAP.match(src, p)
        p = m.end()
        lm = _LIT.match(src, p)
        if not lm:
            break
        parts.append(unescape(lm.group(0)[1:-1]))
        p = lm.end()
    return (''.join(parts), p) if parts else (None, pos)


def split_format(fmt):
    """-> list of ('lit', text) / ('conv', char)"""
    out, last = [], 0
    for m in _CONV.finditer(fmt):
        if m.group(1) == '%':
            continue
        if m.start() > last:
            out.append(('lit', fmt[last:m.start()].replace('%%', '%')))
        out.append(('conv', m.group(1)))
        last = m.end()
    if last < len(fmt):
        out.append(('lit', fmt[last:].replace('%%', '%')))
    return out


def cpp_files(rel_dirs):
    for d in rel_dirs:
        for dp, _, fs in os.walk(os.path.join(ROOT, d)):
            for f in fs:
                if f.endswith(('.cpp', '.hpp', '.h', '.c', '.cc', '.mm')):
                    yield os.path.join(dp, f)


_LOGCALL = re.compile(r'\b(?:mp_log|MP_LOG_LIMITED|LogLine|logLine)\s*\(|\bMP_LOG_LIMITED_N\s*\(\s*[^,]+,')


def build_corpus():
    mp, allf = [], []
    mp_dirs = ['platform', 'net', 'decomp/src/Game/MP', 'decomp/include/Game/MP']
    for path in cpp_files(mp_dirs):
        try:
            src = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in _LOGCALL.finditer(src):
            fmt, _ = literals_after(src, m.end())
            if fmt:
                mp.append(split_format(fmt))
        for lm in _LIT.finditer(src):
            allf.append(split_format(unescape(lm.group(0)[1:-1])))
    # the rest of the game: plain literals only (OSPanic / assert texts, "port:" lines)
    for path in cpp_files(['decomp/src/Game/System', 'decomp/src/Game/Util', 'decomp/src/Game/Scene', 'decomp/src/Game/Player']):
        try:
            src = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for lm in _LIT.finditer(src):
            allf.append(split_format(unescape(lm.group(0)[1:-1])))
        for m in _LOGCALL.finditer(src):   # (an mp_log call outside the multiplayer directories, e.g. "save autoconfirm (host)" in SaveDataHandleSequence.cpp, is an [mp] line too:
            fmt, _ = literals_after(src, m.end())   # without it a rule for it was only satisfiable by a lucky random filling of some %s, i.e. the check was flaky)
            if fmt:
                mp.append(split_format(fmt))
    # the harness and the scripts of tools/mp: python strings (f-string fields and %-conversions become conversions) and shell echo text
    for path in glob.glob(os.path.join(HARNESS, '*.py')) + glob.glob(os.path.join(ROOT, 'tools', 'mp', '*.py')):
        try:
            tree = ast.parse(open(path, encoding='utf-8').read())
        except (OSError, SyntaxError):
            continue
        for node in ast.walk(tree):
            if isinstance(node, ast.JoinedStr):
                segs = []
                for v in node.values:
                    if isinstance(v, ast.Constant) and isinstance(v.value, str):
                        segs.append(('lit', v.value))
                    else:
                        segs.append(('conv', 's'))
                allf.append(segs)
            elif isinstance(node, ast.Constant) and isinstance(node.value, str):
                allf.append(split_format(node.value))
    for path in glob.glob(os.path.join(HARNESS, '*.sh')):
        for lm in re.finditer(r'echo\s+"((?:[^"\\]|\\.)*)"', open(path, encoding='utf-8', errors='replace').read()):
            allf.append(split_format(lm.group(1)))
    allf.extend(mp)
    return mp, allf


# ---------------------------------------------------------------------------------------------------------------- regex side
def lit_runs(rx):
    """Literal runs (strings) of a regex: every maximal run of LITERAL nodes anywhere in it (alternation branches separately)."""
    try:
        tree = sre_parse.parse(rx)
    except Exception as e:  # noqa
        raise ValueError(f'bad regex {rx!r}: {e}')
    runs = []

    def walk(seq, top):
        cur = []

        def flush():
            if cur:
                runs.append((''.join(cur), top))
                cur.clear()
        for op, av in seq:
            name = str(op)
            if name == 'LITERAL':
                cur.append(chr(av))
                continue
            flush()
            if name == 'SUBPATTERN':
                walk(av[-1], top)
            elif name in ('MAX_REPEAT', 'MIN_REPEAT', 'POSSESSIVE_REPEAT'):
                walk(av[2], top and av[0] >= 1)
            elif name == 'BRANCH':
                for b in av[1]:
                    walk(b, False)
            elif name in ('ASSERT', 'ASSERT_NOT'):
                walk(av[1], False)
            elif name == 'GROUPREF_EXISTS':
                for b in av[1:]:
                    if b is not None:
                        walk(b, False)
        flush()
    walk(tree, True)
    return runs


def mined_tokens(rx):
    toks = set()
    for run, _ in lit_runs(rx):
        words = [w for w in re.split(r'[\s=:,;]+', run) if w]
        toks.add(run.strip())
        for w in words:
            toks.add(w)
        for n in (2, 3):
            for i in range(len(words) - n + 1):
                toks.add(' '.join(words[i:i + n]))
    toks.discard('')
    return sorted(toks, key=lambda t: (len(t), t))   # a total order: `toks` is a set of str, so a bare key=len left equal-length tokens in PYTHONHASHSEED order (the random fillings, hence the verdict, varied per run)


def grams(s, n=4):
    return {s[i:i + n] for i in range(len(s) - n + 1)}


INT_POOL = ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '10', '15', '20', '25', '60', '100', '1800', '-1']
STR_POOL = ['x', '1,1', '1,2', '2,1', '1,0', '0,1', '1,2,3', 'host', 'guest', 'g1', 'g2', 'g3', 'bot1', 'bot2', 'bot3', 'fifth', 'good', 'badcode', 'late', 'Luigi', 'Alex', 'wifi', 'direct',
            'nearby', 'native', 'synthetic', '0', '1', '2', 'EggStarGalaxy', 'abyss']
FLT_POOL = ['0.0', '1.5', '28.1', '780.0', '-3000.0']
HEX_POOL = ['0', '1', 'aa3ddccb', 'deadbeef', '1a2b3c4d']


def pool(ch, mined):
    if ch in 'diu':
        return INT_POOL + [t for t in mined if re.fullmatch(r'-?\d+', t)]
    if ch in 'xX':
        return HEX_POOL + [t for t in mined if re.fullmatch(r'[0-9a-f]+', t)]
    if ch in 'eEfFgGaA':
        return FLT_POOL
    if ch == 'c':
        return ['x', '1']
    if ch == 'p':
        return ['0x1000']
    return STR_POOL


def relaxed(rx):
    """$1..$9 of an `order` rule's second regex stand for a captured value."""
    return re.sub(r'\$[1-9]', r'.+?', rx)


class Corpus:
    """Formats with an inverted index of their literal 4-grams (a regex is only tried against formats that share a 4-gram of a literal run)."""
    def __init__(self, fmts, lits_text, prefix=''):
        # (a format that is nearly all conversions - "%s", "%s: %d" - could print anything: it proves nothing)
        fmts = [segs for segs in fmts if sum(len(t.strip()) for k, t in segs if k == 'lit') >= 6]
        self.fmts = [([('lit', prefix)] + segs) if prefix else segs for segs in fmts]
        self.lits = lits_text
        self.index = {}
        for i, segs in enumerate(self.fmts):
            text = ''.join(t for k, t in segs if k == 'lit')
            for g in grams(text):
                self.index.setdefault(g, set()).add(i)
        self.n = len(self.fmts)

    def candidates(self, rx):
        runs = [r.replace('[mp] ', '') for r, _ in lit_runs(rx)]   # (the prefix is on every line: no evidence)
        runs = [r for r in runs if len(r) >= 4]
        if not runs:
            return None
        score = {}
        for r in runs:   # (a format ranks by how much of each literal run of the regex it shares; an alternation's branches each have their own run)
            gs = grams(r)
            hit = {}
            for g in gs:
                for i in self.index.get(g, ()):
                    hit[i] = hit.get(i, 0) + 1
            for i, n in hit.items():
                score[i] = score.get(i, 0.0) + n / len(gs)
        return sorted(score, key=lambda i: (-score[i], i))   # (equal scores in format order: `score` was filled in the iteration order of a set of str, i.e. PYTHONHASHSEED order)


MAX_COMBOS = 300
MAX_FORMATS = 60


def aligned_fillers(segs, runs):
    """For every conversion of a format: the text the regex itself spells at that place (its literal runs, aligned on the literal text before and
    after the conversion): 'slot %d joined name=%s' against 'slot 1 joined name=guest' gives '1' and 'guest'."""
    out = {}
    for i, (k, _) in enumerate(segs):
        if k != 'conv':
            continue
        before = segs[i - 1][1] if i > 0 and segs[i - 1][0] == 'lit' else ''
        after = segs[i + 1][1] if i + 1 < len(segs) and segs[i + 1][0] == 'lit' else ''
        found = []
        if before:
            for n in range(min(len(before), 14), 0, -1):
                suf = before[-n:]
                for run in runs:
                    at = run.find(suf)
                    while at >= 0:
                        rest = run[at + n:]
                        j = -1
                        for m in range(min(len(after), 14), 0, -1):
                            j = rest.find(after[:m]) if m >= 2 or len(after) == 1 else -1
                            if j >= 0:
                                break
                        if j < 0 and after:
                            j = rest.find(after[0])
                        tok = rest[:j] if j >= 0 else (rest.split(' ')[0] if rest else '')
                        if tok and tok not in found:
                            found.append(tok)
                        at = run.find(suf, at + 1)
                if found:
                    break
        out[i] = found
    return out


def can_print(rx, corpus, rng, excuse=()):
    ok, why = _can_print(rx, corpus, rng, excuse, MAX_COMBOS, MAX_FORMATS)
    if not ok and why.startswith('no format'):   # a second, deeper look (more random fillings of the best formats) before giving up
        ok, why = _can_print(rx, corpus, rng, excuse, 6000, 12)
    return ok, why


def _can_print(rx, corpus, rng, excuse, max_combos, max_formats):
    """-> (True, note) when some format can print a line `rx` matches; (False, reason)."""
    crx = re.compile(relaxed(rx))
    mined = mined_tokens(rx)
    runs = [r for r, _ in lit_runs(rx)]
    cand = corpus.candidates(rx)
    ids = cand[:max_formats] if cand is not None else range(min(corpus.n, 300))
    best_unverified = None
    for fi in ids:
        segs = corpus.fmts[fi]
        convs = [i for i, (k, _) in enumerate(segs) if k == 'conv']
        if not convs:
            if crx.search(''.join(t for k, t in segs)):
                return True, 'literal'
            continue
        al_ = aligned_fillers(segs, runs)
        cands = []
        for i in convs:
            c = list(al_.get(i, []))
            for v in pool(segs[i][1], mined) + (mined if segs[i][1] in 'sc' else []):
                if v not in c:
                    c.append(v)
            cands.append(c)

        def combos():
            base = tuple(c[0] for c in cands)
            yield base
            for j, c in enumerate(cands):
                for v in c[1:]:
                    yield base[:j] + (v,) + base[j + 1:]
            for _ in range(max_combos):
                yield tuple((c[0] if (al_.get(convs[j]) and rng.random() < 0.5) else rng.choice(c)) for j, c in enumerate(cands))
        seen = set()
        for combo in combos():
            if combo in seen:
                continue
            seen.add(combo)
            vals = dict(zip(convs, combo))
            line = ''.join(segs[i][1] if segs[i][0] == 'lit' else vals[i] for i in range(len(segs)))
            if crx.search(line):
                # the words the match needed that are no data (names, numbers) must exist in the code (an enumeration like reason=leave)
                used = [v for ci, v in zip(convs, combo) if segs[ci][1] in 'sc' and len(v) >= 4 and not re.fullmatch(r'[-+0-9a-fx.,]+', v)
                        and v not in STR_POOL and v not in excuse]
                missing = [u for u in used if u not in corpus.lits]
                if not missing:
                    return True, 'format'
                best_unverified = missing
    if best_unverified:
        return False, f'matches only with the text {best_unverified!r}, which is nowhere in the code'
    return False, 'no format string of the code can print a line this regex matches'


# ---------------------------------------------------------------------------------------------------------------- scenarios
ENV_RX = {   # env key -> how to pull the regex out of the value
    'HARNESS_END': lambda v: v.rsplit('|', 1)[0] if v.count('|') >= 1 else v,
    'HARNESS_START_ON': lambda v: v.split('|', 1)[1].rsplit('|', 1)[0] if v.count('|') >= 2 else None,
    'HARNESS_KILL': lambda v: v.rsplit('|', 2)[0] if v.count('|') >= 2 else v.split('|')[0],
    'HARNESS_STOP': lambda v: v.rsplit('|', 2)[0] if v.count('|') >= 2 else v.split('|')[0],
}


def scenario_regexes(sc):
    """-> list of (where, regex, kind) for every rule and every harness env regex; plus the rule structure for the shape checks."""
    out, structure = [], []
    for r in sc.get('rules') or []:
        s = r.lstrip('?').strip()
        toks = al.tokenize(s)
        words = [t[1] for t in toks if t[0] == 'w']
        res = [t for t in toks if t[0] == 're']
        kind = words[0] if words else '?'
        for t in res:
            out.append((f'rule {s[:110]}', t[1], kind))
        structure.append((kind, words, s))
    for p in sc.get('processes') or []:
        for k, fn in ENV_RX.items():
            v = (p.get('env') or {}).get(k)
            if v is not None:
                rx = fn(str(v))
                if rx:
                    out.append((f"process {p['name']} {k}", rx, 'env'))
    return out, structure


def known_words():
    """SMG_* names set in the code (for requires.strings and env keys)."""
    names = set()
    for path in cpp_files(['platform', 'decomp/src/Game/MP', 'vita/vgx-game', 'net']):
        try:
            src = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        names.update(re.findall(r'"((?:SMG|PETARI)_[A-Z0-9_]+)"', src))
        names.update(re.findall(r'\b(SMG_MP_TEST_[A-Z0-9_]+)', src))
    return names


def shape_problems(path, sc, structure, names):
    probs = []
    procs = [p['name'] for p in sc.get('processes') or []]
    bots = [b['name'] for b in sc.get('bots') or []]
    if not any(k == 'absent' or (k == 'count' and any(o in w for w in ws for o in ('==', '<=', '<')) ) for k, ws, _ in structure):
        probs.append('no negative rule (absent, or a count bound)')
    named = set()
    for k, ws, _ in structure:
        if len(ws) > 1:
            for who in ws[1].split(','):
                named.add(who)
        for i, w in enumerate(ws):   # `before Q` / `after Q` / `near Q` targets
            if w in ('before', 'after', 'near') and i + 1 < len(ws) and ws[i + 1] != 'shutdown':
                named.update(ws[i + 1].split(','))
    if '*' not in named and 'any' not in named:
        for n in procs:
            if n not in named:
                probs.append(f'no rule names the console {n!r}')
    for n in named - {'*', 'any'} - set(procs) - set(bots):
        if n and re.fullmatch(r'[\w,]+', n) and n not in ('before', 'after', 'near', 'within'):
            if not re.fullmatch(r'\d.*|==|!=|>=|<=|<|>', n):
                probs.append(f'a rule names {n!r}, which is not a process of the scenario')
    for s in ((sc.get('requires') or {}).get('strings') or []):
        if re.fullmatch(r'(SMG|PETARI)_[A-Z0-9_]+', s) and s not in names:
            probs.append(f'requires string {s} does not exist in the code')
    for p in sc.get('processes') or []:
        for k, v in (p.get('env') or {}).items():
            if k.startswith('SMG_MP_TEST_') and k not in names:
                probs.append(f'process {p["name"]} sets {k}, which is not in the code')
            if k == 'SMG_MP_AUTO':
                if not re.match(r'(host|join|join_addr:|join_code:)', str(v)):
                    probs.append(f'process {p["name"]} SMG_MP_AUTO={v!r}: not an autopilot word')
    link = sc.get('link')
    for ev in (link.get('events', []) if isinstance(link, dict) else []):
        if not ('on' in ev or 'at' in ev or 'from' in ev):
            probs.append(f'link event without a quoted on key: {ev}')
    return probs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv
    folder = os.path.join(HARNESS, 'scenarios')
    if args:
        files = args
    elif '--all' in sys.argv:
        files = sorted(glob.glob(folder + '/*.yaml'))
    else:
        files = sorted(glob.glob(folder + '/qa_*.yaml') + glob.glob(folder + '/glitch_*.yaml'))
    if not files:
        print('scenario_regex_test: no scenario found'); return 1
    mp, allf = build_corpus()
    every_literal = '\n'.join(t for segs in allf for k, t in segs if k == 'lit')   # (the words a match needs: any literal of the code, e.g. the "lost" of removeSlot(...))
    mp_c = Corpus(mp, every_literal, prefix='[mp] ')
    all_c = Corpus(allf, every_literal)
    names = known_words()
    rng = random.Random(7)
    # self-check: the test must reject text the game cannot print and accept lines it can (else it proves nothing)
    for rx, want in ((r'\[mp\] flow banana', False), (r'\[mp\] slot 1 left reason=wombat', False), (r'\[mp\] hello rejected reason=bogus', False),
                     (r'\[mp\] joined slot=1 sesion=\d+', False), (r'\[mp\] slot 1 joined name=guest rejoin=0 .*players=2', True),
                     (r'\[mp\] slot 1 left reason=\S+ \(lost\) players=1', True)):
        got = can_print(rx, mp_c, rng, {'guest'})[0]
        if got != want:
            print(f'scenario_regex_test: SELF-CHECK FAILED for /{rx}/: said {got}, expected {want}')
            return 2
    problems = checked = 0
    for f in files:
        sc = rs.load_scenario(f)
        rxs, structure = scenario_regexes(sc)
        probs = shape_problems(f, sc, structure, names)
        excuse = set(p['name'] for p in sc.get('processes') or []) | set(b['name'] for b in sc.get('bots') or [])
        for p in sc.get('processes') or []:
            for k, v in (p.get('env') or {}).items():
                if k in ('SMG_MP_NAME', 'SMG_KEYBOARD_NAME', 'PETARI_STAGE') or k.startswith(('SMG_MP_AUTO', 'SMG_MP_TEST_')):
                    # (data the scenario itself puts in: names, "copy:2", a typed message: the log repeats it)
                    excuse.add(str(v))
                    excuse.update(x.strip() for x in re.split(r'[:,|@]', str(v)) if x.strip())
        for where, rx, kind in rxs:
            checked += 1
            try:
                if rx.startswith(r'\[mp\]'):
                    ok, why = can_print(rx, mp_c, rng, excuse)   # (mp_log puts "[mp] " in front of every format)
                else:
                    ok, why = can_print(rx, all_c, rng, excuse)
                    if not ok:   # a regex without the [mp] prefix may still be part of an mp_log line
                        ok, why = can_print(rx, mp_c, rng, excuse)
            except ValueError as e:
                ok, why = False, str(e)
            if verbose:
                print(('ok   ' if ok else 'BAD  ') + os.path.basename(f) + ' ' + where + '  /' + rx + '/ ' + (why if ok else ''))
            if not ok:
                probs.append(f'{where}: /{rx}/: {why}')
        name = os.path.basename(f)
        problems += len(probs)
        if probs:
            print(f'FAIL {name}')
            for p in probs:
                print('   ' + p)
        elif verbose:
            print(f'ok   {name}')
    print(f'scenario_regex_test: {len(files)} scenarios, {checked} regexes checked against {len(mp)} mp_log formats and {len(allf)} literals: '
          f'{"PASS" if not problems else "FAIL (" + str(problems) + " problems)"}')
    return 1 if problems else 0


sys.exit(main())
