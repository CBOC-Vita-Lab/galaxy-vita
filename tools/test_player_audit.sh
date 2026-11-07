#!/usr/bin/env bash
# Source-level guards for the player-build audit (tests/test_player_audit.py). VGX_ELF=<galaxy_native> also checks the linked symbols.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
python3 -I -m unittest discover -s tests -p test_player_audit.py -v
