#!/usr/bin/env python3
"""stdio31: checks the console text of a GXV_STDIO_SELFTEST=1 run (vita/vgx-game/stdio_safe.cpp).

Usage: python3 tools/stdio31_check_console.py ux0-data-smg/stderr.txt
(the run's length is read from log.txt: one "stdio-selftest: <n> min" line a minute)

stderr.txt is what GXV_STDERR_FILE=1 writes. Each self-test line is
    stdio-selftest t<k> #<seq> <text> sum=<hex4>          (threads 0 and 1, one line every 16 ms each)
    stdio-selftest t<k> #<seq>.long <text> sum=<hex4>     (every 100th line of each of the three threads: 1300 bytes)
The check: every line is whole (its sum matches), threads 0 and 1 have every sequence number from their first to their last exactly once and in order,
and every thread's long lines are there (seq 100, 200, ...). Exit status 0 = clean, 1 = lost / torn / repeated lines, 2 = no self-test lines.
"""
import argparse
import re
import sys

LINE = re.compile(rb"stdio-selftest t(\d) #(\d+)(\.long)? (.*) sum=([0-9a-f]{4})$")


def fnv16(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h & 0xFFFF


def check(raw):
    seqs = {0: [], 1: [], 2: []}
    longs = {0: [], 1: [], 2: []}
    torn = 0
    stray = 0
    rows = raw.split(b"\n")
    for i, row in enumerate(rows):
        if b"stdio-selftest" not in row:
            continue
        m = LINE.search(row)
        if not m or m.start() != 0:
            if i == len(rows) - 1:
                continue  # (the file's last line, cut where the run was stopped: not counted)
            torn += 1  # (cut, merged with another line, or bytes lost)
            continue
        k, seq, is_long, _, want = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4), int(m.group(5), 16)
        body = row[: row.rindex(b" sum=")]
        if is_long:
            ok = len(body) == 1300 and fnv16(body) == want
        else:
            ok = fnv16(body) == want
        if not ok or k not in seqs:
            torn += 1
            continue
        (longs if is_long else seqs)[k].append(seq)
        if k == 2 and not is_long:
            stray += 1  # (thread 2 prints to stdout: its short lines are not expected here)
    report = {"torn": torn, "stray": stray, "threads": {}}
    bad = torn
    total = 0
    for k in (0, 1):
        s = seqs[k]
        total += len(s)
        last = max(s) if s else 0
        first = min(s) if s else 1  # (lines printed before stderr was reopened on stderr.txt are elsewhere)
        repeated = len(s) - len(set(s))
        out_of_order = sum(1 for a, b in zip(s, s[1:]) if b <= a)
        lost = (last - first + 1) - len(set(s)) if s else 0
        report["threads"][k] = {"lines": len(s), "last": last, "lost": lost, "repeated": repeated, "out_of_order": out_of_order}
        bad += lost + repeated + out_of_order
    for k in (0, 1, 2):
        last = max(seqs[k]) if seqs[k] else (max(longs[k]) if longs[k] else 0)
        first = min(seqs[k]) if seqs[k] else (min(longs[k]) if longs[k] else 1)
        expect = set(range(((first + 99) // 100) * 100, (last // 100) * 100 + 1, 100))
        got = set(longs[k])
        missing = len(expect - got)
        report["threads"].setdefault(k, {})["long_lines"] = len(longs[k])
        report["threads"][k]["long_missing"] = missing
        bad += missing
    if total == 0:
        return 2, report
    return (1 if bad else 0), report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stderr_txt")
    a = ap.parse_args()
    with open(a.stderr_txt, "rb") as f:
        raw = f.read()
    status, report = check(raw)
    print(report)
    print({0: "clean", 1: "LOST / TORN / REPEATED LINES", 2: "no stdio-selftest lines (GXV_STDIO_SELFTEST=1 and GXV_STDERR_FILE=1 on a --logging build?)"}[status])
    return status


if __name__ == "__main__":
    sys.exit(main())
