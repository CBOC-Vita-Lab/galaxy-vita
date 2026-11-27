"""Offline macOS QA only. Clone a disposable Vita3K profile, or install and run one local VPK inside it."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OLD = ROOT / "galaxy-trailer/v3-20261115/action-recut/.sim"
TITLE = "SMGV00001"


def digest(path):
    with path.open("rb") as f:
        h = hashlib.sha256()
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
        return h.hexdigest()


def clone(src,dst):
    libc=ctypes.CDLL("/usr/lib/libSystem.B.dylib",use_errno=True)
    if libc.clonefile(os.fsencode(src),os.fsencode(dst),0):
        raise OSError(ctypes.get_errno(),os.strerror(ctypes.get_errno()),str(src))


def profile_settings(stage, normal=False, census=False, retail=False, vram_kib=None):
    lines = [] if normal else [
        "PETARI_BOOT=1","PETARI_GAME_CLOCK=1","PETARI_SCRIPT_ANCHOR=play","PETARI_SCRIPT_LOG=1",
        "PETARI_SCRIPT_LOOP=16",f"PETARI_STAGE={stage}:1","PETARI_STAGE_HOLD=20000","PETARI_TEST_LIVES=99"]
    lines += ["GXV_PICTURE_MODE=60"]
    if retail:
        lines.append("GXV_VSOUT=1")  # (C186) retail vertex outputs, stated explicitly; both pack policies stay at entry.cpp defaults
    else:
        lines.append("GXV_VSOUT=0")
        if not normal:
            lines += ["GXV_SHADERPACK=1","GXV_USERPACK=0"]
    lines.append("GXV_STDERR_FILE=1")
    if not retail and not normal:
        lines += ["GXV_HRW_WARP_DUMP=4","GXV_HRW_WARP_DUMP_FROM=8",
                  "GXV_HRW_WARP_DUMP_MIN_PX=1","GXV_HRW_WARP_EVERY=5"]
    if census:
        lines.append("GXV_POPIN_CENSUS=1")
    if vram_kib is not None:
        if not 0 <= vram_kib <= 98304:
            raise ValueError("Specify 0..98304 KiB of simulated free VRAM")
        lines.append(f"GXV_AUTOQ_VRAM_KIB={vram_kib}")
    return "\n".join(lines)+"\n"


def retail_env(stage, normal):
    """(C186) The retail first-use profile: only the scripted route (unless normal), 60 Hz picture, retail vertex outputs, the log file."""
    if not re.fullmatch(r"[A-Za-z0-9]+", stage):
        raise ValueError("Invalid internal stage name")
    return profile_settings(stage, normal, retail=True)


def check_retail(data, metadata):
    allowed = {"game", "saves", "env.txt"}
    if not metadata["normal_boot"]:
        allowed.add("input.txt")
    if (metadata.get("profile") != "emulator / retail" or metadata.get("game_cache_imported") is not False
            or {p.name for p in data.iterdir()} != allowed
            or (data/"env.txt").read_bytes() != retail_env(metadata["stage"], metadata["normal_boot"]).encode()):
        raise ValueError("Retail first-use check requires the retail environment, no settings overrides and a fresh title cache")


def prepare(d, warm=False, normal=False, stage="HeavenlyBeachGalaxy", census=False, retail=False, vram_kib=None):
    if d.exists():
        raise ValueError(f"Refusing to overwrite profile: {d}")
    if not re.fullmatch(r"[A-Za-z0-9]+", stage):
        raise ValueError("Invalid internal stage name")
    scenario = Path("game/files/StageData") / stage / (stage + "Scenario.arc")
    if not (OLD/"portable/fs/ux0/data/smg"/scenario).is_file():
        raise ValueError(f"No cooked scenario for {stage}")
    print(f"Free bytes before clone: {shutil.disk_usage(ROOT).free}", flush=True)
    def tree(src, dst):
        shutil.copytree(src, dst, copy_function=clone, symlinks=True)
    d.mkdir(parents=True)
    tree(OLD/"Galaxy3ActionCapture.app", d/"C182Vita3K.app")
    oldfs, fs = OLD/"portable/fs", d/"portable/fs"
    # No application, env/settings or previous logs are imported.
    subtrees=("os0","sa0","vs0","ur0","ux0/data/smg/game","ux0/data/smg/saves")
    for rel in subtrees + (("ux0/data/smg/shadercache",) if warm else ()):
        src = oldfs/rel
        if any(p.is_symlink() for p in src.rglob("*")):
            raise ValueError(f"Source subtree contains a symlink: {src}")
        tree(src,fs/rel)
    user = fs/"ux0/user/00/user.xml"
    user.parent.mkdir(parents=True,exist_ok=True)
    clone(oldfs/"ux0/user/00/user.xml",user)
    cfg = (OLD/"config.yml").read_text()
    for key,value in (("pref-path",json.dumps(str(fs))),("resolution-multiplier","1"),
                      ("check-for-updates-mode","0"),("discord-rich-presence","false"),("psn-signed-in","0")):
        cfg, n = re.subn(rf"^{key}:.*$",f"{key}: {value}",cfg,flags=re.M)
        if n != 1:
            raise ValueError(f"Missing or duplicate {key} in config")
    (d/"config.yml").write_text(cfg)
    (d/"portable/config.yml").write_text(cfg)
    data=fs/"ux0/data/smg"
    if not (data/scenario).is_file():
        raise ValueError("Missing cooked scenario for selected internal stage ID")
    (data/"env.txt").write_text(profile_settings(stage, normal, census, retail, vram_kib))
    if not normal:
        clone(OLD.parent/"action-input.txt",data/"input.txt")
    (d/"isolation.json").write_text(json.dumps({"source_profile":str(OLD),"isolated_fs":str(fs),
        "game_clone":"clonefile no-copy fallback","copied_private_test_saves":True,
        "profile":"emulator / retail" if retail and not warm and not census and vram_kib is None else "emulator / test",
        "stage":stage,"normal_boot":normal,
        "original_settings_imported":False,"retail_graphics":retail,"simulated_vram_kib":vram_kib,"game_cache_imported":warm,
        "free_bytes_after":shutil.disk_usage(d).free},indent=2))
    print(d,flush=True)


def host_cache(d):
    # Host SPIR-V and Metal/Vulkan pipeline caches, not the Vita title's shader cache.
    if not (d/"isolation.json").is_file() or (d/"launch.json").exists(): raise ValueError("Fresh prepared profile required")
    shutil.copytree(OLD/"portable/cache",d/"portable/cache",copy_function=clone)
    for cfg in (d/"config.yml", d/"portable/config.yml"):
        data,n=re.subn(r"^shader-cache: false$", "shader-cache: true",cfg.read_text(),flags=re.M)
        if n!=1: raise ValueError("Expected disabled host shader cache in prepared profile")
        cfg.write_text(data)

def run(d, vpk, seconds, seed=None, video=True):
    metadata=json.loads((d/"isolation.json").read_text())
    fs=d/"portable/fs"
    if metadata["isolated_fs"] != str(fs) or fs.resolve()!=fs or (d/"launch.json").exists():
        raise ValueError("Not a fresh isolated test profile")
    data=fs/"ux0/data/smg"
    retail=metadata.get("profile")=="emulator / retail"
    if retail:
        # (C186) a retail first-use run: the profile's env is exactly the retail one, no settings file, no title cache, no seed overlay
        check_retail(data, metadata)
        if seed:
            raise ValueError("A retail first-use profile takes no preliminary seed")
    app=fs/"ux0/app"/TITLE
    app.mkdir(parents=True)
    with zipfile.ZipFile(vpk) as z:
        for member in z.infolist():
            if not (app/member.filename).resolve().is_relative_to(app):
                raise ValueError("Unsafe VPK member")
        if "eboot.bin" not in z.namelist(): raise ValueError("VPK has no eboot")
        z.extractall(app)
        if digest(app/"eboot.bin")!=hashlib.sha256(z.read("eboot.bin")).hexdigest():
            raise ValueError("Installed executable does not match candidate")
    # (C186) the host's own GXV_ / PETARI_ variables never reach the emulator: the profile's env.txt is the only game setting
    env={k:v for k,v in os.environ.items() if not k.startswith(("GXV_", "PETARI_"))}
    for k in ("HOME","XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_DATA_HOME","TMPDIR"):
        directory=d/k.lower(); directory.mkdir(exist_ok=True);env[k]=str(directory)
    env["SDL_VIDEO_MAC_FULLSCREEN_SPACES"]="0"
    owned=d/"C182Vita3K.app/Contents/MacOS/Vita3K"
    rules='(version 1)(allow default)(deny network*)(deny file-write* (subpath '+json.dumps(str(OLD))+'))'
    argv=["/usr/bin/sandbox-exec","-p",rules,str(owned),"-c",str(d/"config.yml"),"--load-config","--keep-config","-B","Vulkan","-r",TITLE,"--frontend","sdl"]
    report={"argv":argv,"profile":metadata.get("profile","emulator / test"),"env_sha256":digest(data/"env.txt"),"vpk":str(vpk),"vpk_sha256":digest(vpk),"installed_eboot_sha256":digest(app/"eboot.bin")}
    if seed:
        if (app/"shadercache").exists():
            raise ValueError("Refusing to overlay an existing package seed")
        with zipfile.ZipFile(seed) as archive:
            members=[f for f in archive.infolist() if f.filename.startswith("shadercache/")]
            if not members or any(not (app/f.filename).resolve().is_relative_to(app/"shadercache") for f in members):
                raise ValueError("Missing or unsafe seed")
            archive.extractall(app,members)
        report.update(preliminary_seed_source=str(seed),preliminary_seed_source_sha256=digest(seed))
    (d/"launch.json").write_text(json.dumps(report,indent=2))
    with (d/"emulator.log").open("wb") as log:
        emu=subprocess.Popen(argv,env=env,cwd=d,stdout=log,stderr=subprocess.STDOUT)
        recorder=None
        stopped=None
        gameplay=fs/"ux0/data/smg/log.txt"
        normal=b"PETARI_BOOT" not in (fs/"ux0/data/smg/env.txt").read_bytes()
        clip=d/("ordinary-boot.mp4" if normal else "scripted-play.mp4")
        try:
            start=time.monotonic(); print("Owned emulator PID",emu.pid,flush=True)
            while time.monotonic()-start<seconds and emu.poll() is None:
                # An RLIMIT_FSIZE also kills Vita3K's sparse mapped guest RAM. Limit
                # only diagnostic output, from outside the emulator.
                if any(p.exists() and p.stat().st_size>32<<20 for p in (d/"emulator.log",d/"portable/vita3k.log")):
                    stopped="log growth watchdog";emu.kill();break
                logbytes=gameplay.read_bytes() if video and recorder is None and gameplay.is_file() else b""
                if video and recorder is None and (b"script: clock started:" in logbytes or
                                         (normal and b"fps:" in logbytes and time.monotonic()-start>100)):
                    exe=OLD.parent/"record_window_hd"
                    recorder_env=dict(env,GALAXY_FRONTEND="sdl",GALAXY_RECORD_SCALE="1")
                    if exe.is_file():
                        # This recorder selects only the named emulator PID and its Vita3K/SMGV00001 window.
                        with (d/"recording.log").open("wb") as recording:
                            recorder=subprocess.Popen([str(exe),str(emu.pid),str(clip),"14"],
                                cwd=d,env=recorder_env,stdout=recording,stderr=subprocess.STDOUT)
                if recorder is not None and recorder.poll()==0 and clip.is_file():
                    stopped="owned emulator clip complete";break
                time.sleep(.2)
        finally:
            if recorder is not None and recorder.poll() is None:
                try: recorder.wait(timeout=20)
                except subprocess.TimeoutExpired: recorder.terminate(); recorder.wait(timeout=5)
            if emu.poll() is None:
                emu.send_signal(signal.SIGTERM)
                try: emu.wait(timeout=8)
                except subprocess.TimeoutExpired: emu.kill(); emu.wait()
            report.update(returncode=emu.returncode,wall_seconds=round(time.monotonic()-start,1))
            report["stop_reason"]=stopped or "time limit or process exit"
            (d/"outcome.json").write_text(json.dumps(report,indent=2))
    print(report["returncode"],report["wall_seconds"],d/"emulator.log")


if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("action",choices=["prepare","host-cache","run"]);p.add_argument("profile",type=Path)
    p.add_argument("--vpk",type=Path);p.add_argument("--preliminary-seed",type=Path);p.add_argument("--warm-test-cache", action="store_true")
    p.add_argument("--stage",default="HeavenlyBeachGalaxy")
    p.add_argument("--retail-graphics",action="store_true",help="Do not force shader features, packs or frame dumping")
    p.add_argument("--simulate-free-vram-kib",type=int,help="Emulator QA only: exercise memory gates without changing the shipping default")
    p.add_argument("--popin-census",action="store_true")
    p.add_argument("--no-video",action="store_true")
    p.add_argument("--normal",action="store_true");p.add_argument("--seconds",type=int,default=145);a=p.parse_args()
    profile=a.profile.resolve()
    if not any(profile.is_relative_to(ROOT/"outputs"/f"galaxy-{version}") for version in ("c182","c183","c184","c185","c186")):
        p.error("Profile must be inside the C182..C186 smoke outputs")
    if a.normal and (a.popin_census or a.stage != "HeavenlyBeachGalaxy"):
        p.error("Normal boot does not use stage or census overrides")
    if a.simulate_free_vram_kib is not None and (a.action != "prepare" or not 0 <= a.simulate_free_vram_kib <= 98304):
        p.error("A 0..98304 KiB simulated budget is only allowed when preparing a new profile")
    if a.action=="prepare": prepare(profile,a.warm_test_cache,a.normal,a.stage,a.popin_census,a.retail_graphics,a.simulate_free_vram_kib)
    elif a.action=="host-cache": host_cache(profile)
    elif a.vpk and 5<=a.seconds<=300: run(profile,a.vpk.resolve(),a.seconds,a.preliminary_seed,not a.no_video)
    else: p.error("Specify a local candidate --vpk and 5..300 seconds")
