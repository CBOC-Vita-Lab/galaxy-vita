#!/usr/bin/env python3
"""Compile units of the Vita build's runtime stage with a profile and count what Clang says about it.

    python3 -I tools/pgo/check_vita_profile.py --ninja <build>/vita-vgx-runtime/build.ninja --profile <build>/pgo/<hash>.profdata \\
        [--select REGEX] [--units N] [--jobs 1]

--ninja is the build.ninja that tools/vgx_build/vita_runtime_gen_vgx.py wrote (so the units carry exactly the console build's
flags); --profile is a profile written for this checkout's paths (build.py puts it in <build>/pgo/; for a profile recorded elsewhere
use the file that build.py prepared).  Each selected unit is compiled to assembly with -fprofile-use and the warnings are counted:
"hash mismatch" is a function whose control flow differs from the one the profile was recorded for (its counts are ignored),
"out of date" is the summary line Clang prints per unit.  Nothing is linked and no object is kept.
Default selection: the renderer units a profile is applied to (platform/vgx/src, platform/src/gx).  Needs the same environment as
build.py (VITASDK on PATH for the sysroot, CPATH of the case shim)."""
import argparse
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def units(ninja, select):
    rows, source, cmd = {}, None, None
    for line in Path(ninja).read_text().splitlines():
        if line.startswith("build ") and ": cc " in line:
            source, cmd = line.split(": cc ", 1)[1].strip(), None
        elif source and line.startswith("  cmd = "):
            cmd = shlex.split(line[len("  cmd = "):].replace("$$", "$"))
        elif source and cmd is not None and line.startswith("  extra = "):
            if select.search(source) and source.endswith(".cpp"):
                rows[source] = cmd + shlex.split(line[len("  extra = "):].replace("$$", "$"))
            source, cmd = None, None
    return rows


def compile_unit(job):
    source, args, profile = job
    args = [a for a in args if not a.startswith("-fprofile-use")]
    result = subprocess.run(args + [f"-fprofile-use={profile}", "-Wno-error", "-S", "-o", "/dev/null", source],
                            capture_output=True, text=True)
    text = result.stderr
    return (source, result.returncode, len(re.findall(r"hash mismatch", text)),
            len(re.findall(r"profile data may be out of date", text)), len(re.findall(r"no profile data available", text)), text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ninja", required=True)
    ap.add_argument("--profile", required=True)
    ap.add_argument("--select", default=r"/platform/vgx/src/|/platform/src/gx/")
    ap.add_argument("--units", type=int, default=0, help="at most this many units (0: all selected)")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--verbose", action="store_true", help="print the warnings")
    args = ap.parse_args()
    rows = units(args.ninja, re.compile(args.select))
    if not rows:
        sys.exit("no unit selected")
    chosen = sorted(rows)[:args.units] if args.units else sorted(rows)
    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(compile_unit, [(s, rows[s], args.profile) for s in chosen]))
    failed = [r for r in results if r[1]]
    for source, rc, mism, ood, miss, text in results:
        print(f"{'FAILED ' if rc else ''}{Path(source).name}: hash mismatch warnings {mism}, out-of-date summaries {ood}")
        if args.verbose and (mism or ood or rc):
            print(text[-2000:])
    print(f"{len(results)} units compiled with -fprofile-use ({len(failed)} failed): "
          f"{sum(r[2] for r in results)} hash mismatch warnings, {sum(r[3] for r in results)} out-of-date summaries")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
