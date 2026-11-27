#!/usr/bin/env python3
"""release30_gates.py: the file-level release gates of tools/release30.sh (release31). Pure Python 3, no network, no build, prints no secrets.

  tree   --root R                         the version strings of the tree R say 3.0 (SMG_APP_VERSION "3.0.0", np_app_version, VPK VERSION 03.00, cboc-release.json)
  art    --root R [--vpk V]               the five contributor PNGs of R are the approved files (sha256, the same table as tests/test_player_settings_assets.py);
                                          with --vpk the four that the package carries are byte-identical to them
  vpk    --vpk V --eboot E                the VPK is a sound zip; its members are on the allow list (eboot, sce_sys bubble files, the compact shader seed, nothing else:
                                          no libshacccg, firmware, game data, HD pack, disc image, Nintendo file); its eboot.bin is E byte for byte; param.sfo says
                                          SMGV00001 and 03.00
  strings --elf F                         no build machine path (/home/claude, /Users/, C:\\Users) in the ELF
  rtdb   IN OUT                           the renderer's compile database (ninja -t compdb of vita-vgx-runtime) with the rule's "> log 2>&1 || {...}" tail cut off,
                                          so release_gates.py wformat can run its commands (shlex would read the redirection as arguments)

Exit 0 pass, 1 a gate failed, 2 unusable input (not a pass).
"""
import argparse, hashlib, re, struct, sys, zipfile
from pathlib import Path

APPROVED = {  # sha256 of the approved 3.0 bubble & LiveArea PNGs (tests/test_player_settings_assets.py APPROVED)
    "icon0.png": "7fa63a19c822bff68d23aacada5b7e1fc97d698364a1145b4159c72ec82a7b2a",
    "pic0.png": "59668c9c65aa3b6550bb4ecee07ab3158f74bf8e28070c9a14da07569b681096",
    "livearea/contents/bg.png": "3d57068135279416adc97b7fe99e2b96fa7d26fdbeb2d493ba63210c0343fa3a",
    "livearea/contents/startup.png": "d1f6e806f1f11f0613abd3ae65dad066eccbde148a8bf47e3ea6f0c91f1bd2c2",
    "livearea/contents/settings.png": "3ed8b34c9e5dcee4df2dcd75728641c577b12fa2affe0e6ee3eca1c7451796ed",
}
SYS = "vita/vgx-game/sce_sys/"
IN_VPK = {"icon0.png": "sce_sys/icon0.png", "pic0.png": "sce_sys/pic0.png", "livearea/contents/bg.png": "sce_sys/livearea/contents/bg.png",
          "livearea/contents/startup.png": "sce_sys/livearea/contents/startup.png"}   # settings.png stays in the tree, it is not packaged (owner, 17 Nov)
ALLOWED = [re.compile(p) for p in (
    r"eboot\.bin", r"sce_sys/param\.sfo", r"sce_sys/icon0\.png", r"sce_sys/pic0\.png",
    r"sce_sys/livearea/contents/(bg\.png|startup\.png|template\.xml)",
    r"shadercache/vgx_shaderlist\.bin", r"shadercache/vgx_stagewarm\.bin",
    r"shadercache/[0-9a-f]{8}/stages\.pack", r"shadercache/[0-9a-f]{8}/v3/seed\.c182")]
FORBIDDEN = re.compile(r"shacccg|\.suprx|\.skprx|firmware|\.rvz|\.wbfs|\.iso$|\.gcm$|\.arc$|\.brres$|\.szs$|\.bcsv$|\.bin\.gz$|hdtex|hd_pack|\.dol$|rmge01|libshacc", re.I)
MAX_MEMBERS = 64   # stagetags checkvpk's own limit for the compact layout


def sha(b): return hashlib.sha256(b).hexdigest()


def fails(name, bad):
    for b in bad: print(f"{name} FAIL: {b}")
    if not bad: return 0
    return 1


def cmd_tree(a):
    r, bad = Path(a.root), []
    def has(rel, pat, what):
        try: t = (r / rel).read_text(errors="replace")
        except OSError as e: bad.append(f"{rel}: {e}"); return
        if not re.search(pat, t): bad.append(f"{rel}: {what}")
    has("platform/src/mp/mp_service.cpp", r'#define SMG_APP_VERSION "3\.0\.0"', 'SMG_APP_VERSION is not "3.0.0"')
    has("net/src/online/np_app_version.h", r'"3\.0\.0"', 'np_app_version.h does not carry "3.0.0"')
    has("vita/vgx-game/CMakeLists.txt", r"vita_create_vpk\([^\n]*VERSION 03\.00\b", "vita_create_vpk is not VERSION 03.00")
    has("cboc-release.json", r'"version":\s*"3\.0"', 'cboc-release.json version is not "3.0"')
    has("cboc-release.json", r'"releaseId":\s*"galaxy-v3\.0"', "cboc-release.json releaseId is not galaxy-v3.0")
    has("server/wrangler.smg.toml", r'LATEST_APP_VERSION\s*=\s*"3\.0\.0"', "LATEST_APP_VERSION is not 3.0.0")
    has("server/wrangler.smg.toml", r'SMG_TIME_BUILDS\s*=\s*"3\.0\.0"', 'SMG_TIME_BUILDS is not exactly "3.0.0" (test ids must stay out)')
    rc = fails("tree", bad)
    if not rc: print("tree ok: SMG_APP_VERSION 3.0.0, np_app_version 3.0.0, VPK VERSION 03.00, cboc-release galaxy-v3.0 / 3.0, service allow-lists 3.0.0")
    return rc


def cmd_art(a):
    r, bad = Path(a.root), []
    for rel, want in APPROVED.items():
        try: got = sha((r / SYS / rel).read_bytes())
        except OSError as e: bad.append(f"{SYS}{rel}: {e}"); continue
        if got != want: bad.append(f"{SYS}{rel}: not the approved file (sha256 {got[:12]}, approved {want[:12]}): never regenerate the contributor artwork")
    if a.vpk:
        try: z = zipfile.ZipFile(a.vpk)
        except Exception as e: print(f"art: cannot open {a.vpk}: {e}"); return 2
        for rel, member in IN_VPK.items():
            try: got = sha(z.read(member))
            except KeyError: bad.append(f"VPK has no {member}"); continue
            if got != APPROVED[rel]: bad.append(f"VPK {member} is not the approved file (sha256 {got[:12]})")
    rc = fails("art", bad)
    if not rc: print("art ok: the five contributor PNGs are the approved files" + (" and the four in the VPK are byte-identical" if a.vpk else ""))
    return rc


def cmd_vpk(a):
    bad = []
    try:
        z = zipfile.ZipFile(a.vpk)
        t = z.testzip()
    except Exception as e:
        print(f"vpk: cannot open {a.vpk}: {e}"); return 2
    if t: bad.append(f"damaged member {t}")
    names = [i.filename for i in z.infolist() if not i.filename.endswith("/")]
    if len(names) != len(set(names)): bad.append("duplicate members")
    if len(names) > MAX_MEMBERS: bad.append(f"{len(names)} members (the compact layout has at most {MAX_MEMBERS}): a loose shader seed")
    for n in names:
        if FORBIDDEN.search(n): bad.append(f"forbidden member {n}")
        elif not any(p.fullmatch(n) for p in ALLOWED): bad.append(f"member {n} is not on the allow list")
    for need in ("eboot.bin", "sce_sys/param.sfo", "sce_sys/icon0.png", "sce_sys/pic0.png", "sce_sys/livearea/contents/template.xml",
                 "shadercache/vgx_stagewarm.bin"):
        if need not in names: bad.append(f"missing member {need}")
    if not any(re.fullmatch(r"shadercache/[0-9a-f]{8}/v3/seed\.c182", n) for n in names): bad.append("missing the compact seed shadercache/<key>/v3/seed.c182")
    if "eboot.bin" in names:
        try:
            if z.read("eboot.bin") != Path(a.eboot).read_bytes(): bad.append(f"the VPK's eboot.bin is not {a.eboot} (the package was made from another eboot)")
        except OSError as e: print(f"vpk: {e}"); return 2
    if "sce_sys/param.sfo" in names:
        sfo = z.read("sce_sys/param.sfo")
        if b"SMGV00001" not in sfo: bad.append("param.sfo: title id is not SMGV00001")
        if b"03.00\0" not in sfo: bad.append("param.sfo: no 03.00 version")
        if b"03.01" in sfo: bad.append("param.sfo carries 03.01")
    rc = fails("vpk", bad)
    if not rc:
        total = sum(i.file_size for i in z.infolist())
        print(f"vpk ok: {len(names)} members, all on the allow list (no libshacccg, firmware, game data, HD pack, disc or Nintendo file), eboot.bin = {a.eboot}, "
              f"SMGV00001 / 03.00, {Path(a.vpk).stat().st_size} bytes packed, {total} unpacked")
        for n in sorted(names): print(f"  member {n}")
    return rc


def cmd_strings(a):
    try: d = Path(a.elf).read_bytes()
    except OSError as e: print(f"strings: {e}"); return 2
    bad = []
    for pat in (rb"/home/claude", rb"/home/[a-z][a-z0-9_-]+/", rb"/Users/[A-Za-z]", rb"C:\\Users\\[A-Za-z]", rb"C:/Users/[A-Za-z]", rb"claude\.ai", rb"session_01"):
        n = len(re.findall(pat, d))
        if n: bad.append(f"{n} occurrence(s) of {pat.decode()} in {a.elf}")
    rc = fails("strings", bad)
    if not rc: print("strings ok: no build-machine home path, no session link in the ELF")
    return rc


def cmd_rtdb(a):
    import json
    try: db = json.load(open(a.inp))
    except (OSError, ValueError) as e: print(f"rtdb: {e}"); return 2
    for e in db:
        if "command" in e: e["command"] = e["command"].split(" > ")[0]
    json.dump(db, open(a.out, "w"))
    print(f"rtdb: {len(db)} entries -> {a.out}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = ap.add_subparsers(dest="cmd", required=True)
    t = s.add_parser("tree"); t.add_argument("--root", required=True)
    t = s.add_parser("art"); t.add_argument("--root", required=True); t.add_argument("--vpk")
    t = s.add_parser("vpk"); t.add_argument("--vpk", required=True); t.add_argument("--eboot", required=True)
    t = s.add_parser("strings"); t.add_argument("--elf", required=True)
    t = s.add_parser("rtdb"); t.add_argument("inp"); t.add_argument("out")
    a = ap.parse_args()
    return {"tree": cmd_tree, "art": cmd_art, "vpk": cmd_vpk, "strings": cmd_strings, "rtdb": cmd_rtdb}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
