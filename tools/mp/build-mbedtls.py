#!/usr/bin/env python3
"""Fetch the pinned Mbed TLS source and build the TLS library of the net/ HTTPS client into a private prefix.

    python3 tools/mp/build-mbedtls.py [--target vita|host32|host64|both|all] [--work DIR] [--fetch-only] [--rebuild]

Galaxy 3.0 multiplayer: vendored from Strikers Charged 1.3 tools/build-mbedtls.py (+ the two checkout helpers of its
tools/build-deps.py, inlined below). Pins and exact cleanup backport: net/deps/lock.json, "network_dependencies".
Recipe: net/deps/Makefile.mbedtls (client-only profile net/deps/mbedtls/np_user_config.h). Output, under DIR (default
build/dependencies; Galaxy's usual root is --work /tls so the Vita prefix is /tls/mbedtls/vita):
    sources/Mbed-TLS_mbedtls/            the pinned checkout (never reset or modified)
    overlays/Mbed-TLS_mbedtls/<hash>/    verified one-file TLS 1.3 cleanup overlay
    mbedtls/<target>/include, lib        the private prefix; pass it to the Vita build as
                                         tools/vgx_build/build.py --mbedtls <that path> (or SMG_MBEDTLS=<that path>)
    mbedtls/<target>/build-inputs.json   immutable prefix build identity (different inputs require a NEW work root)
    mbedtls/<target>/build-receipt.json  patch, source, tools, flags, .d/object/input and library hashes
Nothing is installed into VitaSDK. host32 / host64 are the x86 builds host tests may link.

SPDX-License-Identifier: MIT
Copyright (c) 2026 CBOC-Vita contributors. See licenses/CBOC-tools-MIT.txt."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "net/deps/lock.json"


def git(directory, *args):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    return subprocess.check_output(["git", "-c", "core.hooksPath=" + os.devnull, *args],
                                   cwd=directory, env=env, text=True).strip()


def load_lock(path, key="dependencies"):
    rows = json.loads(path.read_text())[key]
    seen = set()
    if not rows:
        raise ValueError("Dependency lock is empty")
    for row in rows:
        name, url, commit = row["name"], row["url"], row["commit"]
        if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", name) or name.lower() in seen
                or not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\.git", url)
                or not re.fullmatch(r"[0-9a-f]{40}", commit)):
            raise ValueError("Invalid or unpinned dependency lock entry")
        seen.add(name.lower())
    return rows


def ensure_checkout(destination, url, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("A full pinned revision is required")
    if destination.is_symlink():
        raise ValueError("Dependency checkout must not be a symlink")
    if destination.exists():
        if not (destination / ".git").is_dir() or (destination / ".git").is_symlink():
            raise ValueError("Existing dependency directory is not a managed Git checkout")
        if git(destination, "remote", "get-url", "origin") != url:
            raise ValueError("Dependency origin differs; existing files were left alone")
        if git(destination, "rev-parse", "HEAD") != commit:
            raise ValueError("Dependency revision differs; existing files were left alone")
        if git(destination, "status", "--porcelain", "--untracked-files=all"):
            raise ValueError("Dependency has local changes; existing files were left alone")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Only promote a verified checkout; failed downloads remain safe to retry.
    with tempfile.TemporaryDirectory(prefix="." + destination.name + "-fetch-", dir=destination.parent) as temp:
        checkout = Path(temp) / "checkout"
        checkout.mkdir()
        git(checkout, "init", "--quiet")
        git(checkout, "remote", "add", "origin", url)
        git(checkout, "fetch", "--quiet", "--depth=1", "origin", commit)
        git(checkout, "checkout", "--quiet", "--detach", commit)
        if git(checkout, "rev-parse", "HEAD") != commit:
            raise ValueError("Fetched dependency revision did not match")
        if destination.exists() or destination.is_symlink():
            raise ValueError("Dependency destination appeared during download; existing files were left alone")
        checkout.rename(destination)


class _Deps:
    load_lock = staticmethod(load_lock)
    ensure_checkout = staticmethod(ensure_checkout)


def load_build_deps():
    return _Deps


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reject_symlinks(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise ValueError("Build inputs and outputs must not contain symlinks")


def checked_patch(row):
    patches = row.get("patches", [])
    if patches == []:
        return None  # 3.6.8 and later carry the TLS 1.3 transform cleanup upstream: build the pinned source unmodified
    fields = {"path", "sha256", "upstream_commit", "source", "before_sha256", "after_sha256"}
    if len(patches) != 1 or set(patches[0]) != fields:
        raise ValueError("Exactly one complete TLS 1.3 cleanup patch descriptor is required")
    patch = patches[0]
    relative = Path(patch["path"])
    if (relative.is_absolute() or ".." in relative.parts or
            relative.parts[:4] != ("net", "deps", "mbedtls", "patches") or
            patch["source"] != "library/ssl_tls13_keys.c" or
            not re.fullmatch(r"[0-9a-f]{40}", patch["upstream_commit"]) or
            any(not re.fullmatch(r"[0-9a-f]{64}", patch[key])
                for key in ("sha256", "before_sha256", "after_sha256"))):
        raise ValueError("Invalid TLS 1.3 cleanup patch descriptor")
    return patch


def verify_overlay(directory, patch):
    expected = {"library", patch["source"]}
    entries = set()
    for entry in directory.rglob("*"):
        reject_symlinks(entry)
        entries.add(entry.relative_to(directory).as_posix())
    if entries != expected or not (directory / patch["source"]).is_file():
        raise ValueError("TLS overlay contents must contain only the expected source file")
    if sha256(directory / patch["source"]) != patch["after_sha256"]:
        raise ValueError("TLS overlay source hash differs from the lock")


def prepare_overlay(source, destination, row):
    patch = checked_patch(row)
    if patch is None:
        return None, []
    original = source / patch["source"]
    patch_file = ROOT / patch["path"]
    for path in (source, original, patch_file, destination):
        reject_symlinks(path)
    if sha256(patch_file) != patch["sha256"]:
        raise ValueError("TLS patch file hash differs from the lock")
    if sha256(original) != patch["before_sha256"]:
        raise ValueError("Pinned TLS source input hash differs from the lock")
    if destination.exists():
        verify_overlay(destination, patch)
        return destination, [dict(patch)]
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".tls13-overlay-", dir=destination.parent) as temp:
        prepared = Path(temp) / "overlay"
        output = prepared / patch["source"]
        output.parent.mkdir(parents=True)
        shutil.copyfile(original, output)
        # Do not discover an enclosing game checkout when --work is inside it.
        env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                   GIT_CEILING_DIRECTORIES=str(prepared.parent))
        command = ["git", "-c", "core.hooksPath=" + os.devnull, "apply"]
        stats = subprocess.check_output(command + ["--numstat", "-z", str(patch_file)],
                                        cwd=prepared, env=env, stderr=subprocess.PIPE).split(b"\0")
        files = [entry.split(b"\t", 2)[-1].decode("utf-8") for entry in stats if entry]
        if files != [patch["source"]]:
            raise ValueError("TLS patch must change only the expected source file")
        for options in (["--check"], []):
            subprocess.run(command + options + ["--whitespace=nowarn", str(patch_file)],
                           cwd=prepared, env=env, check=True, capture_output=True)
        verify_overlay(prepared, patch)
        if destination.exists() or destination.is_symlink():
            raise ValueError("TLS overlay destination appeared; existing files were left alone")
        prepared.rename(destination)
    return destination, [dict(patch)]


def write_json_atomic(path, value):
    data = json.dumps(value, indent=2, sort_keys=True) + "\n"
    reject_symlinks(path)
    if path.exists() and path.read_text() == data:
        return
    with tempfile.NamedTemporaryFile(mode="w", prefix="." + path.name + "-", dir=path.parent,
                                     delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare_prefix(prefix, identity):
    reject_symlinks(prefix)
    marker = prefix / "build-inputs.json"
    receipt = prefix / "build-receipt.json"
    if prefix.exists() and any(prefix.iterdir()):
        reject_symlinks(marker)
        reject_symlinks(receipt)
        if not marker.is_file() or json.loads(marker.read_text()) != identity:
            raise ValueError("Existing TLS prefix provenance differs; choose a new --work directory")
        if receipt.exists():
            recorded = json.loads(receipt.read_text())
            if any(recorded.get(key) != value for key, value in identity.items()):
                raise ValueError("Existing TLS prefix receipt provenance differs; choose a new --work directory")
            verify_recorded_prefix(prefix, recorded)
        for entry in prefix.rglob("*"):
            reject_symlinks(entry)
    prefix.mkdir(parents=True, exist_ok=True)
    write_json_atomic(marker, identity)
    return receipt.is_file()


def verify_recorded_prefix(prefix, receipt):
    def verify(relative, expected):
        path = Path(relative)
        if (path.is_absolute() or ".." in path.parts or not isinstance(expected, str) or
                not re.fullmatch(r"[0-9a-f]{64}", expected)):
            raise ValueError("Invalid existing TLS prefix receipt provenance")
        selected = prefix / path
        reject_symlinks(selected)
        if not selected.is_file() or sha256(selected) != expected:
            raise ValueError("Existing TLS prefix artifact/input hash changed; choose a new --work directory")

    for field, directory, extension in (("libraries", "lib", ".a"), ("objects", "obj", ".o"),
                                        ("depfiles", "obj", ".d")):
        recorded = receipt.get(field)
        if (not isinstance(recorded, dict) or not recorded or
                set(recorded) != {p.name for p in (prefix / directory).glob("*" + extension)}):
            raise ValueError("Existing TLS prefix artifact inventory provenance differs")
        if field == "libraries" and set(recorded) != {"libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"}:
            raise ValueError("Existing TLS prefix library provenance differs")
        for name, expected in recorded.items():
            if Path(name).name != name:
                raise ValueError("Invalid existing TLS prefix artifact path")
            verify(directory + "/" + name, expected)
    inputs = receipt.get("compile_inputs")
    if not isinstance(inputs, dict) or not inputs:
        raise ValueError("Existing TLS prefix compile input provenance is missing")
    for name, expected in inputs.items():
        label, separator, relative = name.partition("/")
        if not separator or label not in ("source", "overlay", "prefix"):
            raise ValueError("Invalid existing TLS compile input provenance")
        if label == "prefix":
            verify(relative, expected)


def collect_compile_inputs(source, prefix, overlay):
    objdir = prefix / "obj"
    expected = {path.stem for path in (source / "library").glob("*.c")}
    depfiles = {path.stem: path for path in objdir.glob("*.d")}
    objects = {path.stem: path for path in objdir.glob("*.o")}
    if set(depfiles) != expected or set(objects) != expected:
        raise ValueError("Missing or unexpected TLS object/depfile inputs")
    inputs = {}
    for stem, depfile in sorted(depfiles.items()):
        reject_symlinks(depfile)
        reject_symlinks(objects[stem])
        target, separator, prerequisites = depfile.read_text().replace("\\\n", " ").partition(":")
        if not separator or Path(target.strip()) != objects[stem]:
            raise ValueError("Unexpected TLS depfile target")
        dependencies = shlex.split(prerequisites)
        expected_source = overlay / "library/ssl_tls13_keys.c" if stem == "ssl_tls13_keys" and overlay else source / ("library/" + stem + ".c")
        if str(expected_source) not in dependencies:
            raise ValueError("TLS object does not name its expected source input")
        for name in dependencies:
            path = Path(name)
            reject_symlinks(path)
            for label, base in (("source", source), ("prefix", prefix)) + ((("overlay", overlay),) if overlay else ()):
                try:
                    relative = path.relative_to(base)
                except ValueError:
                    continue
                if ".." in relative.parts or not path.is_file():
                    raise ValueError("Invalid TLS compile input")
                inputs[label + "/" + relative.as_posix()] = sha256(path)
                break
            else:
                raise ValueError("TLS compile input is outside verified roots")
    return {"compile_inputs": dict(sorted(inputs.items())),
            "depfiles": {path.name: sha256(path) for _, path in sorted(depfiles.items())},
            "objects": {path.name: sha256(path) for _, path in sorted(objects.items())}}


def tool_identity(command, env):
    executable = shutil.which(command, path=env.get("PATH"))
    if executable is None:
        raise ValueError("TLS build tool not found: " + command)
    version = subprocess.check_output([command, "--version"], env=env, text=True).splitlines()[0]
    return {"command": command, "version": version,
            "executable_sha256": sha256(Path(executable).resolve())}


def build_identity(source, prefix, overlay, row, target, sdk, proofs):
    env = dict(os.environ, VITASDK=str(sdk), LC_ALL="C")
    for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
        env.pop(key, None)
    command = ["make", "-f", str(ROOT / row["recipe"]), "-j2", "SRC=" + str(source),
               "OUT=" + str(prefix), "TARGET=" + target,
               "TLS13_KEYS_SRC=" + str((overlay or source) / "library/ssl_tls13_keys.c")]
    config = subprocess.check_output(command + ["--silent", "print-build-config"], cwd=ROOT, env=env)
    parts = config.decode("utf-8").split("\0")
    if len(parts) != 4 or parts[-1] or not all(parts[:3]):
        raise ValueError("Unexpected TLS recipe build configuration")
    cc, ar, flags, _ = parts
    for original, normalized in (((overlay, "@OVERLAY@"),) if overlay else ()) + ((source, "@SOURCE@"),
                                 (prefix, "@PREFIX@"), (ROOT, "@PROJECT@")):
        flags = flags.replace(str(original), normalized)
    identity = {"schema_version": 2, "dependency": row, "target": target,
                "profile_sha256": sha256(ROOT / row["profile"]),
                "recipe_sha256": sha256(ROOT / row["recipe"]),
                "builder_sha256": sha256(Path(__file__)),
                                "patches": proofs,
                "overlays": {p["source"]: {"source_sha256": p["before_sha256"],
                                          "overlay_sha256": p["after_sha256"]} for p in proofs},
                "compiler": tool_identity(cc, env), "archiver": tool_identity(ar, env),
                "flags": shlex.split(flags)}
    return identity, command, env


def build_target(source, prefix, overlay, row, target, sdk, proofs, rebuild):
    identity, command, env = build_identity(source, prefix, overlay, row, target, sdk, proofs)
    complete = prepare_prefix(prefix, identity)
    if rebuild or not complete:
        command.insert(1, "-B")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    load_build_deps().ensure_checkout(source, row["url"], row["commit"])
    prepare_overlay(source, overlay, row)
    compiled = collect_compile_inputs(source, prefix, overlay)
    libraries = {}
    for name in ("libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"):
        library = prefix / "lib" / name
        reject_symlinks(library)
        libraries[name] = sha256(library)
    receipt = {**identity, **compiled, "libraries": libraries}
    write_json_atomic(prefix / "build-receipt.json", receipt)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", choices=["vita", "host32", "host64", "both", "all"], default="vita")
    parser.add_argument("--work", type=Path, default=ROOT / "build/dependencies")
    parser.add_argument("--fetch-only", action="store_true", help="Prepare the source without compiling")
    parser.add_argument("--rebuild", action="store_true", help="Recompile matching prefix inputs; changed provenance requires a new --work")
    args = parser.parse_args()
    deps = load_build_deps()
    try:
        rows = deps.load_lock(LOCK, "network_dependencies")
        row = next(r for r in rows if r["name"] == "Mbed-TLS_mbedtls")
        reject_symlinks(args.work.absolute())
        work = args.work.resolve()
        for folder in (work, work / "sources", work / "mbedtls"):
            if folder.is_symlink():
                raise ValueError("Build folders must not be symlinks")
        source = work / "sources" / row["name"]
        print("Checking " + row["name"], flush=True)
        deps.ensure_checkout(source, row["url"], row["commit"])
        if args.fetch_only:
            print("Pinned source is ready in " + str(source))
            return 0
        if platform.system() != "Linux" or any(c.isspace() for c in str(ROOT) + str(work)):
            raise ValueError("Build on Linux in paths without spaces")
        targets = {"both": ["vita", "host32"], "all": ["vita", "host32", "host64"]}.get(args.target, [args.target])
        sdk = Path(os.environ.get("VITASDK", "/opt/vitasdk"))
        if "vita" in targets and not (sdk / "bin/arm-vita-eabi-gcc").is_file():
            raise ValueError("Set VITASDK to your Linux VitaSDK directory")
        patch = checked_patch(row)
        overlay, proofs = prepare_overlay(source, work / "overlays" / row["name"] / patch["sha256"] if patch else None, row)
        for target in targets:
            prefix = work / "mbedtls" / target
            build_target(source, prefix, overlay, row, target, sdk, proofs, args.rebuild)
            print("READY: " + str(prefix) + "  (build.py --mbedtls " + str(prefix) + "  or  SMG_MBEDTLS=" + str(prefix) + ")")
        return 0
    except (OSError, ValueError, KeyError, StopIteration, subprocess.CalledProcessError) as error:
        print("STOP: " + str(error), file=sys.stderr)
        print("No existing dependency checkout was reset or removed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
