#!/usr/bin/env python3
"""Checks what diag_stub.mjs received from the game (diag_report.yaml): diag_check.py <stub out dir>. Exit 0 pass / 1 fail."""
import json, os, re, sys
d = sys.argv[1]
bad = 0
def check(ok, what):
    global bad
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok: bad += 1
files = sorted(f for f in os.listdir(d) if re.fullmatch(r'report-\d+\.json', f))
check(len(files) == 2, 'two report posts (429 then 201): %s' % files)
for f in files:
    raw = open(os.path.join(d, f), 'rb').read()
    try:
        b = json.loads(raw.decode('utf-8'))
    except Exception as e:
        check(False, f + ' is JSON: %s' % e); continue
    log = b.get('log', '')
    first = f == files[0]
    check(b.get('v') == 1 and b.get('kind') == ('after_error' if first else 'user'), f + ' v/kind (%s)' % b.get('kind'))
    check(b.get('where') == ('post_session' if first else 'mp_menu'), f + ' where=%s' % b.get('where'))
    check(b.get('note') == ('it froze after the star' if first else ''), f + ' note=%r (a report begun from Having Trouble? starts without a note)' % b.get('note'))
    if first:   # (the offer's report: no check had been run yet)
        check(b.get('check') is None, f + ' check is null (none run yet)')
    else:
        check(isinstance(b.get('check'), dict) and b['check'].get('relay') == 'ok' and b['check'].get('nat') in ('open', 'cone', 'symmetric', 'unknown'), f + ' check=%s' % b.get('check'))
    st = b.get('stats')
    check(isinstance(st, dict) and st.get('transport') == 'relay' and st.get('ended') == 'error:sync' and st.get('relay_used') is True, f + ' stats=%s' % st)
    check(len(log.encode()) <= 48000 and len(log) > 200, f + ' log %d bytes (cap 48000)' % len(log.encode()))
    check(not re.search(r'\b\d{1,3}(\.\d{1,3}){3}\b', log), f + ' log has no IPv4 address')
    check(not re.search(r'[0-9a-fA-F]{32,}', log), f + ' log has no 32+ hex run')
    check(not re.search(r'[\w.+-]+@[\w-]+\.[\w.]+', log), f + ' log has no e-mail')
    check('Bearer' not in log, f + ' log has no token')
    hd = json.load(open(os.path.join(d, f.replace('.json', '.headers.json'))))
    check(str(hd.get('authorization', '')).startswith('Bearer ') and hd.get('x-msc-game') == 'smg', f + ' normal player headers')
    print('     log lines: %d, first: %s' % (log.count('\n'), log.split('\n')[0][:100]))
sl = open(os.path.join(d, 'stub.log')).read()
check('turn check minted' in sl, 'the game asked /v1/turn/check')
check(re.search(r'turn allocate client=', sl) is not None, 'TURN allocate seen')
check(re.search(r'turn permission ip=127\.0\.0\.1', sl) is not None, 'CreatePermission for our own (public) address')
check(re.search(r'turn data-ind from=127\.0\.0\.1:\d+', sl) is not None, 'probes from the second socket came back as Data indications')
check('turn released' in sl, 'the allocation was released')
check(re.search(r'turn permission ip=127\.0\.0\.2', sl) is not None, 'a second permission for the probe socket\'s own public IP (STUN said it differs)')
check('turn drop (no permission)' not in sl, 'no probe was dropped for lack of permission')
sys.exit(1 if bad else 0)
