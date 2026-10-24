#!/usr/bin/env bash
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
export VITA_RUNTIME_TEST_TMP=/build/audit-test-tmp
python3 -B -m unittest discover -s /source/vita/portability -p 'test_*.py' 2>&1 | tee /build/tests.log
python3 -B /source/vita/portability/runtime_audit.py "$@"
