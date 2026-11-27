"""Validate the host-only trace patch without changing the donor or launching it."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
BASE = "92bda548f3aa0691102471a72725587e00676383"
PATCH = HERE / "vita3k-92bda548-trace.patch"
TUS = ["vita3k/modules/SceGxm/SceGxm.cpp", "vita3k/renderer/src/vulkan/screen_renderer.cpp"]
INCLUDE = Path("vita3k/util/include")
HEADER = str(INCLUDE / "util/long_present_trace.h")


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream", type=Path, nargs="?", help="also verify against this Vita3K repository containing the exact base")
    parser.add_argument("--compile-db", type=Path,
                        help="optional: read this existing native build compile_commands.json to validate the real TUs")
    a = parser.parse_args()
    repo = a.upstream.resolve() if a.upstream else None
    if a.compile_db and repo is None:
        parser.error("--compile-db requires the upstream repository")
    with TemporaryDirectory() as tmp:
        stage = Path(tmp)
        paths = run("git", "apply", "--numstat", str(PATCH), cwd=stage, capture_output=True,
                    text=True).stdout.splitlines()
        if sorted(line.split("\t")[-1] for line in paths) != sorted(TUS + [HEADER]):
            raise SystemExit("Unexpected patch paths")
        if repo is not None:
            for path in TUS:
                dst = stage / path
                dst.parent.mkdir(parents=True)
                dst.write_bytes(run("git", "-C", repo, "show", f"{BASE}:{path}", capture_output=True).stdout)
        include = [] if repo is not None else ["--include=" + HEADER]
        run("git", "apply", "--check", "--whitespace=error", *include, str(PATCH), cwd=stage)
        run("git", "apply", *include, str(PATCH), cwd=stage)
        model = stage / "model"
        run(os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
            HERE / "tests/include", "-I", stage / INCLUDE, HERE / "tests/model.cpp", "-o", model)
        for flag in ("0", "1", "invalid"):
            run(model, "on" if flag == "1" else "off", env={**os.environ, "VITA3K_LONG_PRESENT_TRACE": flag})
        print(("Exact-base patch" if repo is not None else "Distributed trace header") + " and isolated fake-clock tests: PASS")
        if not a.compile_db:
            return
        db = json.loads(a.compile_db.read_text())
        for path in TUS:
            matches = [row for row in db if Path(row["file"]).resolve() == repo / path]
            if len(matches) != 1:
                raise SystemExit(f"Missing unique native compile entry for {path}")
            row = matches[0]
            cmd = row.get("arguments") or shlex.split(row["command"])
            # Retain the real configuration, generated headers and dependencies,
            # but forbid the production object and dependency output paths.
            keep = []
            it = iter(cmd)
            for arg in it:
                if arg in ("-o", "-MF", "-MT", "-MQ"):
                    next(it)
                elif arg not in ("-c", "-MD", "-MMD", "-MP", row["file"]):
                    keep.append(arg)
            run(keep[0], "-I", stage / INCLUDE, "-iquote", (repo / path).parent, *keep[1:],
                "-fsyntax-only", stage / path, cwd=row["directory"])
            print(f"Native translation unit syntax: PASS {path}")


if __name__ == "__main__":
    main()
