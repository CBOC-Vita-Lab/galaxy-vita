#!/usr/bin/env python3
"""tools/mp/harness/sweep.sh without a game: a fake run_scenario.py (SWEEP_RUNNER) gives scripted verdicts and writes a run folder with a
verdict.json. Checks: the table, INVALID rerun up to 2 times then kept, a PASS folder deleted, a FAIL folder kept with its failed rules, SKIP
not rerun, the load wait (a fake loadavg is not possible, so SWEEP_MAX_LOAD=1000 / SWEEP_LOAD_WAIT_MAX=1 with a tiny limit)."""
import json, os, subprocess, sys, tempfile, textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
SWEEP = os.path.join(HERE, '..', '..', 'tools', 'mp', 'harness', 'sweep.sh')

FAKE = textwrap.dedent('''\
    #!/usr/bin/env python3
    import json, os, sys
    args = sys.argv[1:]
    out = args[args.index('-o') + 1] if '-o' in args else '.'
    name = os.path.splitext(os.path.basename(args[-1]))[0]
    state = os.path.join(out, '.count-' + name)
    n = int(open(state).read()) if os.path.exists(state) else 0
    open(state, 'w').write(str(n + 1))
    script = {'p_pass': ['PASS'], 'p_inv': ['INVALID', 'INVALID', 'PASS'], 'p_inv3': ['INVALID'] * 5, 'p_fail': ['FAIL'], 'p_skip': ['SKIP']}[name]
    v = script[min(n, len(script) - 1)]
    folder = os.path.join(out, name + '-%d' % n)
    print('run_scenario: fake')
    print('\\n== scenarios')
    if v == 'SKIP':
        print('SKIP  %-28s binary lacks X  ' % name)
    else:
        os.makedirs(folder)
        res = [{'rule': 'expect host /a/', 'ok': True, 'soft': False, 'detail': ''}]
        if v != 'PASS':
            res.append({'rule': 'expect guest /b/', 'ok': False, 'soft': False, 'detail': 'x'})
            res.append({'rule': '?expect guest /c/', 'ok': False, 'soft': True, 'detail': 'x'})
        json.dump({'results': res, 'verdict': v, 'machine': {'procs': [{'name': 'host', 'speed': 0.9}, {'name': 'guest', 'speed': 0.75}]}},
                  open(os.path.join(folder, 'verdict.json'), 'w'))
        print('%-5s %-28s 1 s wall, runs ok  %s' % (v, name, folder))
    sys.exit(0 if v == 'PASS' else 1)
''')


def main():
    with tempfile.TemporaryDirectory() as d:
        runner = os.path.join(d, 'fake_run_scenario.py')
        open(runner, 'w').write(FAKE)
        os.chmod(runner, 0o755)
        status = os.path.join(d, 'status.tsv')
        env = dict(os.environ, SWEEP_RUNNER=runner, SWEEP_GAMERUN='', SWEEP_MAX_LOAD='100000', SWEEP_STATUS=status)
        runs = os.path.join(d, 'runs')
        os.makedirs(runs)
        names = ['p_pass', 'p_inv', 'p_inv3', 'p_fail', 'p_skip']
        r = subprocess.run(['bash', SWEEP, '-d', runs] + names, env=env, capture_output=True, text=True)
        rows = {l.split('\t')[0]: l.rstrip('\n').split('\t') for l in open(status).read().splitlines()[1:]}
        assert set(rows) == set(names), rows
        assert rows['p_pass'][1] == 'PASS' and rows['p_pass'][5] == '1' and rows['p_pass'][6] == '(deleted)', rows['p_pass']
        assert not any(x.startswith('p_pass-') for x in os.listdir(runs)), 'a PASS folder must be deleted'
        assert rows['p_inv'][1] == 'PASS' and rows['p_inv'][5] == '3', rows['p_inv']          # INVALID twice, then PASS on the third attempt
        assert rows['p_inv3'][1] == 'INVALID' and rows['p_inv3'][5] == '3', rows['p_inv3']    # reruns are limited to 2
        assert os.path.isdir(rows['p_inv3'][6]), 'an INVALID folder is kept'
        assert rows['p_fail'][1] == 'FAIL' and rows['p_fail'][3] == '0.75' and rows['p_fail'][4] == 'expect guest /b/', rows['p_fail']
        assert rows['p_skip'][1] == 'SKIP' and rows['p_skip'][5] == '1', rows['p_skip']
        assert r.returncode != 0, 'a sweep with a FAIL / INVALID returns non-zero'
        # the default list: 21 scenarios of the audit, every one has a file
        lst = [l.split('#')[0].strip() for l in open(os.path.join(os.path.dirname(SWEEP), 'sweep_order.txt'))]
        lst = [l for l in lst if l]
        assert len(lst) == 21, len(lst)
        for n in lst:
            assert os.path.exists(os.path.join(os.path.dirname(SWEEP), 'scenarios', n + '.yaml')), n
    print('sweep_test: PASS')


main()
