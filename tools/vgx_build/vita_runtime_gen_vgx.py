#!/usr/bin/env python3
"""Generate a ninja build for the vgx flavour of the Vita "runtime" objects.

Uses the base command from the source build's compile_commands.json
so the flag set is exactly the one of the 1,834 game units) but
  * uses a fresh compile database configured in the same source checkout;
  * platform code gets -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 and -I platform/vgx/src; the old renderer's
    files (GLES/EGL, the sceGxm worker, the recorder) and vita/runtime/gpu_*.cpp are excluded;
  * platform/vgx/src/*.cpp (except the null back end) and platform/vgx/vita/*.cpp are compiled as a group
    of their own: no forced compat.h / prelude, the Strikers vgx flags (-DNOMINMAX -D__VGX_VITA__, the GLES2
    shim, the prebuilt vitaGL headers); same ABI flags (-fno-short-enums) as the platform code that calls them.

usage: vita_runtime_gen_vgx.py <galaxy-root> <vita-source-build-dir> <runtime-build-dir> <vitagl-deps-dir> [commit]
"""
import json
import os
import re
import shlex
import sys
from pathlib import Path

EXCLUDE_PREFIX = ("platform/android/", "platform/src/xr/", "platform/desktop/", "platform/vgx/")
EXCLUDE_FILES = {
    # legacy renderer (GLES 3.2 / EGL), the sceGxm renderer of the previous agent, the recorder
    "platform/src/gx/gl_renderer.cpp": "legacy GLES 3.2 renderer",
    "platform/src/gx/headless_render.cpp": "legacy EGL headless renderer",
    "platform/src/gx/gx_recorder.cpp": "legacy recorder (PORT_GX_VGX_ONLY: vgx_galaxy.cpp replaces it)",
    "platform/src/gx/gx_ram_texture.cpp": "sceGxm texture path (excluded by the desktop CMake too)",
}
EXCLUDE_REGEX = [
    (re.compile(r"^platform/src/gx/vita_[a-z_]+\.cpp$"), "older sceGxm renderer"),
    (re.compile(r"^vita/runtime/gpu_[a-z_]+\.cpp$"), "older sceGxm renderer"),
]


def base_command(entry):
    original = entry.get("arguments") or shlex.split(entry["command"])
    command, index = [], 0
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


def strip_forced_includes(command):
    out, i = [], 0
    while i < len(command):
        if command[i] == "-include":
            i += 2
            continue
        out.append(command[i])
        i += 1
    return out


def inventory(root):
    paths = set()
    for directory in ("platform", "vita/runtime"):
        for suffix in ("*.cpp", "*.c"):
            paths.update((root / directory).rglob(suffix))
    rows = []
    for path in sorted(paths):
        source = path.relative_to(root).as_posix()
        group, reason = "game-int32", ""
        selection = "attempt"
        if source.startswith("vita/runtime/"):
            group = "SDK-short"
        for prefix in EXCLUDE_PREFIX:
            if source.startswith(prefix):
                selection, reason = "excluded", prefix + " (own group or not for the Vita)"
        if source in EXCLUDE_FILES:
            selection, reason = "excluded", EXCLUDE_FILES[source]
        for rx, why in EXCLUDE_REGEX:
            if rx.match(source):
                selection, reason = "excluded", why
        if source.startswith("platform/vgx/src/") and source.endswith(".cpp") and not source.endswith("vgx_backend_null.cpp"):
            selection, group, reason = "attempt", "vgx", ""
        if source == "platform/vgx/vita/vgx_host_galaxy_vita.cpp":
            selection, group, reason = "attempt", "vgx", ""
        if source == "platform/vgx/vita/vgx_host_vita.cpp":
            reason = "Strikers host, kept for reference"
        rows.append((source, selection, group, reason))
    return rows


def esc(text):
    return text.replace("$", "$$")


def main():
    root, source_build, out, deps = (Path(a).resolve() for a in sys.argv[1:5])
    commit = sys.argv[5] if len(sys.argv) > 5 else "unknown"
    database = json.loads((source_build / "compile_commands.json").read_text())
    bases = {suffix: base_command(next(e for e in database if e["file"].endswith(suffix)))
             for suffix in (".c", ".cpp")}
    vgx_base = strip_forced_includes(bases[".cpp"])
    rows = inventory(root)
    out.mkdir(parents=True, exist_ok=True)
    (out / "inventory.tsv").write_text("source\tselection\tgroup\treason\n" + "".join("\t".join(r) + "\n" for r in rows))
    lines = [
        "ninja_required_version = 1.10",
        "rule cc",
        # A failed compile leaves no object behind and its diagnostics stay in <obj>.log.
        "  command = $cmd $extra -MD -MF $out.d -o $out -c $in > $out.log 2>&1 || { rm -f $out; cat $out.log; exit 1; }",
        "  description = CC $in",
        "  depfile = $out.d",
        "  deps = gcc",
        "",
    ]
    vgx_defs = [
        "-DNOMINMAX", "-D__VGX_VITA__", "-DGXV_ABSIDX_DEFAULT=0", "-DGXV_STRICT_DRAW_PROFILE=1",
        f"-I{root / 'platform/vgx/vita/include'}", f"-I{deps / 'include'}", f"-I{root / 'platform/vgx/src'}",
        f"-I{root / 'platform/vgx/third_party'}", f"-I{root / 'platform/vgx/vita'}",
        f"-DGXV_COMMIT=\"{commit}\"", "-Wno-unused-function",
    ]
    for source, selection, group, reason in rows:
        if selection != "attempt":
            continue
        suffix = ".c" if source.endswith(".c") else ".cpp"
        if group == "vgx":
            cmd, extra = vgx_base, vgx_defs + shlex.split(os.environ.get("VITA_VGX_CFLAGS", ""))  # (e.g. -O3: later flags win)
        else:
            cmd = bases[suffix]
            extra = ["-DPORT_PLATFORM_CODE=1", f"-I{root / 'vita/runtime'}", "-DPORT_GX_VGX=1", "-DPORT_GX_VGX_ONLY=1",
                     f"-I{root / 'platform/vgx/src'}"]
            if os.environ.get("VITA_MEM2_MIB"):  # experiment: bigger MEM2 bank (the original Wii value is 64)
                extra.append(f"-DPORT_MEM2_SIZE={int(os.environ['VITA_MEM2_MIB']) << 20}u")
            if os.environ.get("VITA_MEM1_MIB"):
                extra.append(f"-DPORT_MEM1_SIZE={int(os.environ['VITA_MEM1_MIB']) << 20}u")
            if group == "SDK-short":
                extra.append("-fshort-enums")
        # Experiment switch: extra compiler flags for every runtime unit (for example -ftrivial-auto-var-init=zero).
        extra = extra + shlex.split(os.environ.get("VITA_EXTRA_CFLAGS", ""))
        obj = out / "objects" / (source + ".obj")
        lines.append(f"build {obj}: cc {root / source}")
        lines.append(f"  cmd = {esc(shlex.join(cmd))}")
        lines.append(f"  extra = {esc(shlex.join(extra))}")
    (out / "build.ninja").write_text("\n".join(lines) + "\n")
    n = sum(1 for r in rows if r[1] == "attempt")
    print(f"generated {out / 'build.ninja'}: {n} units ({sum(1 for r in rows if r[2] == 'vgx' and r[1] == 'attempt')} vgx), "
          f"{len(rows) - n} excluded")


if __name__ == "__main__":
    main()
