#!/bin/bash
# Collect exactly the sources the practice partner needs into one .tgz that can be copied to a Mac (or Linux box) and built there with
# ./build.sh. Nothing but code and the build / run scripts: no game data, no saves, no tests, nothing private.
#   tools/mp/partner/make_source_bundle.sh [OUT.tgz]
#   MP_BUILD_ID=3.1-dev tools/mp/partner/make_source_bundle.sh   the Vita build's build id goes into the bundle (build-id.txt; ./build.sh uses it)
#   DATA_ROOT=/data/cooked tools/mp/partner/make_source_bundle.sh   also writes data-hash.txt (the version fingerprint's game-data part:
#                                                                  8 hex digits worked out from DATA_ROOT/sys/fst.bin; DATA_HASH=xxxxxxxx gives it directly)
# The bundle keeps the repository's folder layout (platform/, net/, decomp/include/Game/MP/, tools/mp/partner/) so the same build.sh
# works in both places. The partner always speaks the protocol of the tree it was bundled from: bundle from the tree the Vita build
# comes from.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cd "$root"
cxx=${CXX:-g++}
command -v "$cxx" >/dev/null 2>&1 || cxx=c++

rev=unknown
if git -C "$root" rev-parse --short HEAD >/dev/null 2>&1; then
  rev=$(git -C "$root" rev-parse --short HEAD)
  git -C "$root" diff --quiet HEAD -- 2>/dev/null || rev="$rev+edited"
fi
out=${1:-partner-source-${rev//+/-}.tgz}
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac

work=$(mktemp -d "${TMPDIR:-/tmp}/partner-bundle.XXXXXX")
trap 'rm -rf "$work"' EXIT
stage="$work/galaxy-partner"
mkdir -p "$stage"

N=net/src; M=platform/src/mp; P=tools/mp/partner
sources="$P/partner.cpp $P/partner_udp.cpp $P/partner_stubs.cpp
 $M/mp_core.cpp $M/mp_core_sys.cpp $M/mp_core_vita.cpp $M/mp_reliable.cpp $M/mp_lan.cpp $M/mp_snapshot.cpp
 $M/mp_config.cpp $M/mp_log.cpp $M/mp_transport_loop.cpp $M/mp_transport_vlink.cpp $M/mp_conn_online.cpp $M/mp_conn_direct.cpp
 $M/mp_demo.cpp $M/mp_keepsafe.cpp $M/mp_savefile.cpp $M/mp_savecheck.cpp
 $N/vlink.cpp $N/vlink_udp.cpp $N/online/np_net.cpp $N/online/np_stun.cpp $N/online/np_crypto.cpp $N/online/np_fault.cpp
 $N/online/third_party/monocypher/monocypher.c"

# every file the compiler reads for those sources (headers and the .cpp files that are included), as repository-relative paths
list="$work/files.txt"
: > "$list"
for f in $sources; do echo "$f" >> "$list"; done
for f in $sources; do
  case "$f" in *.c) continue ;; esac
  "$cxx" -std=c++20 -MM -I"$root/platform/include" -I"$root/net/include" -I"$root/$M" -I"$root/$N" -I"$root/$N/online" "$root/$f" |
    tr ' \\' '\n\n' | grep -v '^$' | grep -v ':$' | grep '/' >> "$list" || true
done
for h in $N/online/third_party/monocypher/monocypher.h $N/online/third_party/monocypher/LICENCE.md; do echo "$h" >> "$list"; done
# normalise to repository-relative, drop duplicates and anything outside the tree (system headers are never listed by -MM)
sed -e "s#^$root/##" "$list" | sort -u > "$work/rel.txt"
if grep -v '^[A-Za-z0-9_./+-]*$' "$work/rel.txt" | grep -q .; then echo "odd file names in the list"; exit 1; fi
while read -r f; do
  [ -f "$root/$f" ] || { echo "missing: $f"; exit 1; }
  case "$f" in
    /*|../*) echo "outside the tree: $f"; exit 1 ;;
  esac
  mkdir -p "$stage/$(dirname "$f")"
  cp "$root/$f" "$stage/$f"
done < "$work/rel.txt"

# the run-and-build files
for f in build.sh README.md; do cp "$here/$f" "$stage/$P/$f"; done
chmod +x "$stage/$P/build.sh"
echo "$rev" > "$stage/$P/SOURCE-REV"
[ -n "$MP_BUILD_ID" ] && echo "$MP_BUILD_ID" > "$stage/$P/build-id.txt"   # (the Vita build's build id, e.g. 3.1-dev: it is part of the version fingerprint)
cat > "$stage/build.sh" <<'EOS'
#!/bin/sh
# Builds ./partner (see README.md).
here=$(cd "$(dirname "$0")" && pwd)
OUT="$here/partner" exec "$here/tools/mp/partner/build.sh" "$@"
EOS
chmod +x "$stage/build.sh"
cp "$here/README.md" "$stage/README.md"

# the game-data part of the version fingerprint
hash=${DATA_HASH:-}
if [ -z "$hash" ] && [ -n "$DATA_ROOT" ] && [ -f "$DATA_ROOT/sys/fst.bin" ]; then
  OUT="$work/partner-native" "$here/build.sh" >/dev/null
  hash=$("$work/partner-native" --print-data-hash --data-root "$DATA_ROOT")
fi
if [ -n "$hash" ]; then
  echo "$hash" > "$stage/data-hash.txt"
  echo "data hash $hash written to data-hash.txt"
else
  echo "No data hash (set DATA_ROOT or DATA_HASH): the partner will be told it by hand: ./partner --data-hash HEX"
fi

( cd "$work" && tar czf "$out" galaxy-partner )
echo "Bundle: $out ($(wc -c < "$out") bytes, source $rev)"
echo "Files:"
( cd "$stage" && find . -type f | sort | sed 's#^\./#  #' )
