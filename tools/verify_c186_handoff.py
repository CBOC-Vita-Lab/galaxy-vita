"""Replay the incremental source bundle without borrowing objects from the candidate checkout."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BASE = "e44c45d8bed8f089d3dc75eb2b4b3091bbb1c491"
REF = "refs/heads/c186"


def git(tree, *args):
    return subprocess.check_output(["git", "-C", tree, *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_repository", type=Path, help="local Git repo containing integ-3.1")
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--test", action="store_true", help="run the public host suite from the reconstructed tree")
    args = parser.parse_args()
    bundle = args.bundle.resolve(strict=True)
    repo = args.base_repository.resolve(strict=True)
    if git(repo, "rev-parse", f"{BASE}^{{commit}}") != BASE:
        raise ValueError("Missing integration prerequisite")
    refs = git(repo, "bundle", "list-heads", str(bundle)).splitlines()
    if len(refs) != 1 or refs[0].split()[1] != REF:
        raise ValueError(f"Unexpected bundle refs: {refs!r}")
    head = refs[0].split()[0]

    with tempfile.TemporaryDirectory(prefix="c186-replay-") as temp:
        work = Path(temp)
        git(work, "init", "-q")
        # Fetch only ancestors of the advertised integration commit. Do not use
        # local clone, alternates, or a worktree sharing the producer's objects.
        git(work, "fetch", "-q", "--no-tags", repo.as_uri(), BASE)
        if (work/".git/objects/info/alternates").exists() or subprocess.run(
                ["git", "-C", work, "cat-file", "-e", head + "^{commit}"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            raise ValueError("Candidate already accessible before import")
        git(work, "bundle", "verify", str(bundle))
        git(work, "fetch", "-q", "--no-tags", str(bundle), REF)
        git(work, "checkout", "-q", "--detach", "FETCH_HEAD")
        if git(work, "rev-parse", "HEAD") != head or git(work, "merge-base", BASE, head) != BASE:
            raise ValueError("Reconstructed tip does not extend the requested integration")
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        subprocess.run([sys.executable, "-I", "tools/check-files.py"], cwd=work, env=env, check=True)
        for name in ("docs/console-tests-C186.md", "tools/test_c186.sh", "tools/test_c186_far_pose.sh"):
            git(work, "ls-files", "--error-unmatch", name)
        if args.test:
            subprocess.run(["bash", "tools/test_c186.sh"], cwd=work, env=env, check=True)
        print(f"Clean bundle replay: {BASE} -> {head}, tree {git(work, 'rev-parse', 'HEAD^{tree}')}")


if __name__ == "__main__":
    main()
