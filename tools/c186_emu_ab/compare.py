"""Compare complete scene-aligned play windows, failing closed on a mismatched arm."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib
from replay import validate

FPS = re.compile(r" fps: ([\d.]+) s: shown ([\d.]+) fps; game ([\d.]+) turns/s; (\d+) skipped; (\d+) hitches over 100 ms \(longest (\d+) ms\); picture (\d+x\d+)")
ANCHOR = "script: clock started: t = 0 at the first play turn in HoneyBeeKingdomGalaxy scenario 1"
LOAD = "load-split: scene build of HoneyBeeKingdomGalaxy"


def world(path):
    if not path.is_file():
        return None
    # These are 320x182 filter-0 RGB8 PNGs emitted by witness.py (not arbitrary images).
    p = path.read_bytes()
    if p[:8] != b"\x89PNG\r\n\x1a\n" or p[16:29] != struct.pack(">IIBBBBB", 320,182,8,2,0,0,0):
        raise ValueError("Wrong thumbnail layout")
    data = zlib.decompress(p[41:41+struct.unpack(">I", p[33:37])[0]])
    if len(data) != 182*961 or any(data[y*961] for y in range(182)):
        raise ValueError("Unexpected thumbnail scanlines")
    pixels = [data[y*961+1+x*3:y*961+4+x*3] for y in range(36,146,2) for x in range(56,264,2)]
    return {"distinct": len(set(pixels)), "lit": round(sum(max(c)>28 for c in pixels)/len(pixels),4),
            "colorful": round(sum(max(c)-min(c)>20 for c in pixels)/len(pixels),4)}


def stamp(line):
    h, m, sec = line.split(" ", 1)[0].split(":")
    return int(h)*3600 + int(m)*60 + float(sec)


def summary(folder, label):
    d = folder / label
    identity = json.loads((d / "inputs.json").read_text())
    text = (d / "env.txt").read_bytes()
    validate(label, text.decode())
    if identity.pop("env") != hashlib.sha256(text).hexdigest():
        raise ValueError("Environment SHA mismatch")
    outcome = json.loads((d / "outcome.json").read_text())
    samples = (d / "guest-samples.txt").read_text().splitlines()
    if (d / "modekeys.txt").is_file():
        samples.extend((d / "modekeys.txt").read_text().splitlines())
    issues = []
    for point in ("play", "return"):
        visual = world(d / (point + "-world.png"))
        if not visual or visual["distinct"] < 64 or visual["lit"] < .10 or visual["colorful"] < .05:
            issues.append(f"{point} framebuffer missing/blank or insufficient scene color")
    if outcome["reason"] != "route-complete" or outcome["container_exit"] != 0:
        issues.append("incomplete first traversal")
    if not any("3.0-C186.1" in line for line in samples):
        issues.append("C186.1 boot missing")
    mode = "0" if label == "B" else "5"
    # Env overrides are logged as GXV_HRW=0(the ...); reject 05 or another key.
    effective = re.compile(r"\sGXV_HRW=" + mode + r"(?=\s|\(|$)")
    if not any("modekeys: Performance mode:" in l and effective.search(l) for l in samples):
        issues.append("effective HRW mode not confirmed")
    anchors = [i for i, line in enumerate(samples) if ANCHOR in line]
    if len(anchors) < 2:
        return identity, {"issues": issues + ["missing matching end anchor"]}
    start = stamp(samples[anchors[0]])
    between = samples[anchors[0]+1:anchors[1]]
    load = next((l for l in between if LOAD in l), None)
    if not load:
        return identity, {"issues": issues + ["missing first retry load marker"]}
    # The loader reports completion and the preceding scene-build wall time.
    # Exclude an extra second for delayed rounding and overlap around that marker.
    end = stamp(load) - float(re.search(r"wall ([\d.]+) s", load)[1]) - 1
    windows, edges = [], []
    for line in between:
        match = FPS.search(line)
        if not match:
            continue
        seconds, fps, turns, skipped, hitches, longest = map(float, match.groups()[:6])
        t = stamp(line)
        if t-seconds >= start + .06 and t <= end:
            if seconds <= 0 or (edges and abs(t-seconds - edges[-1][1]) > .12):
                issues.append("missing, overlapping or out-of-order play windows")
            windows.append((seconds, fps, turns, skipped, hitches, longest))
            edges.append((t-seconds, t))
            if match[7] != "960x544":
                issues.append("not the expected picture size")
    if len(windows) < 3:
        return identity, {"issues": issues + ["fewer than three complete play windows"]}
    dt = sum(w[0] for w in windows)
    derived = sum(w[0]*w[2] for w in windows)
    if dt <= 0 or end <= start or round(derived) <= 0:
        return identity, {"issues": issues + ["no positive sampled duration and game progress"]}
    return identity, {"issues": issues, "windows": len(windows), "seconds": round(dt, 2),
                      "play_to_retry_seconds": round(end-start, 3),
                      "first_sample_start_offset_s": round(edges[0][0]-start, 3),
                      "last_sample_end_offset_s": round(edges[-1][1]-start, 3),
                      "sampled_turns_approx": round(derived),
                      "shown_fps": round(sum(w[0]*w[1] for w in windows)/dt, 3),
                      "game_turns_per_s": round(derived/dt, 3), "hitches_gt100ms": int(sum(w[4] for w in windows)),
                      "worst_hitch_ms": int(max(w[5] for w in windows))}


def compare(folder):
    a, ra = summary(folder, "A")
    b, rb = summary(folder, "B")
    failures = list(dict.fromkeys(ra["issues"] + rb["issues"]))
    if a != b:
        failures.append("identities differ beyond HRW intervention")
    if not failures:
        ta, tb = ra["sampled_turns_approx"], rb["sampled_turns_approx"]
        frac = max(ta, tb) / min(ta, tb) - 1
        # Different input progress would confound a timing delta.
        if frac > .08:
            failures.append("more than 8% difference in covered turns")
        # Normalized positions are a phase check, not proof of exact turn or pixel
        # alignment. Whole-route speed may legitimately differ between the arms.
        da, db = ra["play_to_retry_seconds"], rb["play_to_retry_seconds"]
        tolerance = max(.08, 1 / min(da, db))
        for edge in ("first_sample_start_offset_s", "last_sample_end_offset_s"):
            if abs(ra[edge] / da - rb[edge] / db) > tolerance:
                failures.append("sampled route phase differs at " + edge)
    data = {"profile": "emulator / retail shader profile / Linux CPU software rendering", "A_assist_on": ra,
            "B_assist_off": rb, "comparison_accepted": not failures, "limitations": failures,
            "phase_basis": "Guest-time fraction of anchor-to-retry proxy; not proof of per-turn alignment."}
    if not failures:
        data["on_minus_off_shown_fps"] = round(ra["shown_fps"] - rb["shown_fps"], 3)
    return data


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("folder", type=Path)
    print(json.dumps(compare(p.parse_args().folder), indent=2))
