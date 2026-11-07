#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
python3 "$root/tests/fallout_capture_test.py" "${1:-$(mktemp -d)}"
