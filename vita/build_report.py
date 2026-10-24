#!/usr/bin/env python3
"""Audit the entire compile database, not just Ninja's final progress line."""
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def arguments(entry):
    return entry.get("arguments") or shlex.split(entry["command"])


def compile_identity(command):
    # CMake's database omits Ninja's dependency-output arguments. They do not
    # change generated code; preserve every other flag in its original order.
    identity = []
    index = 0
    while index < len(command):
        argument = command[index]
        if argument in ("-MF", "-MT", "-MQ"):
            index += 2
        elif argument in ("-MD", "-MMD"):
            index += 1
        else:
            identity.append(argument)
            index += 1
    return identity


def inspect_unit(entry):
    command = arguments(entry)
    output = Path(entry["directory"]) / command[command.index("-o") + 1]
    source = Path(entry["file"])
    unit = {"source": str(source), "output": str(output)}
    record_path = Path(str(output) + ".result.json")
    if not record_path.exists():
        return dict(unit, status="unattempted")
    record = json.loads(record_path.read_text())
    unit["log"] = record["log"]
    unit["compiler_exit_code"] = record["exit_code"]
    if (compile_identity(record["command"]) != compile_identity(command) or record["source_changed_during_compile"] or
            record["source_sha256"] != digest(source)):
        return dict(unit, status="stale")
    if record["exit_code"] != 0:
        return dict(unit, status="failed", exit_code=record["exit_code"])
    if not output.exists():
        return dict(unit, status="invalid_object", reason="successful record without object")
    with output.open("rb") as stream:
        header = stream.read(52)
    if (len(header) < 52 or header[:7] != b"\x7fELF\x01\x01\x01" or
            struct.unpack_from("<HH", header, 16) != (1, 40) or
            struct.unpack_from("<I", header, 36)[0] >> 24 != 5):
        return dict(unit, status="invalid_object", reason="not ELF32 little-endian ARM EABI5 relocatable")
    return dict(unit, status="completed", object_sha256=digest(output))


def source_snapshot(root):
    files = {"CMakeLists.txt": digest(root / "CMakeLists.txt")}
    for directory in (root / "decomp", root / "platform/include", root / "vita"):
        for path in sorted(directory.rglob("*")):
            if path.is_relative_to(root / "vita/runtime"):
                continue
            if path.is_file() and path.suffix in (".h", ".hpp", ".inc", ".c", ".cpp", ".cmake", ".py", ".sh", ".txt"):
                files[str(path.relative_to(root))] = digest(path)
    return files


def main(root, build, exit_code):
    commands = json.loads((build / "compile_commands.json").read_text())
    with (build / "upstream-source-manifest.tsv").open() as stream:
        expected = {str(root / item["source"]) for item in csv.DictReader(stream, delimiter="\t")}
    actual = {item["file"] for item in commands}
    parity = {"matched": actual == expected, "upstream_count": len(expected),
              "sidecar_count": len(actual), "missing": sorted(expected - actual),
              "extra": sorted(actual - expected)}
    units = []
    groups = defaultdict(Counter)
    errors = Counter()
    examples = defaultdict(list)
    for entry in commands:
        unit = inspect_unit(entry)
        source = Path(entry["file"]).relative_to(root)
        group = {"Game": "GAME", "JSystem": "JSYSTEM", "nw4r": "NW4R", "RVL_SDK": "SDK"}[source.parts[2]]
        unit["group"] = group
        groups[group][unit["status"]] += 1
        units.append(unit)
        if unit.get("compiler_exit_code", 0) != 0:
            for match in re.finditer(r"^(.+?):(\d+):(\d+): (?:fatal )?error: (.+)$", Path(unit["log"]).read_text(errors="replace"), re.MULTILINE):
                message = match[4]
                errors[message] += 1
                if len(examples[message]) < 5:
                    examples[message].append({"file": match[1], "line": int(match[2]), "column": int(match[3]), "source": str(source)})
    before = json.loads((build / "source-state-before.json").read_text())
    after = source_snapshot(root)
    (build / "source-state-after.json").write_text(json.dumps(after, indent=2) + "\n")
    changed = sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))
    counts = Counter(unit["status"] for unit in units)
    compiler_exits = Counter("completed" if unit["compiler_exit_code"] == 0 else "failed"
                            for unit in units if "compiler_exit_code" in unit)
    archive = build / "libgalaxy_vita_sources.a"
    archive_completed = False
    members = []
    if archive.exists() and counts["completed"] == len(commands) and exit_code == 0:
        members = subprocess.check_output(["llvm-ar", "t", str(archive)], text=True).splitlines()
        archive_completed = len(members) == len(commands)
    # The ELF header is checked for every object; retain CPU/FPU attributes per group.
    with (build / "object-attributes.txt").open("w") as stream:
        for group in groups:
            sample = next((unit for unit in units if unit["group"] == group and unit["status"] == "completed"), None)
            if sample:
                subprocess.run(["llvm-readelf", "-h", "-A", sample["output"]], stdout=stream, stderr=subprocess.STDOUT, check=True)
    report = {
        "revision": (build / "source-revision.txt").read_text().strip(),
        "ninja_exit_code": exit_code, "inventory": parity, "total_units": len(commands),
        "counts": dict(counts), "groups": {key: dict(value) for key, value in groups.items()},
        "recorded_compiler_exits": dict(compiler_exits),
        "archive": {"completed": archive_completed, "member_count": len(members), "path": str(archive),
                    "sha256": digest(archive) if archive_completed else None},
        "source_changes_during_build": changed,
        "errors": [{"message": message, "occurrences": count, "examples": examples[message]} for message, count in errors.most_common()],
        "units": units,
    }
    (build / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("revision", "ninja_exit_code", "inventory", "counts", "groups", "archive", "source_changes_during_build")}, indent=2))
    valid = parity["matched"] and not changed and not any(counts[key] for key in ("stale", "invalid_object", "unattempted"))
    if exit_code == 0:
        valid = valid and archive_completed
    return 0 if valid else 2


if __name__ == "__main__":
    if sys.argv[1] == "snapshot":
        Path(sys.argv[3]).write_text(json.dumps(source_snapshot(Path(sys.argv[2])), indent=2) + "\n")
    else:
        sys.exit(main(Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])))
