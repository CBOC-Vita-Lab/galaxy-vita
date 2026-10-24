#!/usr/bin/env python3
"""Embed only hash-matched, full-precision compiler output; no runtime GXP file parser."""
import argparse
import hashlib
import json
import os
from pathlib import Path


def owned(path, root, prefix):
    root = Path(root).resolve()
    path = (root / path).resolve()
    base = (root / prefix).resolve()
    if base == root or not base.is_relative_to(root) or not path.is_relative_to(base):
        raise ValueError(f"shader artifact outside {prefix}: {path}")
    return path


def read_reports(root, reports):
    root = Path(root).resolve()
    entries = {}
    for name in reports:
        path = owned(name, root, "build-shacc-tools")
        report = json.loads(path.read_text())
        if not isinstance(report, dict) or report.get("accepted") is not True or report.get("runner_error") or \
                report.get("stage") not in ("vertex", "fragment") or report.get("entry") != "main" or \
                type(report.get("optimization")) is not int or report["optimization"] not in range(4) or \
                any(report.get(flag) is not False for flag in ("fastmath", "fastprecision", "fastint")):
            raise ValueError(f"incomplete/failed/fast compiler record: {path}")
        source = owned(report["source"], root, "build-gx-vita-shader")
        output = report.get("output")
        if not output and report.get("backend") == "pinned Dynarmic A32/native ARM64":
            output = path.with_suffix(".gxp")
        if not output:
            raise ValueError(f"missing compiler output: {path}")
        gxp_path = owned(output, root, "build-shacc-tools")
        source_bytes = source.read_bytes()
        gxp = gxp_path.read_bytes()
        if hashlib.sha256(source_bytes).hexdigest() != report.get("source_sha256") or \
                (report.get("source_sha256_after") is not None and report["source_sha256_after"] != report["source_sha256"]) or \
                hashlib.sha256(gxp).hexdigest() != report.get("gxp_sha256") or \
                type(report.get("gxp_size")) is not int or len(gxp) != report["gxp_size"] or not 8 <= len(gxp) <= 4*1024*1024:
            raise ValueError(f"source/compiler blob drift or incomplete GXP: {path}")
        key = (report["stage"], source_bytes.decode("ascii"))
        if key in entries and entries[key] != gxp:
            raise ValueError(f"conflicting compiler artifact for identical source/profile: {path}")
        entries[key] = gxp
    if not entries:
        raise ValueError("empty compiler bundle")
    return [(stage, source, gxp) for (stage, source), gxp in sorted(entries.items())]


def emit(entries, header_include="../platform/src/gx/vita_material_cache.h"):
    lines = ["// Generated from verified compiler records. Keep this artifact local/ignored.",
        "#pragma once", "#include " + json.dumps(header_include), "#include <utility>", "namespace gpu {",
        "inline bool installBundledVitaShaders(VitaShaderLibrary& library,std::string& error) {",
        "    VitaShaderLibrary candidate=library;"]
    for index, (stage, source, gxp) in enumerate(entries):
        lines.append(f"    static const unsigned char gxp{index}[]={{")
        for start in range(0, len(gxp), 24):
            lines.append("        " + ",".join(f"0x{value:02x}" for value in gxp[start:start+24]) + ",")
        lines.append("    };")
        lines.append(f"    if (!installVitaShader(candidate,{{VitaShaderStage::{stage.title()}," +
            json.dumps(source) + f",std::vector<unsigned char>(gxp{index},gxp{index}+sizeof(gxp{index})),0}},error)) return false;")
    lines += ["    library=std::move(candidate); error.clear(); return true;", "}", "}", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="append", required=True)
    parser.add_argument("--output", default="build-gxm-material/vita_shader_bundle.h")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = owned(args.output, root, "build-gxm-material")
    entries = read_reports(root, args.report)
    include = Path(os.path.relpath(root / "platform/src/gx/vita_material_cache.h", destination.parent)).as_posix()
    source = emit(entries, include)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(source)
    print(f"Verified shader bundle: {len(entries)} stages, {sum(len(item[2]) for item in entries)} GXP bytes; {destination.relative_to(root)}")


if __name__ == "__main__":
    main()
