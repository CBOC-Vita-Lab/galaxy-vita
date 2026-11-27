"""Build or read back the local C186 package. No shader generation or console access."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

HERE = Path(__file__).resolve().parent
TREE = Path(os.environ.get("C186_TREE") or HERE.parents[1]).resolve()
if not (TREE / "vita/vgx-game/CMakeLists.txt").is_file():
    raise SystemExit("Set C186_TREE to the source checkout when using a copied packager")
OUT = Path(os.environ.get("C186_OUT", TREE.parent / "outputs/galaxy-c186.2")).resolve()
DONOR = Path(os.environ.get("C186_DONOR", TREE.parent / "claude-galaxy/perf-handoff-1115/Galaxy-Vita-2.3-test19.vpk"))
DONOR_SHA = "b30de6817be13a1ace80243fcdd902be5ef31762907128f3880308d7bc1c6256"
BASE = "e44c45d8bed8f089d3dc75eb2b4b3091bbb1c491"
PACK = OUT / "Galaxy-Vita-3.0-C186.2.vpk"
SCRATCH = OUT / "link/unseeded.vpk"
ICON = "sce_sys/"
LIVE = ICON + "livearea/contents/"
INERT = LIVE + "settings.png"
ART = {
    ICON + "icon0.png": "22fc0fc7817993913ab94d59d8183c2f1e2c77e49dc85576bc8effb05a001c73",
    ICON + "pic0.png": "b4f739a6431f3754e5b58181e829bceba5e28dabbe5722b615a35dbe77c243cc",
    LIVE + "startup.png": "154187070bedd6bb5fd63065ec1d114b766a51ffb0ad386e4546e653ffd1620f",
    LIVE + "bg.png": "c9e3ee83f32cf424abbc67cee71e9696cd29ad049599276a3e757d544b9fd7cb",
    INERT: "3ed8b34c9e5dcee4df2dcd75728641c577b12fa2affe0e6ee3eca1c7451796ed",
}
sys.path.insert(0, str(TREE / "tools"))
from c182_compact_seed import compact


def h(data):
    return sha256(data).hexdigest()


def check(condition, message):
    if not condition:
        raise ValueError(message)


def git(*args):
    return subprocess.check_output(["git", "-C", TREE, *args])


def file_map(z):
    names = [m.filename for m in z.infolist() if not m.is_dir()]
    check(len(names) == len(set(names)), "Duplicate VPK path")
    return {n: z.read(n) for n in names}


def sfo(data):
    magic, _, keys, strings, size = struct.unpack_from("<4sIIII", data)
    check(magic == b"\0PSF", "Invalid SFO magic")
    result = {}
    for i in range(size):
        key, fmt, length, _, offset = struct.unpack_from("<HHIII", data, 20 + i * 16)
        name = data[keys+key:data.index(b"\0", keys+key)].decode()
        value = data[strings+offset:strings+offset+length]
        if fmt == 0x0204:
            result[name] = value.rstrip(b"\0").decode()
        elif fmt == 0x0404 and length == 4:
            result[name] = int.from_bytes(value, "little")
    return result


def expected():
    rev = (OUT / "link/built-revision").read_text().strip()
    check(git("rev-parse", "HEAD").decode().strip() == rev, "Checkout not at the linked source revision")
    git("merge-base", "--is-ancestor", BASE, rev)
    m = (OUT / "link/source-manifest-C186.json").read_bytes()
    check(m == git("show", rev + ":docs/SOURCE-MANIFEST.json"), "Build manifest and committed source differ")
    manifest = json.loads(m)
    check(all(h((TREE / r["path"]).read_bytes()) == r["sha256"] for r in manifest["files"]),
          "Working-tree file differs from the linked source manifest")
    with ZipFile(SCRATCH) as raw:
        items = file_map(raw)
    expected_base = set(ART) - {INERT} | {LIVE + "template.xml", ICON + "param.sfo", "eboot.bin"}
    check(set(items) == expected_base, "Unexpected integration CMake VPK contents")
    check(items["eboot.bin"] == (OUT / "eboot-C186.bin").read_bytes(), "Mismatched final executable")
    meta = sfo(items[ICON + "param.sfo"])
    check((meta["TITLE_ID"], meta["APP_VER"], meta["ATTRIBUTE2"]) == ("SMGV00001", "03.00", 12),
          "Unexpected bubble identity/version/memory budget")
    check(items[LIVE + "template.xml"] == git("show", BASE + ":vita/vgx-game/" + LIVE + "template.xml"),
          "Changed integration LiveArea template")
    # Integration no longer references its old Settings tile. Retain the fifth original PNG as data only.
    items[INERT] = git("show", rev + ":vita/vgx-game/" + INERT)
    for n, digest in ART.items():
        check(h(items[n]) == digest and items[n] == git("show", BASE + ":vita/vgx-game/" + n),
              f"Not the original contributor art: {n}")
    check(h(DONOR.read_bytes()) == DONOR_SHA, "Not the pinned test19 seed")
    with ZipFile(DONOR) as old:
        seed = {n:v for n,v in file_map(old).items() if n.startswith("shadercache/")}
    packed = compact(seed)
    for n in ("shadercache/vgx_shaderlist.bin", "shadercache/vgx_stagewarm.bin"):
        check(packed[n] == seed[n], f"Modified donor {n}")
    check(not any(n.endswith(".keys") for n in packed), "Unexpected shader supplement")
    check(sum(n.endswith(".gxp") for n in seed) == 5174, "Wrong loose shader count")
    items.update(packed)
    return items, rev


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("build", "verify"))
    a = ap.parse_args()
    items, rev = expected()
    if a.mode == "build":
        tmp = PACK.with_suffix(".vpk.tmp")
        with ZipFile(tmp, "w") as z:
            for name in sorted(items):
                entry = ZipInfo(name, (2026, 11, 17, 0, 0, 0))
                entry.compress_type = ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                z.writestr(entry, items[name], compresslevel=6)
        with ZipFile(tmp) as z:
            check(z.testzip() is None and file_map(z) == items, "Generated VPK readback mismatch")
        tmp.replace(PACK)
    with ZipFile(PACK) as z:
        check(z.testzip() is None and set(z.namelist()) == set(items) and file_map(z) == items,
              "Delivered VPK differs from the pinned inputs")
    print(f"{PACK.name}: {PACK.stat().st_size:,} bytes, {len(items)} entries, SHA256 {h(PACK.read_bytes())}")
    print(f"eboot.bin SHA256 {h(items['eboot.bin'])}; source {rev}")
    print("SMGV00001 / 03.00 / ATTRIBUTE2=12. Exact integration LiveArea, five contributor PNGs, 5,174 shader payloads; "
          "no C184/C185 supplement or modified donor stage/list records.")
    if a.mode == "verify":
        subprocess.run([sys.executable, "-I", "tools/check-files.py"], cwd=TREE, check=True)
        subprocess.run([sys.executable, "tools/test_c182_seed.py", DONOR], cwd=TREE, check=True)
        subprocess.run(["bash", "tools/test_c182_firstvisit.sh", PACK], cwd=TREE, check=True)
        print(f"Host packager: Python {sys.version.split()[0]}, zlib {zlib.ZLIB_RUNTIME_VERSION}")


if __name__ == "__main__":
    main()
