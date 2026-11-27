#!/usr/bin/env python3
"""A console stand-in for relay tests: binds SMG_LINK_PORT, sends numbered datagrams to every SMG_LINK_BCAST target for HARNESS_SECONDS,
answers nothing, and writes <HARNESS_RUN_DIR>/probe.json {"sent": n, "got": {"<src port>": [seq, ...]}}. Started by run_pair.sh -b."""
import json, os, socket, struct, sys, time

port = int(os.environ['SMG_LINK_PORT'])
targets = [(h, int(p)) for h, p in (t.rsplit(':', 1) for t in os.environ['SMG_LINK_BCAST'].split(','))]
seconds = float(os.environ.get('HARNESS_SECONDS', '5')) - 1.5
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
s.settimeout(0.002)
got, sent, end = {}, 0, time.time() + max(1.0, seconds)
nxt = time.time()
while time.time() < end:
    if time.time() >= nxt:
        for t in targets:
            s.sendto(struct.pack('!I', sent), t)
        sent += 1
        nxt += 0.01
    try:
        d, src = s.recvfrom(2048)
        got.setdefault(str(src[1]), []).append(struct.unpack('!I', d[:4])[0])
    except socket.timeout:
        pass
json.dump({'sent': sent, 'got': got}, open(os.path.join(os.environ['HARNESS_RUN_DIR'], 'probe.json'), 'w'))
