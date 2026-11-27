"""Configure the real ARMHF target, inspect its argv and compile real perf.cpp."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

BRIDGES = {
    "port_workload_view_entry_begin", "port_workload_view_entry_end",
    "port_workload_anim_begin", "port_workload_anim_end",
    "port_workload_anim_category_begin", "port_workload_anim_category_end",
    "port_workload_anim_family_begin", "port_workload_anim_family_end",
    "port_workload_anim_material_checkpoint", "port_workload_anim_material_diff_context",
    "port_workload_anim_material_diff_counts",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "cmake", "-S", str(root / "armhf"), "-B", str(out / "configure"), "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={root / 'armhf/toolchain.cmake'}", "-DCMAKE_BUILD_TYPE=",
        "-DARMHF_VGX_BACKEND=gl-stub", "-DARMHF_VITA_RUNTIME_FLAGS=ON",
    ], check=True)
    rows = json.loads((out / "configure/compile_commands.json").read_text())
    perf = None
    platform_count = 0
    for row in rows:
        argv = row.get("arguments") or shlex.split(row["command"])
        platform = any("CMakeFiles/platform.dir/" in item for item in argv)
        definitions = [item for item in argv if item.startswith("-DPORT_WORKLOAD_GAME_BRIDGES")]
        if platform:
            platform_count += 1
            assert definitions == ["-DPORT_WORKLOAD_GAME_BRIDGES=1"], (row["file"], definitions)
            assert not any(item in ("-D__vita__", "-D__vita__=1") for item in argv), row["file"]
        else:
            assert not definitions, (row["file"], definitions)
        if row["file"] == str(root / "platform/src/port/perf.cpp"):
            perf = (row, argv)
    assert platform_count > 0 and perf, "actual target must contain the platform and perf unit"
    row, argv = perf
    output_index = argv.index("-o") + 1
    obj = out / "production-perf.o"
    argv[output_index] = str(obj)
    subprocess.run(argv, cwd=row["directory"], check=True)
    result = subprocess.run(["arm-linux-gnueabihf-nm", "--defined-only", str(obj)],
                            check=True, capture_output=True, text=True)
    (out / "production-perf-symbols.txt").write_text(result.stdout)
    types = {parts[-1]: parts[-2] for line in result.stdout.splitlines() if len(parts := line.split()) >= 3}
    defined = set(types)
    assert {name for name in defined if name.startswith("port_workload_")} == BRIDGES, sorted(BRIDGES - defined)
    assert all(types[name] == "T" for name in BRIDGES), "bridges must be strong production definitions"
    assert "port_thread_cpu_us" in defined, "Linux CPU clock must remain exported"
    assert "port_native_phase_cpu_checkpoint" not in defined, "ARMHF phase-CPU gate must remain closed"
    print(f"PASS: actual CMake contract on {platform_count} platform units; eleven real ARMHF bridge symbols and Linux CPU clock")


if __name__ == "__main__":
    main()
