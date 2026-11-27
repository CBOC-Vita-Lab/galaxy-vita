#!/bin/bash
# Does the game's HTTPS client accept the chains a Cloudflare Worker on Let's Encrypt's new hierarchy can serve? Offline, no server contacted.
#   tests/mp/tls_chain_test.sh                 build tests/mp/tls_chain_test.cpp (the client's own verifier, host64 Mbed TLS), make synthetic
#                                              chains with openssl, assert every verdict (offline verify AND a real handshake on 127.0.0.1)
#   tests/mp/tls_chain_test.sh REAL.pem [HOST] [DATE]   also verify a recorded served chain against the game's bundled roots only
#                                              (default host galaxy.psvita.dev, date today UTC)
# MBEDTLS=<prefix> (default /tls/mbedtls/host64: tools/mp/build-mbedtls.py --target host64 --work /tls). OUT=<dir> (default /tmp/tls_chain_test).
# The synthetic anchors reach the verifier only through --extra-root (NP_HTTPS_TEST_HOOKS); the game's anchor set is np_https_roots.h alone.
set -uo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
MBEDTLS=${MBEDTLS:-/tls/mbedtls/host64}
OUT=${OUT:-$MP_TMP/tls_chain_test}
REAL=${1:-}
REAL_HOST=${2:-galaxy.psvita.dev}
REAL_DATE=${3:-$(date -u +%Y-%m-%d)}
CXX=${CXX:-clang++}
rm -rf "$OUT"; mkdir -p "$OUT"
BIN="$OUT/tls_chain_test"
$CXX -std=c++20 -g -O1 -Wall -Wextra -Wno-unused-parameter -DNP_HAVE_MBEDTLS=1 -DNP_HTTPS_TEST_HOOKS=1 \
  -Inet/src -Inet/include -I"$MBEDTLS/include" \
  tests/mp/tls_chain_test.cpp net/src/np_https_tls_mbedtls.cpp net/src/np_https_sys.cpp \
  "$MBEDTLS/lib/libmbedtls.a" "$MBEDTLS/lib/libmbedx509.a" "$MBEDTLS/lib/libmbedcrypto.a" -lpthread -o "$BIN" 2>"$OUT/build.err" \
  || { head -40 "$OUT/build.err"; echo "BUILD FAILED"; exit 2; }

FAIL=0; PASS=0
TABLE="$OUT/table.txt"; : >"$TABLE"

# ---- the recorded chain first (the game's anchors only) ---------------------------------------------------------------------------
if [ -n "$REAL" ]; then
  echo "== recorded chain $REAL, host $REAL_HOST, date $REAL_DATE (bundled roots only)"
  "$BIN" verify "$REAL" "$REAL_HOST" "$REAL_DATE"; echo "exit $?"
fi

# ---- synthetic PKI ---------------------------------------------------------------------------------------------------------------
P="$OUT/pki"; mkdir -p "$P"
cat >"$P/ext.cnf" <<'EOF'
[root]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[cross]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[int0]
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,digitalSignature,keyCertSign,cRLSign
extendedKeyUsage = serverAuth,clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[int]
basicConstraints = critical,CA:TRUE
keyUsage = critical,digitalSignature,keyCertSign,cRLSign
extendedKeyUsage = serverAuth,clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
subjectAltName = DNS:cboc-vita.workers.dev,DNS:*.cboc-vita.workers.dev
[leaf_rsa]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
subjectAltName = DNS:cboc-vita.workers.dev,DNS:*.cboc-vita.workers.dev
[leaf_nowild]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:cboc-vita.workers.dev
[leaf_client]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
subjectAltName = DNS:cboc-vita.workers.dev,DNS:*.cboc-vita.workers.dev
[ca]
default_ca = CA_default
[CA_default]
database = $ENV::CADIR/index.txt
new_certs_dir = $ENV::CADIR
serial = $ENV::CADIR/serial
policy = anything
unique_subject = no
copy_extensions = none
[anything]
countryName = optional
organizationName = optional
commonName = optional
EOF

key() {  # key NAME ec256|ec384|rsa1024|rsa2048|rsa4096
  case $2 in
    ec256) openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$P/$1.key" 2>/dev/null ;;
    ec384) openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "$P/$1.key" 2>/dev/null ;;
    rsa*)  openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:${2#rsa} -out "$P/$1.key" 2>/dev/null ;;
  esac
}
# cert OUT KEYNAME "SUBJECT" ISSUER(cert name, or self) EXT START END [MD]   (START / END: YYYYMMDDHHMMSSZ)
cert() {
  local out=$1 k=$2 subj=$3 iss=$4 ext=$5 start=$6 end=$7 md=${8:-sha256}
  export CADIR="$P/ca-$out"; rm -rf "$CADIR"; mkdir -p "$CADIR"; : >"$CADIR/index.txt"; echo 1000 >"$CADIR/serial"
  openssl req -new -key "$P/$k.key" -subj "$subj" -out "$CADIR/req.csr" 2>/dev/null || { echo "req $out failed"; exit 2; }
  if [ "$iss" = self ]; then
    openssl ca -config "$P/ext.cnf" -batch -notext -selfsign -keyfile "$P/$k.key" -in "$CADIR/req.csr" -out "$P/$out.pem" \
      -extfile "$P/ext.cnf" -extensions "$ext" -startdate "$start" -enddate "$end" -md "$md" -rand_serial -preserveDN >"$CADIR/log" 2>&1
  else
    openssl ca -config "$P/ext.cnf" -batch -notext -cert "$P/$iss.pem" -keyfile "$P/${iss%%__*}.key" -in "$CADIR/req.csr" -out "$P/$out.pem" \
      -extfile "$P/ext.cnf" -extensions "$ext" -startdate "$start" -enddate "$end" -md "$md" -rand_serial -preserveDN >"$CADIR/log" 2>&1
  fi
  [ -s "$P/$out.pem" ] || { cat "$CADIR/log"; echo "cert $out failed"; exit 2; }
}
cat_pems() { local o=$1; shift; : >"$P/$o"; for c in "$@"; do cat "$P/$c.pem" >>"$P/$o"; done; }

R0=20200101000000Z; R1=20401231000000Z     # anchors / roots
I0=20250101000000Z; I1=20300101000000Z     # intermediates
L0=20261001000000Z; L1=20261230000000Z     # the leaf: 90 days around the test date
NOW=2026-11-11
# Distinct names (not the real ones) so no synthetic certificate can ever match a real anchor by name; the impostor case below uses the
# real names on purpose.
DN_X1="/C=US/O=Test Security Research Group/CN=Test Root X1"
DN_X2="/C=US/O=Test Security Research Group/CN=Test Root X2"
DN_YE="/C=US/O=TEST/CN=Test Root YE"
DN_YR="/C=US/O=TEST/CN=Test Root YR"
DN_YE1="/C=US/O=Test Encrypt/CN=TE1"
DN_YR1="/C=US/O=Test Encrypt/CN=TR1"
DN_LEAF="/CN=cboc-vita.workers.dev"

echo "making the synthetic PKI (openssl $(openssl version | cut -d' ' -f2))"
key x1 rsa4096; key x2 ec384; key ye ec384; key yr rsa4096; key ye1 ec384; key yr1 rsa2048; key sub ec384; key leaf ec256; key leafr rsa2048
key leaf1024 rsa1024; key evil ec384; key evilye ec384; key evilye1 ec384
cert x1 x1 "$DN_X1" self root $R0 $R1 sha256                      # RSA anchor (as ISRG Root X1)
cert x2 x2 "$DN_X2" self root $R0 $R1 sha384                      # P-384 anchor (as ISRG Root X2)
cert x2__by_x1 x2 "$DN_X2" x1 cross $R0 $R1 sha256                # X2 cross-signed by X1 (served cert 3 of the record)
cert ye ye "$DN_YE" self root $R0 $R1 sha384                      # the new ECDSA root, self-signed (not served)
cert ye__by_x2 ye "$DN_YE" x2 cross $I0 $R1 sha384                # the new root cross-signed by X2 (served cert 2)
cert ye1 ye1 "$DN_YE1" ye int0 $I0 $I1 sha384                     # issuing CA (served cert 1), pathlen:0 as Let's Encrypt's are
cert ye1__np ye1 "$DN_YE1" ye int $I0 $I1 sha384                    # the same without pathlen (for the deeper chain)
cert sub sub "/C=US/O=Test Encrypt/CN=TE1 Sub" ye1__np int $I0 $I1 sha384
cert sub__p0 sub "/C=US/O=Test Encrypt/CN=TE1 Sub" ye1 int $I0 $I1 sha384
cert leaf leaf "$DN_LEAF" ye1 leaf $L0 $L1 sha384                 # ECDSA P-256 leaf (served cert 0)
cert leaf_sub leaf "$DN_LEAF" sub leaf $L0 $L1 sha384
cert leaf_subp0 leaf "$DN_LEAF" sub__p0 leaf $L0 $L1 sha384
cert leaf_exp leaf "$DN_LEAF" ye1 leaf 20260701000000Z 20260929000000Z sha384
cert leaf_fut leaf "$DN_LEAF" ye1 leaf 20261201000000Z 20270301000000Z sha384
cert leaf_nowild leaf "$DN_LEAF" ye1 leaf_nowild $L0 $L1 sha384
cert leaf_client leaf "$DN_LEAF" ye1 leaf_client $L0 $L1 sha384
cert leaf_sha1 leaf "$DN_LEAF" ye1 leaf $L0 $L1 sha1
cert yr__by_x1 yr "$DN_YR" x1 cross $I0 $R1 sha256                # RSA: the new RSA root cross-signed by X1
cert yr yr "$DN_YR" self root $R0 $R1 sha256
cert yr1 yr1 "$DN_YR1" yr int0 $I0 $I1 sha256
cert leafr leafr "$DN_LEAF" yr1 leaf_rsa $L0 $L1 sha256
cert leaf1024 leaf1024 "$DN_LEAF" yr1 leaf_rsa $L0 $L1 sha256
# an impostor: the REAL names of the recorded chain (C=US, O=ISRG, CN=Root YE issued by the real ISRG Root X2 DN), keys of nobody
cert evil evil "/C=US/O=Internet Security Research Group/CN=ISRG Root X2" self root $R0 $R1 sha384
cert evilye evilye "/C=US/O=ISRG/CN=Root YE" evil cross $I0 $R1 sha384
cert evilye1 evilye1 "/C=US/O=Let's Encrypt/CN=YE1" evilye int0 $I0 $I1 sha384
cert evil_leaf leaf "$DN_LEAF" evilye1 leaf $L0 $L1 sha384
# deep chains for the depth limit (MBEDTLS_X509_MAX_INTERMEDIATE_CA 6): leaf <- d1 <- ... <- dN <- ye__by_x2 <- anchor X2
deep() {  # deep N: N intermediates under the cross-signed root, i.e. N+1 intermediates in all
  local n=$1 prev=ye__by_x2 i list=""
  for i in $(seq 1 "$n"); do key "d${n}_$i" ec256; cert "d${n}_$i" "d${n}_$i" "/O=Test/CN=Deep $n.$i" "$prev" int $I0 $I1 sha256; prev="d${n}_$i"; done
  cert "leaf_deep$n" leaf "$DN_LEAF" "$prev" leaf $L0 $L1 sha256
  list="leaf_deep$n"; for i in $(seq "$n" -1 1); do list="$list d${n}_$i"; done
  cat_pems "deep$n.pem" $list ye__by_x2
}
# the cross-signed certificates are signed by the anchor's key: the issuer key file name is the part before "__"
deep 5; deep 6
cat_pems anchor_x2.pem x2
cat_pems anchor_x1.pem x1
cat_pems anchor_x1x2.pem x1 x2
cat_pems a.pem leaf ye1 ye__by_x2 x2__by_x1
cat_pems a_nox1.pem leaf ye1 ye__by_x2
cat_pems b.pem leaf ye1
cat_pems b2.pem leaf ye1 x2__by_x1
cat_pems c.pem leafr yr1 yr__by_x1
cat_pems d.pem leaf_sub sub ye1__np ye__by_x2 x2__by_x1
cat_pems d_p0.pem leaf_subp0 sub__p0 ye1 ye__by_x2 x2__by_x1
cat_pems e_exp.pem leaf_exp ye1 ye__by_x2 x2__by_x1
cat_pems e_fut.pem leaf_fut ye1 ye__by_x2 x2__by_x1
cat_pems e_nowild.pem leaf_nowild ye1 ye__by_x2 x2__by_x1
cat_pems e_client.pem leaf_client ye1 ye__by_x2 x2__by_x1
cat_pems e_sha1.pem leaf_sha1 ye1 ye__by_x2 x2__by_x1
cat_pems e_rsa1024.pem leaf1024 yr1 yr__by_x1
cat_pems e_evil.pem evil_leaf evilye1 evilye
cat_pems e_evil_anchor.pem evil

H=smg-online.cboc-vita.workers.dev
# check NAME CHAIN HOST DATE ANCHORS(file or -) EXPECT(ACCEPT or the flag names that must be set) [handshake: key name]
check() {
  local name=$1 chain=$2 host=$3 date=$4 anchors=$5 expect=$6 hskey=${7:-}
  local extra=(); [ "$anchors" != - ] && extra=(--extra-root "$P/$anchors")
  local out line ok=1
  out=$("$BIN" verify "$P/$chain" "$host" "$date" "${extra[@]}" 2>&1)
  line=$(echo "$out" | grep -E '^verify: ' | sed -n 1p)
  if [ "$expect" = ACCEPT ]; then [[ "$line" == "verify: ACCEPT" ]] || ok=0
  else
    [[ "$line" == "verify: REFUSE"* ]] || ok=0
    for f in ${expect//,/ }; do [[ "$line" == *"$f"* ]] || ok=0; done
  fi
  local hs="-"
  if [ -n "$hskey" ] && [ $ok = 1 ]; then
    for v in tls1_3 tls1_2; do
      # the leaf + the rest of the served chain, as Cloudflare sends it
      awk 'BEGIN{n=0} /BEGIN CERT/{n++} n>1' "$P/$chain" >"$P/$chain.rest"
      awk 'BEGIN{n=0} /BEGIN CERT/{n++} n==1' "$P/$chain" >"$P/$chain.leaf"
      local port spid up=0 try
      for try in 1 2 3 4; do   # (a random free port; a busy or slow one is replaced)
        port=$(( 20000 + RANDOM % 20000 ))
        openssl s_server -accept "127.0.0.1:$port" -quiet -$v -cert "$P/$chain.leaf" -key "$P/$hskey.key" \
          -cert_chain "$P/$chain.rest" -www >"$P/srv.$name.$v.log" 2>&1 &
        spid=$!
        # listening yet? (a bare TCP probe: the server drops it and keeps serving; it is killed after the one real client)
        for _ in $(seq 1 100); do kill -0 $spid 2>/dev/null || break; (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && { up=1; break; }; sleep 0.1; done
        [ $up = 1 ] && break
        kill $spid 2>/dev/null; wait $spid 2>/dev/null
      done
      local hout hline
      hout=$("$BIN" handshake "$port" "$host" "$date" --${v/_/} "${extra[@]}" 2>&1)
      kill $spid 2>/dev/null; wait $spid 2>/dev/null
      hline=$(echo "$hout" | grep -E '^handshake: (ACCEPT|REFUSE|ERROR)' | sed -n 1p)
      # the handshake must reach the same verdict and the same library flags as the offline verifier
      local vflags=${line#verify: }; local hflags=${hline#handshake: }
      if [ "$vflags" != "$hflags" ]; then ok=0; echo "  handshake $v differs: [$hline] vs [$line]"; echo "$hout" | sed 's/^/    /' | head -8; fi
      hs="${hs/-/}${hs:+ }${v/tls1_/1.}"
    done
    hs="same in TLS ${hs# }"
  fi
  if [ $ok = 1 ]; then PASS=$((PASS+1)); st=PASS; else FAIL=$((FAIL+1)); st=FAIL; echo "$out" | sed 's/^/    /'; fi
  printf '%-4s %-12s %-62s -> %s  [handshake: %s]\n' "$st" "$name" "$(echo "$chain $host $date anchors=$anchors" | cut -c1-62)" "${line#verify: }" "$hs" | tee -a "$TABLE"
}

echo "== synthetic chains (expected verdicts asserted; handshake = a real TLS handshake by the game's code on 127.0.0.1)"
check a          a.pem         $H $NOW anchor_x2.pem    ACCEPT leaf
check a-no-x1    a_nox1.pem    $H $NOW anchor_x2.pem    ACCEPT leaf
check a-via-x1   a.pem         $H $NOW anchor_x1.pem    ACCEPT
check b          b.pem         $H $NOW anchor_x2.pem    NOT_TRUSTED leaf
check b2         b2.pem        $H $NOW anchor_x1x2.pem  NOT_TRUSTED
check c          c.pem         $H $NOW anchor_x1.pem    ACCEPT leafr
check d          d.pem         $H $NOW anchor_x2.pem    ACCEPT leaf
check d-pathlen  d_p0.pem      $H $NOW anchor_x2.pem    NOT_TRUSTED
check depth-6    deep5.pem     $H $NOW anchor_x2.pem    ACCEPT
check depth-7    deep6.pem     $H $NOW anchor_x2.pem    "flags=0xffffffff,client=OTHER" leaf
check e-host     a.pem         wrong.example.org $NOW anchor_x2.pem CN_MISMATCH leaf
check e-2level   a.pem         a.smg-online.cboc-vita.workers.dev $NOW anchor_x2.pem CN_MISMATCH
check e-apex     a.pem         cboc-vita.workers.dev $NOW anchor_x2.pem ACCEPT
check e-nowild   e_nowild.pem  $H $NOW anchor_x2.pem    CN_MISMATCH
check e-expired  e_exp.pem     $H $NOW anchor_x2.pem    EXPIRED leaf
check e-future   e_fut.pem     $H $NOW anchor_x2.pem    FUTURE leaf
check e-clock2011 a.pem        $H 2011-01-01 anchor_x2.pem FUTURE
check e-clock2041 a.pem        $H 2041-06-01 anchor_x2.pem EXPIRED
check e-impostor e_evil.pem    $H $NOW -                NOT_TRUSTED
check e-impostor2 e_evil.pem   $H $NOW anchor_x2.pem    NOT_TRUSTED
# SHA-1: this build has no SHA-1 at all, so the handshake SKIPS a certificate signed with it (ssl_parse_certificate_chain); the path then
# starts at the issuing CA, which is not for the host name: refused, CN_MISMATCH (the skip is logged)
check e-sha1     e_sha1.pem    $H $NOW anchor_x2.pem    CN_MISMATCH
check e-rsa1024  e_rsa1024.pem $H $NOW anchor_x1.pem    BAD_KEY
check e-eku      e_client.pem  $H $NOW anchor_x2.pem    EXT_KEY_USAGE leaf
check prod-only  a.pem         $H $NOW -                NOT_TRUSTED
echo "== $PASS passed, $FAIL failed (table: $TABLE)"
exit $FAIL
