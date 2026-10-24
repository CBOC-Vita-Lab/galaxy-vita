#!/usr/bin/env python3
"""Build the Super Mario Galaxy Vita executable (eboot.bin) from this source. No packaging or device writes.

    python3 -I tools/vgx_build/build.py check|source|lto|runtime|link|verify|all [--build-dir DIR] [--jobs N]

Stages: source (the 1,834 game units, Clang), lto (the same units as ThinLTO bitcode, linked into one object),
runtime (the Vita runtime and the vgx renderer), link (eboot.bin). `all` runs them in that order.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
# The player build: no diagnostics and no console output, but a basic log in ux0:data/smg/log.txt (frame rate and hitches
# every 5 s, picture-size changes, settings, errors), written in batches.
QUIET_FLAGS = "-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1"
# The renderer is compiled at -O3 (3 % fewer instructions per picture than -O2 in the ARM harness).
VGX_FLAGS = "-O3"
DEFAULT_LABEL = "1.0"
ARTIFACTS = {
    "gameArchive": "vita-source/libgalaxy_vita_sources.a",
    "gameArchiveLto": "vita-source-lto/libgalaxy_vita_sources.a",
    "runtimeArchive": "vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a",
    "elf": "vita-vgx-game/galaxy_native", "velf": "vita-vgx-game/galaxy_native.velf",
    "runtime": "vita-vgx-game/eboot.bin",
}
OUTPUT_DIRS = ("logs", "vita-source", "vita-source-lto", "vita-vgx-runtime", "vita-vgx-game", "case-shim")


def digest(path):
    with path.open("rb") as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def verify_files(root, records, label):
    for name, expected in records.items():
        path = root / name
        if not path.is_file():
            raise ValueError(f"{label}: missing {name}")
        if digest(path) != expected:
            raise ValueError(f"{label}: hash mismatch for {name}")


def require_files(root, names, label):
    missing = [name for name in names if not (root / name).is_file()]
    if missing:
        raise ValueError(f"{label}: missing {', '.join(missing)}")


def build_environment(sdk, build, inherited, logging=False):
    env = dict(inherited)
    for key in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
                "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
                "VITA_VGX_CFLAGS", "VITA_EXTRA_CFLAGS", "VITA_MEM1_MIB", "VITA_MEM2_MIB"):
        env.pop(key, None)
    env.update(VITASDK=str(sdk), PATH=str(sdk / "bin") + os.pathsep + env.get("PATH", ""),
               CPATH=str(build / "case-shim"), VITA_VGX_CFLAGS=VGX_FLAGS)
    if not logging:
        env["VITA_EXTRA_CFLAGS"] = QUIET_FLAGS
    return env


def run(args, env, log=None, cwd=None):
    args = list(map(str, args))
    if log is None:
        return subprocess.check_output(args, env=env, text=True, cwd=cwd)
    with log.open("w") as stream:
        result = subprocess.run(args, env=env, stdout=stream, stderr=subprocess.STDOUT, cwd=cwd)
    if result.returncode:
        raise ValueError(f"Build failed: {args[0]}; see {log}")
    print(f"Completed: {log.name}", flush=True)


def input_root(name):
    value = os.environ.get(name)
    if not value or not Path(value).is_dir():
        raise ValueError(f"{name} must name an existing input directory")
    return Path(value).resolve()


def validate_output_tree(build, root, sdk, deps):
    if build in (root, sdk, deps) or any(path.is_relative_to(build) for path in (root, sdk, deps)):
        raise ValueError("Build directory must not contain the source, SDK or dependency directory")
    if build.is_relative_to(sdk) or build.is_relative_to(deps):
        raise ValueError("Build directory must not modify SDK or dependency inputs")
    if build.is_relative_to(root) and build != root / "build-vgx":
        raise ValueError("Use build-vgx or an external directory, not another part of the source tree")

    def fail_walk(error):
        raise error

    for name in OUTPUT_DIRS:
        output = build / name
        if output.is_symlink():
            raise ValueError(f"Build output must not be a symlink: {output}")
        if not output.exists():
            continue
        if not output.is_dir():
            raise ValueError(f"Build output must be a directory: {output}")
        for directory, dirs, files in os.walk(output, onerror=fail_walk, followlinks=False):
            for child in dirs + files:
                path = Path(directory) / child
                if not path.is_symlink():
                    continue
                # Only the read-only header aliases deliberately created by the shim may link out.
                target = path.resolve()
                if name == "case-shim" and path.is_file() and target.is_relative_to(root) \
                        and not target.is_relative_to(build):
                    continue
                raise ValueError(f"Build output contains an unsafe symlink: {path}")


def recorded_hashes():
    """Artifact hashes recorded for the release in docs/BUILD-VERIFICATION.json (only real SHA-256 values)."""
    record = json.loads((ROOT / "docs/BUILD-VERIFICATION.json").read_text())
    return {key: value for key, value in record.get("sha256", {}).items()
            if key in ARTIFACTS and re.fullmatch(r"[0-9a-f]{64}", str(value))}


def write_lto_ninja(database, out):
    """The game units as ThinLTO bitcode: the exact commands of the source stage plus -flto=thin."""
    lines = ["ninja_required_version = 1.10", "rule cc", "  command = cd $dir && $cmd", "  depfile = $out.d",
             "  deps = gcc", "  description = LTO-CC $src", ""]
    objects = []
    for row in database:
        source = row["file"]
        if "/decomp/" not in source:
            continue
        args = row.get("arguments") or shlex.split(row["command"])
        if "-o" not in args:
            continue
        name = hashlib.sha1(source.encode()).hexdigest()[:10] + "_" + os.path.basename(source) + ".bc.o"
        obj = str(out / "obj" / name)
        args = list(args)
        args[args.index("-o") + 1] = obj
        clean, skip = [], False
        for arg in args:
            if skip:
                skip = False
                continue
            if arg in ("-MF", "-MT", "-MQ"):
                skip = True
                continue
            if arg in ("-MD", "-MMD"):
                continue
            clean.append(arg)
        clean[1:1] = ["-MD", "-MF", obj + ".d", "-flto=thin"]
        objects.append(obj)
        lines += [f"build {obj}: cc {source}", f"  dir = {row['directory']}", f"  src = {os.path.basename(source)}",
                  "  cmd = " + " ".join(shlex.quote(a) for a in clean).replace("$", "$$")]
    if not objects:
        raise ValueError("No game units in the compile database; run the source stage first")
    (out / "obj").mkdir(parents=True, exist_ok=True)
    (out / "build.ninja").write_text("\n".join(lines) + "\n")
    (out / "objects.txt").write_text("\n".join(objects) + "\n")
    return len(objects)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stage", choices=("check", "source", "lto", "runtime", "link", "verify", "all"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-vgx")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--no-lto", action="store_true", help="link the plain game archive instead of the ThinLTO one")
    parser.add_argument("--logging", action="store_true",
                        help="a test build that keeps the port's log (ux0:data/smg/log.txt); never for a release")
    parser.add_argument("--label", default=DEFAULT_LABEL, help="the build label the renderer logs (default 1.0)")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        raise ValueError("--jobs must be between 1 and 32")
    if not re.fullmatch(r"[A-Za-z0-9.+_-]{1,40}", args.label):
        raise ValueError("--label may only use letters, digits and . + _ -")
    sdk, deps = input_root("VITASDK"), input_root("VGL_DEPS")
    inputs = json.loads((HERE / "inputs.json").read_text())
    require_files(sdk, inputs["sdk"], "VITASDK")
    require_files(deps, inputs["dependencies"], "VGL_DEPS")
    source = json.loads((ROOT / "docs/SOURCE-MANIFEST.json").read_text())
    verify_files(ROOT, {row["path"]: row["sha256"] for row in source["files"]}, "Source")
    if args.stage == "check":
        print(f"Source ({len(source['files'])} files), dependency and SDK inputs present. No build started.")
        return

    build = args.build_dir.resolve()
    for path in (ROOT, build, sdk, deps):
        if re.search(r"[\s:$]", str(path)):
            raise ValueError("Build paths must not contain whitespace, ':' or '$'; use the documented container mounts")
    validate_output_tree(build, ROOT, sdk, deps)
    game_archive = ARTIFACTS["gameArchive" if args.no_lto else "gameArchiveLto"]
    if args.stage == "verify":
        expected = recorded_hashes()
        if not expected:
            for key in ("runtimeArchive", "elf", "velf", "runtime"):
                path = build / ARTIFACTS[key]
                print(f"{key}: {digest(path) if path.is_file() else 'missing'}")
            print("No release hashes are recorded yet in docs/BUILD-VERIFICATION.json.")
            return
        verify_files(build, {ARTIFACTS[key]: value for key, value in expected.items()}, "Build")
        print(f"All {len(expected)} recorded artifacts match docs/BUILD-VERIFICATION.json.")
        return

    env = build_environment(sdk, build, os.environ, args.logging)
    clang = run(["clang", "--version"], env)
    if not re.search(r"clang version 18\.", clang):
        print("Note: the release was built with Clang 18.1.3; another version gives a different executable.",
              file=sys.stderr)
    build.mkdir(parents=True, exist_ok=True)
    logs = build / "logs"
    logs.mkdir(exist_ok=True)
    shim = build / "case-shim"
    if not shim.exists():
        run([sys.executable, "-I", HERE / "make_case_shim.py", ROOT, shim], env, logs / "case-shim.log")
    source_build, lto, runtime, game = (build / name for name in
                                        ("vita-source", "vita-source-lto", "vita-vgx-runtime", "vita-vgx-game"))
    if args.stage == "all":
        stages = ("source", "runtime", "link") if args.no_lto else ("source", "lto", "runtime", "link")
    else:
        stages = (args.stage,)
    for stage in stages:
        print(f"Building {stage}...", flush=True)
        if stage == "source":
            run(["cmake", "-U", "CMAKE_C_FLAGS", "-U", "CMAKE_CXX_FLAGS", "-G", "Ninja",
                 "-S", ROOT / "vita", "-B", source_build,
                 f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/vita/clang-vita-toolchain.cmake",
                 f"-DVITASDK={sdk}", "-DCMAKE_BUILD_TYPE=Release"], env, logs / "source-configure.log")
            run(["ninja", "-C", source_build, f"-j{args.jobs}", "galaxy_vita_sources"], env, logs / "source.log")
        elif stage == "lto":
            database = json.loads((source_build / "compile_commands.json").read_text())
            count = write_lto_ninja(database, lto)
            print(f"{count} game units as ThinLTO bitcode", flush=True)
            run(["ninja", "-C", lto, f"-j{args.jobs}"], env, logs / "lto-compile.log")
            # One relocatable native object: ThinLTO imports and inlines across units; -r keeps every global symbol
            # (the game is linked --whole-archive, and the runtime calls into it by name).
            run(["ld.lld", "-r", "-o", lto / "galaxy_lto.o", f"--thinlto-jobs={args.jobs}", "--lto-O2",
                 "-mllvm", "-emulated-tls", f"@{lto / 'objects.txt'}"], env, logs / "lto-link.log")
            (lto / "libgalaxy_vita_sources.a").unlink(missing_ok=True)
            run(["llvm-ar", "rcs", lto / "libgalaxy_vita_sources.a", lto / "galaxy_lto.o"], env, logs / "lto-archive.log")
        elif stage == "runtime":
            database = json.loads((source_build / "compile_commands.json").read_text())
            if not database or any(not Path(row["file"]).is_relative_to(ROOT) for row in database):
                raise ValueError("Compile database belongs to a different source checkout")
            run([sys.executable, "-I", HERE / "vita_runtime_gen_vgx.py", ROOT, source_build,
                 runtime, deps, args.label], env, logs / "runtime-configure.log")
            run(["ninja", "-C", runtime, f"-j{args.jobs}"], env, logs / "runtime.log")
            run([sys.executable, "-I", HERE / "vita_runtime_report_vgx.py", ROOT, runtime], env, logs / "runtime-report.log")
        else:
            require_files(build, (game_archive, ARTIFACTS["runtimeArchive"]), "Link inputs")
            cxx_flags = "" if args.logging else QUIET_FLAGS
            run(["cmake", "-G", "Ninja", "-S", ROOT / "vita/vgx-game", "-B", game,
                 f"-DCMAKE_TOOLCHAIN_FILE={sdk}/share/vita.toolchain.cmake", "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DPORT_SILENT_STDIO={'OFF' if args.logging else 'ON'}",
                 f"-DRUNTIME_ARCHIVE={build / ARTIFACTS['runtimeArchive']}",
                 f"-DGAME_ARCHIVE={build / game_archive}", f"-DVGL_DEPS={deps}",
                 f"-DCMAKE_CXX_FLAGS={cxx_flags}"], env, logs / "link-configure.log")
            run(["cmake", "--build", game, "--target", "eboot.bin", "--parallel", args.jobs], env, logs / "link.log")
            print(f"eboot.bin: {digest(build / ARTIFACTS['runtime'])}")
            print("No VPK created and no device changed. Package it with the CBOC release VPK's sce_sys and"
                  " shadercache (docs/BUILDING.md).")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
