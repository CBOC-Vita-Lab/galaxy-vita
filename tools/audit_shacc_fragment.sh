#!/usr/bin/env bash
# One isolated diagnostic compile, never a batch or emulator bootstrap.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCAL="$ROOT/local/shacc-tools"
PY="$LOCAL/host-venv/bin/python"

usage() {
    printf '%s\n' \
      'Usage: bash tools/audit_shacc_fragment.sh SOURCE [OPTIMIZATION=1] [LABEL]' \
      'One fragment compile: main, useFx=1, all fast flags off, 2B instructions,' \
      '110 seconds wall, nice +10. Local ignored JSON/PC profile/log outputs.' \
      'Requires the prepared owner-local module and isolated profiling helpers.'
}

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then usage; exit 0; fi
if (( $# < 1 || $# > 3 )); then usage >&2; exit 2; fi
SOURCE="$1"
OPTIMIZATION="${2:-1}"
LABEL="${3:-fragment-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
if [[ ! "$OPTIMIZATION" =~ ^[0-3]$ || ! "$LABEL" =~ ^[A-Za-z0-9][A-Za-z0-9_-]*$ ]]; then
    printf '%s\n' 'Invalid optimization or label.' >&2
    exit 2
fi
cd "$ROOT"
if [[ ! -f "$SOURCE" || ! -x "$PY" || ! -f "$LOCAL/instruction_profile.py" ||
      ! -f "$LOCAL/private/libshacccg.elf" ]]; then
    printf '%s\n' 'Missing source or prepared isolated toolchain; no provisioning attempted.' >&2
    exit 2
fi
mkdir -p "$LOCAL/logs" "$LOCAL/home" "$LOCAL/tmp"
OUT="$LOCAL/logs/$LABEL.gxp"
if [[ -e "$OUT" || -e "${OUT%.gxp}.json" || -e "${OUT%.gxp}.profile.jsonl" ||
      -e "$LOCAL/logs/$LABEL.log" ]]; then
    printf '%s\n' 'Label already has evidence; choose a fresh label.' >&2
    exit 2
fi
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
exec "$PY" "$LOCAL/bounded.py" --seconds 110 --log "$LOCAL/logs/$LABEL.log" -- \
    "$PY" "$LOCAL/compile_shader.py" "$SOURCE" --stage fragment --entry main \
    --optimization "$OPTIMIZATION" --instruction-limit 2000000000 --profile --out "$OUT"
