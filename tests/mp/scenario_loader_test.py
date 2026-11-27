#!/usr/bin/env python3
"""A link event key `on:` must survive the YAML loader as the string 'on' (not True), in every emu_*/qa_* scenario."""
import glob, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'mp', 'harness'))
import run_scenario as rs

def walk(v, bad, path=''):
    if isinstance(v, dict):
        for k, x in v.items():
            if isinstance(k, bool): bad.append(path + '/' + str(k))
            walk(x, bad, path + '/' + str(k))
    elif isinstance(v, list):
        for i, x in enumerate(v): walk(x, bad, path + f'[{i}]')

def main():
    d = os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'mp', 'harness', 'scenarios')
    n = events = 0
    for f in sorted(glob.glob(d + '/*.yaml')):
        sc = rs.load_scenario(f); bad = []; walk(sc, bad)
        assert not bad, (f, bad)
        link = sc.get('link')
        for ev in (link.get('events', []) if isinstance(link, dict) else []):
            assert 'on' in ev or 'at' in ev or 'from' in ev, (f, ev); events += 'on' in ev
        n += 1
    assert events >= 3, events
    print(f'scenario_loader_test: {n} scenarios, {events} link events keep their "on" key: PASS')
main()
