#!/usr/bin/env python3
"""A stand-in for the desktop game (platform/desktop/main.cpp) for the harness self-test.

  harness_fakegame.py <cooked-disc> <run-dir> <seconds>

Like the game: game.log ("HH:MM:SS.mmm text"), every line also on stderr, shots/frame_NNNN.png, exit 0 at the end, exit 1
without a shot. FAKE_SCRIPT="t:text;t:text" logs text t seconds after start (port_log: game.log + stderr);
"t:!text" writes text to stderr only (like the weak mp_log). FAKE_CRASH=t prints the desktop crash line at t and exits 139.
FAKE_SPEED=x stamps every game.log line "@f<frame>" as a game running at x times real time. FAKE_EXIT_AT=t ends the run with exit 0 at t seconds. FAKE_ALLOC_MB=n touches n MiB of memory at 0.3 s (a RLIMIT_AS limit makes it die with MemoryError, exit 1). FAKE_SAVE_WRITE=t appends a byte to the single-player GameData.bin at t. SIGTERM ends it early (exit 0).
"""
import datetime, os, signal, sys, time

data, run, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
os.makedirs(os.path.join(run, 'shots'), exist_ok=True)
log = open(os.path.join(run, 'game.log'), 'w')
stop = False


def on_term(*_):
    global stop
    stop = True


signal.signal(signal.SIGTERM, on_term)


def port_log(text, file=True):
    if file:
        now = datetime.datetime.now()
        stamp = f'@f{int((time.time() - start) * 59.94 * speed)} ' if speed else ''   # (FAKE_SPEED=x: PETARI_LOG_FRAMES stamps of a game running at x times real time)
        log.write(now.strftime('%H:%M:%S.') + f'{now.microsecond // 1000:03d} {stamp}{text}\n')
        log.flush()
    sys.stderr.write(text + '\n')
    sys.stderr.flush()


events = []
for item in filter(None, os.environ.get('FAKE_SCRIPT', '').split(';')):
    t, text = item.split(':', 1)
    events.append((float(t), text))
events.sort()
exit_at = float(os.environ['FAKE_EXIT_AT']) if os.environ.get('FAKE_EXIT_AT') else None   # clean exit 0 at t (an early clean exit)
crash = float(os.environ['FAKE_CRASH']) if os.environ.get('FAKE_CRASH') else None
save_write = float(os.environ['FAKE_SAVE_WRITE']) if os.environ.get('FAKE_SAVE_WRITE') else None
hog = []
speed = float(os.environ.get('FAKE_SPEED', '0') or 0)
start = time.time()
port_log(f'desktop: fake game data={data} input={os.environ.get("PETARI_INPUT", "")} online={os.environ.get("SMG_ONLINE_URL", "")}')
shots = 0
next_shot = 0.5
next_tick = 0.5
while not stop:
    el = time.time() - start
    if el >= seconds:
        break
    while events and events[0][0] <= el:
        _, text = events.pop(0)
        port_log(text[1:], file=False) if text.startswith('!') else port_log(text)
    if os.environ.get('FAKE_ALLOC_MB') and el >= 0.3 and not hog:
        try:
            hog.append(bytearray(b'\1') * (int(os.environ['FAKE_ALLOC_MB']) << 20))
        except MemoryError:
            port_log('FATAL: host heap exhausted: a request found no room (fake)')
            os._exit(1)
    if exit_at is not None and el >= exit_at:
        break
    if crash is not None and el >= crash:
        sys.stderr.write('desktop crash signal=0x000000000000000b address=0x0000000000000000 pc=0x0000000098001000 sp=0x00007ffd00000000\n')
        sys.stderr.flush()
        os._exit(139)
    if save_write is not None and el >= save_write:
        save_write = None
        with open(os.path.join(run, 'saves/title/00010000/524d4750/data/GameData.bin'), 'ab') as f:
            f.write(b'\0')
    if speed and el >= next_tick:   # (a console that logs little: a tick line keeps its frame stamp moving)
        next_tick += 0.5
        port_log('fake tick', file=True)
    if el >= next_shot:
        open(os.path.join(run, 'shots', f'frame_{shots:04d}.png'), 'wb').write(b'\x89PNG\r\n\x1a\n')
        shots += 1
        next_shot += 0.5
    time.sleep(0.02)
port_log(f'desktop: terminal captures={shots}')
sys.exit(0 if shots else 1)
