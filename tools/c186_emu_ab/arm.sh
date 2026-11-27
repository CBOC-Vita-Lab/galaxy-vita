#!/bin/sh
# Run inside a new offline container, never on the console or host display.
set -eu
cd /tmp
mkdir -p v3k/home v3k/xdg/config v3k/xdg/cache v3k/xdg/data v3k/xdg/runtime v3k/fb
cd v3k
export HOME=$PWD/home XDG_CONFIG_HOME=$PWD/xdg/config XDG_CACHE_HOME=$PWD/xdg/cache
export XDG_DATA_HOME=$PWD/xdg/data XDG_RUNTIME_DIR=$PWD/xdg/runtime
chmod 700 "$XDG_RUNTIME_DIR"
export APPIMAGE=$PWD/Vita3K.AppImage APPDIR=$PWD/squashfs-root
export LP_NUM_THREADS=2 LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy LANG=C.UTF-8 LC_ALL=C.UTF-8
unset VK_LOADER_LAYERS_ENABLE VK_INSTANCE_LAYERS VK_LAYER_MESA_SCREENSHOT_CONFIG
cp /emu "$APPIMAGE"
chmod 755 "$APPIMAGE"
"$APPIMAGE" --appimage-extract >/dev/null
for part in shaders-builtin translations gui-configs; do
    src=$(find "$APPDIR/usr/share" -type d -name "$part" -print -quit)
    test -n "$src"; dest=$APPDIR/usr/bin/$part
    if test "$part" = gui-configs; then mkdir -p "$APPDIR/usr/bin/data"; dest=$APPDIR/usr/bin/data/$part; fi
    if test ! -e "$dest"; then ln -s "$src" "$dest"; fi
done
for driver in /usr/share/vulkan/icd.d/lvp_icd*.json; do test -f "$driver"; export VK_DRIVER_FILES="$driver"; break; done
fs=portable/fs
data=$fs/ux0/data/smg
mkdir -p "$data/game" "$fs/ux0/app/SMGV00001"
for part in os0 sa0 vs0 ur0; do cp -a "/input/$part" "$fs/"; done
cp -a /input/ux0/user "$fs/ux0/"
ln -s /input/ux0/data/smg/game/files "$data/game/files"
cp -a /input/ux0/data/smg/game/sys /input/ux0/data/smg/game/sizes.cache "$data/game/"
cp -a /input/ux0/data/smg/saves "$data/"
cp /route "$data/input.txt"
cp /env "$data/env.txt"
cp /config portable/config.yml
cp -a /title/. "$fs/ux0/app/SMGV00001/"
test ! -e "$data/shadercache"; test ! -e portable/cache; test ! -e "$HOME/.cache"
chown -R 1000:1000 /tmp/v3k
setpriv --reuid=1000 --regid=1000 --clear-groups Xvfb :75 -ac -nolisten tcp -screen 0 1280x800x24 -fbdir /tmp/v3k/fb >/tmp/xvfb.log 2>&1 &
xpid=$!
trap 'kill "$xpid" 2>/dev/null || :' EXIT
for _ in $(seq 1 20); do test ! -S /tmp/.X11-unix/X75 || break; sleep .2; done
test -S /tmp/.X11-unix/X75
export DISPLAY=:75
setpriv --reuid=1000 --regid=1000 --clear-groups "$APPDIR/AppRun" -r SMGV00001 >/private/console.log 2>&1 &
pid=$!
start=$(date +%s)
reason=timeout
first=0
second=0
visible_streak=0
fb=fb/Xvfb_screen0
# Sample three scanlines inside the SDL game's viewport, not the emulator UI.
# Do this only AFTER the second anchor, outside the timed first traversal.
world_lit() {
    off=$(od -An -v -tu1 -N100 "$fb" | awk '
        {for(i=1;i<=NF;i++) a[n++]=$i}
        function be(i){return ((a[i]*256+a[i+1])*256+a[i+2])*256+a[i+3]}
        END{if(n==100 && be(4)==7 && be(16)==1280 && be(20)==800 && be(44)==32)
                print be(0)+12*be(76); else exit 1}') || return 1
    for y in 140 260 390; do
        od -An -v -tu1 -j "$((off+y*5120+220*4))" -N2080 "$fb"
    done | awk '{
        for(i=1;i<=NF;i++) {
            c[n%4]=$i
            if(n%32==2){max=min=c[0]; for(k=1;k<=2;k++){if(c[k]>max)max=c[k]; if(c[k]<min)min=c[k]}
                count++; if(max>28)lit++; if(max-min>20)color++}
            n++
        }
    } END {exit !(count>=150 && lit/count>=.15 && color/count>=.08)}'
}
anchor='script: clock started: t = 0 at the first play turn in HoneyBeeKingdomGalaxy scenario 1'
while test "$(($(date +%s) - start))" -lt "${AB_MAX_SECONDS:-320}"; do
    if ! kill -0 "$pid" 2>/dev/null; then reason=process-exited; break; fi
    if test -f "$data/log.txt"; then
        anchors=$(grep -cF "$anchor" "$data/log.txt" || :)
        if test "$anchors" -ge 1 && test "$first" -eq 0; then first=$(date +%s); fi
        if test "$first" -gt 0 && test "$(($(date +%s) - first))" -ge 7 && test ! -f /private/play.xwd; then cp fb/Xvfb_screen0 /private/play.xwd; fi
        if test "$anchors" -ge 2; then
            now=$(date +%s)
            if test "$second" -eq 0; then second=$now; fi
            delay=$((now-second))
            if test "$delay" -ge 7; then
                if world_lit; then visible_streak=$((visible_streak+1)); else visible_streak=0; fi
                if test "$visible_streak" -ge 2; then
                    cp "$fb" /private/return.xwd
                    printf '%s\n' "$delay" >/private/return-delay.txt
                    reason=route-complete
                    break
                fi
            fi
            if test "$delay" -ge 35; then reason=no-visible-return; break; fi
        fi
    fi
    sleep 1
done
kill "$pid" 2>/dev/null || :
for _ in 1 2 3 4 5; do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
kill -KILL "$pid" 2>/dev/null || :
wait "$pid" || :
test ! -f "$data/log.txt" || cp "$data/log.txt" /private/game.log
printf '%s\n' "$reason" >/private/reason
