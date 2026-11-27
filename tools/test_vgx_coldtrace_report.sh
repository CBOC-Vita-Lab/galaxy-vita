#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -DVGX_COLDTRACE=1 -pthread \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_coldtrace_report.cpp -l:libasan.so.8 -l:libubsan.so.1 -o /tmp/coldtrace-report
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    timeout 30s /tmp/coldtrace-report
    timeout 30s /tmp/coldtrace-report dump > /tmp/coldtrace-report.json
    python3 tests/vgx_coldtrace_report.py /tmp/coldtrace-report.json
    python3 tools/analyze_vgx_coldtrace.py /tmp/coldtrace-report.json --json > /tmp/coldtrace-analysis.json
    python3 -c '"'"'import json; d=json.load(open("/tmp/coldtrace-analysis.json")); i=d["items"][0]; assert i["total_us"]==208000 and i["observed_worker_overlap_us"]==60000 and i["dominant_phase"]=="shark" and i["phase_partition_valid"] and not i["attribution_complete"]'"'"'
    timeout 30s /tmp/coldtrace-report full > /tmp/coldtrace-full.json
    python3 tests/vgx_coldtrace_report.py /tmp/coldtrace-full.json full
    python3 tools/analyze_vgx_coldtrace.py /tmp/coldtrace-full.json --json > /tmp/coldtrace-full-analysis.json
    timeout 30s /tmp/coldtrace-report upload > /tmp/coldtrace-upload.json
    python3 -c '"'"'import json; d=json.load(open("/tmp/coldtrace-upload.json")); assert len(d["executions"])==16 and all(s["frame"]!=404 for s in d["executions"]); s=d["max_upload_execution"]; assert s["frame"]==404 and s["roots"]==4 and s["origin_us"][5]==1000 and s["max_item"]["total_us"]==250'"'"'
    python3 tools/analyze_vgx_coldtrace.py /tmp/coldtrace-upload.json --json > /tmp/coldtrace-upload-analysis.json
  '
