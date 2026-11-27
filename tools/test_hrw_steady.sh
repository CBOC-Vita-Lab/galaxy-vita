#!/usr/bin/env bash
# warpsteady31: host test of port/hrw_steady.h (the plane ramp, the 30 hold, the edge limit), then the same test against mutants of the header: each mutant must FAIL.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/hrw_steady_test}; mkdir -p "$out"
CXX=${CXX:-g++}
build() {  # $1 = include dir with port/hrw_steady.h, $2 = binary
    nice -n 10 $CXX -std=c++17 -O1 -Wall -I"$1" -I"$root/platform/include" "$root/tests/hrw_steady_test.cpp" -o "$2"
}
build "$root/platform/include" "$out/base" || exit 1
"$out/base" || { echo "BASE FAILED"; exit 1; }
# mutants: name | sed expression (applied to a copy of the header)
mutants=(
 "ramp-no-scale-K|s/o.K\[i\] = static_cast<float>(p.K\[i\] \* w)/o.K[i] = p.K[i]/"
 "ramp-no-scale-kMax|s/o.kMax = static_cast<float>(p.kMax \* w)/o.kMax = p.kMax/"
 "ramp-g-not-divided|s/p.g\[i\] \/ w/p.g[i]/"
 "ramp-fade-not-scaled|s/o.fadeB = static_cast<float>(p.fadeB \* w)/o.fadeB = p.fadeB/"
 "ramp-up-jumps|s/level = std::min(level + 1, steps);/level = steps;/"
 "ramp-decay-jumps|s/level = std::max(level - 1, 0);/level = 0;/"
 "ramp-decay-no-weight-check|s/have \&\& level > 0;/have;/"
 "ramp-reset-keeps-level|s/void Reset() { level = 0;/void Reset() {/"
 "ramp-off-not-off|s/if (steps <= 0) return 1.0;/if (steps <= 0) return 0.5;/"
 "hold-never-active|s/nowMs < until/false/"
 "hold-no-motion-filter|s/holdMs > 0.0 \&\& motionReason/holdMs > 0.0/"
 "hold-off-arms|s/if (holdMs > 0.0 \&\& motionReason)/if (motionReason)/"
 "hold-too-short|s/until = nowMs + holdMs;/until = nowMs + holdMs * 0.5;/"
 "hold-no-cap|s/std::clamp<long>(ms, 0, 5000)/ms/"
 "edge-ignores-new-key|s/edgePx >= 0 ? edgePx : edgeMaxPx/edgeMaxPx/"
 "edge-no-hysteresis|s/lastEdge < limit \* 0.75/lastEdge < limit/"
 "edge-strict|s/lastEdge > limit)/lastEdge >= limit)/"
)
killed=0; total=0
for m in "${mutants[@]}"; do
    name=${m%%|*}; expr=${m#*|}
    d="$out/mut_$name"; mkdir -p "$d/port"
    sed -e "$expr" "$root/platform/include/port/hrw_steady.h" > "$d/port/hrw_steady.h"
    if cmp -s "$d/port/hrw_steady.h" "$root/platform/include/port/hrw_steady.h"; then echo "MUTANT $name: sed did not change the header (test bug)"; exit 1; fi
    ((total++))
    if build "$d" "$d/bin" 2>/dev/null && "$d/bin" >/dev/null 2>&1; then echo "MUTANT SURVIVED: $name"; else ((killed++)); fi
done
echo "mutants killed: $killed of $total"
[ "$killed" -eq "$total" ]
