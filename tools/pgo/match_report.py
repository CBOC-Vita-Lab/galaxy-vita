#!/usr/bin/env python3
"""How much of a profile recorded on the armhf harness applies to the PS Vita build.

For a random sample of game units, both targets are compiled with -fprofile-generate (IR output only, nothing is
linked).  An IR profile is keyed by function name and a hash of the function's control-flow graph, both of which
are in the instrumented IR, so the two targets can be compared without running anything.  The profile (merged
.profdata, or the text form of it) then supplies the counts: a function of the Vita build is "matched" if the armhf
build has the same name and CFG hash, and its weight is the sum of its counters in the profile.

    python3 -I tools/pgo/match_report.py --vita-db <vita-source>/compile_commands.json \\
        --armhf-db <armhf-obj>/compile_commands.json --profile game.profdata [--units 120] [--seed 1] [--jobs 1]

Both databases must come from the same checkout (the paths of the sources are used as they are); CPATH for the case-
alias headers (tools/vgx_build/make_case_shim.py) is taken from the environment.  Extra compiler flags for the IR
pass can be given with --flags.  Prints a summary; --json writes the per-function rows."""
import argparse
import hashlib
import json
import os
import random
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PROFD = re.compile(r"^@__profd_(\S+) = .*?\{ i64 (-?\d+), i64 (-?\d+), ", re.M)
PROFC = re.compile(r"^@__profc_(\S+) = .*?\[(\d+) x i64\]", re.M)


def load(db):
    rows = {}
    for row in json.loads(Path(db).read_text()):
        if "/decomp/" not in row["file"]:
            continue
        rows[row["file"]] = row.get("arguments") or shlex.split(row["command"])
    return rows


def ir_command(args, source, out, flags):
    args = list(args)
    if "-o" in args:
        i = args.index("-o")
        del args[i:i + 2]
    if "-c" in args:
        i = args.index("-c")
        del args[i:i + 2]
    cleaned, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a in ("-MF", "-MT", "-MQ"):
            skip = True
        elif a not in ("-MD", "-MMD") and not a.startswith(("-fprofile-use", "-fprofile-generate")):
            cleaned.append(a)
    return cleaned + flags + ["-S", "-emit-llvm", "-o", str(out), str(source)]


def keys_of(ir):
    """{name hash: (CFG hash, counters)} of the instrumented functions in an IR text."""
    counters = {name: int(n) for name, n in PROFC.findall(ir)}
    out = {}
    for name, name_hash, cfg_hash in PROFD.findall(ir):
        out[int(name_hash)] = (int(cfg_hash), counters.get(name, 0))
    return out


def compile_unit(job):
    source, args, work, tag, flags = job
    out = Path(work) / (hashlib.sha1((tag + source).encode()).hexdigest()[:12] + ".ll")
    result = subprocess.run(ir_command(args, source, out, flags), capture_output=True, text=True,
                            preexec_fn=lambda: os.nice(15))
    if result.returncode:
        return source, tag, None, result.stderr[-300:]
    return source, tag, keys_of(out.read_text()), ""


def profile_records(path):
    """{MD5 name hash: (CFG hash, [counters])} from a .profdata (via llvm-profdata) or its text form."""
    path = Path(path)
    if path.read_bytes()[:8] == b"\xfflprofi\x81":
        tool = next((t for t in ("llvm-profdata", "llvm-profdata-18") if subprocess.run(
            ["which", t], capture_output=True).returncode == 0), None)
        if not tool:
            sys.exit("llvm-profdata not found")
        text = subprocess.run([tool, "merge", "--text", "-o", "-", str(path)], capture_output=True, text=True,
                              check=True).stdout
    else:
        text = path.read_text()
    records, lines, i = {}, text.splitlines(), 0
    while i < len(lines):
        line = lines[i]
        if line and not line.startswith(("#", ":")) and i + 7 < len(lines) and lines[i + 1].startswith("# Func Hash"):
            cfg = int(lines[i + 2])
            n = int(lines[i + 4])
            counts = [int(x) for x in lines[i + 6:i + 6 + n]]
            name_hash = struct.unpack("<q", hashlib.md5(line.encode()).digest()[:8])[0]
            records[name_hash] = (cfg, counts)
            i += 6 + n
        else:
            i += 1
    return records


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vita-db", required=True)
    ap.add_argument("--armhf-db", required=True)
    ap.add_argument("--profile", required=True)
    ap.add_argument("--units", type=int, default=120, help="sample size (0: every unit)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--flags", default="-flto=thin")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    vita, arm = load(args.vita_db), load(args.armhf_db)
    files = sorted(set(vita) & set(arm))
    random.seed(args.seed)
    sample = files if args.units <= 0 else random.sample(files, min(args.units, len(files)))
    profile = profile_records(args.profile)
    flags = ["-fprofile-generate"] + args.flags.split()
    with tempfile.TemporaryDirectory() as work:
        jobs = [(f, db[f], work, tag, flags) for f in sample for tag, db in (("vita", vita), ("arm", arm))]
        with ThreadPoolExecutor(args.jobs) as pool:
            results = list(pool.map(compile_unit, jobs))
    per = {}
    failed = 0
    for source, tag, keys, err in results:
        if keys is None:
            failed += 1
            print(f"compile failed ({tag}) {source}: {err}", file=sys.stderr)
        else:
            per.setdefault(source, {})[tag] = keys
    n_arm = n_vita = same = diff_hash = only_arm = only_vita = 0
    weight_arm = weight_same = executed_arm = executed_same = 0
    rows = []
    for source, tags in sorted(per.items()):
        if "arm" not in tags or "vita" not in tags:
            continue
        a, v = tags["arm"], tags["vita"]
        n_arm += len(a)
        n_vita += len(v)
        only_vita += sum(1 for k in v if k not in a)
        for k, (cfg, _) in a.items():
            rec = profile.get(k)
            weight = sum(rec[1]) if rec and rec[0] == cfg else 0
            hot = weight > 0
            weight_arm += weight
            executed_arm += hot
            if k in v and v[k][0] == cfg:
                same += 1
                weight_same += weight
                executed_same += hot
                verdict = "match"
            elif k in v:
                diff_hash += 1
                verdict = "hash"
            else:
                only_arm += 1
                verdict = "arm-only"
            rows.append((source, hex(k & (2**64 - 1)), verdict, weight))
    print(f"sample: {len(per)} units ({failed} compile failures); seed {args.seed}")
    print(f"functions instrumented: armhf {n_arm}, vita {n_vita}")
    print(f"  same name and CFG hash on both: {same} ({100.0 * same / max(n_arm, 1):.2f} % of armhf)")
    print(f"  same name, different CFG hash:  {diff_hash}; only in armhf: {only_arm}; only in vita: {only_vita}")
    print(f"  executed in the profile: armhf {executed_arm}; of them matched on vita {executed_same} "
          f"({100.0 * executed_same / max(executed_arm, 1):.2f} %)")
    print(f"  counter-weighted match of the executed functions: {100.0 * weight_same / max(weight_arm, 1):.3f} %")
    if args.json:
        args.json.write_text(json.dumps(rows))


if __name__ == "__main__":
    main()
