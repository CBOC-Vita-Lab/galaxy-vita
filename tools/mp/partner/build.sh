#!/bin/sh
# Build the practice partner with the computer's own compiler (macOS: Apple clang; Linux: g++ or clang++). No other software needed.
#   ./build.sh                 builds ./partner next to this script (OUT=path to put it elsewhere)
#   MP_BUILD_ID=xyz ./build.sh the build id of the Vita build, when it was not the default "dev" (see the line the program prints at start)
#   CXX=clang++ ./build.sh     another compiler
# Works from the game's source tree (tools/mp/partner) and from the bundle make_source_bundle.sh makes (same folder layout).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${OUT:-$here/partner}
os=$(uname -s)

cxx=${CXX:-}
if [ -z "$cxx" ]; then
  for c in c++ clang++ g++; do
    if command -v "$c" >/dev/null 2>&1; then cxx=$c; break; fi
  done
fi
if [ -z "$cxx" ] || ! command -v "$cxx" >/dev/null 2>&1; then
  echo "No C++ compiler found."
  if [ "$os" = "Darwin" ]; then
    echo "On a Mac, install Apple's command line tools once, then run this again:"
    echo "    xcode-select --install"
  else
    echo "Install one (for example: sudo apt install g++), then run this again."
  fi
  exit 1
fi
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || cc=$cxx   # (clang++ / g++ also compile C when told -x c)

# which source tree is this (for the "source" word in the program's first line)
rev=unknown
if [ -f "$here/SOURCE-REV" ]; then rev=$(head -n 1 "$here/SOURCE-REV")
elif command -v git >/dev/null 2>&1 && git -C "$root" rev-parse --short HEAD >/dev/null 2>&1; then
  rev=$(git -C "$root" rev-parse --short HEAD)
  git -C "$root" diff --quiet HEAD -- 2>/dev/null || rev="$rev+edited"
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/partner-build.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
# relative paths from the tree's root: the list below is split on spaces, so a folder name with a space in it ("Vita Conversion") must not appear
cd "$root"
N=net/src
M=platform/src/mp
P=tools/mp/partner

# the build id of the Vita build: MP_BUILD_ID in the environment, else build-id.txt next to this script (make_source_bundle.sh writes it)
if [ -z "$MP_BUILD_ID" ] && [ -f "$here/build-id.txt" ]; then MP_BUILD_ID=$(head -n 1 "$here/build-id.txt" | tr -d '\r\n '); fi
defs="-DPARTNER_SOURCE_REV=\"$rev\""
[ -n "$MP_BUILD_ID" ] && defs="$defs -DMP_BUILD_ID=\"$MP_BUILD_ID\""

echo "Building the practice partner with $cxx ($os $(uname -m)) ..."
# monocypher (plain C, the session crypto's tiny library) first: C and C++ are compiled differently
"$cc" -O2 -c -w "$N/online/third_party/monocypher/monocypher.c" -o "$tmp/monocypher.o"

# every source to an object file (in parallel), then one link
srcs="$P/partner.cpp $P/partner_udp.cpp $P/partner_stubs.cpp
  $M/mp_core.cpp $M/mp_core_sys.cpp $M/mp_core_vita.cpp $M/mp_reliable.cpp $M/mp_lan.cpp $M/mp_snapshot.cpp
  $M/mp_config.cpp $M/mp_log.cpp $M/mp_transport_loop.cpp $M/mp_transport_vlink.cpp $M/mp_conn_online.cpp $M/mp_conn_direct.cpp
  $M/mp_demo.cpp $M/mp_keepsafe.cpp $M/mp_savefile.cpp $M/mp_savecheck.cpp
  $N/vlink.cpp $N/vlink_udp.cpp $N/online/np_net.cpp $N/online/np_stun.cpp $N/online/np_crypto.cpp $N/online/np_fault.cpp"
objs=""
n=0
for f in $srcs; do
  n=$((n + 1))
  o="$tmp/$n.o"
  objs="$objs $o"
  # shellcheck disable=SC2086
  "$cxx" -std=c++20 -O2 -pthread -w $defs \
    -Iplatform/include -Inet/include -I"$M" -I"$N" -I"$N/online" \
    -c "$f" -o "$o" || { echo "failed: $f"; touch "$tmp/failed"; } &
  # (a few at a time: a small computer should stay usable)
  if [ $((n % 4)) -eq 0 ]; then wait; fi
done
wait
if [ -f "$tmp/failed" ]; then echo "The build failed (see above)."; exit 1; fi

# The session layer has optional hooks (weak references: the online service, the Vita's radio) that this build leaves out. Linux
# links them as "not there"; the Mac's linker wants to be told that missing ones are fine.
ldflags=""
if [ "$os" = "Darwin" ]; then ldflags="-Wl,-undefined,dynamic_lookup"; fi
# shellcheck disable=SC2086
"$cxx" -pthread $ldflags -o "$out" $objs "$tmp/monocypher.o" ||
  { [ -n "$ldflags" ] && echo "Trying again without the linker option ..." && "$cxx" -pthread -o "$out" $objs "$tmp/monocypher.o"; }

echo "Built: $out"
"$out" --version
echo "The protocol number and build id above must match the Vita's game (a different number: build again from the Vita's source)."
