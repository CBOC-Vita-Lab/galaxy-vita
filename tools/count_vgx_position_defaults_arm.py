#!/usr/bin/env python3
"""Count QEMU single-instruction trace slots across the real decoder entry.

No elapsed-time claim. The entry's cached dispatch, callees and returns are all
inside the boundary. Marker/caller instructions and setup/warmup are excluded.
"""
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def symbols(binary):
    text = subprocess.check_output(["arm-linux-gnueabihf-nm", "-C", str(binary)], text=True)
    names = {"fused": set()}
    for line in text.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue
        if fields[2] in ("position_count_begin", "position_count_end", "position_plan_begin", "position_plan_end") or fields[2].startswith(("vgx::DecodeVertices(", "vgx::GetPlanIn(")):
            names[fields[2].split("(")[0]] = int(fields[0], 16) & ~1
        if re.search(r"DecodeAttr<0, [0-4], [23], false, false, true>", fields[2]):
            names["fused"].add(int(fields[0], 16) & ~1)
    return names


def condition_passes(condition, psr):
    n, z, c, v = ((psr >> bit) & 1 for bit in (31, 30, 29, 28))
    return (z, not z, c, not c, n, not n, v, not v,
            c and not z, not c or z, n == v, n != v,
            not z and n == v, z or n != v, True, True)[condition]


def trace(binary, mode, output, calibration=False):
    names = symbols(binary)
    env = dict(os.environ, GXV_POSDEFAULT=str(mode), GXV_DECODE="1")
    command = ["qemu-arm", "-cpu", "cortex-a9", "-L", "/usr/arm-linux-gnueabihf",
               "-one-insn-per-tb", "-d", "in_asm,cpu,nochain", str(binary), str(output.with_suffix(".bin"))]
    instruction = re.compile(r"^0x([0-9a-f]+):")
    registers = re.compile(r"R12=([0-9a-f]+) R13=([0-9a-f]+) R14=([0-9a-f]+) R15=([0-9a-f]+)")
    psr_pattern = re.compile(r"^PSR=([0-9a-f]+)")
    encoded = {}
    records = []
    plan_records = []
    armed = None
    active = None
    pc = lr = instruction_pc = 0
    trace_hash = hashlib.sha256()
    with output.with_suffix(".stdout").open("w") as stdout:
        process = subprocess.Popen(command, env=env, stdout=stdout, stderr=subprocess.PIPE, text=True, bufsize=1)
        for line in process.stderr:
            if line.startswith("0x"):
                match = instruction.match(line)
                if match:
                    instruction_pc = int(match[1], 16)
            # The pinned image emits raw little-endian bytes, not mnemonic text.
            elif line.startswith(("OBJD-T:", "OBJD-A:")):
                encoded[instruction_pc] = line.split()[1]
            elif line.startswith("R12="):
                match = registers.match(line)
                if not match:
                    raise RuntimeError("Unrecognized QEMU register record")
                lr, pc = int(match[3], 16), int(match[4], 16)
            elif line.startswith("PSR="):
                psr = int(psr_pattern.match(line)[1], 16)
                if pc == names["position_count_begin"]:
                    if armed or active:
                        raise RuntimeError("Nested measurement")
                    armed = "decode"
                if pc == names.get("position_plan_begin"):
                    if armed or active:
                        raise RuntimeError("Nested plan measurement")
                    armed = "plan"
                if armed and pc == names["vgx::DecodeVertices" if armed == "decode" else "vgx::GetPlanIn"]:
                    active = {"slots": 0, "executed": 0, "condition_failed": 0, "fused_calls": 0,
                              "return": lr & ~1, "kind": armed}
                    armed = None
                if active and pc == active["return"]:
                    del active["return"]
                    kind = active.pop("kind")
                    (records if kind == "decode" else plan_records).append(active)
                    active = None
                if active:
                    if pc not in encoded:
                        raise RuntimeError(f"Missing decoded instruction at {pc:x}")
                    raw = encoded[pc]
                    itstate = (((psr >> 10) & 0x3f) << 2) | ((psr >> 25) & 3)
                    condition = (itstate >> 4) if psr & 0x20 and itstate else 14
                    if not psr & 0x20:
                        condition = int.from_bytes(bytes.fromhex(raw), "little") >> 28
                    passed = condition_passes(condition, psr)
                    active["slots"] += 1
                    active["executed"] += int(passed)
                    active["condition_failed"] += int(not passed)
                    active["fused_calls"] += int(pc in names["fused"])
                    trace_hash.update(f"{pc:x} {raw} {psr:08x} {int(passed)}\n".encode())
                if pc in (names["position_count_end"], names.get("position_plan_end")) and (armed or active):
                    raise RuntimeError("Decoder boundary did not close")
        if process.wait():
            raise RuntimeError(f"QEMU failed for {binary.name} mode {mode}")
    if armed or active:
        raise RuntimeError("Unclosed measurement")
    if calibration:
        if len(records) != 1 or records[0] != {"slots": 13, "executed": 11, "condition_failed": 2, "fused_calls": 0}:
            raise RuntimeError(f"ARM/Thumb predicate calibration failed: {records}")
        print("PASS: known ARM/Thumb calibration = 13 encountered / 11 predicate-executed instructions", flush=True)
        return records
    manifest = []
    for line in output.with_suffix(".stdout").read_text().splitlines():
        if line.startswith("case="):
            manifest.append({key: int(value) for key, value in (word.split("=") for word in line.split())})
    if len(records) != len(manifest) or len(records) != 92 or len(plan_records) != 92:
        raise RuntimeError(f"Expected 92 decode/plan traces, got {len(records)} / {len(plan_records)} / {len(manifest)}")
    result = [dict(case, **record, plan=plan) for case, record, plan in zip(manifest, records, plan_records)]
    for row in result:
        if row["fused_calls"] != int(mode == 1 and row["eligible"]):
            raise RuntimeError(f"Unexpected fused-kernel coverage: {row}")
    output.with_suffix(".json").write_text(json.dumps({"trace_sha256": trace_hash.hexdigest(), "cases": result}, indent=2) + "\n")
    return result


def main():
    out = Path(sys.argv[1]).resolve()
    trace(out / "calibration.elf", 0, out / "calibration", calibration=True)
    results = {}
    for label, binary, mode in (("pristine", "pristine", 0), ("off", "current", 0), ("on", "current", 1)):
        print(f"Tracing {label}: Cortex-A9 Thumb2 quiet -O2", flush=True)
        results[label] = trace(out / f"{binary}.elf", mode, out / label)
    for label, mode, decode in (("oracle", 2, 1), ("general", 0, 0)):
        env = dict(os.environ, GXV_POSDEFAULT=str(mode), GXV_DECODE=str(decode))
        with (out / f"{label}.stdout").open("w") as stdout, (out / f"{label}.stderr").open("w") as stderr:
            subprocess.run(["qemu-arm", "-cpu", "cortex-a9", "-L", "/usr/arm-linux-gnueabihf",
                            str(out / "current.elf"), str(out / f"{label}.bin")], env=env, stdout=stdout, stderr=stderr, check=True)
    reference = (out / "pristine.bin").read_bytes()
    for label in ("off", "on", "oracle", "general"):
        if (out / f"{label}.bin").read_bytes() != reference:
            raise RuntimeError(f"Exact complete output/guards/spans/matrix comparison failed: {label}")
    if "MISMATCH" in (out / "oracle.stderr").read_text():
        raise RuntimeError("The runtime oracle recorded a mismatch")
    rows = []
    for old, off, on in zip(results["pristine"], results["off"], results["on"]):
        keys = ("case", "extras", "type", "components", "count", "direct", "eligible", "stride")
        if any(old[key] != off[key] or old[key] != on[key] for key in keys):
            raise RuntimeError("Mismatched case identities")
        row = {key: old[key] for key in keys}
        for label, value in (("pristine", old), ("off", off), ("on", on)):
            row[label] = value["executed"]
            row[f"{label}_slots"] = value["slots"]
            row[f"{label}_plan"] = value["plan"]["executed"]
        row["off_added"] = off["executed"] - old["executed"]
        row["on_removed"] = old["executed"] - on["executed"]
        row["off_plan_added"] = off["plan"]["executed"] - old["plan"]["executed"]
        row["on_plan_added"] = on["plan"]["executed"] - old["plan"]["executed"]
        rows.append(row)
    with (out / "counts.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)
    print(f"PASS: exact {len(reference)} output bytes across pristine/off/on/oracle/general; sha256={hashlib.sha256(reference).hexdigest()}")
    print("PASS: 92 warm total-entry counts per configuration; 80 eligible cases / 6800 eligible vertices; 12 excluded cases / 1020 vertices")
    print("Instruction counts are not cycles, game speed, GPU savings, or Vita FPS.")


if __name__ == "__main__":
    main()
