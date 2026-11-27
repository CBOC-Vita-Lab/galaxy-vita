#!/usr/bin/env bash
# seedgate31 A0: the profile word. Host test (no docker, no GL): tests/vgx_profile_word.cpp (the word changes with every text switch of VGX_TEXT_SWITCHES and with
# nothing else; the list and the generator agree), then the same word computed by the TOOL that gates a seed (tools/pack24/refill_pack.py reads the list from the
# same header) for several environments: the two must be equal to the last digit, and the release tree's own word (entry.cpp defaults) must be the word of
# GXV_VSOUT=1 (retail), not of GXV_VSOUT=0.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/profile-word.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
src="$root/platform/vgx/src"
${CXX:-g++} -std=c++20 -O1 -Wall -I"$src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_profile_word.cpp" -o "$tmp/pw"
"$tmp/pw" test "$src"
python3 -I - "$root" "$tmp/pw" <<'PY'
import importlib.util, os, subprocess, sys
root, pw = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("rp", root + "/tools/pack24/refill_pack.py"); rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)
items, opt, gen = rp.load_text_switches(root), rp.load_opt_switches(root), rp.gen_version(root)
assert tuple(n for n, _ in items) == rp.TEXT_SWITCHES, "TEXT_SWITCHES is not the header's list"
def cxx_word(env):
    out = subprocess.run([pw, "print"], capture_output=True, text=True, env=dict(os.environ, **env), check=True).stdout
    line = out.splitlines()[0]
    assert line.startswith("profile: word="), line
    sw = {}
    for l in out.splitlines()[1:]:
        for kv in l.split()[1:]: k, v = kv.split("=", 1); sw[k] = v
    return line.split()[1].split("=")[1], sw
# the C++ and the Python word agree, for the default environment and several others (including optional switches)
base = {n: "" for n, _ in items}
for env in ({}, {"GXV_VSOUT": "1"}, {"GXV_VSOUT": "1", "GXV_TEXVEC2": "1", "GXV_FS_TRIM_PARTS": "47"},
            {"GXV_TEVIN_CANON": "2", "GXV_FOG_BIND": "2", "GXV_IND_VS": "1"}, {"GXV_VSOUT": ""},
            {"GXV_FS_PROJ": "1", "GXV_VS_NOBRANCH": "1", "GXV_FS_HALF_FOG": "1", "GXV_FS_REGS": "1", "GXV_FS_ATEARLY": "1"}):
    cw, sw = cxx_word({k: v for k, v in env.items()})
    pwd = rp.profile_word(items, dict(env), gen, opt)
    assert cw == pwd, f"C++ {cw} / python {pwd} for {env}"
    expected_sw = [k for k, _ in items] + [k for k, d in opt if env.get(k) and env[k] != d]
    assert expected_sw == list(sw), (expected_sw, list(sw))
    for k, v in env.items():
        if v: assert sw[k] == v, (k, sw[k], v)
print("ok: python word == C++ word in 6 environments (including opt switches)")
# the release: entry.cpp's defaults. Retail is GXV_VSOUT=1: the word of that environment, not the word of VSOUT=0
word, values, _, _ = rp.release_profile(root)
assert values["GXV_VSOUT"] == "1", values["GXV_VSOUT"]
cw, _ = cxx_word(dict(values))
assert cw == word, f"release word {word} / C++ under the release's own values {cw}"
vs0 = dict(values, GXV_VSOUT="0")
w0, _ = cxx_word(vs0)
assert w0 != word and rp.profile_word(items, vs0, gen, opt) == w0
print(f"ok: release word {word} (GXV_VSOUT=1); the VSOUT=0 profile {w0} differs")
PY
echo "ok: test_vgx_profile_word"
