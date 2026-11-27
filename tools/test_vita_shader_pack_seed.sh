#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $# == 3 ]] || { printf 'usage: %s private-seed.vpk full-candidate-ABI new-output-directory\n  env: EXPECT_RECORDS (compiled files the seed must hold; default 2158, seed 1116), GATE_SOURCE (a source tree: also run the seedgate31 closure gate, tools/pack24/refill_pack.py --gate-only,\n       on the seed; GATE_PROFILE_LOG: a log of the run that made it, checked against the tree'"'"'s profile word)\n' "$0" >&2; exit 2; }
seed=$1 identity=$2 out=$3
[[ -f $seed && ! -e $out ]] || { printf 'seed missing or output already exists\n' >&2; exit 2; }
mkdir -p "$out"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" --mount "type=bind,source=$seed,target=/seed.vpk,readonly" \
  --mount "type=bind,source=$out,target=/results" -e PACK_IDENTITY="$identity" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    python3 - <<'"'"'PY'"'"'
import hashlib, json, os, pathlib, re, zipfile
identity = os.environ["PACK_IDENTITY"]
key = 2166136261
for byte in identity.encode():
    key = ((key ^ byte) * 16777619) & 0xffffffff
root = pathlib.Path("/results/cache") / f"{key:08x}" / "v3"
pattern = re.compile(r"shadercache/([0-9a-f]{8})/v3/([vf])/([0-9A-F]{2})/([0-9A-F]{1,16})\.gxp")
records, payload, roots, seen = 0, 0, set(), set()
with zipfile.ZipFile("/seed.vpk") as archive:
    for info in archive.infolist():
        match = pattern.fullmatch(info.filename)
        if not match:
            continue
        old, kind, bucket, name = match.groups()
        assert bucket == f"{int(name, 16) >> 56:02X}" and name == f"{int(name, 16):X}"
        assert (kind, name) not in seen and info.file_size <= 256 * 1024
        seen.add((kind, name)); roots.add(old)
        data = archive.read(info)
        target = root / kind / bucket / (name + ".gxp")
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as file:
            file.write(data)
        records += 1; payload += len(data)
assert records == int(os.environ["EXPECT_RECORDS"]) and len(roots) == 1, (records, roots)
pathlib.Path("/results/root.txt").write_text(str(root))
pathlib.Path("/results/seed-input.json").write_text(json.dumps({"seed_sha256": hashlib.file_digest(open("/seed.vpk", "rb"), "sha256").hexdigest(), "source_keys": sorted(roots), "candidate_key": f"{key:08x}", "full_identity": identity, "records": records, "payload_bytes": payload}, indent=2) + "\n")
PY
    cache=$(< /results/root.txt)
    clang++ -std=c++17 -stdlib=libc++ -O2 -Iplatform/vgx/third_party tools/vita_shader_pack.cpp -o /results/build-pack
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -Iplatform/vgx/third_party \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vita_shader_pack_roundtrip.cpp -o /results/roundtrip -l:libasan.so.8 -l:libubsan.so.1
    /results/build-pack "$cache" "$PACK_IDENTITY" /results/stages.pack
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 /results/roundtrip "$cache" "$PACK_IDENTITY" /results/stages.pack "$EXPECT_RECORDS"
    if /results/build-pack "$cache" "wrong-ABI" /results/must-not-exist.pack; then exit 1; fi
    test ! -e /results/must-not-exist.pack
  '
# (seedgate31 A2) the closure gate on the same seed: profile word, every stage record's two key recipes and two compiled files, every cap. A closure failure fails the script.
if [[ -n ${GATE_SOURCE:-} ]]; then
  gate_args=(--package "$seed" --source "$GATE_SOURCE" --gate-only --identity "$identity" --out "$out/gate")
  [[ -z ${GATE_PROFILE_LOG:-} ]] || gate_args+=(--profile-log "$GATE_PROFILE_LOG")
  python3 -I "$root/tools/pack24/refill_pack.py" "${gate_args[@]}"
fi
