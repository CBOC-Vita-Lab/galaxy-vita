"""Compare real backend effects to a no-diagnostics baseline in fresh processes."""
import json
import os
import pathlib
import subprocess
import sys

binary = sys.argv[1]
quiet = sys.argv[2] == "0"
modes = ("baseline", "drawtime", "draw-at", "passtime", "pass-at", "prof1", "prof2", "prof3",
         "csv", "csv-ram", "constattr", "scene", "validate", "cut", "probe", "simulate", "lifecycle", "functional", "uniform-reuse", "uniform-control")
results = {}
for mode in modes:
    cache = pathlib.Path("/tmp/backend-sim-cache")
    if mode == "simulate":
        cache.mkdir(exist_ok=True)
        for file in cache.iterdir():
            file.unlink()
    try:
        completed = subprocess.run([binary, mode], check=True, capture_output=True, text=True, timeout=30)
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr)
        raise
    results[mode] = json.loads(completed.stdout)
    if mode == "simulate":
        results[mode]["sim_files"] = len(list(cache.iterdir()))

baseline = results["baseline"]
pixels = bytes.fromhex(baseline["pixels"])
center = (16 * 32 + 16) * 4
assert pixels[center] > 50 and pixels[center + 1] > 170 and pixels[center + 2] > 100, "baseline triangle visible"
functional = results["functional"]
assert not baseline["cache_off"] and baseline["dxt1"] and baseline["warm_draw_delta"] == 0
assert functional["cache_off"] and not functional["dxt1"] and functional["warm_draw_delta"] == 1
assert functional["gl_calls"] > baseline["gl_calls"], "cache-off control really issues more GL state calls"
failures = []
for mode, result in results.items():
    def require(good, why):
        if not good:
            failures.append(f"{mode}: {why}")
    if quiet:
        require(result["diagnostic_env_reads"] == 0, "diagnostics0 still reads diagnostic settings")
        require(result["unexpected_env_reads"] == 0, "diagnostics0 reads a setting outside the functional allowlist")
        for key in ("prof_commands", "draw_profiler", "scene", "validation", "csv", "simulated_ms", "sim_hits"):
            require(not result[key], f"diagnostics0 enables {key}")
        if mode not in ("lifecycle", "functional", "uniform-control"):
            for key in ("finishes", "draws", "gl_calls", "readbacks", "pixels"):
                require(result[key] == baseline[key], f"stale diagnostics change baseline {key}")
        if mode == "simulate":
            require(result["sim_files"] == 0, "diagnostics0 creates fake cache files")
    else:
        if mode in ("drawtime", "draw-at", "passtime", "pass-at", "prof1"):
            require(result["finishes"] > baseline["finishes"], "requested GPU timing no longer runs")
        if mode.startswith("prof"):
            require(result["prof_commands"] > 0, "requested CPU profiling no longer runs")
        if mode == "prof3":
            require(result["draw_profiler"], "requested draw profiler no longer runs")
        if mode in ("csv", "scene"):
            require(result[mode], "requested diagnostic no longer runs")
        if mode == "csv-ram":
            require(result["csv"], "paused RAM CSV export no longer runs")
        if mode == "validate":
            require(result["validation"], "requested validation no longer runs")
        if mode == "probe":
            require(result["readbacks"] > baseline["readbacks"], "pixel probe no longer reads back")
        if mode == "cut":
            require(result["draws"] < baseline["draws"] and result["pixels"] != baseline["pixels"], "command-cut control no longer cuts")
        if mode == "simulate":
            require(result["simulated_ms"] == 208 and result["sim_hits"] == 1 and result["sim_files"] > 0,
                    "simulated cache control no longer runs")
        if mode not in ("cut", "lifecycle"):
            require(result["pixels"] == baseline["pixels"], "diagnostic changes foreground pixels")
    print(f"{mode}: finishes={result['finishes']} draws={result['draws']} csv={result['csv']} diagnostic_reads={result['diagnostic_env_reads']}")
assert results["uniform-control"]["pixels"] == results["uniform-reuse"]["pixels"], "uniform reuse preserves exact reference pixels"
assert results["uniform-control"]["gl_calls"] > results["uniform-reuse"]["gl_calls"], "kill switch performs the reference uploads"
if failures:
    raise AssertionError("\n".join(failures))

# The container runs all builds against this first build's exact pixel/call data.
keys = ("pixels", "draws", "gl_calls", "readbacks", "finishes", "warm_draw_delta", "cache_off", "dxt1")
reference = pathlib.Path("/tmp/backend-quiet-reference.json")
controls = {mode: {key: results[mode][key] for key in keys} for mode in ("baseline", "functional", "uniform-control")}
if reference.exists():
    expected = json.loads(reference.read_text())
    for mode, values in controls.items():
        for key, actual in values.items():
            assert actual == expected[mode][key], f"cross-build {mode}: diagnostics/trace flags change {key}"
    print("PASS: exact cross-build pixels and GL counts match the first diagnostics/trace configuration")
else:
    reference.write_text(json.dumps(controls))
    print("First diagnostics/trace configuration recorded for cross-build comparison")
print(f"PASS: 20 real-Mesa scenarios diagnostics={sys.argv[2]} trace={os.environ['QUIET_TRACE']} (not Vita FPS)")
