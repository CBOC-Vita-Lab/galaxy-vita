#!/usr/bin/env python3
"""Host test of tools/console31/abpair.py on synthetic logs (run: python3 tests/abpair_test.py)."""
import importlib.util, os, random, sys, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('abpair', os.path.join(root, 'tools/console31/abpair.py'))
ab = importlib.util.module_from_spec(spec); spec.loader.exec_module(ab)

fails = 0
def check(c, what):
    global fails
    if not c:
        fails += 1; print('FAIL', what)

def stamp(t):
    t %= 86400
    return '%02d:%02d:%02d.%03d' % (t // 3600, t % 3600 // 60, t % 60, int((t % 1) * 1000))

def make(path, rng, fps_mean, noise=1.0, start=23 * 3600 + 59 * 60 + 10, stages=('A', 'B'), secs=140, hitch=0):
    """A synthetic basic log: each stage a flow line then fps lines every 5 s (the clock crosses midnight by default)."""
    t = start
    with open(path, 'w') as f:
        for st in stages:
            f.write('%s flow: scene controller: wave bank -> normal (scene Game, stage %s scenario 1)\n' % (stamp(t), st))
            for i in range(secs // 5):
                t += 5
                fps = max(5.0, rng.gauss(fps_mean, noise))
                f.write('%s fps: 5.0 s: shown %.1f fps; game %.1f turns/s; 0 skipped; %d hitches over 100 ms (longest 30 ms); picture 960x544; 0 shaders prepared (0 ms)\n' % (stamp(t), fps, fps, hitch))
                if i % 6 == 5:
                    f.write('%s q30lock: stage %s: game pair (drawn + undrawn) mean 30.0 ms = drawn 17.0 + undrawn 13.0, p95 %.1f ms (40 pairs)\n' % (stamp(t), st, 36.0 - (fps_mean - 50) * 0.5))

def run(on, off, extra=()):
    args = ab.argparse.Namespace(metric='frame-p95', regex=None, lower_better=False, higher_better=False, bin=30.0, skip=20.0, boot=3000, seed=1, min_windows=4)
    for k, v in extra: setattr(args, k, v)
    return ab.compare(on, off, args)

with tempfile.TemporaryDirectory() as d:
    p = lambda n: os.path.join(d, n)
    rng = random.Random(7)
    # on is faster (higher fps) by 3 fps: lower frame time
    make(p('a'), rng, 58); make(p('b'), rng, 58); make(p('c'), rng, 55); make(p('d'), rng, 55)
    r = run([p('a'), p('b')], [p('c'), p('d')])
    check(r['verdict'] == 'on better' and r['windows'] >= 8, 'on better: %r' % (r,))
    r = run([p('a'), p('b')], [p('c'), p('d')], [('metric', 'fps')])
    check(r['verdict'] == 'on better' and r['diff'] > 2, 'fps higher is better: %r' % (r,))
    r = run([p('c'), p('d')], [p('a'), p('b')])
    check(r['verdict'] == 'off better', 'off better')
    r = run([p('a'), p('b')], [p('c'), p('d')], [('metric', 'hitches')])
    check(r['verdict'] == 'no difference', 'hitches equal: no difference')
    # pair-p95 line (the custom line metric of Quality 30)
    r = run([p('a'), p('b')], [p('c'), p('d')], [('metric', 'pair-p95')])
    check(r['windows'] > 0 and r['verdict'] == 'on better', 'pair-p95 on better: %r' % (r,))
    # equal means: no difference
    make(p('e'), rng, 56, 2.0); make(p('f'), rng, 56, 2.0); make(p('g'), rng, 56, 2.0); make(p('h'), rng, 56, 2.0)
    r = run([p('e'), p('f')], [p('g'), p('h')])
    check(r['verdict'] == 'no difference', 'equal arms: no difference %r' % (r,))
    # a stage only one run has is not matched; too few windows: no difference
    make(p('i'), rng, 40, stages=('A', 'X'), secs=60); make(p('j'), rng, 58, stages=('A', 'Y'), secs=60)
    r = run([p('i')], [p('j')])
    check(r['windows'] <= 2 and r['verdict'] == 'no difference' and 'fewer' in r['note'], 'unmatched stages: %r' % (r,))
    # the midnight wrap (start 23:59:10 in make) was crossed above: windows exist
    check(run([p('a')], [p('c')])['windows'] >= 4, 'midnight wrap')
    # custom regex, lower better
    r = run([p('a'), p('b')], [p('c'), p('d')], [('metric', 'custom'), ('regex', r'p95 ([\d.]+) ms'), ('lower_better', True)])
    check(r['verdict'] == 'on better', 'custom regex')
    # cli errors
    check(ab.main(['--on', p('a')] + ['--off', p('c'), p('d')]) == 2, 'unequal counts exit 2')
    check(ab.main(['--on', p('a')] + ['--off', p('c'), '--metric', 'custom']) == 2, 'custom without regex exit 2')
    # swapping arms flips the verdict and the sign
    f1 = run([p('a'), p('b')], [p('c'), p('d')]); f2 = run([p('c'), p('d')], [p('a'), p('b')])
    check(abs(f1['diff'] + f2['diff']) < 1e-9, 'antisymmetric')
print('abpair test: %s' % ('FAIL (%d)' % fails if fails else 'ok'))
sys.exit(1 if fails else 0)
