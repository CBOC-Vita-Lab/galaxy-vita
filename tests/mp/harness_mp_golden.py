#!/usr/bin/env python3
"""Golden-log check of the PREPARED multiplayer scenarios (tools/mp/harness/scenarios/mp_*.yaml).

  python3 tests/mp/harness_mp_golden.py [scenario-name ...]

For each scenario, fake host/guest(/bot) processes (harness_fakegame.py) print the "[mp] ..." lines the scenario expects,
with the proposed wording, at plausible times; the scenario's own rules must then all pass. A second, broken variant per
scenario (the guest's key line removed or changed) must fail. This proves the rule sets are consistent and satisfiable,
and documents in executable form the exact log lines the MP agents are asked to print.
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = os.path.join(ROOT, 'tools', 'mp', 'harness')
SC = os.path.join(H, 'scenarios')
FAKE = os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py')
OUT = tempfile.mkdtemp(prefix='harness_golden_')

JOIN_H = ['0.1:[mp] core ready build=test proto=1', '0.2:[mp] link: udp port 41110 up', '0.25:[mp] auto: host started',
          '0.3:[mp] hosting session=0000beef conn=wifi code=', '0.9:[mp] slot 1 joined name=guest rejoin=0 players=2',
          '0.95:[mp] session started (host)', '1.0:[mp] flow session start role=host slot=0 mode={mode}',
          '1.04:[mp] save check limits galaxies=48 gala=48 masks=48 flags=120 values=40 story=40',
          '1.05:[mp] snapshot sent slot=1 bytes=8192 crc=1a2b3c4d', '1.25:[mp] snapshot delivered slot=1 crc=1a2b3c4d',
          '1.3:[mp] puppet send first serial=1 bytes=68', '1.4:[mp] puppet slot=1 state first serial=1', '1.41:[mp] puppet slot=1 appear model=Luigi']
JOIN_G = ['0.1:[mp] core ready build=test proto=1', '0.2:[mp] link: udp port 41111 up', '0.3:[mp] auto: scanning',
          '0.6:[mp] auto: joining host', '0.92:[mp] joined slot=1 session=0000beef host=0 epoch=1 players=2',
          '1.0:[mp] flow session start role=guest slot=1 mode={mode}',
          '1.19:[mp] save check limits galaxies=48 gala=48 masks=48 flags=120 values=40 story=40', '1.2:[mp] snapshot received bytes=8192 crc=1a2b3c4d ok=1',
          '1.22:[mp] save snapshot applied slot=1', '1.3:[mp] puppet send first serial=1 bytes=68',
          '1.35:[mp] puppet slot=0 state first serial=1', '1.36:[mp] puppet slot=0 appear model=Mario']


def lines(*groups, mode=1):
    return ';'.join(l.format(mode=mode) for g in groups for l in g)


MOVE_H = ['2.0:[OS] port: jumping to EggStarGalaxy scenario 1', '2.0:[mp] flow move type=0 galaxy=EggStarGalaxy scen=1 serial=2 role=host',
          '3.3:[mp] flow barrier release serial=2 waited=400ms players=2']
MOVE_G = ['2.1:[mp] flow command move type=0 galaxy=EggStarGalaxy scen=1 sel=0 serial=2',
          '2.15:[mp] flow move type=0 galaxy=EggStarGalaxy scen=1 serial=2 role=guest',
          '3.35:[mp] flow barrier release serial=2 waited=100ms players=2']
BACK_H = ['4.0:[OS] port: jumping to AstroGalaxy scenario 1', '4.0:[mp] flow move type=0 galaxy=AstroGalaxy scen=1 serial=3 role=host',
          '4.9:[mp] flow barrier release serial=3 waited=300ms players=2']
BACK_G = ['4.1:[mp] flow move type=0 galaxy=AstroGalaxy scen=1 serial=3 role=guest',
          '4.95:[mp] flow barrier release serial=3 waited=100ms players=2']
REG = ['3.5:[mp] world reg n=812 crc=0badf00d dups=0 overflow=0 serial=2']

# entry: the entry screen and the menus instead of the autopilot (mp_entry_join)
ENTRY_H = ['0.1:[mp] entry: screen shown cursor=single', '0.2:[mp] entry: chose multiplayer', '0.25:[mp] ui: multiplayer menu from the entry screen',
           '0.4:[mp] ui: single-player file 2 picked for copy', '0.45:[mp] ui: save choice kind=2 coop=1 sp=2',
           '0.5:[mp] hosting session=0000beef conn=wifi code=', '0.9:[mp] slot 1 joined name=guest rejoin=0 players=2',
           '0.95:[mp] ui: host save kind=2 coop=1 sp=2 ready', '1.0:[mp] entry: launch file=1',
           '1.05:[mp] snapshot sent slot=1 bytes=8192 crc=1a2b3c4d', '1.3:[OS] port: player at 2837.5 785.2 -3952.3 (on the ground)',
           '1.41:[mp] puppet slot=1 appear model=Luigi self=0']
ENTRY_G = ['0.1:[mp] entry: screen shown cursor=single', '0.2:[mp] entry: chose multiplayer', '0.25:[mp] ui: multiplayer menu from the entry screen',
           '0.92:[mp] joined slot=1 session=0000beef host=0 epoch=0 players=2', '1.0:[mp] entry: launch file=1',
           '1.2:[mp] snapshot received bytes=8192 crc=1a2b3c4d ok=1', '1.22:[mp] save snapshot applied slot=1',
           '1.36:[mp] puppet slot=0 appear model=Mario self=0']

GOLDEN = {
    'mp_entry_join': ({'host': lines(ENTRY_H), 'guest': lines(ENTRY_G)},
                      ('guest', 'entry: launch file=1', 'entry: chose single')),
    'mp_join': ({'host': lines(JOIN_H), 'guest': lines(JOIN_G)},
                ('guest', 'snapshot received bytes=8192 crc=1a2b3c4d ok=1', 'snapshot received bytes=8192 crc=ffffffff ok=0')),
    'mp_puppet': ({'host': lines(JOIN_H), 'guest': lines(JOIN_G)}, ('host', 'appear model=Luigi', 'appear model=Mario')),
    'mp_move': ({'host': lines(JOIN_H, MOVE_H, BACK_H), 'guest': lines(JOIN_G, MOVE_G, BACK_G)},
                ('guest', 'galaxy=AstroGalaxy scen=1 serial=3 role=guest', 'galaxy=AstroGalaxy scen=1 serial=4 role=guest')),
    'mp_star': ({'host': lines(JOIN_H, MOVE_H, ['3.95:[mp] flow star got id=1 grand=0 by=1', '4.6:[mp] star delivered id=1 path=native',
                                                 '5.0:[mp] snapshot offered bytes=8192 crc=5e5e5e5e',
                                                 '5.0:[mp] flow move type=4 galaxy=AstroGalaxy scen=1 serial=3 role=host']),
                 'guest': lines(JOIN_G, MOVE_G, ['3.9:[mp] flow star got id=1 grand=0 by=1', '4.7:[mp] save suppressed (guest)',
                                                  '5.1:[mp] flow move type=4 galaxy=AstroGalaxy scen=1 serial=3 role=guest'])},
                ('host', 'star delivered id=1 path=native', 'star delivered id=1 path=record')),
    'mp_race': ({'host': lines(JOIN_H, MOVE_H, ['2.05:[mp] race armed serial=2 galaxy=EggStarGalaxy scen=1 sel=1',
                                                 '3.4:[mp] race countdown serial=2 layout=1', '3.5:[mp] race go serial=2',
                                                 '4.52:[mp] race finish slot=1 frames=900', '5.0:[mp] race finish slot=0 frames=1500 star=1',
                                                 '5.2:[mp] race results serial=2 n=2 first=1 frames=900',
                                                 '6.0:[mp] race over serial=2 (moved to AstroGalaxy)'], mode=2),
                 'guest': lines(JOIN_G, MOVE_G, ['2.2:[mp] race armed serial=2 galaxy=EggStarGalaxy scen=1 sel=1',
                                                  '3.55:[mp] race go received serial=2', '4.5:[mp] race finish slot=1 frames=900 star=1',
                                                  '5.25:[mp] race results serial=2 n=2 first=1 frames=900'], mode=2)},
                ('guest', 'race results serial=2 n=2 first=1 frames=900', 'race results serial=2 n=2 first=0 frames=1500')),
    'mp_switch': ({'host': lines(JOIN_H, MOVE_H, REG, ['4.0:[mp] world switch zone=0 bit=5 on=1 src=local']),
                   'guest': lines(JOIN_G, MOVE_G, ['3.6:[mp] world reg n=812 crc=0badf00d dups=0 overflow=0 serial=2',
                                                    '4.2:[mp] world switch zone=0 bit=5 on=1 src=remote'])},
                  ('guest', 'world switch zone=0 bit=5 on=1 src=remote', 'world switch zone=0 bit=6 on=1 src=remote')),
}
# mp_lost_peer: the guest's run is shorter (seconds 4 vs 7)
GOLDEN['mp_lost_peer'] = ({'host': lines(JOIN_H, ['5.5:[mp] slot 1 left reason=lost (no packets) players=1',
                                                   '5.6:[mp] puppet slot=1 hide reason=left']),
                           'guest': lines(JOIN_G)}, ('host', 'slot 1 left reason=lost', 'slot 1 quiet reason=lost'))


def run(name, scripts, seconds):
    cmd = [os.path.join(H, 'run_pair.sh'), '-n', name, '-o', OUT, '-s', '7']
    for label, script in scripts.items():
        cmd += ['-p', f'{label}::0:{seconds.get(label, 7)}', '-P', f'{label}=FAKE_SCRIPT={script}']
    env = dict(os.environ, HARNESS_BIN=FAKE, HARNESS_OUT=OUT)
    subprocess.call(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(OUT, name)


def judge(folder, scenario):
    p = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), folder, '--scenario', scenario, '-q'], env=dict(os.environ, HARNESS_MIN_LINES='0'),
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    names = sys.argv[1:] or sorted(GOLDEN)
    bad = 0
    for name in names:
        scripts, (who, old, new) = GOLDEN[name]
        seconds = {'guest': 4} if name == 'mp_lost_peer' else {}
        sc = os.path.join(SC, name + '.yaml')
        rc, out = judge(run(name, scripts, seconds), sc)
        ok_good = rc == 0
        broken = dict(scripts)
        assert old in broken[who], (name, old)
        broken[who] = broken[who].replace(old, new)
        rc2, out2 = judge(run(name + '-broken', broken, seconds), sc)
        ok_bad = rc2 == 1
        print(f'{"ok  " if ok_good and ok_bad else "FAIL"} {name}: golden {"passes" if ok_good else "FAILS"}, '
              f'broken ({who}: {old!r} -> {new!r}) {"fails as it should" if ok_bad else "PASSES (rules too weak)"}')
        if not ok_good:
            print(out)
        if not ok_bad:
            print(out2)
        bad += not (ok_good and ok_bad)
    print(f'harness_mp_golden: {len(names) - bad}/{len(names)} scenarios ok ({OUT})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
