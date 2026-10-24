#!/usr/bin/env python3
"""Reuse generated flags for ABI checks or separate, real runtime objects."""
import json
from pathlib import Path
import shlex
import subprocess
import sys


def base_command(entry):
    original = entry.get("arguments") or shlex.split(entry["command"])
    command = []
    index = 0
    while index < len(original):
        argument = original[index]
        if argument in ("-o", "-c", "-MF", "-MT", "-MQ"):
            index += 2
            continue
        if argument in ("-MD", "-MMD"):
            index += 1
            continue
        command.append(argument)
        index += 1
    return command


def compile_object(base, source, output, extra):
    command = base + extra + ["-o", str(output), "-c", str(source)]
    return subprocess.run(["nice", "-n", "10", sys.executable,
                           "/source/vita/record_compile.py", *command]).returncode


def main():
    database = json.loads(Path("/build/compile_commands.json").read_text())
    entry = next(item for item in database if item["file"].endswith(".cpp"))
    base = base_command(entry)
    if sys.argv[1] == "boundary":
        selected = []
        for group in ("Game", "JSystem", "nw4r", "RVL_SDK"):
            selected.append(next(item for item in database if f"/src/{group}/" in item["file"]))
        selected.append(next(item for item in database if item["file"].endswith("/Game/AreaObj/AreaObj.cpp")))
        outputs = []
        for item in selected:
            command = item.get("arguments") or shlex.split(item["command"])
            outputs.append(command[command.index("-o") + 1])
        command = ["nice", "-n", "10", "ninja", "-C", "/build", "-j2", "-k1", *outputs]
        print(shlex.join(command), flush=True)
        return subprocess.run(command).returncode
    if sys.argv[1] == "abi":
        source = Path("/source/vita/source_abi_checks.cpp")
        output = Path("/build/abi/source-layout.obj")
        positive = compile_object(base, source, output, ["-Xclang", "-fdump-record-layouts"])
        if positive:
            return positive
        negative = compile_object(base, source, Path("/build/abi/short-enums-negative.obj"), ["-fshort-enums", "-Xclang", "-fdump-record-layouts"])
        if negative == 0:
            print("ABI negative control unexpectedly accepted short enums", file=sys.stderr)
            return 2
        print("Source ABI layouts passed; short-enum negative control rejected")
        return 0
    if sys.argv[1] != "runtime":
        raise ValueError("Use abi, boundary, or runtime")
    allowed = {"vita/runtime/input.cpp", "vita/runtime/audio.cpp", "platform/src/audio/aaudio_out.cpp"}
    sources = sys.argv[2:] or sorted(allowed)
    results = []
    for source in sources:
        if source not in allowed:
            raise ValueError(f"Not an authorized standalone runtime source: {source}")
        # SDK-only adapters use the VitaSDK enum ABI; the shared audio unit keeps
        # the game ABI. All crossing callbacks/PortPadState fields are scalars.
        extra = ["-DPORT_PLATFORM_CODE=1", "-I/source/vita/runtime"]
        sdk_only = source.startswith("vita/runtime/")
        if sdk_only:
            extra.append("-fshort-enums")
        output = Path("/build/standalone-runtime") / (Path(source).stem + ".obj")
        status = compile_object(base, Path("/source") / source, output, extra)
        results.append({"source": source, "exit_code": status, "output": str(output), "enum_abi": "SDK-short" if sdk_only else "game-int32"})
    Path("/build/runtime-object-report.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))
    return 1 if any(result["exit_code"] for result in results) else 0


if __name__ == "__main__":
    sys.exit(main())
