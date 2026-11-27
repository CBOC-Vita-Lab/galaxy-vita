#!/usr/bin/env python3
"""Correlate guest picture gaps with Vita3K file writes using independent I/O events."""
import argparse
import json
from pathlib import Path
import re
from statistics import median_low


DAY = 86400000
STAMP = re.compile(r"^\[?(\d\d):(\d\d):(\d\d)\.(\d{3})\]? ")
OPEN = re.compile(r"\[export_sceIoOpen\]: Opening file: .*/([^/]+\.arc)$")
LOADED = re.compile(r"\[mp\] mem ui archive request [^ ]+ \([^)]*/([^/)]+\.arc)\) loaded-now=1$")
GAP = re.compile(r"hitch: frame (\d+) (\d+) ms \(back end (\d+)\):")
PART = re.compile(r"stutter: frame (\d+) took \d+ ms in the back end \((in play|quiet)\):.*?present (\d+) ms,")


def clock(line):
    match = STAMP.match(line)
    if not match:
        raise ValueError("expected a timestamp at the selected line")
    h, m, s, ms = map(int, match.groups())
    if h > 23 or m > 59 or s > 59:
        raise ValueError("invalid timestamp")
    return ((h * 60 + m) * 60 + s) * 1000 + ms


def delta(a, b):
    return (a - b + DAY // 2) % DAY - DAY // 2


def correlate(guest, host, anchors, minimum=100):
    if len(anchors) < 2:
        raise ValueError("at least two independently paired archive events are required")
    points = []
    prev = (0, 0)
    for g, h in anchors:
        if not (prev[0] < g <= len(guest) and prev[1] < h <= len(host)):
            raise ValueError("anchors must be distinct, ordered and inside both logs")
        gl, hl = guest[g - 1], host[h - 1]
        ga, ha = LOADED.search(gl), OPEN.search(hl)
        if not ga or not ha or ga[1] != ha[1]:
            raise ValueError("anchors are not matching archive load/open messages")
        points.append((clock(gl), clock(hl)))
        prev = g, h
    start_g, start_h = points[0]
    guest_axis = [delta(g, start_g) for g, _ in points]
    host_axis = [delta(h, start_h) for _, h in points]
    errors = [g - h for g, h in zip(guest_axis, host_axis)]
    if (min(b-a for a, b in zip(guest_axis, guest_axis[1:])) <= 0 or
            guest_axis[-1] < 10000 or max(errors) - min(errors) > 5):
        raise ValueError("unreliable clock match: require separated anchors and <=5 ms spread")
    shift = median_low(errors)
    saves, opened = [], None
    for i, line in enumerate(host, 1):
        if "[save_pipeline_cache]: Saving pipeline cache..." in line:
            opened = (delta(clock(line), start_h) + shift, i)
        elif "[save_pipeline_cache]: Pipeline cache saved" in line and opened is not None:
            end = delta(clock(line), start_h) + shift
            if 0 <= end - opened[0] < 60000:
                saves.append((opened[0], end, opened[1], i))
            opened = None
    details = {}
    for line in guest:
        m = PART.search(line)
        if m:
            details[(int(m[1]), clock(line))] = (m[2], int(m[3]))
    frames = []
    for i, line in enumerate(guest, 1):
        m = GAP.search(line)
        if not m or int(m[2]) < minimum:
            continue
        frame, gap, backend = map(int, m.groups())
        timestamp = clock(line)
        end = delta(timestamp, start_g)
        if not -90000 <= end <= guest_axis[-1] + 90000:
            continue  # Never extrapolate through an unrelated boot or timezone change.
        overlaps = []
        for begin, finish, first, last in saves:
            length = min(end, finish) - max(end-gap, begin)
            if length > 0:
                overlaps.append(dict(host_lines=[first, last], overlap_ms=length,
                                     logged_write_ms=finish-begin, end_before_guest_log_ms=end-finish))
        # These two adjacent guest lines can fall on either side of a millisecond.
        adjacent = [details[(frame, (timestamp + d) % DAY)] for d in (0, 1, 2)
                    if (frame, (timestamp + d) % DAY) in details]
        bucket, present = adjacent[0] if len(adjacent) == 1 else (None, None)
        frames.append(dict(frame=frame, line=i, picture_interval_ms=gap, backend_ms=backend,
                           present_ms=present, renderer_bucket=bucket, overlapping_writes=overlaps))
    return dict(anchor_line_pairs=anchors, anchor_span_ms=guest_axis[-1],
                clock_spread_ms=max(errors)-min(errors), frames=frames,
                scope="Rounded guest picture gaps, not input-to-display latency. Only completed, logged host file "
                      "write intervals count; GPU cache serialization may precede them. Overlap is not causation. "
                      "A renderer in-play flag does not establish that the player had control.")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("guest", type=Path)
    p.add_argument("host", type=Path)
    p.add_argument("--anchor", action="append", required=True, metavar="GUEST_LINE:HOST_LINE")
    args = p.parse_args()
    try:
        pairs = [tuple(map(int, item.split(":"))) for item in args.anchor]
        with args.guest.open(errors="replace") as g, args.host.open(errors="replace") as h:
            report = correlate(g.read().splitlines(), h.read().splitlines(), pairs)
    except (OSError, ValueError) as exc:
        p.exit(2, f"timeline unavailable: {exc}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
