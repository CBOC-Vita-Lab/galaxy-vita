#!/usr/bin/env python3
"""Build the Super Mario Galaxy Vita executable (eboot.bin) from this source. No packaging or device writes.

    python3 -I tools/vgx_build/build.py check|source|lto|runtime|link|verify|all [--build-dir DIR] [--jobs N]

Stages: source (the 1,834 game units, Clang), lto (the same units as ThinLTO bitcode, linked into one object),
runtime (the Vita runtime and the vgx renderer), link (eboot.bin). `all` runs them in that order.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
# The player build: no diagnostics and no console output, but a basic log in ux0:data/smg/log.txt (frame rate and hitches
# every 5 s, picture-size changes, settings, errors), written in batches.
QUIET_FLAGS = "-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1"
# The renderer is compiled at -O3 (3 % fewer instructions per picture than -O2 in the ARM harness).
VGX_FLAGS = "-O3"
DEFAULT_LABEL = "1.2"
DEFAULT_ONLINE_URL = "https://galaxy.psvita.dev"   # the Galaxy service (Worker custom domain); keep in step with cmake/MpBuildDefs.cmake
ARTIFACTS = {
    "gameArchive": "vita-source/libgalaxy_vita_sources.a",
    "gameArchiveLto": "vita-source-lto/libgalaxy_vita_sources.a",
    "runtimeArchive": "vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a",
    "elf": "vita-vgx-game/galaxy_native", "velf": "vita-vgx-game/galaxy_native.velf",
    "runtime": "vita-vgx-game/eboot.bin",
}
OUTPUT_DIRS = ("logs", "vita-source", "vita-source-lto", "vita-vgx-runtime", "vita-vgx-game", "case-shim", "pgo")
# An indexed clang profile (llvm-profdata merge output) starts with this; a .profraw or a text profile does not.
PROFDATA_MAGIC = b"\xfflprofi\x81"
# Functions with internal linkage are keyed in a profile by "<source path>:<name>", so a profile recorded in another
# checkout names them with that checkout's path.
# (the game units are under decomp/, the renderer's under platform/ and vita/runtime/)
PROFILE_TREES = rb"(?:decomp/(?:src|libs|include)|platform/(?:src|vgx|include)|vita/(?:runtime|include))"
PROFILE_PATH_RX = re.compile(rb"(/[A-Za-z0-9_.@+-]+(?:/[A-Za-z0-9_.@+-]+)*)/" + PROFILE_TREES + rb"/")
PROFILE_TEXT_PATH_RX = re.compile(rb"^(/[^:\n]*?)/" + PROFILE_TREES + rb"/", re.M)


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
                "VITA_VGX_CFLAGS", "VITA_EXTRA_CFLAGS", "VITA_MEM1_MIB", "VITA_MEM2_MIB", "VITA_PGO_PROFILE"):
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


def link_check(build, env, log):
    """After the link (linkfix31): every symbol of tools/vita_must_define.txt is defined strongly in the ELF, and no weak-only reference names
    code that a link input has but the link left out (a weak reference never pulls an archive member: integ-3.1's first Vita build lost the
    native Settings' pause hint this way). tools/check_weak_links.py; the allow list (tools/weak_links.allow) names the optional hooks."""
    args = [sys.executable, "-I", ROOT / "tools/check_weak_links.py", build / ARTIFACTS["elf"], "--build-dir", build,
            "--allow", ROOT / "tools/weak_links.allow", "--must-define", ROOT / "tools/vita_must_define.txt"]
    with log.open("w") as stream:
        result = subprocess.run(list(map(str, args)), env=env, stdout=stream, stderr=subprocess.STDOUT)
    lines = log.read_text(errors="replace").splitlines()
    if result.returncode:
        for line in lines:
            if line.startswith("FAIL "):
                print(line, file=sys.stderr)
        raise ValueError(f"Link check failed (the ELF lacks code it must have); see {log}")
    print(f"Completed: {log.name} ({lines[-1] if lines else 'no output'})", flush=True)


def profile_path_prefix(data):
    """The checkout path the profile was recorded in (from the names of its internal-linkage functions), or None."""
    found = PROFILE_PATH_RX.search(data)
    return found.group(1).decode() if found else None


def prepare_profile(profile, root, directory, env):
    """The profile the game units are compiled with, as <directory>/<sha256 prefix>.profdata.

    The name changes with the content, so a different profile rebuilds the units (ninja does not watch the profile).
    A profile recorded in another checkout is rewritten to this one's path with llvm-profdata, so that the functions
    with internal linkage still match; one without such functions, or from this checkout, is copied unchanged."""
    data = profile.read_bytes()
    if profile.suffix == ".gz":
        data = gzip.decompress(data)
    if not data.startswith(PROFDATA_MAGIC):
        raise ValueError(f"{profile} is not an indexed profile: merge the .profraw files with llvm-profdata merge -o")
    directory.mkdir(parents=True, exist_ok=True)
    prefix = profile_path_prefix(data)
    note = "used as it is"
    if prefix is not None and prefix != str(root):
        tool = shutil.which("llvm-profdata", path=env["PATH"]) or shutil.which("llvm-profdata-18", path=env["PATH"])
        if not tool:
            raise ValueError(f"The profile was recorded in {prefix}; rewriting it for {root} needs llvm-profdata")
        plain = directory / "profile-input.profdata"
        plain.write_bytes(data)
        text = directory / "profile.txt"
        run([tool, "merge", "--text", "-o", text, plain], env)
        plain.unlink()
        body = text.read_bytes()
        found = PROFILE_TEXT_PATH_RX.search(body)
        if found:
            prefix = found.group(1).decode()
            body = re.sub(rb"^" + re.escape(found.group(1)) + rb"(?=/(?:decomp|platform|vita)/)", str(root).encode(), body,
                          flags=re.M)
        text.write_bytes(body)
        rewritten = directory / "profile-rewritten.profdata"
        run([tool, "merge", "-o", rewritten, text], env)
        text.unlink()
        data = rewritten.read_bytes()
        rewritten.unlink()
        note = f"paths rewritten from {prefix}"
    out = directory / (hashlib.sha256(data).hexdigest()[:12] + ".profdata")
    out.write_bytes(data)
    print(f"PGO profile {out.name} ({len(data)} bytes, {note})", flush=True)
    return out


def cmake_source_root(build_dir):
    """The path the source stage's compile commands really use for this checkout.

    CMake keeps the source directory of an existing build directory's cache even when it is given the same directory under another
    name (a symlink such as /source/source -> the checkout), so a build directory first configured through the link keeps compiling
    through it, and Clang keys a profile's internal-linkage functions by that path, not by the resolved one (696 functions of the game
    never matched because the profile was rewritten to ROOT).  A fresh build directory uses ROOT."""
    cache = build_dir / "vita-source" / "CMakeCache.txt"
    try:
        for line in cache.read_text().splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                home = Path(line.split("=", 1)[1])
                if home.name == "vita" and home.parent.resolve() == ROOT:
                    return home.parent
    except OSError:
        pass
    return ROOT


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
    parser.add_argument("--pgo-profile", type=Path, default=None, metavar="FILE.profdata",
                        help="compile the game units and the renderer's units (platform/vgx/src, platform/src/gx) with this merged clang profile, "
                             ".profdata or .profdata.gz (-fprofile-use; tools/pgo/README.md); "
                             "default none: the plain build")
    parser.add_argument("--label", default=DEFAULT_LABEL, help="the build label the renderer logs (default 1.2)")
    # MP3: the TLS library of the multiplayer HTTPS client (net/); without it net/ is linked without TLS (Same Wi-Fi / Nearby only, no https service)
    parser.add_argument("--mbedtls", type=Path, default=Path(os.environ["SMG_MBEDTLS"]) if os.environ.get("SMG_MBEDTLS") else None,
                        metavar="PREFIX", help="mbedTLS prefix from tools/mp/build-mbedtls.py --target vita (default: $SMG_MBEDTLS, else none)")
    # core2: the multiplayer build choices (cmake/MpBuildDefs.cmake): the online service compiled in (default none: the build contacts
    # nothing by itself) and the build id of the multiplayer fingerprint (default dev)
    parser.add_argument("--online-url", default=os.environ.get("NP_ONLINE_URL", DEFAULT_ONLINE_URL), metavar="https://HOST[:PORT]|none",
                        help="online service compiled into the build (default: $NP_ONLINE_URL, else https://galaxy.psvita.dev; "
                             "'none' compiles no service in; there is no fallback to the old workers.dev address)")
    parser.add_argument("--build-id", default=os.environ.get("MP_BUILD_ID", "dev"),
                        help="multiplayer build id: consoles with different ids never play together (default: $MP_BUILD_ID, else dev)")
    parser.add_argument("--seed", type=Path, default=None, metavar="VPK|SHADERCACHE",
                        help="(fix31) link stage: also make the release VPK <build-dir>/Galaxy-Vita.vpk = the CMake smg-vgx.vpk plus this seed's shadercache/ (a VPK, "
                             "or a folder that is or holds shadercache/), its stage list tagged by tools/seedrun/stagetags.py; fails when the packaged list has no stamp")
    args = parser.parse_args()
    if args.seed is not None and not args.seed.exists():
        raise ValueError(f"--seed {args.seed}: not found")
    if args.online_url == "none":
        args.online_url = ""
    if args.online_url and not re.fullmatch(r"https://[A-Za-z0-9.-]+(:[0-9]+)?", args.online_url):
        raise ValueError("--online-url must be https://host[:port] (or none)")
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,40}", args.build_id):
        raise ValueError("--build-id may only use letters, digits and . _ + -")
    mp_defs = [f"-DNP_ONLINE_URL={args.online_url}", f"-DMP_BUILD_ID={args.build_id}"]
    if not 1 <= args.jobs <= 32:
        raise ValueError("--jobs must be between 1 and 32")
    if not re.fullmatch(r"[A-Za-z0-9.+_-]{1,40}", args.label):
        raise ValueError("--label may only use letters, digits and . + _ -")
    sdk, deps = input_root("VITASDK"), input_root("VGL_DEPS")
    inputs = json.loads((HERE / "inputs.json").read_text())
    require_files(sdk, inputs["sdk"], "VITASDK")
    require_files(deps, inputs["dependencies"], "VGL_DEPS")
    mbedtls = None
    if args.mbedtls is not None:   # MP3
        mbedtls = args.mbedtls.resolve()
        require_files(mbedtls, inputs["mbedtls"], "SMG_MBEDTLS")
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
    pgo = pgo_runtime = None
    # The path the game units are compiled through (see cmake_source_root); the renderer's units (the runtime stage) are always
    # generated from the resolved ROOT, so a profile for each stage names its own path.
    game_root = cmake_source_root(build)
    # VITA_PGO_GENERATE=1 (environment): the instrumented build that records a profile on the console (tools/pgo/README.md,
    # "Record on the console"); CMake and the runtime generator read the variable, a profile is not used with it.
    pgo_generate = env.get("VITA_PGO_GENERATE") == "1"
    if pgo_generate:
        print("VITA_PGO_GENERATE=1: instrumented build (-fprofile-generate, the dump thread, the profile runtime)"
              + ("; --pgo-profile is ignored" if args.pgo_profile is not None else ""), flush=True)
    elif args.pgo_profile is not None:
        if not args.pgo_profile.is_file():
            raise ValueError(f"--pgo-profile: no such file {args.pgo_profile}")
        if args.stage in ("source", "all"):
            pgo = prepare_profile(args.pgo_profile.resolve(), game_root, build / "pgo", env)
        if args.stage in ("runtime", "all"):
            pgo_runtime = pgo if game_root == ROOT and pgo else prepare_profile(args.pgo_profile.resolve(), ROOT, build / "pgo", env)
        if args.stage == "lto":
            print("Note: --pgo-profile applies when the source stage is configured; the lto stage reuses its commands",
                  file=sys.stderr)
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
                 f"-DVITASDK={sdk}", "-DCMAKE_BUILD_TYPE=Release", f"-DVITA_PGO_PROFILE={pgo or ''}"] + mp_defs, env, logs / "source-configure.log")
            run(["ninja", "-C", source_build, f"-j{args.jobs}", "galaxy_vita_sources"], env, logs / "source.log")
        elif stage == "lto":
            database = json.loads((source_build / "compile_commands.json").read_text())
            count = write_lto_ninja(database, lto)
            print(f"{count} game units as ThinLTO bitcode", flush=True)
            run(["ninja", "-C", lto, f"-j{args.jobs}"], env, logs / "lto-compile.log")
            # One relocatable native object: ThinLTO imports and inlines across units; -r keeps every global symbol
            # (the game is linked --whole-archive, and the runtime calls into it by name).
            # The game's ThinLTO back end at -O3 (1.3; GAME_LTO_OPT=2 gives 1.2's -O2): more inlining and unrolling of the J3D
            # animation/material loops and the actors' per-turn work, the game thread's limit. The units' own FP, aliasing and
            # wrapping options are kept in their bitcode; only the optimisation level of the native code generation changes.
            lto_opt = os.environ.get("GAME_LTO_OPT", "3")
            if lto_opt not in ("2", "3"):
                raise ValueError("GAME_LTO_OPT must be 2 or 3")
            # GAME_LTO_MLLVM: extra code-generation options for the ThinLTO back end, space separated (each is passed as
            # -mllvm <opt>). Empty by default. (-enable-ipra=false was tried on 11 Nov against the crash of the console-profile
            # builds and changed nothing: the cause was an uninitialised array in JASChannel.cpp, fixed there.)
            lto_mllvm = []
            for opt in os.environ.get("GAME_LTO_MLLVM", "").split():
                lto_mllvm += ["-mllvm", opt]
            run(["ld.lld", "-r", "-o", lto / "galaxy_lto.o", f"--thinlto-jobs={args.jobs}", f"--lto-O{lto_opt}",
                 "-mllvm", "-emulated-tls", *lto_mllvm, f"@{lto / 'objects.txt'}"], env, logs / "lto-link.log")
            (lto / "libgalaxy_vita_sources.a").unlink(missing_ok=True)
            run(["llvm-ar", "rcs", lto / "libgalaxy_vita_sources.a", lto / "galaxy_lto.o"], env, logs / "lto-archive.log")
        elif stage == "runtime":
            database = json.loads((source_build / "compile_commands.json").read_text())
            if not database or any(not Path(row["file"]).is_relative_to(game_root) for row in database):
                raise ValueError("Compile database belongs to a different source checkout")
            runtime_env = dict(env, VITA_PGO_PROFILE=str(pgo_runtime)) if pgo_runtime else env
            run([sys.executable, "-I", HERE / "vita_runtime_gen_vgx.py", ROOT, source_build,
                 runtime, deps, args.label], runtime_env, logs / "runtime-configure.log")
            run(["ninja", "-C", runtime, f"-j{args.jobs}"], env, logs / "runtime.log")
            run([sys.executable, "-I", HERE / "vita_runtime_report_vgx.py", ROOT, runtime], env, logs / "runtime-report.log")
        else:
            require_files(build, (game_archive, ARTIFACTS["runtimeArchive"]), "Link inputs")
            cxx_flags = "" if args.logging else QUIET_FLAGS
            run(["cmake", "-G", "Ninja", "-S", ROOT / "vita/vgx-game", "-B", game,
                 f"-DCMAKE_TOOLCHAIN_FILE={sdk}/share/vita.toolchain.cmake", "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DPORT_SILENT_STDIO={'OFF' if args.logging else 'ON'}",
                 f"-DRUNTIME_ARCHIVE={build / ARTIFACTS['runtimeArchive']}",
                 f"-DGAME_ARCHIVE={build / game_archive}", f"-DVGL_DEPS={deps}", f"-DSMG_MBEDTLS={mbedtls or ''}",
                 f"-DCMAKE_CXX_FLAGS={cxx_flags}"] + mp_defs, env, logs / "link-configure.log")
            run(["cmake", "--build", game, "--target", "eboot.bin", "--parallel", args.jobs], env, logs / "link.log")
            link_check(build, env, logs / "link-check.log")
            print(f"eboot.bin: {digest(build / ARTIFACTS['runtime'])}")
            stagetags = ROOT / "tools/seedrun/stagetags.py"
            if args.seed is not None:
                # (fix31) the packaging step that tags the stage list cannot be missed: the release VPK is made here and checked; a list without the
                # stamp fails the build (the game would run without the first-visit priorities and the old player seed: a regression against C183)
                run(["cmake", "--build", game, "--target", "smg-vgx.vpk", "--parallel", args.jobs], env, logs / "vpk.log")
                vpk = build / "Galaxy-Vita.vpk"
                vpk.unlink(missing_ok=True)
                run([sys.executable, "-I", stagetags, "package", game / "smg-vgx.vpk", args.seed.resolve(), vpk], env, logs / "package.log")
                run([sys.executable, "-I", stagetags, "checkvpk", vpk], env, logs / "package-check.log")
                print(f"Galaxy-Vita.vpk: {digest(vpk)} (compact seed, stage list tagged and stamped: stagetags.py checkvpk passed)")
            else:
                print("No VPK created and no device changed. Release VPK: build.py link --seed <seed VPK or shadercache folder> makes and checks it; by hand,"
                      " `python3 -I tools/seedrun/stagetags.py package <smg-vgx.vpk> <seed> <out.vpk>` (docs/BUILDING.md), then"
                      " `stagetags.py checkvpk <out.vpk>` must pass (an untagged stage list loses the first-visit priorities).")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
