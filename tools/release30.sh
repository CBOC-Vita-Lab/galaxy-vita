#!/bin/bash
# release30.sh <frozen-ref> <out-dir> --seed <seed VPK or shadercache folder> [--skip-wformat] [--dry-run]
#
# The Galaxy Vita 3.0 release build, package and gates in one go, from a FROZEN ref (release31). Galaxy ships as 3.0: VPK 03.00, SMG_APP_VERSION 3.0.0,
# multiplayer build id 3.0.0, build label 3.0. "3.1" is only the internal branch name and the test builds' label (build id 3.1-dev): this script
# refuses a test id and cannot be told another one (LABEL / BUILD_ID in the environment are ignored on purpose).
#
# Order (the coordinator's, docs/RELEASE-3.0-CHECKLIST.md section 4d):
#   1. pre-flight: the frozen ref's tree says 3.0 (tools/release30_gates.py tree) and carries the five approved contributor PNGs; the seed exists
#   2. build:      LABEL=3.0 BUILD_ID=3.0.0 REPRO=1 /home/claude/w/build31.sh <ref> <out>      (ThinLTO + PGO + TLS; ~35 min cold, the ONE build slot)
#                  build31.sh itself runs the 15-symbol multiplayer check, tools/check_weak_links.py with vita_must_define.txt and the TLS check
#   3. package:    cmake target smg-vgx.vpk (the base VPK: eboot, param.sfo VERSION 03.00, the contributor art), then
#                  tools/seedrun/stagetags.py package <base> <seed> <out>/Galaxy-Vita.vpk   (compact seed: one v3/seed.c182, the stage list tagged),
#                  then stagetags.py checkvpk. The VPK is made only here; it is never copied together by hand.
#   4. gates (each one is logged to <out>/logs/gate-<name>.log; all of them run, then the verdict):
#        tree  buildid  links  mp15  tls  stack  wformat(3 databases)  art  vpk  checkvpk  strings  pgo
#   5. <out>/RELEASE-INFO.txt: ref, commit, sha256 of the eboot and of the VPK, build id, label, profile, the gates passed / failed / skipped.
# Exit 0 only when every gate ran and passed. 1 = a gate failed. 3 = all that ran passed but a gate was skipped (--skip-wformat): not a release.
# Other exits: 2 = usage / refused (test id, no seed, tree is not 3.0, a build is running), 5 = the build or the package step failed.
#
# --dry-run: no build, no package; <out-dir> is an EXISTING build's output (BUILD-INFO.txt, eboot.bin, galaxy_native). Runs the gates against it and
#   the cache build tree as they are, wformat on 5 units only, a scratch VPK made from that eboot (nothing is written into <out-dir>; the result goes to
#   $DRY_DIR, default a mktemp folder, printed). A test build (3.1-dev) must FAIL the buildid gate: that is the proof the refusal works.
#   tools/release30.sh HEAD /home/claude/w/out/console4 --seed <seed> --dry-run
#
# Environment (optional): REPO=/home/claude/w/galaxy  CACHE=/home/claude/w/cache/b31  KIT=/home/claude/w/kit/c181kit  JOBS=2  TLS=/tls/mbedtls/vita (the mbedTLS the build links; the tls gate checks the same path)
# Needs root (unshare -m), /opt/vitasdk, clang-18. Disk: the build cache stays under ~2 GB over the ThinLTO cache; wformat keeps nothing.
set -uo pipefail
RELEASE_ID=3.0.0; RELEASE_LABEL=3.0; VPK_VERSION=03.00
REPO=${REPO:-/home/claude/w/galaxy}; CACHE=${CACHE:-/home/claude/w/cache/b31}; KIT=${KIT:-/home/claude/w/kit/c181kit}; JOBS=${JOBS:-2}
BUILD31=${BUILD31:-/home/claude/w/build31.sh}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BD=/build-c183-clean-final
REF=""; OUT=""; SEED=""; SKIPW=0; DRY=0
while [ $# -gt 0 ]; do case "$1" in
  --seed) SEED=${2:-}; shift 2;; --skip-wformat) SKIPW=1; shift;; --dry-run) DRY=1; shift;;
  -h|--help) sed -n 2,34p "$0"; exit 0;; -*) echo "unknown option $1"; exit 2;;
  *) if [ -z "$REF" ]; then REF=$1; elif [ -z "$OUT" ]; then OUT=$1; else echo "too many arguments"; exit 2; fi; shift;; esac; done
[ -n "$REF" ] && [ -n "$OUT" ] || { sed -n 2,6p "$0"; exit 2; }
# the release id is fixed here; refuse anything that tries to change it
for v in LABEL BUILD_ID; do
  eval "cur=\${$v:-}"; want=$RELEASE_LABEL; [ "$v" = BUILD_ID ] && want=$RELEASE_ID
  [ -z "$cur" ] || [ "$cur" = "$want" ] || { echo "REFUSED: $v=$cur in the environment; a release is LABEL=$RELEASE_LABEL BUILD_ID=$RELEASE_ID (test ids such as 3.1-dev are never released)"; exit 2; }
done
unset LABEL BUILD_ID
SHA=$(git -C "$REPO" rev-parse --verify "$REF^{commit}") || { echo "unknown ref $REF"; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -n "$SEED" ] && SEED=$(realpath "$SEED")
if [ "$DRY" = 1 ]; then LOGD=${DRY_DIR:-$(mktemp -d /tmp/release30-dry.XXXXXX)}; mkdir -p "$LOGD"; else LOGD=$OUT/logs; mkdir -p "$LOGD"; fi
GATES_PASS=(); GATES_FAIL=(); GATES_SKIP=()
say() { echo "== release30: $*"; }

# ---- namespace helper: the build's own mounts (/source <- $CACHE/root, $BD <- $CACHE/build, /kit), so the paths inside the compile databases exist
in_ns() { CACHE=$CACHE KIT=$KIT BD=$BD NSCMD="$1" unshare -m --propagation private bash -uo pipefail -c '
  for d in /source $BD /kit; do [ -d "$d" ] || mkdir -p "$d"; done
  mount --bind "$CACHE/root" /source; mount --bind "$CACHE/build" "$BD"; mount --bind "$KIT" /kit
  export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH; cd /source/source; eval "$NSCMD"'; }
# gate <name> <command string>: run by bash (in the build's namespace when NS=1), logged, recorded. A gate passes on exit 0.
gate() { local n=$1; shift; local rc
  say "gate $n"
  if [ "${NS:-0}" = 1 ]; then in_ns "nice -n 10 $*" > "$LOGD/gate-$n.log" 2>&1; else nice -n 10 bash -c "$*" > "$LOGD/gate-$n.log" 2>&1; fi; rc=$?
  tail -n 6 "$LOGD/gate-$n.log" | cut -c1-240 | sed 's/^/     /'
  if [ $rc = 0 ]; then GATES_PASS+=("$n"); say "gate $n: PASS"; else GATES_FAIL+=("$n(rc=$rc)"); say "gate $n: FAIL rc=$rc (see $LOGD/gate-$n.log)"; fi; }
skip() { GATES_SKIP+=("$1"); say "gate $1: SKIPPED ($2)"; }

# ---- 1. pre-flight (on the ref itself, before any build)
PRE=$(mktemp -d "${TMPDIR:-/tmp}/release30-pre.XXXXXX"); trap 'rm -rf "$PRE"' EXIT
git -C "$REPO" archive "$SHA" platform/src/mp/mp_service.cpp net/src/online/np_app_version.h vita/vgx-game/CMakeLists.txt cboc-release.json \
    server/wrangler.smg.toml vita/vgx-game/sce_sys tools/release_gates.py tools/release30_gates.py 2>/dev/null | tar -x -C "$PRE"
if [ "$DRY" = 0 ]; then
  [ -f "$PRE/tools/release30_gates.py" ] || { echo "REFUSED: $REF does not carry tools/release30_gates.py (run this script from a checkout of the frozen ref)"; exit 2; }
  python3 -I "$PRE/tools/release30_gates.py" tree --root "$PRE" || { echo "REFUSED: the tree of $REF is not a 3.0 tree"; exit 2; }
  python3 -I "$PRE/tools/release30_gates.py" art --root "$PRE" || { echo "REFUSED: the contributor artwork of $REF is not the approved set"; exit 2; }
  [ -n "$SEED" ] && [ -e "$SEED" ] || { echo "REFUSED: --seed <seed VPK or shadercache folder> is required (the merged, tagged seed: checklist 4)"; exit 2; }
  [ "$(git -C "$HERE" rev-parse HEAD 2>/dev/null)" = "$SHA" ] || say "NOTE: this checkout is not at $SHA; the gates below use the frozen tree that build31.sh exports, not this checkout"
  # ---- 2. build
  say "build: LABEL=$RELEASE_LABEL BUILD_ID=$RELEASE_ID REPRO=1 $BUILD31 $REF $OUT   ($(date -Is))"
  LABEL=$RELEASE_LABEL BUILD_ID=$RELEASE_ID REPRO=1 JOBS=$JOBS REPO=$REPO CACHE=$CACHE KIT=$KIT "$BUILD31" "$SHA" "$OUT" || { echo "BUILD FAILED (build31.sh rc $?)"; exit 5; }
fi

# the build is over: take the cache lock so nothing builds under the gates
exec 9>"$CACHE/lock"; flock -n 9 || { echo "REFUSED: another build31.sh holds $CACHE/lock"; exit 2; }
INFO=$OUT/BUILD-INFO.txt
[ -f "$INFO" ] && [ -f "$OUT/galaxy_native" ] && [ -f "$OUT/eboot.bin" ] || { echo "no BUILD-INFO.txt / galaxy_native / eboot.bin in $OUT"; exit 2; }
kv() { sed -n "s/^$1 //p" "$INFO" | head -1; }
EBOOT_SHA=$(sha256sum "$OUT/eboot.bin" | cut -d' ' -f1)
if [ "$DRY" = 0 ]; then
  [ "$(kv commit)" = "$SHA" ] || { echo "REFUSED: $INFO is commit $(kv commit), not $SHA"; exit 2; }
  [ "$(kv label)" = "$RELEASE_LABEL" ] && [ "$(kv build_id)" = "$RELEASE_ID" ] || { echo "REFUSED: BUILD-INFO says label $(kv label) build_id $(kv build_id)"; exit 2; }
  [ "$(kv eboot_sha256)" = "$EBOOT_SHA" ] || { echo "REFUSED: eboot.bin does not match BUILD-INFO.txt"; exit 2; }
  case "$(kv source_date_epoch)" in unset*|"") echo "REFUSED: the build was not REPRO=1"; exit 2;; esac
  [ "$(sha256sum "$CACHE/build/vita-vgx-game/eboot.bin" | cut -d' ' -f1)" = "$EBOOT_SHA" ] || { echo "REFUSED: the cache's eboot.bin is not $OUT/eboot.bin (another build ran since)"; exit 2; }
  TOOLS=$CACHE/root/source; ROOT=$CACHE/root/source          # the frozen tree build31.sh exported
else
  TOOLS=$HERE/..; ROOT=$HERE/..                              # dry run: this checkout's gate logic
fi
TOOLS=$(cd "$TOOLS" && pwd); ROOT=$(cd "$ROOT" && pwd)

# ---- 3. package
VPK=$OUT/Galaxy-Vita.vpk
if [ "$DRY" = 0 ]; then
  say "package: base VPK from the build's cmake target, then stagetags package"
  in_ns 'cmake --build $BD/vita-vgx-game --target smg-vgx.vpk --parallel 2' > "$LOGD/vpk.log" 2>&1 || { echo "PACKAGE FAILED: smg-vgx.vpk target (see $LOGD/vpk.log)"; exit 5; }
  BASE=$CACHE/build/vita-vgx-game/smg-vgx.vpk
  rm -f "$VPK"
  python3 -I "$TOOLS/tools/seedrun/stagetags.py" package "$BASE" "$SEED" "$VPK" > "$LOGD/package.log" 2>&1 || { echo "PACKAGE FAILED: stagetags package (see $LOGD/package.log)"; tail -5 "$LOGD/package.log"; exit 5; }
else
  VPK=$LOGD/dry-Galaxy-Vita.vpk
  if [ -n "$SEED" ] && [ -e "$SEED" ]; then
    say "dry-run: scratch VPK from $OUT/eboot.bin (a stand-in for the cmake target: vita-mksfoex + vita-pack-vpk)"
    S=$ROOT/vita/vgx-game/sce_sys
    /opt/vitasdk/bin/vita-mksfoex -s TITLE_ID=SMGV00001 -s APP_VER=$VPK_VERSION -d ATTRIBUTE2=12 "Super Mario Galaxy" "$LOGD/param.sfo" >/dev/null 2>&1 \
     && /opt/vitasdk/bin/vita-pack-vpk -s "$LOGD/param.sfo" -b "$OUT/eboot.bin" -a "$S/icon0.png=sce_sys/icon0.png" -a "$S/pic0.png=sce_sys/pic0.png" \
        -a "$S/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png" -a "$S/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png" \
        -a "$S/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml" "$LOGD/dry-base.vpk" >/dev/null 2>&1 \
     && python3 -I "$TOOLS/tools/seedrun/stagetags.py" package "$LOGD/dry-base.vpk" "$SEED" "$VPK" > "$LOGD/package.log" 2>&1 || { echo "dry-run scratch VPK failed (see $LOGD)"; VPK=""; }
  else VPK=""; fi
fi

# ---- 4. gates
say "gates ($([ $DRY = 1 ] && echo dry-run || echo release)); logs in $LOGD"
G="python3 -I $TOOLS/tools/release30_gates.py"
RG="python3 -I /source/source/tools/release_gates.py"
[ "$DRY" = 1 ] && RG="python3 -I $TOOLS/tools/release_gates.py"
gate tree "$G tree --root $ROOT"
gate buildid "python3 -I $TOOLS/tools/release_gates.py buildid --elf $OUT/galaxy_native --info $INFO --expect $RELEASE_ID --release"
# the checks build31.sh runs after the link, repeated on the ELF in the out dir
MUST="tools/vita_must_define.txt"; ALLOW="tools/weak_links.allow"
NS=1 gate links "python3 -I tools/check_weak_links.py $OUT/galaxy_native --build-dir $BD --allow $ALLOW --must-define $MUST"
NM=/opt/vitasdk/bin/arm-vita-eabi-nm; $NM "$OUT/galaxy_native" > "$LOGD/release.nm" 2>/dev/null
SYMS="np_vita_radio_dialog_render np_vita_radio_dialog_active np_vita_radio_dialog_prepare np_vita_radio_rx_late_take_max np_vita_radio_ring_take_peak np_vita_ime_dialog_render np_vita_ime_dialog_active sceNetCtlInetGetState sceNetCtlInetGetInfo sceNetCtlInetRegisterCallback sceNetCtlInetUnregisterCallback sceNetCtlCheckCallback sceNetGetStatisticsInfo mp_diag_report_begin mp_diag_check_begin"
gate mp15 "n=0; miss=''; for s in $SYMS; do n=\$((n+1)); awk -v s=\$s '\$NF==s && \$2 ~ /^[Tt]\$/ {f=1} END {exit !f}' $LOGD/release.nm || miss=\"\$miss \$s\"; done; [ \$n = 15 ] && [ -z \"\$miss\" ] && echo \"mp15 ok: 15/15 symbols defined (T/t)\" || { echo \"mp15 FAIL: \$n listed, missing:\$miss\"; exit 1; }"
gate tls "grep -q ' T mbedtls_ssl_handshake\$' $LOGD/release.nm && ! grep -q 'no SMG_MBEDTLS' $CACHE/build/logs/link-configure.log && grep -q 'SMG_MBEDTLS:.*='"${TLS:-/tls/mbedtls/vita}"'\$' $CACHE/build/vita-vgx-game/CMakeCache.txt && grep -q 'VGL_SHADER_READER_ARCHIVE:.*vitaGL-reader-vp30.a\$' $CACHE/build/vita-vgx-game/CMakeCache.txt && echo 'tls ok: mbedtls_ssl_handshake defined, link configured with SMG_MBEDTLS=${TLS:-/tls/mbedtls/vita}, reader vp30 linked'"
NS=1 gate stack "$RG stack --root /source/source --build-dir $BD --jobs $JOBS"
# -Wformat over all units: the three databases (game units, the link stage's, the renderer's)
if [ "$SKIPW" = 1 ]; then skip wformat "--skip-wformat"
else
  LIM=""; [ "$DRY" = 1 ] && LIM="--limit 5"
  say "gate wformat (the game units take about 45 minutes at one niced job$([ $DRY = 1 ] && echo '; dry-run: 5 units a database'))"
  NS=1 gate wformat "ninja -C $BD/vita-vgx-runtime -t compdb > $LOGD/runtime-db.raw 2>/dev/null; python3 -I $TOOLS/tools/release30_gates.py rtdb $LOGD/runtime-db.raw $LOGD/runtime-db.json
    rc=0; for db in $BD/vita-source/compile_commands.json $BD/vita-vgx-game/compile_commands.json $LOGD/runtime-db.json; do
      echo \"-- \$db\"; $RG wformat --db \$db --cpath $BD/case-shim --jobs $JOBS --nice 10 $LIM || rc=\$?; done; exit \$rc"
  rm -f "$LOGD/runtime-db.json" "$LOGD/runtime-db.raw"
fi
gate strings "$G strings --elf $OUT/galaxy_native"
if [ -n "$VPK" ] && [ -f "$VPK" ]; then
  gate art "$G art --root $ROOT --vpk $VPK"
  gate vpk "$G vpk --vpk $VPK --eboot $OUT/eboot.bin"
  gate checkvpk "python3 -I $TOOLS/tools/seedrun/stagetags.py checkvpk $VPK"
else skip art "no VPK"; skip vpk "no VPK"; skip checkvpk "no VPK"; fi
# the profile of the release: the kit's copy (what the build read) is the tree's own tools/pgo/smg.profdata.gz
KPSHA=$(sha256sum "$KIT/scripts/pgo/smg.profdata.gz" | cut -d' ' -f1); TPSHA=$(sha256sum "$ROOT/tools/pgo/smg.profdata.gz" | cut -d' ' -f1)
gate pgo "[ $KPSHA = $TPSHA ] && echo 'pgo ok: kit profile = tools/pgo/smg.profdata.gz ($KPSHA)' || { echo 'pgo FAIL: the kit profile ($KPSHA) is not the tree profile ($TPSHA): the re-recorded profile was installed in one place only'; exit 1; }"
MISM=$(grep -c "hash mismatch" "$OUT/logs/lto-compile.log" 2>/dev/null || true)

# ---- 5. RELEASE-INFO.txt
VPKSHA=""; [ -n "$VPK" ] && [ -f "$VPK" ] && VPKSHA=$(sha256sum "$VPK" | cut -d' ' -f1)
if [ ${#GATES_FAIL[@]} -gt 0 ]; then VERDICT="FAILED"; RC=1; elif [ ${#GATES_SKIP[@]} -gt 0 ]; then VERDICT="INCOMPLETE (a gate was skipped: not a release)"; RC=3; else VERDICT="PASSED"; RC=0; fi
INFOOUT=$OUT/RELEASE-INFO.txt; [ "$DRY" = 1 ] && INFOOUT=$LOGD/RELEASE-INFO.DRYRUN.txt
{ echo "release 3.0 (VPK $VPK_VERSION, SMG_APP_VERSION 3.0.0)"; echo "ref $REF"; echo "commit $SHA"; echo "build_id $(kv build_id)"; echo "label $(kv label)"
  echo "eboot_sha256 $EBOOT_SHA"; echo "vpk_sha256 ${VPKSHA:-none}"; echo "vpk_file Galaxy-Vita.vpk"; echo "elf_sha256 $(sha256sum "$OUT/galaxy_native" | cut -d' ' -f1)"
  echo "pgo_profile_sha256 $KPSHA"; echo "pgo_hash_mismatch_warnings_this_run ${MISM:-n/a}"; echo "source_date_epoch $(kv source_date_epoch)"
  echo "mode $([ $DRY = 1 ] && echo DRY-RUN || echo release)"; echo "gates_passed ${GATES_PASS[*]:-none}"; echo "gates_failed ${GATES_FAIL[*]:-none}"; echo "gates_skipped ${GATES_SKIP[*]:-none}"
  echo "not_run_here emulator gate (release_gates.py emu, 3 stages), PGO twin (4c), console session"; echo "verdict $VERDICT"; echo "finished $(date -Is)"; } > "$INFOOUT"
echo; cat "$INFOOUT"
[ "$DRY" = 1 ] && echo "(dry-run files in $LOGD)"
exit $RC
