#!/usr/bin/env python3
"""Summarise a vgx runtime build and archive the objects: <dir>/libgalaxy_vita_runtime_vgx.a
usage: vita_runtime_report_vgx.py <galaxy-root> <runtime-build-dir>
Exit status 1 when a unit that was attempted failed."""
import re
import subprocess
import sys
from pathlib import Path

out = Path(sys.argv[2]).resolve()
rows = [line.split("\t") for line in (out / "inventory.tsv").read_text().splitlines()[1:]]
good, bad, excluded = [], [], []
first_error = re.compile(r"^(.+?):(\d+):(\d+): (fatal error|error): (.*)$", re.M)
report = []
for source, selection, group, reason in rows:
    obj = out / "objects" / (source + ".obj")
    if selection != "attempt":
        excluded.append((source, reason))
        report.append((source, "excluded", group, reason))
    elif obj.exists():
        good.append(obj)
        report.append((source, "compiled", group, ""))
    else:
        log = Path(str(obj) + ".log")
        text = log.read_text(errors="replace") if log.exists() else ""
        m = first_error.search(text)
        why = f"{m[4]}: {m[5]}  ({m[1]}:{m[2]})" if m else (text.strip().splitlines() or ["no diagnostics captured"])[0]
        bad.append((source, why))
        report.append((source, "FAILED", group, why))
archive = out / "libgalaxy_vita_runtime_vgx.a"
if good and not bad:
    archive.unlink(missing_ok=True)
    subprocess.run(["llvm-ar", "rcs", str(archive), *map(str, sorted(good))], check=True)
(out / "runtime-report.tsv").write_text("source\tstatus\tgroup\treason\n" + "".join("\t".join(r) + "\n" for r in report))
print(f"runtime units: {len(good)} compiled, {len(bad)} failed, {len(excluded)} excluded")
for source, why in bad:
    print(f"  FAILED {source}\n         {why}")
print(f"archive: {archive} ({archive.stat().st_size if archive.exists() else 0} bytes, {len(good)} members)")
sys.exit(1 if bad or not good else 0)
