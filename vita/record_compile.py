#!/usr/bin/env python3
"""Keep per-object diagnostics and provenance even when Ninja keeps going."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    command = sys.argv[1:]
    source = Path(command[command.index("-c") + 1])
    output = Path(command[command.index("-o") + 1]).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    log = Path(str(output) + ".compiler.log")
    before = digest(source)
    started = time.time()
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    after = digest(source)
    record = {
        "source": str(source), "output": str(output), "command": command,
        "source_sha256": before, "source_changed_during_compile": before != after,
        "exit_code": result.returncode, "started": started,
        "seconds": time.time() - started, "log": str(log),
    }
    Path(str(output) + ".result.json").write_text(json.dumps(record, indent=2) + "\n")
    if result.returncode:
        print(f"Compiler failed: {source}; diagnostics: {log}", flush=True)
        print(log.read_text(errors="replace"), end="", flush=True)
    if before != after:
        print(f"Source changed during compile: {source}", file=sys.stderr)
        return 1
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
