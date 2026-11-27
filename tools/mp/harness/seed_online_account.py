#!/usr/bin/env python3
"""Pre-registers one console's online account on the LOCAL fake service (127.0.0.1 only) and writes the game's account file there.
  seed_online_account.py <http://127.0.0.1:PORT> <online dir> <name>
Writes <online dir>/online.dat in the layout of platform/src/mp/mp_service.cpp (SaveAccount: 'SMGO', version 1, pid, token[64], name[20],
code[12], crc32 of everything before it, all little endian). Prints the friend code. Refuses any address that is not plain http://127.0.0.1."""
import json, os, struct, sys, urllib.request, zlib

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    url, folder, name = sys.argv[1].rstrip('/'), sys.argv[2], sys.argv[3]
    if not url.startswith('http://127.0.0.1:'):
        sys.exit('seed_online_account: only the local fake service (http://127.0.0.1:PORT) is allowed, got ' + url)
    req = urllib.request.Request(url + '/v1/register', data=json.dumps({'name': name}).encode(), method='POST',
                                 headers={'Content-Type': 'application/json', 'X-Msc-Game': 'smg', 'X-Msc-Proto': '1', 'X-Msc-App': '3.0.0'})
    a = json.load(urllib.request.urlopen(req, timeout=15))
    token, code = a['token'].encode(), a['code'].encode()
    buf = bytearray(4 + 4 + 4 + 64 + 20 + 12 + 4)
    buf[0:4] = b'SMGO'
    struct.pack_into('<II', buf, 4, 1, a['pid'])
    buf[12:12 + len(token)] = token
    buf[76:76 + len(a['name'].encode())] = a['name'].encode()
    buf[96:96 + len(code)] = code
    struct.pack_into('<I', buf, len(buf) - 4, zlib.crc32(bytes(buf[:-4])) & 0xFFFFFFFF)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'online.dat'), 'wb') as f:
        f.write(buf)
    print(a['code'])

main()
