"""Run a serial C186.1 emulator comparison. The only shared inputs are read-only.

python3 replay.py A B  (new arms only; use a different --out for a replay)
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zipfile
from witness import export

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
VPK = ROOT / "outputs/galaxy-c186.1/Galaxy-Vita-3.0-C186.1.vpk"
FS = ROOT / "galaxy-trailer/v3-20261115/action-recut/.sim/portable/fs"
ROUTE = ROOT / "outputs/galaxy-c186/retail-startup-qa/portable/fs/ux0/data/smg/input.txt"
EMU = ROOT / "codex-strikers/trailer-20261018/simulator/Vita3K-4124.AppImage"
CONFIG = ROOT / "codex-strikers/build/vita3k-linux/game-inputs/config-vk.yml"
IMAGE = "codex-strikers-vita3k-linux:4124"
EMPTY = hashlib.sha256(b"").hexdigest()
BASE = ("PETARI_BOOT=1\nPETARI_GAME_CLOCK=1\nPETARI_SCRIPT_ANCHOR=play\nPETARI_SCRIPT_LOG=1\n"
        "PETARI_SCRIPT_LOOP=16\nPETARI_STAGE=HoneyBeeKingdomGalaxy:1\nPETARI_STAGE_HOLD=20000\n"
        "PETARI_TEST_LIVES=99\nGXV_PICTURE_MODE=60\nGXV_VSOUT=1\nGXV_STDERR_FILE=1\n")


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def tree(path):
    h = hashlib.sha256()
    for f in sorted(path.rglob("*")):
        if f.is_symlink():
            raise ValueError("Symlink in immutable fixture: " + str(f))
        if f.is_file():
            h.update(f.relative_to(path).as_posix().encode() + b"\0" + sha(f).encode() + b"\n")
    return h.hexdigest()


def validate(label, text):
    expected = BASE + ("GXV_HRW=0\n" if label == "B" else "")
    if text != expected:
        raise ValueError("Non-retail or unmatched arm environment")


def negative():
    for label, value in (("A", BASE + "GXV_HRW=0\n"), ("B", BASE), ("A", BASE + "GXV_PACK=1\n"),
                         ("A", BASE.replace("VSOUT=1", "VSOUT=0"))):
        try:
            validate(label, value)
        except ValueError:
            continue
        raise RuntimeError("Negative env control failed")


def display(path):
    if not path.exists():
        return None
    data = path.read_bytes()
    hdr = struct.unpack(">25I", data[:100])
    header, version, _, _, w, h, _, endianness, _, _, _, bpp, stride, _, _, _, _, _, _, colors = hdr[:20]
    if (version, w, h, bpp, endianness) != (7, 1280, 800, 32, 0):
        raise ValueError("Unexpected Xvfb framebuffer format")
    off = header + 12 * colors
    pixels = [data[off + y*stride + x*4:off + y*stride + x*4 + 3]
              for y in range(128, 424, 4) for x in range(220, 740, 4)]
    return {"sampled": len(pixels), "distinct_bgr": len(set(pixels)),
            "lit_fraction": round(sum(max(p) > 28 for p in pixels) / len(pixels), 4),
            "colorful_fraction": round(sum(max(p)-min(p) > 20 for p in pixels) / len(pixels), 4)}


def run(labels, out):
    negative()
    out = out.resolve()
    if sha(VPK) != "5b075db717b9e54397dd2bf5892a189ac9d8e09de258aee900b9bca23c41a554":
        raise ValueError("Unexpected package")
    if sha(ROUTE) != "8f06da92b31ed911ffba0d00cc3796f506b6f0b88daf32f8fd771d8a13835985":
        raise ValueError("Unexpected route")
    if not set(labels) <= {"A", "B", "A2"} or len(labels) != len(set(labels)):
        raise ValueError("Unknown or duplicate label")
    out.mkdir(parents=True, exist_ok=True)
    if any((out / label).exists() for label in labels):
        raise ValueError("Refusing to overwrite an existing arm")
    image = subprocess.check_output(["docker", "image", "inspect", "--format", "{{.Id}}", IMAGE], text=True).strip()
    if not image.startswith("sha256:6978973493ab"):
        raise ValueError("Unrecognized emulator image")
    print("Hashing immutable inputs once per invocation", flush=True)
    common = {"vpk": sha(VPK), "emulator": sha(EMU), "config": sha(CONFIG), "input": sha(ROUTE),
              "game": tree(FS / "ux0/data/smg/game"), "saves": tree(FS / "ux0/data/smg/saves"),
              "firmware": {p: tree(FS / p) for p in ("os0", "sa0", "vs0", "ur0", "ux0/user")},
              "guest_writable_cache_at_launch": EMPTY, "host_writable_cache_at_launch": EMPTY,
              "image": image, "layer_capture": False, "renderer": "Vita3K Linux ARM64 / Mesa llvmpipe"}
    with tempfile.TemporaryDirectory(prefix="galaxy-emulator-ab-") as scratch:
        tmp = Path(scratch).resolve()
        # Freeze the driver for both containers. A bind mount of a live source
        # script can otherwise change under the shell that is executing it.
        driver = tmp / "arm.sh"
        driver.write_bytes((HERE / "arm.sh").read_bytes())
        common["arm_script"] = sha(driver)
        title = tmp / "title"
        title.mkdir()
        with zipfile.ZipFile(VPK) as z:
            if any(not (title / n).resolve().is_relative_to(title) for n in z.namelist()):
                raise ValueError("Invalid package")
            z.extractall(title)
        common["eboot"] = sha(title / "eboot.bin")
        common["title"] = tree(title)
        for label in labels:
            arm = out / label
            arm.mkdir()
            private = tmp / label
            private.mkdir()
            envfile = arm / "env.txt"
            envfile.write_text(BASE + ("GXV_HRW=0\n" if label == "B" else ""))
            validate(label, envfile.read_text())
            (arm / "inputs.json").write_text(json.dumps(dict(common, env=sha(envfile)), indent=2) + "\n")
            args = ["docker", "run", "--rm", "--pull", "never", "--platform", "linux/arm64",
                    "--network", "none", "--cpus", "4", "--memory", "4g", "--shm-size", "256m",
                    "--label", "local.qa=galaxy-c186.1-ab"]
            for src, dst in [(driver, "/arm.sh"), (FS, "/input"), (EMU, "/emu"),
                             (ROUTE, "/route"), (title, "/title"), (envfile, "/env"), (CONFIG, "/config")]:
                args.extend(["-v", f"{src}:{dst}:ro"])
            args.extend(["-v", f"{private}:/private", "--entrypoint", "sh", image, "/arm.sh"])
            print(f"Starting isolated arm {label}", flush=True)
            result = subprocess.run(args)
            evidence = {}
            for name in ("console.log", "game.log"):
                p = private / name
                if p.exists():
                    evidence[name + "_sha256"] = sha(p)
            visual = {p: display(private / (p + ".xwd")) for p in ("play", "return")}
            game = (private / "game.log").read_text(errors="replace") if (private / "game.log").exists() else ""
            patterns = (" fps: ", "script: clock started", "load-split: scene build of HoneyBee",
                        "hrw: pictures (", "settings: picture mode", "3.0-C186.1", "hrw: mode ",
                        "first-run", "xskip: control (first controllable turn)", "input: loaded", "modekeys: Performance")
            filtered = [line for line in game.splitlines() if any(k in line for k in patterns)]
            (arm / "guest-samples.txt").write_text("\n".join(filtered) + "\n")
            reason = (private / "reason").read_text().strip() if (private / "reason").exists() else "launcher-failed"
            report = dict(evidence, reason=reason, container_exit=result.returncode, visual=visual,
                          controls="fresh caches; no host display, network, screenshot or timing layers")
            if (private / "return-delay.txt").is_file():
                report["return_visible_delay_s_outside_timing"] = int((private / "return-delay.txt").read_text())
            export(private, arm)
            (arm / "outcome.json").write_text(json.dumps(report, indent=2) + "\n")
            print(label, reason, visual, flush=True)
            if result.returncode != 0 or not game or reason == "process-exited":
                raise SystemExit("Incomplete emulator run; no comparison")


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("arms", nargs="*", choices=["A", "B", "A2"])
    cli.add_argument("--out", type=Path, default=ROOT / "outputs/galaxy-c186.1/emulator-ab")
    cli.add_argument("--self-test", action="store_true")
    a = cli.parse_args()
    negative()
    if not a.self_test:
        run(a.arms or ["A", "B"], a.out)
