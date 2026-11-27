#!/usr/bin/env bash
# Inside a network-disabled compiler container. Source always comes from a named git object.
# (c186merge) This is the recipe of the C186.2 VPK (source a178bd3f on e44c45d8). It is kept on integ-3.1 as that build's record; it is not
# integ-3.1's release build (build-3.0-vita.sh / tools/vgx_build/build.py). Its "Unexpected linked marker" gate below encodes C186's scope
# (no C185 items); integ-3.1 deliberately carries C185's plant reveal, Observatory mask warm-up and the (off) depth-edge option, so a build of an
# integ-3.1 revision fails that gate by design.
set -euo pipefail
rev=${1:?expected a committed revision}
phase=${2:-release}
base=e44c45d8bed8f089d3dc75eb2b4b3091bbb1c491
export VITASDK=/opt/vitasdk VGL_DEPS=/vdeps SMG_MBEDTLS=/tls/mbedtls/vita
export BUILD_DIR=/build-c186 SRC_ROOT=/source/source LABEL=3.0-C186.2 PGO_REWRITE=1
export PATH="$VITASDK/bin:$PATH"
gitc=(git -c safe.directory=/worktree -C /worktree)
sha=$("${gitc[@]}" rev-parse --verify "$rev^{commit}")
"${gitc[@]}" merge-base --is-ancestor "$base" "$sha"
[[ $phase == warm || $phase == release ]]
if [[ $phase == release ]]; then [[ $sha != "$base" ]]; fi
export SOURCE_DATE_EPOCH=$("${gitc[@]}" show -s --format=%ct "$base")
sha256sum -c <<'C186_INPUTS'
c9341512bd3dee1d1f728f5faf42b88a35d17a3348cb70c0329d41fb78e115f1  /in/kit.tgz
15aca426c4d091a8e2b3e1cf8731463943a49882e209320afea765bcaca349a4  /in/vdeps.tgz
72690fd40991ae10b813a1166d99322e113c66befdaa242b5658a153c707b5a6  /in/vitasdk.tgz
40c4d7ce2610cba931007383fab8e3480d6922072b62fc9cfd1418651925934d  /in/tls.tgz
C186_INPUTS
mkdir -p /source /vdeps /tls/sources "$BUILD_DIR" /kit
if [[ ! -f /kit/ready ]]; then
    tar -xzf /in/vitasdk.tgz -C /opt
    tar -xzf /in/vdeps.tgz -C /vdeps --strip-components=1
    tar -xzf /in/tls.tgz -C /tls/sources
    tar -xzf /in/kit.tgz -C /kit --strip-components=1
    (cd /kit && sha256sum -c SHA256SUMS)
    arm-vita-eabi-gcc --version
    touch /kit/ready
fi
# Never compile ignored, untracked or stale files from the developer's checkout.
if [[ -d $SRC_ROOT ]]; then mv "$SRC_ROOT" "$(mktemp -d /source/previous.XXXXXX)/source"; fi
mkdir "$SRC_ROOT"
"${gitc[@]}" archive "$sha" | tar -xf - -C "$SRC_ROOT"
cd "$SRC_ROOT"
if [[ -s /source/built-rev ]]; then
    old=$(cat /source/built-rev)
    # Archives give unchanged files the new commit's mtime. Only object inputs
    # whose bytes changed should be newer than the warm objects.
    find . -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
    "${gitc[@]}" diff --no-renames --name-only --diff-filter=AM -z "$old" "$sha" |
        xargs -0 -r touch -h --
fi
if [[ $phase == release ]]; then python3 -I tools/check-files.py; fi
if [[ ! -f "$SMG_MBEDTLS/build-receipt.json" ]]; then
    export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=/tls/sources/Mbed-TLS_mbedtls
    python3 -I tools/mp/build-mbedtls.py --work /tls --target vita
fi
export PGO_PROFILE=/kit/scripts/pgo/smg.profdata.gz READER=/kit/scripts/vgl/libvitaGL-reader-vp30.a
mkdir -p /home/claude/g23/tools/tools
cp -p /kit/g23-tools/* /home/claude/g23/tools/tools/
sed -i "s/--jobs 2/--jobs 6/;s/--label 1.2-CLAUDEPERF/--label $LABEL --build-id $LABEL/" /home/claude/g23/tools/tools/bstage.sh
sed -i "s/'--label', label/'--label', label, '--build-id', label/;s/'-j2'/'-j6'/" /home/claude/g23/tools/tools/build_rl.py
trap 'rm -f "$SRC_ROOT/tools/vgx_build/build_fast.py"' EXIT
bash /kit/scripts/build_fast.sh "$SRC_ROOT" "$READER" c186
printf '%s\n' "$sha" > /source/built-rev
[[ $phase == release ]] || exit 0
elf="$BUILD_DIR/vita-vgx-game/galaxy_native"
for marker in 'GXV_ARCHIVE_BATCH' 'GXV_J3D_ENV_REUSE' 'GXV_DIAG_FULL_FAR_POSE' 'sockets: %d open (%d epoll)' 'settings: restart returned' 'assist60' 'GXM display queue (whole frame)' 'present path' "$LABEL"; do
    grep -aFq "$marker" "$elf" || { echo "Missing linked marker: $marker"; exit 1; }
done
if grep -E 'bool Historical\(|firstvisit::Historical\(' platform/vgx/src/{vgx_stage_first_visit.h,vgx_backend_gl.cpp}; then
    echo 'Historical per-stage shader priority is not allowed in C186.'; exit 1
fi
for marker in 'c184-honey.keys' 'c185-honey7.keys' 'C184: Honeyhive covered-load recipes' 'GXV_C185_HONEY_7' 'GXV_HONEY_PLANT_REVEAL' 'GXV_HRW41_DEPTH_EDGE' 'GXV_C185_ASTRO_MASK_WARM'; do
    if grep -aFq "$marker" "$elf"; then echo "Unexpected linked marker: $marker"; exit 1; fi
done
arm-vita-eabi-nm "$elf" > /tmp/c186.nm
# (c186merge) C186.1's own nine-symbol Settings gate is folded into the general post-link check of integ-3.1 (linkfix31):
# every name of tools/vita_must_define.txt (the Settings hooks among them) must be a global strong definition.
python3 -I tools/check_weak_links.py "$elf" --build-dir "$BUILD_DIR" --nm arm-vita-eabi-nm --readelf arm-vita-eabi-readelf \
    --allow tools/weak_links.allow --must-define tools/vita_must_define.txt
for n in np_vita_radio_dialog_render np_vita_radio_dialog_active np_vita_radio_dialog_prepare np_vita_radio_rx_late_take_max np_vita_radio_ring_take_peak np_vita_ime_dialog_render np_vita_ime_dialog_active sceNetCtlInetGetState sceNetCtlInetGetInfo sceNetCtlInetRegisterCallback sceNetCtlInetUnregisterCallback sceNetCtlCheckCallback sceNetGetStatisticsInfo mp_diag_report_begin mp_diag_check_begin mbedtls_ssl_handshake; do
    awk -v s="$n" '$NF==s && $2 ~ /^[Tt]$/ {f=1} END {exit !f}' /tmp/c186.nm || { echo "Missing strong symbol: $n"; exit 1; }
done
grep -Fq "=$SMG_MBEDTLS" "$BUILD_DIR/vita-vgx-game/CMakeCache.txt"
grep -Fq "=$READER" "$BUILD_DIR/vita-vgx-game/CMakeCache.txt"
grep -Fq -- '-fprofile-use=' "$BUILD_DIR/vita-source/compile_commands.json"
grep -Fq -- '-fprofile-use=' "$BUILD_DIR/vita-vgx-runtime/build.ninja"
mkdir -p /out/link
cp "$BUILD_DIR"/logs/{link-configure,link,runtime-report,lto-compile}.log /out/link/
cp "$SMG_MBEDTLS/build-receipt.json" /out/link/tls-build-receipt.json
cp docs/SOURCE-MANIFEST.json /out/link/source-manifest-C186.json
cp "$BUILD_DIR/vita-vgx-game/eboot.bin" /out/eboot-C186.bin
cp "$elf" /out/galaxy_native-C186
cmake --build "$BUILD_DIR/vita-vgx-game" --target smg-vgx.vpk
cp "$BUILD_DIR/vita-vgx-game/smg-vgx.vpk" /out/link/unseeded.vpk
printf '%s\n' "$sha" > /out/link/built-revision
sha256sum "$PGO_PROFILE" "$READER" /in/{kit,vdeps,vitasdk,tls}.tgz > /out/link/build-inputs.sha256
sha256sum /out/{eboot-C186.bin,galaxy_native-C186}
