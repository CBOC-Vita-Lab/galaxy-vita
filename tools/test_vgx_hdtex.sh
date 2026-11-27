#!/usr/bin/env bash
# HD texture replacement through the real texture cache (native g++, no Docker): a synthetic
# one-texture pack written by tools/hdtex/make_test_pack.py (build_pack.py's key and writer),
# looked up by vgx_hdtex.cpp, for each mode of tests/vgx_hdtex.cpp.
#   tools/test_vgx_hdtex.sh [build dir, default /tmp/vgx-hdtex-test]
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$MP_TMP/vgx-hdtex-test}
mkdir -p "$out"
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/test.pack"
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/test-eye.pack" Test:SyntheticEye
${CXX:-g++} -std=c++20 -O1 -g -fsanitize=address,undefined -DVGX_NO_NEON -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_hdtex.cpp" "$root/platform/vgx/src/vgx_hdtex.cpp" "$root/platform/vgx/src/vgx_state.cpp" "$root/platform/vgx/src/vgx_draw.cpp" "$root/platform/vgx/src/vgx_fesplit.cpp" \
  "$root/platform/vgx/src/vgx_dlcache.cpp" "$root/platform/vgx/src/vgx_guestmem.cpp" "$root/platform/vgx/src/vgx_bpsync.cpp" \
  "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_texcache.cpp" "$root/platform/vgx/src/vgx_texdecode.cpp" "$root/platform/vgx/src/vgx_texcomp.cpp" \
  "$root/platform/vgx/src/vgx_stats.cpp" "$root/platform/vgx/src/vgx_backend_null.cpp" -o "$out/vgx_hdtex" -lpthread
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
for mode in replace clamp budget dryrun copy changed trilinear; do
  "$out/vgx_hdtex" "$out/test.pack" "$mode"
done
# hdtex24: the texture cache's age rule and caps (GXV_TEX_SCENEDROP=3) against replacement textures (counted at their HD size, freed when idle, uploaded
# again from the pack), and a pack larger than GXV_HDTEX_PRELOAD (its first part in memory, the rest read from the card)
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/age64.pack" Test:Synthetic --hd 64
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/prefix-early.pack" Test:Synthetic --filler first
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/prefix-late.pack" Test:Synthetic --filler last
for mode in aged agedcard cap; do "$out/vgx_hdtex" "$out/age64.pack" "$mode"; done
"$out/vgx_hdtex" "$out/test.pack" capsmall
"$out/vgx_hdtex" "$out/prefix-early.pack" prefixcard
"$out/vgx_hdtex" "$out/prefix-late.pack" prefixram
# eye textures (name contains "eye") keep the game's own image unless GXV_HDTEX_EYES=1
for mode in eyes eyeson; do
  "$out/vgx_hdtex" "$out/test-eye.pack" "$mode"
done
# which entries are the player's own (GXV_CHARAA_AUTO): Mario, his forms and Luigi; not the NPC Luigi, the Lumas, Peach or the key items.
# A texture with several names (shared by archives) counts when one of them is the player's.
for entry in "Mario:MarioBody player" "MarioHair:Hair player" "BeeMario:Bee player" "TeresaMario:Boo player" "InvincibleMarioMat:Star player" \
             "Luigi:LuigiBody player" "BeeLuigi:Bee player" "TeresaLuigi:Boo player" "Luma:Body notplayer" "LuigiNPC:Body notplayer" \
             "Peach:Dress notplayer" "StarPiece:Star notplayer" "Test:MarioLike notplayer" "NotMario:Body notplayer"; do
  set -- $entry
  python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/player.pack" "$1"
  "$out/vgx_hdtex" "$out/player.pack" replace "$2"
done
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/player.pack" "Luma:Body Mario:Body"
"$out/vgx_hdtex" "$out/player.pack" replace player
# mario24: GXV_PLAYER_MIPVIS=1 sends the player's levels as flat colours (and only his), the others' bytes pass through
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/player.pack" "Mario:MarioBody"
GXV_PLAYER_MIPVIS=1 "$out/vgx_hdtex" "$out/player.pack" replace player
python3 -E -P "$root/tools/hdtex/make_test_pack.py" "$out/player.pack" "Luma:Body"
GXV_PLAYER_MIPVIS=1 "$out/vgx_hdtex" "$out/player.pack" replace notplayer
echo "PASS: 8 HD texture replacement scenarios, 6 cache and preload scenarios, 15 player-texture classifications"
