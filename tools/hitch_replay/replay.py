"""Run one disposable, offline macOS C186.1 hitch experiment; never mount a Vita."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import time
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
ROOT = REPO.parent
sys.path.insert(0, str(REPO / "tools"))
import c182_vita3k_smoke as smoke
from c186_hitch_cache import make_host_overlay
from analyze_replay import EMU_RELEASE, FINISHED, MOVING_FINISHED

CANDIDATE = ROOT / "outputs/galaxy-c186.1/Galaxy-Vita-3.0-C186.1.vpk"
VPK_SHA = "5b075db717b9e54397dd2bf5892a189ac9d8e09de258aee900b9bca23c41a554"
EMU_SHA = "3aa35f2a0de1377ce5ca706831f169dade623eb18609deb60db6ec19373104ea"
CHECKPOINT = "VITA3K_ASYNC_PIPELINE_CHECKPOINT"
MOVING_INPUT_SHA = "8f06da92b31ed911ffba0d00cc3796f506b6f0b88daf32f8fd771d8a13835985"
MOVING_ENV_SHA = "2f7ed1ed91b62c060a0c7954c3f0a6ec63b83cec9a2a8ae0b106cb29cc8176ff"
DRIVER_CACHE = Path("portable/cache/shaders/SMGV00001/eboot.bin/pipeline-cache-vk15.dat")
ANCHOR = "script: clock started: t = 0 at the first play turn in HoneyBeeKingdomGalaxy scenario 1"
ENV = """PETARI_BOOT=1
PETARI_GAME_CLOCK=1
PETARI_SCRIPT_ANCHOR=play
PETARI_SCRIPT_LOG=1
PETARI_STAGE=HoneyBeeKingdomGalaxy:1,HoneyBeeKingdomGalaxy:1
PETARI_STAGE_HOLD=2400
GXV_PICTURE_MODE=60
GXV_VSOUT=1
GXV_STDERR_FILE=1
GXV_FIRSTUSE_TURN=1
GXV_HITCH25_TURN=1
"""


def sha_tree(p):
    h = hashlib.sha256()
    for f in sorted(p.rglob("*")):
        if f.is_symlink():
            raise ValueError("linked files in fixture")
        if f.is_file():
            h.update(f.relative_to(p).as_posix().encode() + b"\0" + smoke.digest(f).encode() + b"\n")
    return h.hexdigest()


def prepare(args):
    d = args.output / "private" / args.label
    vpk=args.vpk or CANDIDATE
    if d.exists() or shutil.disk_usage(args.output).free < 5 << 30 or smoke.digest(vpk) != (args.vpk_sha256 or VPK_SHA):
        raise ValueError("a fresh destination, 5 GiB disk reserve and a matching package SHA-256 are required")
    if args.fixture:
        smoke.OLD=args.fixture.resolve(strict=True)
    if args.output == smoke.OLD or smoke.OLD in args.output.parents or d in smoke.OLD.parents:
        raise ValueError("output may not overlap the source fixture")
    if smoke.digest(smoke.OLD/"Galaxy3ActionCapture.app/Contents/MacOS/Vita3K") != EMU_SHA:
        raise ValueError("template emulator is not the tested Mac build (before assigning its isolated bundle ID)")
    source_sha = args.local_emulator_sha256 or EMU_SHA
    if args.local_emulator and (not args.local_emulator.is_file() or smoke.digest(args.local_emulator)!=source_sha):
        raise ValueError("locally built QA emulator does not match the supplied SHA-256")
    smoke.prepare(d, False, "HoneyBeeKingdomGalaxy")
    smoke.host_cache(d)
    seed_digest = smoke.digest(d/DRIVER_CACHE)
    overlay=None
    if args.warm_from:
        previous = args.output/"private"/args.warm_from
        prior = json.loads((previous/"outcome.json").read_text())
        if prior.get("stop")!=FINISHED or prior.get("exit") not in (0,-15):
            raise ValueError("warming requires a completed traversal")
        donor=json.loads((previous/"start.json").read_text())
        if donor.get("emulator_source_sha256",donor.get("fixture_emulator",EMU_SHA))!=source_sha:
            raise ValueError("donor emulator executable does not match")
        if donor["vpk"] != smoke.digest(vpk) or donor["eboot"] != hashlib.sha256(z_payload(vpk)).hexdigest():
            raise ValueError("donor is from another package or executable")
        if EMU_RELEASE not in (previous/"host.log").read_text(errors="replace"):
            raise ValueError("donor emulator release does not match")
        src, dst = (previous/DRIVER_CACHE).parent, (d/DRIVER_CACHE).parent
        for p in (src,dst):
            if p.resolve(strict=True) != p: raise ValueError("symlink in private title cache path")
        staging=d/"overlay.pending"
        overlay=make_host_overlay(src,dst,staging,"all")
        dst.rename(d/"title-cache.before")
        staging.rename(dst)
    fs = d / "portable/fs"
    data = fs / "ux0/data/smg"
    if set(p.name for p in data.iterdir()) != {"game","saves","input.txt","env.txt"}:
        raise ValueError("unexpected guest files")
    if args.route=="moving":
        settings=smoke.retail_env("HoneyBeeKingdomGalaxy",False).replace(
            "PETARI_STAGE=HoneyBeeKingdomGalaxy\n","PETARI_STAGE=HoneyBeeKingdomGalaxy:1\n")
        script=(HERE/"moving-input.txt").read_bytes()
        if hashlib.sha256(script).hexdigest()!=MOVING_INPUT_SHA or hashlib.sha256(settings.encode()).hexdigest()!=MOVING_ENV_SHA:
            raise ValueError("archived moving route hashes do not match")
    else:
        settings,script=ENV,b"1000:A\n"
    (data/"env.txt").write_text(settings)
    (data/"input.txt").write_bytes(script)
    app = fs / "ux0/app" / smoke.TITLE
    app.mkdir(parents=True)
    with zipfile.ZipFile(vpk) as z:
        if any(not (app / n).resolve().is_relative_to(app) for n in z.namelist()):
            raise ValueError("unsafe candidate path")
        z.extractall(app)
    bundle = d / "C182Vita3K.app"
    if args.local_emulator:
        target=bundle/"Contents/MacOS/Vita3K"
        copy=target.with_suffix(".new")
        smoke.clone(args.local_emulator,copy)
        if smoke.digest(copy)!=source_sha: raise ValueError("private emulator copy differs from the pinned source")
        copy.replace(target)
    bundle_id = "local.codex.galaxy.hitch-lab." + args.label
    info = bundle / "Contents/Info.plist"
    plist = plistlib.loads(info.read_bytes())
    plist["CFBundleIdentifier"] = bundle_id
    info.chmod(0o644)
    info.write_bytes(plistlib.dumps(plist))
    subprocess.run(["codesign","--force","--sign","-","--identifier",bundle_id,str(bundle)],check=True)
    cache_root = Path(subprocess.check_output(["getconf","DARWIN_USER_CACHE_DIR"],text=True).strip()).resolve(strict=True)
    seed, mac = cache_root / args.metal_seed_id, cache_root / bundle_id
    if seed.is_symlink() or not seed.is_dir() or mac.exists() or mac.resolve() != mac or seed==mac:
        raise ValueError("private warm cache required and output must be new")
    seed_sha=sha_tree(seed)
    shutil.copytree(seed,mac,copy_function=smoke.clone)
    if sha_tree(mac)!=seed_sha: raise ValueError("warming the private Metal cache was not byte-exact")
    if smoke.digest(data/"input.txt")!=hashlib.sha256(script).hexdigest() or (data/"env.txt").read_text()!=settings:
        raise ValueError("unmatched input or environment at launch")
    expected_payload=hashlib.sha256(z_payload(vpk)).hexdigest()
    if smoke.digest(app/"eboot.bin") != expected_payload: raise ValueError("installed eboot differs from package")
    receipt = {"vpk": smoke.digest(vpk), "eboot":expected_payload,"fixture_emulator":EMU_SHA,
               "emulator_source_sha256":source_sha,"emulator_provenance":"local QA build" if args.local_emulator else "stock QA build",
               "host_checkpoint_switch":args.checkpoint,"resigned_emulator_sha256":smoke.digest(bundle/"Contents/MacOS/Vita3K"),
               "data_save":sha_tree(data/"saves"), "input":smoke.digest(data/"input.txt"),
               "env":smoke.digest(data/"env.txt"),"warm_host_cache":sha_tree(d/"portable/cache"),
               "warm_metal_seed":sha_tree(mac),"guest_cache":"empty","bundle_id":bundle_id,
               "label":args.label,"profile":"Mac Vita3K retail VSOUT=1, no forced packs","route":args.route,
               "pipeline_before_intervention":seed_digest,"pipeline_at_launch":smoke.digest(d/DRIVER_CACHE),
               "cache_file_bytes":(d/DRIVER_CACHE).stat().st_size,"host_cache_donor_arm":args.warm_from,
               "host_cache_intervention":"all" if args.warm_from else "none","overlay":overlay}
    if (data/"shadercache").exists(): raise ValueError("guest cache not clear")
    (d/"start.json").write_text(json.dumps(receipt,indent=2)+"\n")
    return d, mac, bundle, data


def z_payload(vpk):
    with zipfile.ZipFile(vpk) as archive:
        return archive.read("eboot.bin")


def run(args):
    if sys.platform!="darwin":
        raise ValueError("the replay requires the Mac Vita3K renderer")
    processes=subprocess.check_output(["ps","-axo","comm="],text=True)
    if any(Path(s.strip()).name=="Vita3K" for s in processes.splitlines()):
        raise ValueError("another emulator is running; use an exclusive slot")
    d, mac, bundle, data = prepare(args)
    moving=args.route=="moving"
    finish=MOVING_FINISHED if moving else FINISHED
    env = {k:v for k,v in os.environ.items() if not k.startswith(("VITA3K_","PETARI_","SMG_","GXV_","DYLD_"))}
    if args.checkpoint is not None:
        env[CHECKPOINT]=str(args.checkpoint)
    for k in ("HOME","XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_DATA_HOME","TMPDIR"):
        sub=d/k.lower()
        sub.mkdir()
        env[k]=str(sub)
    env.update(SDL_VIDEO_MAC_FULLSCREEN_SPACES="0", SDL_AUDIO_DRIVER="dummy")
    rules='(version 1)(allow default)(deny network*)(deny file-write*)(allow file-write* (subpath '+json.dumps(str(d))+') (subpath '+json.dumps(str(mac))+') (literal "/dev/null"))'
    cmd=["/usr/bin/sandbox-exec","-p",rules,str(bundle/"Contents/MacOS/Vita3K"),"-c",str(d/"config.yml"),"--load-config","--keep-config","-B","Vulkan","-r",smoke.TITLE,"--frontend","sdl"]
    with (d/"host.log").open("xb") as out:
        with subprocess.Popen(cmd,env=env,cwd=d,stdout=out,stderr=subprocess.STDOUT) as p:
            start=time.monotonic()
            reason="time limit"
            post=None
            try:
                while time.monotonic()-start < args.seconds:
                    if p.poll() is not None:
                        reason="emulator exited"
                        break
                    if any(f.exists() and f.stat().st_size > 48<<20 for f in (d/"host.log",d/"portable/vita3k.log")):
                        reason="diagnostic growth limit"
                        break
                    if (data/"log.txt").exists() and (data/"log.txt").read_text(errors="replace").count(ANCHOR)>=(1 if moving else 2):
                        if post is None: post=time.monotonic()
                        if time.monotonic()-post >= (85 if moving else 18):
                            reason=finish
                            break
                    time.sleep(1)
            finally:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(7)
                    except subprocess.TimeoutExpired: p.kill()
            rc=p.wait()
    outcome={"exit":rc,"seconds":round(time.monotonic()-start,1),"stop":reason}
    (d/"outcome.json").write_text(json.dumps(outcome,indent=2)+"\n")
    print(json.dumps(outcome))
    if not (data/"log.txt").is_file(): return 2
    log=(data/"log.txt").read_text(errors="replace")
    return 0 if (reason==finish and rc in (0,-15) and (log.count(ANCHOR)>=1 if moving else log.count(ANCHOR)==2)
                 and EMU_RELEASE in (d/"host.log").read_text(errors="replace")) else 2


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    safe=lambda s: s if re.fullmatch(r"[a-z0-9-]{1,24}",s) else ap.error("invalid label")
    ap.add_argument("label",type=safe)
    ap.add_argument("--output",type=Path,required=True,help="private experiment root, outside the original fixture")
    ap.add_argument("--fixture",type=Path,help="private template with emulator .app, config.yml, portable/fs and warm portable/cache")
    ap.add_argument("--metal-seed-id",default="local.codex.galaxy.settings-qa.old-3",help="existing private QA-only Metal cache bundle identifier")
    ap.add_argument("--vpk",type=Path,help="candidate package; requires matching --vpk-sha256")
    ap.add_argument("--vpk-sha256",help="expected candidate identity; the default with neither flag is the frozen C186.1 VPK")
    ap.add_argument("--warm-from",type=safe,help="precondition from a completed private test arm; guest inputs remain unchanged")
    ap.add_argument("--route",choices=("stationary","moving"),default="stationary",help="two stationary entries, or frozen jump/spin/steering loop for 85s after first control")
    ap.add_argument("--local-emulator",type=Path,help="independently built QA Vita3K executable at the same 4232 revision; use only in disposable profiles")
    ap.add_argument("--local-emulator-sha256",help="expected local QA executable SHA-256 before private signing")
    ap.add_argument("--checkpoint",type=int,choices=(0,1),help="only with a pinned local QA executable: host's optional async checkpoint switch")
    ap.add_argument("--seconds",type=int,default=230,choices=range(70,361),metavar="70..360")
    args=ap.parse_args()
    if args.warm_from and args.route!="stationary": ap.error("the moving comparison uses an original, not transplanted, host cache")
    if bool(args.vpk)!=bool(args.vpk_sha256) or (args.vpk_sha256 and not re.fullmatch(r"[0-9a-f]{64}",args.vpk_sha256)):
        ap.error("supply --vpk and a lowercase expected --vpk-sha256 together, or neither")
    if (bool(args.local_emulator)!=bool(args.local_emulator_sha256)
        or (args.local_emulator_sha256 and not re.fullmatch(r"[0-9a-f]{64}",args.local_emulator_sha256))
        or (args.checkpoint is not None and not args.local_emulator)):
        ap.error("the optional host checkpoint requires --local-emulator and matching lowercase --local-emulator-sha256")
    if not re.fullmatch(r"local\.codex\.galaxy\.[a-z0-9.-]+",args.metal_seed_id):
        ap.error("only a private QA Metal seed bundle is accepted")
    fixture=(args.fixture or smoke.OLD).resolve(strict=True)
    args.output=args.output.resolve()
    if fixture==args.output or fixture in args.output.parents or args.output in fixture.parents:
        ap.error("test output and original fixture must be disjoint")
    if args.output.exists() and any(args.output.iterdir()):
        existing_profiles=(args.output/"private").glob("*/isolation.json")
        if not any(p.parent.resolve()==p.parent and json.loads(p.read_text()).get("profile")=="emulator / retail" for p in existing_profiles):
            ap.error("refusing a nonempty directory that is not an existing private QA lab")
    args.output.mkdir(exist_ok=True,parents=True)
    with (args.output/".replay.lock").open("a") as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        sys.exit(run(args))
