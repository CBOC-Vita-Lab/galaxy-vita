#!/usr/bin/env python3
"""Per-run memory line for the run summary (run_one.sh): what the game's own logs say about its memory, plus the process peaks.

  memsummary.py <run-dir> [--json out.json]     prints one line, e.g.
  mem: banks MEM1 24 MiB MEM2 64 MiB (vita limits ON) | peak RSS 612 MiB, address space 4210 MiB | host heap in use max 58120 KiB |
       free min napa 8123 KiB gddr 11200 KiB cache 1044 KiB | heap errors 0

Read from game.log + console.log (nothing is measured here that the game did not print):
  "mem: native MEM1 0x..+<hex> MEM2 0x..+<hex>"           the bank sizes the process mapped (port_mem_init)
  "host heap ...: ... in use N KiB", "memory: C heap in use N KiB", "heap: scene N destroyed ...: C heap in use N KiB"   host (C heap) use
  "[mp] puppet heap init/play napa=N gddr=N cache=N"      free bytes of the scene Napa heap, scene GDDR and file cache (mp_log, bytes)
  "heap error:", "HEAP FULL", "FATAL: host heap exhausted", "OSPanic"    heap exhaustion (HeapMemoryWatcher's error callback, entry.cpp)
The ceiling flags come from result.json (supervise.py). A binary without the PETARI_VITA_LIMITS hook prints banks of 128 MiB: "vita limits OFF".
"""
import json, os, re, sys


def read(path):
    try:
        return open(path, encoding='utf-8', errors='replace').read().splitlines()
    except OSError:
        return []


def summarize(run):
    lines = read(os.path.join(run, 'game.log')) + read(os.path.join(run, 'console.log'))
    res = {}
    try:
        res = json.load(open(os.path.join(run, 'result.json')))
    except (OSError, ValueError):
        pass
    out = {'mem1_mib': None, 'mem2_mib': None, 'host_heap_in_use_max_kib': None, 'free_min': {}, 'heap_errors': [], 'vita_limits_env': None}
    env = {}
    for l in read(os.path.join(run, 'env.txt')):
        if '=' in l:
            k, v = l.split('=', 1)
            env[k] = v
    out['vita_limits_env'] = env.get('PETARI_VITA_LIMITS') == '1'
    host_max = 0
    for l in lines:
        m = re.search(r'mem: native MEM1 \S+\+([0-9a-f]+) MEM2 \S+\+([0-9a-f]+)', l)
        if m and out['mem1_mib'] is None:
            out['mem1_mib'], out['mem2_mib'] = int(m[1], 16) >> 20, int(m[2], 16) >> 20
        m = re.search(r'(?:host heap [^:]*:.*?|memory: C heap |C heap )in use (\d+) KiB', l)
        if m:
            host_max = max(host_max, int(m[1]))
        m = re.search(r'puppet heap (?:init|play)\b.*?napa=(\d+) gddr=(\d+) cache=(\d+)', l)
        if m:
            for k, v in zip(('napa', 'gddr', 'cache'), m.groups()):
                kib = int(v) // 1024
                out['free_min'][k] = min(out['free_min'].get(k, kib), kib)
        if re.search(r'heap error:|HEAP FULL|FATAL: host heap exhausted|OSPanic', l):
            text = re.sub(r'^(\d{10,}|\d\d:\d\d:\d\d\.\d+)\s+(@f\d+\s+)?', '', l)[:160]   # (game.log and console.log both carry a line: counted once)
            if text not in out['heap_errors'] and len(out['heap_errors']) < 5:
                out['heap_errors'].append(text)
    out['host_heap_in_use_max_kib'] = host_max or None
    out['peak_rss_mib'] = (res.get('peak_hwm_kb') or 0) // 1024 or None
    out['peak_vm_mib'] = (res.get('peak_vm_kb') or 0) // 1024 or None
    out['memory_ceiling_exceeded'] = bool(res.get('memory_ceiling_exceeded'))
    out['as_limit_mib'] = res.get('as_limit_mib')
    out['rss_ceiling_mib'] = res.get('rss_ceiling_mib')
    return out


def line(o):
    if o['mem1_mib'] is None:
        banks = 'banks not logged'
    else:
        limited = o['mem1_mib'] == 24 and o['mem2_mib'] >= 64
        banks = f"banks MEM1 {o['mem1_mib']} MiB MEM2 {o['mem2_mib']} MiB (vita limits {'ON' if limited else 'OFF'}"
        if o['vita_limits_env'] and not limited:
            banks += '; REQUESTED BUT THE BINARY IGNORED IT'
        banks += ')'
    parts = [banks]
    parts.append(f"peak RSS {o['peak_rss_mib']} MiB" + (f", address space {o['peak_vm_mib']} MiB" if o['peak_vm_mib'] else ''))
    if o['host_heap_in_use_max_kib']:
        parts.append(f"host heap in use max {o['host_heap_in_use_max_kib']} KiB")
    if o['free_min']:
        parts.append('free min ' + ' '.join(f'{k} {v} KiB' for k, v in o['free_min'].items()))
    if o['as_limit_mib']:
        parts.append(f"RLIMIT_AS {o['as_limit_mib']} MiB")
    if o['rss_ceiling_mib']:
        parts.append(f"RSS ceiling {o['rss_ceiling_mib']} MiB" + (' EXCEEDED' if o['memory_ceiling_exceeded'] else ''))
    parts.append(f"heap errors {len(o['heap_errors'])}")
    return 'mem: ' + ' | '.join(parts)


if __name__ == '__main__':
    o = summarize(sys.argv[1])
    if '--json' in sys.argv:
        json.dump(o, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
    print(line(o))
