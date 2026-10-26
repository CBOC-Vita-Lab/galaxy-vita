#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
python3 -m unittest discover -s tests -p test_vita_boot_dialog.py -v
