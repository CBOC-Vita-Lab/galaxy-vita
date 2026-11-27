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

Environment: VITA_PGO_PROFILE=<merged .profdata> compiles the renderer's units (platform/vgx/src, platform/src/gx) with
-fprofile-use (build.py --pgo-profile sets it).  VITA_PGO_GENERATE=1 compiles those same units with -fprofile-generate instead (the
instrumented build that records a profile on the console, tools/pgo/README.md "Record on the console"); a profile is then ignored.
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


# The runtime units a profile (tools/pgo/smg.profdata.gz) is recorded for: the renderer, as the armhf harness builds it.
PGO_PREFIXES = ("platform/vgx/src/", "platform/src/gx/")


def base_command(entry):
    original = entry.get("arguments") or shlex.split(entry["command"])
    command, index = [], 0
    while index < len(original):
        argument = original[index]
        if argument in ("-o", "-c", "-MF", "-MT", "-MQ"):
            index += 2
            continue
        if argument in ("-MD", "-MMD") or argument.startswith(("-fprofile-use", "-fprofile-instr-use", "-fprofile-generate")):
            # (a game-unit profile does not describe the runtime's files; nor does the game units' instrumentation apply to them)
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
    pgo_profile = os.environ.get("VITA_PGO_PROFILE", "")
    pgo_generate = os.environ.get("VITA_PGO_GENERATE") == "1"
    if pgo_generate:
        pgo_profile = ""  # (recording: the units are instrumented, none is compiled with a profile)
    if pgo_profile and not Path(pgo_profile).is_file():
        raise SystemExit(f"VITA_PGO_PROFILE: no such file {pgo_profile}")
    database = json.loads((source_build / "compile_commands.json").read_text())
    bases = {suffix: base_command(next(e for e in database if e["file"].endswith(suffix)))
             for suffix in (".c", ".cpp")}
    vgx_base = strip_forced_includes(bases[".cpp"])
    # core2: the multiplayer build choices come with the source stage's flags (cmake/MpBuildDefs.cmake on vita_source_options): say
    # which ones the platform units get, and refuse a database made before they were wired (no MP_BUILD_ID at all)
    mp_flags = [f for f in bases[".cpp"] if f.startswith(("-DMP_BUILD_ID=", "-DNP_ONLINE_URL="))]
    if not any(f.startswith("-DMP_BUILD_ID=") for f in mp_flags):
        raise SystemExit("the source stage's compile database has no -DMP_BUILD_ID: configure the source stage again (build.py source)")
    print("multiplayer build flags: " + " ".join(mp_flags))
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
        "-Wno-unused-function",
    ]
    # label32: the build label (it holds the commit, so it changes every build) goes only to the two units that print it; given to
    # every renderer unit, it made all 24 of them recompile on every build (~1 min a build) for two log lines.
    label_units = {"platform/vgx/vita/vgx_host_galaxy_vita.cpp", "platform/vgx/vita/vgx_host_vita.cpp"}
    for source, selection, group, reason in rows:
        if selection != "attempt":
            continue
        suffix = ".c" if source.endswith(".c") else ".cpp"
        if group == "vgx":
            cmd, extra = vgx_base, vgx_defs + ([f"-DGXV_COMMIT=\"{commit}\""] if source in label_units else []) + shlex.split(os.environ.get("VITA_VGX_CFLAGS", ""))  # (e.g. -O3: later flags win)
        else:
            cmd = bases[suffix]
            extra = ["-DPORT_PLATFORM_CODE=1", f"-I{root / 'vita/runtime'}", "-DPORT_GX_VGX=1", "-DPORT_GX_VGX_ONLY=1",
                     f"-I{root / 'platform/vgx/src'}"]
            if source.startswith("platform/src/mp/"):
                # MP3: the session layer sees net/'s C API and VLink (vlink.h, np_glue.h, np_https.h, np_json.h); without it
                # mp_transport_vlink.cpp / mp_service.cpp compile their "no net/" fallbacks and the Vita has no multiplayer link.
                extra.append(f"-I{root / 'net/include'}")
            if os.environ.get("VITA_MEM2_MIB"):  # experiment: bigger MEM2 bank (the original Wii value is 64)
                extra.append(f"-DPORT_MEM2_SIZE={int(os.environ['VITA_MEM2_MIB']) << 20}u")
            if os.environ.get("VITA_MEM1_MIB"):
                extra.append(f"-DPORT_MEM1_SIZE={int(os.environ['VITA_MEM1_MIB']) << 20}u")
            if group == "SDK-short":
                extra.append("-fshort-enums")
        # Experiment switch: extra compiler flags for every runtime unit (for example -ftrivial-auto-var-init=zero).
        extra = extra + shlex.split(os.environ.get("VITA_EXTRA_CFLAGS", ""))
        # Profile-guided optimisation (build.py --pgo-profile, tools/pgo/README.md): the renderer's units, i.e. the vgx front end
        # and back end and the GX glue they are fed by.  Every other runtime unit stays as it was (the profile has nothing for it).
        if pgo_profile and source.endswith(".cpp") and source.startswith(PGO_PREFIXES):
            extra.append(f"-fprofile-use={pgo_profile}")
        elif pgo_generate and source.endswith(".cpp") and source.startswith(PGO_PREFIXES):
            extra.append("-fprofile-generate")  # (the flag of the armhf harness: the profile's function hashes are the same)
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
