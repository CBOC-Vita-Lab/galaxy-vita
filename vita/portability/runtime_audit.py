#!/usr/bin/env python3
"""Bounded, real-SDK runtime compile and whole-archive symbol boundary audit."""
import collections
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time


API_FAMILIES = {
    "memory": r"\b(?:mmap|munmap|mprotect|makeSharedFd|memfd_create|syscall|ftruncate|port_low_alloc|port_phys_to_host)\b",
    "scheduler": r"\b(?:pthread_\w+|sigaction|SIGURG|_Unwind_\w+|clock_gettime|nanosleep|usleep)\b|std::(?:thread|mutex|condition_variable)",
    "file_transport": r"\b(?:pread|pwrite|open|close|read|write|lseek|mkdir|opendir|readdir|stat|rename|unlink)\b",
    "network_transport": r"\b(?:socket|connect|bind|listen|accept|send|recv|sendto|recvfrom|sceNet\w*)\b",
    "gpu": r"\b(?:gl[A-Z]\w+|egl[A-Z]\w+|GL_SHADER_STORAGE_BUFFER|sceGxm\w*|port_gx_\w+)\b",
    "sony_input_audio": r"\b(?:sceCtrl\w*|sceMotion\w*|sceTouch\w*|sceAudio\w*|sceKernel\w*|port_vita_\w+)\b",
}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_game_archive(game, report):
    archive = report.get("archive", {})
    if (report.get("counts") != {"completed": 1834} or report.get("ninja_exit_code") != 0
            or not report.get("inventory", {}).get("matched") or report.get("source_changes_during_build") != []
            or not archive.get("completed") or archive.get("member_count") != 1834
            or archive.get("sha256") != digest(game)):
        raise RuntimeError("Expected complete, stable, digest-verified 1834-unit source archive")
    return archive["sha256"]


def inventory(root):
    paths = set()
    for directory in ("platform", "vita/runtime"):
        for suffix in ("*.cpp", "*.c"):
            paths.update((root / directory).rglob(suffix))
    rows = []
    for path in sorted(paths):
        source = path.relative_to(root).as_posix()
        excluded = source.startswith("platform/android/") or source == "platform/src/xr/xr_app.cpp"
        sdk_only = source.startswith("vita/runtime/")
        parts = Path(source).parts
        group = "adapters" if sdk_only else parts[2] if source.startswith("platform/src/") else parts[1]
        rows.append({"source": source, "group": group,
                     "selection": "excluded" if excluded else "attempt",
                     "reason": "Android/OpenXR entry point; upstream desktop excludes it" if excluded else "non-Android runtime or diagnostic source",
                     "enum_abi": "SDK-short" if sdk_only else "game-int32"})
    return rows


def base_command(entry):
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    result = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value in ("-o", "-c", "-MF", "-MT", "-MQ"):
            index += 2
        elif value in ("-MD", "-MMD"):
            index += 1
        else:
            result.append(value)
            index += 1
    return result


def arm_object(data):
    return (len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01"
            and struct.unpack_from("<HH", data, 16) == (1, 40)
            and struct.unpack_from("<I", data, 36)[0] >> 24 == 5)


def unit_status(exit_code, valid_object, changed):
    if exit_code:
        return "failed"
    if changed:
        return "input-drift"
    return "completed" if valid_object else "invalid-object"


def diagnostics(log):
    pattern = re.compile(r"^(.+?):(\d+):(\d+): (fatal error|error): (.*)$", re.MULTILINE)
    return [{"source": match[1], "line": int(match[2]), "column": int(match[3]),
             "severity": match[4], "message": match[5]} for match in pattern.finditer(log)]


def parse_nm(output):
    pattern = re.compile(r"^(.+?): (\S+) ([A-Za-z?])(?:\s|$)")
    return [{"owner": match[1], "symbol": match[2], "type": match[3]}
            for line in output.splitlines() if (match := pattern.match(line))]


def symbol_graph(rows):
    definitions = collections.defaultdict(set)
    strong = collections.defaultdict(set)
    references = collections.defaultdict(set)
    weak = collections.defaultdict(set)
    for row in rows:
        kind, name, owner = row["type"], row["symbol"], row["owner"]
        if kind == "U":
            references[name].add(owner)
        elif kind in ("w", "v"):
            weak[name].add(owner)
        elif kind.upper() not in ("N", "?"):
            definitions[name].add(owner)
            if kind not in ("W", "V", "C"):
                strong[name].add(owner)
    return {"definitions": {k: sorted(v) for k, v in definitions.items()},
            "unresolved": {k: sorted(v) for k, v in references.items() if k not in definitions},
            "weak_unresolved": {k: sorted(v) for k, v in weak.items() if k not in definitions},
            "duplicates": {k: sorted(v) for k, v in strong.items() if len(v) > 1}}


def run(command, log, timeout=300):
    start = time.monotonic()
    try:
        result = subprocess.run(["nice", "-n", "10", *command], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
        status, output = result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        status = 124
        output = (error.stdout or b"").decode(errors="replace") if isinstance(error.stdout, bytes) else error.stdout or ""
        output += "\nAudit command timed out; process terminated.\n"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("$ nice -n 10 " + shlex.join(command) + "\n" + output)
    return {"command": command, "exit_code": status, "duration_seconds": round(time.monotonic() - start, 3),
            "log": str(log)}, output


def snapshot(root):
    paths = set()
    for directory in ("platform", "vita/runtime", "vita/portability", "vita/include"):
        paths.update(path for path in (root / directory).rglob("*") if path.is_file() and "__pycache__" not in path.parts)
    for directory in ("decomp/include", "decomp/libs"):
        paths.update(path for path in (root / directory).rglob("*") if path.suffix in (".h", ".hpp", ".inc"))
    paths.add(root / "CMakeLists.txt")
    bundle = root / "build-gxm-material/vita_shader_bundle.h"
    if bundle.is_file():
        paths.add(bundle)
    return {path.relative_to(root).as_posix(): digest(path) for path in sorted(paths)}


def generated_inputs(root, source):
    # __has_include and failed compilation may produce no depfile. Track both
    # absent and present generated data instead of silently reusing old bytes.
    if source != "platform/src/gx/vita_shader_bundle.cpp":
        return {}
    name = "build-gxm-material/vita_shader_bundle.h"
    path = root / name
    return {name: digest(path) if path.is_file() else None}


def dependency_paths(path):
    if not path.exists():
        return []
    text = path.read_text().replace("\\\n", " ")
    return shlex.split(text.split(":", 1)[1])


def compile_unit(row, bases, root, build):
    source = root / row["source"]
    output = build / "objects" / (row["source"] + ".obj")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    depfile = output.with_suffix(".d")
    depfile.unlink(missing_ok=True)
    before = digest(source)
    generated_before = generated_inputs(root, row["source"])
    extra = ["-DPORT_PLATFORM_CODE=1", "-I/source/vita/runtime"]
    if row["enum_abi"] == "SDK-short":
        extra.append("-fshort-enums")
    command = bases[source.suffix] + extra + ["-MD", "-MF", str(depfile), "-o", str(output), "-c", str(source)]
    record, log = run(command, output.with_suffix(".compiler.log"), timeout=180)
    after = digest(source)
    generated_after = generated_inputs(root, row["source"])
    valid = output.exists() and arm_object(output.read_bytes()[:52])
    dependencies = {name: digest(Path(name)) for name in dependency_paths(depfile) if Path(name).is_file()}
    record.update(row, source_sha256_before=before, source_sha256_after=after,
                  image_id=os.environ.get("VITA_RUNTIME_IMAGE_ID"),
                  output=str(output), status=unit_status(record["exit_code"], valid,
                                                       before != after or generated_before != generated_after),
                  object_sha256=digest(output) if valid else None,
                  dependencies=dependencies, generated_inputs=generated_after, errors=diagnostics(log))
    write_json(output.with_suffix(".result.json"), record)
    return record


def nm(paths, log, defined=False):
    command = ["llvm-nm", "--extern-only", "--print-file-name", "--format=posix"]
    if defined:
        command.append("--defined-only")
    record, output = run(command + [str(path) for path in paths], log)
    if record["exit_code"]:
        raise RuntimeError(f"Symbol extraction failed: {log}")
    rows = parse_nm(output)
    if paths and not rows:
        raise RuntimeError(f"Symbol parser returned no records: {log}")
    return rows


def dependency_inventory(root, rows):
    result = []
    for row in rows:
        text = (root / row["source"]).read_text(errors="replace")
        includes = re.findall(r"^\s*#\s*include\s*([<\"][^>\"]+[>\"])", text, re.MULTILINE)
        apis = {}
        for family, pattern in API_FAMILIES.items():
            found = [{"line": index, "tokens": sorted(set(re.findall(pattern, line)))}
                     for index, line in enumerate(text.splitlines(), 1) if re.search(pattern, line)]
            if found:
                apis[family] = found
        result.append({**row, "includes": includes, "lexical_api_references": apis})
    return result


def symbol_audit(root, build, game, records):
    completed = sorted(Path(record["output"]) for record in records if record["status"] == "completed")
    archive = build / "libgalaxy_vita_runtime_partial.a"
    archive.unlink(missing_ok=True)
    archive_record, _ = run(["llvm-ar", "rcs", str(archive), *map(str, completed)], build / "archive.log")
    members_record, members = run(["llvm-ar", "t", str(archive)], build / "archive-members.log")
    archive_record.update(member_count=len(members.splitlines()), sha256=digest(archive),
                          complete_runtime=False, note="Only successfully compiled, stable units; exclusions/failures are not passes")
    if archive_record["exit_code"] or members_record["exit_code"] or archive_record["member_count"] != len(completed):
        raise RuntimeError("Partial archive creation/accounting failed")

    graph_rows = nm([game, archive], build / "symbols-game-runtime.log")
    graph = symbol_graph(graph_rows)
    runtime_graph = symbol_graph(nm([archive], build / "symbols-runtime.log"))
    sdk = Path("/opt/vitasdk/arm-vita-eabi/lib")
    gcc = Path("/opt/vitasdk/lib/gcc/arm-vita-eabi/15.2.0/libgcc.a")
    sdk_libraries = sorted(path for path in sdk.glob("*.a") if not path.name.endswith("_weak.a")) + [gcc]
    write_json(build / "sdk-library-provenance.json", {str(path): digest(path) for path in sdk_libraries})
    sdk_graph = symbol_graph(nm(sdk_libraries, build / "symbols-sdk-providers.log", defined=True))
    unresolved = graph["unresolved"]
    names = sorted(unresolved)
    demangle_record, demangled = run_demangler(names, build / "symbols-demangled.txt")
    rows = [{"symbol": name, "demangled": demangled[index], "referenced_by": unresolved[name],
             "sdk_candidate_providers": sdk_graph["definitions"].get(name, []),
             "referenced_by_runtime": name in runtime_graph["unresolved"]}
            for index, name in enumerate(names)]
    counts = {"strong_unresolved_before_sdk": len(rows),
              "weak_unresolved_before_sdk": len(graph["weak_unresolved"]),
              "sdk_candidate_available": sum(bool(row["sdk_candidate_providers"]) for row in rows),
              "no_sdk_candidate": sum(not row["sdk_candidate_providers"] for row in rows),
              "strong_duplicate_symbols": len(graph["duplicates"])}

    # No CRT, entry-point shim, GC, unresolved-symbol suppression, or renderer
    # replacement: these are whole-input link diagnostics, not a Vita program.
    inputs = ["--whole-archive", str(game), str(archive), "--no-whole-archive"]
    relocatable, _ = run(["ld.lld", "-m", "armelf", "-r", "--error-limit=0", *inputs,
                          "-o", str(build / "game-runtime.rel.o")], build / "link-relocatable.log")
    chosen = [sdk / name for name in sdk_link_names() if (sdk / name).exists()] + [gcc]
    strict, strict_output = run(["ld.lld", "-m", "armelf", "--no-undefined", "--error-limit=0", "--entry=port_boot",
                                *inputs, "--start-group", *map(str, chosen), "--end-group",
                                "-o", str(build / "boundary-not-bootable.elf")], build / "link-strict-sdk.log")
    strict["undefined_symbols"] = sorted(set(re.findall(r"undefined symbol: (.+)", strict_output)))
    strict["groups"] = boundary_groups(strict["undefined_symbols"])
    strict["duplicate_symbols"] = sorted(set(re.findall(r"duplicate symbol: (.+)", strict_output)))
    strict["selected_sdk_libraries"] = {str(path): digest(path) for path in chosen}
    strict["note"] = "Diagnostic only: no crt0, Vita application entry, import conversion, or packaging; entire game retained"
    report = {"counts": counts, "unresolved": rows, "weak_unresolved": graph["weak_unresolved"],
              "weak_sdk_candidate_providers": {name: sdk_graph["definitions"].get(name, []) for name in graph["weak_unresolved"]},
              "strong_duplicates": graph["duplicates"], "relocatable_link": relocatable,
              "strict_sdk_link": strict, "demangle": demangle_record,
              "sdk_provider_note": "Candidates from installed static libraries, including private stubs; availability is not successful linking or an approved API choice"}
    write_json(build / "symbol-report.json", report)
    return archive_record, report


def sdk_link_names():
    # Newlib/GCC supply C/C++ and unwinding. Sony's Libc import library exports
    # a second unwinder, which the first real link diagnosed as a collision.
    return ["libstdc++.a", "libsupc++.a", "libpthread.a", "libm.a", "libc.a",
            "libSceLibKernel_stub.a", "libSceKernelThreadMgr_stub.a", "libSceSysmem_stub.a",
            "libSceIofilemgr_stub.a", "libSceProcessmgr_stub.a", "libSceAudio_stub.a",
            "libSceCtrl_stub.a", "libSceMotion_stub.a", "libSceTouch_stub.a",
            "libSceRtc_stub.a", "libSceNet_stub.a", "libSceKernelModulemgr_stub.a", "libSceGxm_stub.a",
            "libSceDisplay_stub.a"]


def boundary_groups(symbols):
    groups = collections.defaultdict(list)
    for name in sorted(symbols):
        if name in ("sigaction", "posix_memalign"):
            group = "scheduler_allocator_sdk_gaps"
        elif name.startswith(("sceRtc", "sceNet")):
            group = "sdk_service_library_closure"
        elif name.startswith("port_vr_"):
            group = "vr_game_hooks"
        elif name.startswith(("port_gx_", "gpu::")) or name == "gPresentedFrames":
            group = "gpu_recording_presentation"
        elif name.startswith(("DC", "LC", "port_mem_")) or "Arena" in name or name in (
                "OSInitAlloc", "OSProtectRange", "port_low_alloc", "port_is_game_memory", "port_phys_to_host"):
            group = "memory_cache"
        elif name.startswith(("OS", "PPC", "port_log", "port_host_", "port_os_")) or name == "port_fatal":
            group = "time_alarm_logging_reset"
        elif name.startswith("port_headless_"):
            group = "desktop_capture"
        elif name in ("_fini", "_init", "_free_vita_newlib", "__dso_handle"):
            group = "startup"
        else:
            group = "other"
        groups[group].append(name)
    return dict(sorted(groups.items()))


def symbols_only(root, build, game, expected_game_sha):
    report = json.loads((build / "runtime-report.json").read_text())
    report["game_archive"]["sha256"] = expected_game_sha
    for record in report["units"]:
        if record["status"] != "completed":
            continue
        if digest(root / record["source"]) != record["source_sha256_after"]:
            raise RuntimeError(f"Changed completed source; no silent reuse: {record['source']}")
        if digest(Path(record["output"])) != record["object_sha256"]:
            raise RuntimeError(f"Changed completed object: {record['output']}")
        if any(digest(Path(name)) != sha for name, sha in record["dependencies"].items()):
            raise RuntimeError(f"Changed completed dependency: {record['source']}")
        if generated_inputs(root, record["source"]) != record.get("generated_inputs", {}):
            raise RuntimeError(f"Changed completed generated input: {record['source']}")
    before = snapshot(root)
    for name in ("runtime-report.json", "symbol-report.json", "link-strict-sdk.log", "archive.log", "archive-members.log"):
        path = build / name
        backup = path.with_name(path.stem + "-primary" + path.suffix)
        if not backup.exists():
            shutil.copyfile(path, backup)
    archive, symbols = symbol_audit(root, build, game, report["units"])
    run(["clang", "--version"], build / "clang-version-final.log")
    run(["ld.lld", "--version"], build / "lld-version-final.log")
    attributes, _ = run(["llvm-readelf", "-A", "/build/objects/platform/src/os/os_thread.cpp.obj",
                         "/build/objects/platform/src/sdk/mtx.c.obj",
                         *[record["output"] for record in report["units"] if record["group"] == "adapters" and record["status"] == "completed"]],
                         build / "runtime-object-attributes.log")
    run(["llvm-objdump", "-d", "-r", "--disassemble-symbols=pthread_attr_setstack,pthread_attr_setstackaddr,pthread_attr_setstacksize,pthread_kill,pthread_cancel,pthread_create,pte_osThreadCreate",
         "/opt/vitasdk/arm-vita-eabi/lib/libpthread.a"], build / "pthread-implementation.log")
    after = snapshot(root)
    write_json(build / "symbol-audit-source-state-before.json", before)
    write_json(build / "symbol-audit-source-state-after.json", after)
    report["symbol_audit_input_changes"] = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    if report.get("batch_history"):
        report["batch_history"][-1]["symbol_audit_input_changes"] = report["symbol_audit_input_changes"]
    report["latest_symbol_audit_image_id"] = os.environ.get("VITA_RUNTIME_IMAGE_ID")
    baseline = json.loads(Path("/gamebuild/source-state-after.json").read_text())
    report["game_archive"]["current_input_differences_from_game_snapshot"] = [name for name, sha in baseline.items()
        if not (root / name).is_file() or digest(root / name) != sha]
    report["archive"], report["symbols"] = archive, symbols
    report["object_attributes"] = attributes
    report["symbols"]["initial_link_failure"] = json.loads((build / "symbol-report-primary.json").read_text())["strict_sdk_link"]
    write_json(build / "runtime-report.json", report)
    if digest(game) != expected_game_sha:
        raise RuntimeError("Read-only game archive changed during symbol analysis")
    (build / "runtime-summary.md").write_text(markdown_report(report))
    print(json.dumps({"counts": report["counts"], "symbols": symbols["counts"],
                      "strict_link_exit": symbols["strict_sdk_link"]["exit_code"],
                      "strict_undefined_count": len(symbols["strict_sdk_link"]["undefined_symbols"]),
                      "symbol_audit_input_changes": report["symbol_audit_input_changes"]}, indent=2))
    return 1 if (symbols["strict_sdk_link"]["exit_code"] or report["symbol_audit_input_changes"]
                 or any(record["status"] != "completed" for record in report["units"])
                 or report["sources_added_during_batch"]) else 0


def needs_compile(row, old, root):
    if old is None or digest(root / row["source"]) != old["source_sha256_after"]:
        return True
    if old.get("status") in ("input-drift", "invalid-object"):
        return True
    if generated_inputs(root, row["source"]) != old.get("generated_inputs", {}):
        return True
    return any(not Path(name).is_file() or digest(Path(name)) != sha for name, sha in old["dependencies"].items())


def delta_batch(root, build, game, bases, expected_game_sha):
    report = json.loads((build / "runtime-report.json").read_text())
    old = {record["source"]: record for record in report["units"]}
    before = snapshot(root)
    selected = inventory(root)
    pending = []
    previous = json.loads((build / "source-state-final.json").read_text())
    for row in selected:
        if row["selection"] == "excluded":
            continue
        record = old.get(row["source"])
        changed_error_header = record and any(error["source"].startswith("/source/") and
            before.get(error["source"][8:]) != previous.get(error["source"][8:]) for error in record["errors"])
        if needs_compile(row, record, root) or changed_error_header:
            pending.append(row)
    print(f"Runtime delta: {len(pending)} new/changed units; unchanged failures are retained, not repeated.", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        for record in executor.map(lambda row: compile_unit(row, bases, root, build), pending):
            old[record["source"]] = record
            print(f"delta {record['status']}: {record['source']}", flush=True)
    after = snapshot(root)
    changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    records = [old[row["source"]] for row in selected if row["selection"] == "attempt"]
    for record in records:
        dependent_changes = [name for name, sha in record["dependencies"].items()
                             if not Path(name).is_file() or digest(Path(name)) != sha]
        if dependent_changes and record["status"] == "completed":
            record["status"] = "input-drift"
        record["current_dependency_differences"] = dependent_changes
        write_json(Path(record["output"]).with_suffix(".result.json"), record)
    groups = collections.defaultdict(collections.Counter)
    errors = collections.defaultdict(set)
    for row in selected:
        if row["selection"] == "excluded":
            groups[row["group"]]["excluded"] += 1
    for record in records:
        groups[record["group"]][record["status"]] += 1
        for error in record["errors"]:
            errors[error["message"]].add(record["source"])
    report.setdefault("batch_history", [{"mode": "full", "attempted": report["attempted"], "counts": report["counts"]}])
    report["batch_history"].append({"mode": "delta", "attempted": len(pending), "sources": [row["source"] for row in pending],
                                    "input_changes": changed})
    report.update(attempted=len(records), excluded=len(selected) - len(records),
                  counts=dict(collections.Counter(record["status"] for record in records)),
                  groups={key: dict(value) for key, value in sorted(groups.items())}, units=records,
                  error_groups={key: sorted(value) for key, value in sorted(errors.items())},
                  source_changes_during_batch=changed,
                  sources_added_during_batch=sorted({row["source"] for row in inventory(root)} - {row["source"] for row in selected}))
    write_json(build / "inventory.json", selected)
    write_json(build / "dependency-inventory.json", dependency_inventory(root, selected))
    write_json(build / "delta-source-state-before.json", before)
    write_json(build / "delta-source-state-after.json", after)
    write_json(build / "runtime-report.json", report)
    return symbols_only(root, build, game, expected_game_sha)


def run_demangler(names, log):
    result = subprocess.run(["llvm-cxxfilt"], input="\n".join(names) + "\n", text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(names):
        raise RuntimeError("Demangled symbol count mismatch")
    log.write_text(result.stdout)
    return {"command": ["llvm-cxxfilt"], "exit_code": result.returncode}, lines


def markdown_report(report):
    lines = ["# Vita Runtime Portability Batch", "", f"Baseline: `{report['revision']}` (dirty state retained).",
             f"Attempted {report['attempted']} units; counts: `{report['counts']}`; excluded {report['excluded']}.", "",
             "| Group | Completed | Failed | Drift / Invalid | Excluded |", "| --- | ---: | ---: | ---: | ---: |"]
    for group, counts in report["groups"].items():
        lines.append(f"| {group} | {counts.get('completed', 0)} | {counts.get('failed', 0)} | {counts.get('input-drift', 0) + counts.get('invalid-object', 0)} | {counts.get('excluded', 0)} |")
    lines += ["", "## Real Compiler Error Groups", ""]
    for message, sources in report["error_groups"].items():
        lines.append(f"- `{message}`: " + ", ".join(f"`{source}`" for source in sources))
    lines += ["", "## Symbol Boundary", "", json.dumps(report["symbols"]["counts"], sort_keys=True),
              f"Relocatable link exit: {report['symbols']['relocatable_link']['exit_code']}; strict SDK link exit: {report['symbols']['strict_sdk_link']['exit_code']}.",
              "", "All failures and exclusions remain visible. A partial archive or relocatable link is not runtime portability, executable boot, graphics, or gameplay proof."]
    for group, symbols in report["symbols"]["strict_sdk_link"].get("groups", {}).items():
        lines.append(f"- {group} ({len(symbols)}): " + ", ".join(f"`{name}`" for name in symbols))
    return "\n".join(lines) + "\n"


def main():
    root, build, gamebuild = Path("/source"), Path("/build"), Path("/gamebuild")
    game = gamebuild / "libgalaxy_vita_sources.a"
    game_report = json.loads((gamebuild / "build-report.json").read_text())
    expected_game_sha = validate_game_archive(game, game_report)
    if sys.argv[1:] == ["--symbols-only"]:
        return symbols_only(root, build, game, expected_game_sha)
    if sys.argv[1:] not in ([], ["--delta"]):
        raise ValueError("Use no arguments, --delta, or --symbols-only")
    database = json.loads((gamebuild / "compile_commands.json").read_text())
    bases = {suffix: base_command(next(entry for entry in database if entry["file"].endswith(suffix))) for suffix in (".c", ".cpp")}
    if sys.argv[1:] == ["--delta"]:
        return delta_batch(root, build, game, bases, expected_game_sha)
    write_json(build / "base-flags.json", bases)
    before = snapshot(root)
    write_json(build / "source-state-before.json", before)
    selected = inventory(root)
    write_json(build / "inventory.json", selected)
    write_json(build / "dependency-inventory.json", dependency_inventory(root, selected))
    versions, _ = run(["clang", "--version"], build / "clang-version.log")
    run(["ld.lld", "--version"], build / "lld-version.log")
    records = []
    attempts = [row for row in selected if row["selection"] == "attempt"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(compile_unit, row, bases, root, build) for row in attempts]
        for future in concurrent.futures.as_completed(futures):
            record = future.result()
            records.append(record)
            print(f"[{len(records)}/{len(attempts)}] {record['status']}: {record['source']}", flush=True)
    after = snapshot(root)
    changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    write_json(build / "source-state-after.json", after)
    for record in records:
        dependent_changes = [name for name in record["dependencies"] if name.startswith("/source/") and name[8:] in changed]
        if dependent_changes and record["status"] == "completed":
            record["status"] = "input-drift"
        record["dependent_changes_during_batch"] = dependent_changes
        write_json(Path(record["output"]).with_suffix(".result.json"), record)
    archive, symbols = symbol_audit(root, build, game, records)
    if digest(game) != expected_game_sha:
        raise RuntimeError("Read-only game archive changed during runtime audit")
    after_link = snapshot(root)
    final_changes = sorted(name for name in before.keys() | after_link.keys() if before.get(name) != after_link.get(name))
    write_json(build / "source-state-final.json", after_link)
    baseline = json.loads((gamebuild / "source-state-after.json").read_text())
    game_input_differences = [name for name, sha in baseline.items() if (root / name).is_file() and digest(root / name) != sha]
    counts = dict(collections.Counter(record["status"] for record in records))
    groups = collections.defaultdict(collections.Counter)
    errors = collections.defaultdict(set)
    for row in selected:
        if row["selection"] == "excluded":
            groups[row["group"]]["excluded"] += 1
    for record in records:
        groups[record["group"]][record["status"]] += 1
        for error in record["errors"]:
            errors[error["message"]].add(record["source"])
    added = sorted({row["source"] for row in inventory(root)} - {row["source"] for row in selected})
    report = {"revision": (build / "source-revision.txt").read_text().strip(),
              "game_archive": {"path": str(game), "sha256": expected_game_sha, "members": 1834,
                               "current_input_differences_from_game_snapshot": game_input_differences},
              "attempted": len(attempts), "excluded": len(selected) - len(attempts), "counts": counts,
              "groups": {key: dict(value) for key, value in sorted(groups.items())},
              "source_changes_during_batch": final_changes, "sources_added_during_batch": added,
              "error_groups": {key: sorted(value) for key, value in sorted(errors.items())},
              "archive": archive, "symbols": symbols, "units": sorted(records, key=lambda row: row["source"]),
              "batch_complete": True, "full_runtime_portable": False, "compiler_version_record": versions}
    write_json(build / "runtime-report.json", report)
    (build / "runtime-summary.md").write_text(markdown_report(report))
    print(json.dumps({key: report[key] for key in ("attempted", "excluded", "counts", "groups", "source_changes_during_batch", "sources_added_during_batch", "batch_complete")}, indent=2))
    print(json.dumps(symbols["counts"], indent=2))
    return 1 if counts.get("failed") or counts.get("input-drift") or counts.get("invalid-object") or final_changes or added or symbols["strict_sdk_link"]["exit_code"] else 0


if __name__ == "__main__":
    sys.exit(main())
