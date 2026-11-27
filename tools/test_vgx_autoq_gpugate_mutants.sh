#!/usr/bin/env bash
# s1117b: each rule of the GPU gate must be caught by tests/vgx_autoq_gpugate.cpp when taken out. Copies the controller to a scratch folder per mutant,
# breaks one thing there and expects the test to fail (Config::Legacy() keeps the gate off: with the rules as they are the gate cannot change a Legacy trace).
#   tools/test_vgx_autoq_gpugate_mutants.sh [scratch dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$(mktemp -d)}
cxx=${CXX:-g++}
caught=0; missed=0
mutant() {   # name, file (under platform/vgx/src), old, new
    local name=$1 file=$2 old=$3 new=$4 test=${5:-gpugate} d="$work/$1"
    rm -rf "$d"; mkdir -p "$d/platform/vgx" "$d/platform/include" "$d/tests"
    cp -r "$root/platform/vgx/src" "$d/platform/vgx/"; cp -r "$root/platform/include/port" "$d/platform/include/"; cp "$root/tests/"vgx_autoq_gpugate.cpp "$root/tests/"vgx_autoq_aniso.cpp "$d/tests/"
    if ! python3 -I - "$d/platform/vgx/src/$file" "$old" "$new" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if s.count(old) != 1: sys.exit("pattern found %d times" % s.count(old))
open(p, "w").write(s.replace(old, new))
EOF
    then echo "BROKEN $name (the line to change was not found)"; missed=$((missed + 1)); return; fi
    if ! $cxx -std=c++17 -O1 -w "$d/tests/vgx_autoq_$test.cpp" "$d/platform/vgx/src/vgx_autoq.cpp" -o "$d/t" > "$d/build.log" 2>&1; then echo "BROKEN $name (does not build)"; missed=$((missed + 1)); return; fi
    if "$d/t" > "$d/log" 2>&1; then echo "MISSED $name (see $d/log)"; missed=$((missed + 1)); else echo "CAUGHT $name"; caught=$((caught + 1)); rm -rf "$d"; fi
}
C=vgx_autoq.cpp; H=vgx_autoq.h
mutant fast60-ungated $C 'if (Fast60() && !p23Hold && gateOpen && QgSettled())' 'if (Fast60() && !p23Hold && QgSettled())'
mutant over-ungated $C 'const bool over = gateOpen && (hold30' 'const bool over = (hold30'
mutant bound-ungated $C 'const bool bound = gateOpen && (hold30' 'const bool bound = (hold30'
mutant pendingbelow-ungated $C 'if (pendingBelow_ && mayChange && GpuGateOpen() && f.presentUs' 'if (pendingBelow_ && mayChange && f.presentUs'
mutant busy-85 $H 'double gateBusy = 0.85;' 'double gateBusy = 0.95;'
mutant busy-85-low $H 'double gateBusy = 0.85;' 'double gateBusy = 0.80;'
mutant busy-70 $H 'double gateBusyLate = 0.70;' 'double gateBusyLate = 0.60;'
mutant late-8 $H 'double gateLateFrac = 0.08;' 'double gateLateFrac = 0.20;'
mutant window-90 $H 'int gateWindow = 90;' 'int gateWindow = 30;'
mutant recent-off $H 'return measured >= minPics && (share >= cfg_.gateBusy ||' 'return false && (share >= cfg_.gateBusy ||'
mutant min-pics-open $H 'if (measured < minPics) return true;' 'if (measured < 1) return true;'
mutant unmeasured-counts $H '            if (!p.known) continue;' ''
mutant load-keeps-ring $C '        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0) gate_.Clear();' ''
mutant period-is-interval $C 'std::min<uint64_t>(interval, static_cast<uint64_t>(budgetMs * 1000.0))' 'interval'
mutant cadence-budget $C 'const double budgetMs = f.cadence30 ? std::max(cfg_.budgetMs, 1000.0 / 30.0) : cfg_.budgetMs;' 'const double budgetMs = cfg_.budgetMs;'
mutant switch-ignored $H '        if (!cfg_.gpuGate) return true;' ''
echo "gate mutants: $caught caught, $missed missed"
[ $missed -eq 0 ]
