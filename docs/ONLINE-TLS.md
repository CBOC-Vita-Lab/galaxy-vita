# HTTPS client: trust, verification, failures, tests (Galaxy 3.0)

The online service client of Galaxy 3.0 (`net/src/np_https*`, vendored from Strikers Charged 1.3; see `net/GALAXY-CHANGES.txt`) talks
to the Cloudflare Worker `https://galaxy.psvita.dev` (a Worker custom domain; until the release the old `smg-online.cboc-vita.workers.dev` address answers too, but no build falls back to it) (account, friends, rankings, signaling). This page records what it
trusts and why, how a certificate is checked, how failures reach the player, and how to test and regenerate it. Adapted from the
Strikers page of the same name (same owner).

## What it is

- Our own TLS: Mbed TLS 3.6.8 (`net/deps/lock.json`, built by `tools/mp/build-mbedtls.py --work /tls` with the client-only profile
  `net/deps/mbedtls/np_user_config.h`) over `sceNet` sockets. Not `sceHttp`/`sceSsl`: the roots are ours, not the Vita's.
- TLS 1.2 and 1.3 (`np_https_tls_mbedtls.cpp`, `mbedtls_ssl_conf_min/max_tls_version`), SNI, HTTP/1.1 GET/POST, `Content-Length`,
  chunked and close-delimited answers, optional keep-alive (10 s), `Accept-Encoding: identity` (a compressed answer is `PROTOCOL`),
  redirects never followed (`REDIRECT`).
- No ALPN (`MBEDTLS_SSL_ALPN` off: the server falls back to HTTP/1.1), no session tickets (`MBEDTLS_SSL_SESSION_TICKETS` off: a TLS 1.3
  NewSessionTicket is skipped by the library), no renegotiation, no PSK, no early data.
- TLS 1.3 suites: AES-128-GCM-SHA256, AES-256-GCM-SHA384, CHACHA20-POLY1305-SHA256; TLS 1.2: ECDHE-ECDSA / ECDHE-RSA with AES-GCM or
  ChaCha20-Poly1305. Key exchange groups: x25519, P-256, P-384 (no P-521, no Brainpool, no FFDHE, no post-quantum hybrid).

## How a certificate is checked (the verification path)

1. `tls::Init` (`np_https_tls_mbedtls.cpp`) loads the 8 roots of `np_https_roots.h` once, each checked against its embedded SHA-256
   first; any mismatch or parse failure, or a wrong count, fails closed (`UNAVAILABLE`, nothing sent). Authmode is
   `MBEDTLS_SSL_VERIFY_REQUIRED`; there is no switch to turn it off. The certificate profile is `kCertProfile` =
   `mbedtls_x509_crt_profile_default`: signature hashes SHA-256/384/512 (this build has no SHA-1 or MD5 code at all), RSA keys of at least
   2048 bits, EC keys on P-256 / P-384.
2. Before any network use, `RunRequest` (`np_https.cpp`) compares the console clock with the build floor: `NP_HTTPS_ROOTS_DAY`
   (2026-10-30) or the day `np_https.cpp` was compiled, whichever is later, minus one day. Earlier (a reset Vita shows 2011) = `CLOCK`.
3. `tls::Open` calls `mbedtls_ssl_set_hostname(host)`: the SNI and the name the certificate must carry.
4. In the handshake Mbed TLS parses the served certificates in order (one it cannot parse because its signature algorithm is not built,
   e.g. SHA-1, is skipped; any other parse error ends the handshake), then `mbedtls_x509_crt_verify_restartable(chain, our roots, no CRL,
   profile, host)`:
   - **Path building**: for each certificate the issuer is looked for among the trusted roots FIRST, then among the served certificates.
     So a served chain `leaf <- YE1 <- Root YE (cross-signed by ISRG Root X2) <- ISRG Root X2 (cross-signed by X1)` ends at our own
     self-signed ISRG Root X2 after three certificates; the served X2-by-X1 cross-sign is not needed and not used (synthetic cases a,
     a-no-x1). Without the Root YE cross-sign in the served chain, the path stops at YE1: `NOT_TRUSTED` (case b).
   - **Depth**: `MBEDTLS_X509_MAX_INTERMEDIATE_CA 6` (our profile): at most 6 intermediates between the leaf and a trusted root (the
     cross-signed root counts as one). A 7th makes the library give up (`MBEDTLS_ERR_X509_FATAL_ERROR`, every flag set). The recorded chain
     uses 2. `pathLenConstraint` is enforced (YE1 has pathlen 0 at Let's Encrypt: nothing may hang below it).
   - **Host name**: if the leaf has subjectAltName DNS entries only they count (the CN is ignored); a `*.` wildcard matches exactly one
     label. A leaf with `DNS:*.cboc-vita.workers.dev` accepts `smg-online.cboc-vita.workers.dev` but not `a.smg-online...`; a leaf with
     only `DNS:cboc-vita.workers.dev` (the CN shown in the recorded chain) would be refused for our host. The recorded chain shows
     subjects only: its SAN is not known here (check it on the real PEM).
   - **Dates** of every certificate in the path against `np_https_wallclock` = the Vita's RTC (UTC).
   - **Revocation**: none. No CRL (`MBEDTLS_X509_CRL_PARSE_C` off), no OCSP, no stapling. Short-lived leaves are the mitigation.
5. Then the handshake's secondary checks: the leaf's keyUsage must allow digitalSignature and its extKeyUsage serverAuth (TLS 1.2 also
   checks the leaf's EC curve against our groups).

## What the player sees

| Cause | np_https | service client | message window |
| --- | --- | --- | --- |
| no Wi-Fi | `OFFLINE` | `MP_ERR_NO_WIFI` | "Couldn't connect to Wi-Fi." |
| clock before the build | `CLOCK` | `MP_ERR_SERVICE_DOWN` | "The online service isn't available right now..." |
| any certificate refusal | `CERT` (+ flags, chain in the log) | `MP_ERR_SERVICE_DOWN` | same |
| DNS / TCP connect failure | `DNS` / `CONNECT` | service calls: `SERVICE_DOWN`; signaling: text contains "Wi-Fi": `NO_WIFI` | |
| HTTP 429 (incl. Cloudflare 1015) | `HTTP_STATUS` | `MP_ERR_RATE_LIMITED` | "Too many tries in a short time." |
| HTTP 5xx | `HTTP_STATUS` | `MP_ERR_SERVICE_DOWN` | |

So "certificate problem" and "wrong clock" are NOT distinguishable from "service down" in the menus today; only the log
(`ux0:data/smg/log.txt`, `[net-https] certificate for ...: REFUSED (flags ...)` and one line per chain certificate) tells them apart.
Until tls (Nov 11 2026), key-usage refusals and a too-long chain read as `TLS_HANDSHAKE`; they are `CERT` now.

## The root set

`tools/net/gen_https_roots.py` writes `net/src/np_https_roots.h` (DER + SHA-256 per root, about 11 KB).

| Root | Valid until | SHA-256 (from the header) | Who may serve it |
| --- | --- | --- | --- |
| GTS Root R1 | 2036-06-22 | d947432a...57a7f4cf | Google Trust Services, WR1-WR4 |
| GTS Root R4 | 2036-06-22 | 349dfa40...031b3c7d | Google Trust Services, WE1-WE4 |
| ISRG Root X1 | 2035-06-04 | 96bcec06...bddf08c6 | Let's Encrypt RSA (and cross-signs) |
| ISRG Root X2 | 2040-09-17 | 69729b8e...3ccb1470 | Let's Encrypt ECDSA; also the issuer of the "Root YE" cross-sign served today |
| SSL.com TLS ECC Root CA 2022 | 2046-08-19 | c32ffd9f...cc1ba143 | Cloudflare TLS Issuing ECC CA |
| SSL.com TLS RSA Root CA 2022 | 2046-08-19 | 8faf7d2e...bc17f2ed | Cloudflare TLS Issuing RSA CA |
| SSL.com Root Certification Authority ECC | 2041-02-12 | 3417bb06...bdcebc65 | SSL.com Transit ECC CA R2 |
| SSL.com Root Certification Authority RSA | 2041-02-12 | 85666a56...20123b69 | SSL.com Transit RSA CA R2 |

Checked 14 Nov 2026 for the custom domain `galaxy.psvita.dev`: Cloudflare's CA page (developers.cloudflare.com/ssl/reference/certificate-authorities/, updated Apr 16 2026) lists Let's Encrypt, Google Trust Services and SSL.com for Universal certificates (Sectigo for backup certificates only), so the set below covers every Universal SSL chain; the only open item is Let's Encrypt's new roots (below), which workers.dev has too.

Why these: Cloudflare's edge certificates come from Google Trust Services, Let's Encrypt and SSL.com (Sectigo for backups only); see the
Strikers page for the research. Known gaps: a Sectigo backup chain; an SSL.com Transit chain served only with a Sectigo cross-sign; and
**Let's Encrypt's new roots** (below).

### Regenerating (byte for byte)

    NP_HTTPS_ROOTS_DAY=2026-10-30 python3 -I tools/net/gen_https_roots.py /etc/ssl/certs > net/src/np_https_roots.h

reproduces the current header exactly with Ubuntu 24.04's `ca-certificates 20260816ubuntu0.24.04.1` (checked Nov 11 2026: `cmp` equal).
Needed files: `GTS_Root_R1.pem GTS_Root_R4.pem ISRG_Root_X1.pem ISRG_Root_X2.pem SSL.com_TLS_ECC_Root_CA_2022.pem
SSL.com_TLS_RSA_Root_CA_2022.pem SSL.com_Root_Certification_Authority_ECC.pem SSL.com_Root_Certification_Authority_RSA.pem` (Mozilla
store names). Changing `NP_HTTPS_ROOTS_DAY` changes the clock floor: use the day you regenerate.

## Let's Encrypt's new hierarchy (recorded Nov 11 2026)

The deployed Worker served: `CN=cboc-vita.workers.dev` <- `Let's Encrypt YE1` <- `ISRG Root YE` (cross-signed by `ISRG Root X2`) <-
`ISRG Root X2` (cross-signed by `ISRG Root X1`). With the cross-signs served, our client ends the path at the bundled ISRG Root X2
(synthetic proof: `tests/mp/tls_chain_test.sh` cases a / a-no-x1 / a-via-x1, offline and with a real handshake). The day Cloudflare
stops sending the Root YE cross-sign (as Let's Encrypt drops cross-signs over time), every console fails with `CERT` / untrusted (case
b). Hence:

### Adding ISRG Root YE / YR (prepared; do NOT type certificates by hand)

1. **Get the PEMs** from Let's Encrypt's own certificates page (letsencrypt.org/certificates, the self-signed "ISRG Root YE" and "ISRG
   Root YR", not a cross-signed version) and put them in `/home/claude/g3/share/tls/` as `isrg-root-ye.pem`, `isrg-root-yr.pem`
   (the coordinator), plus `served-chain.pem` (`openssl s_client -showcerts -connect galaxy.psvita.dev:443 -servername
   galaxy.psvita.dev`, run by a person on a machine allowed to contact it).
2. **Check the SHA-256 of each DER against two independent sources**, both of which must agree with the file:
   `openssl x509 -in isrg-root-ye.pem -outform der | sha256sum`, compared with two of: (a) a fingerprint Let's Encrypt itself publishes
   for that root (its certificates page or its community announcement), (b) the Certificate Transparency entry at crt.sh (search by that
   SHA-256: the subject must be `ISRG Root YE`, self-signed), (c) Mozilla's CCADB root report or the Chrome Root Store list, if the root is
   included there by then. Use sources reached independently (not one page linking to the other's file). A difference = stop.
   Also check the served `Root YE` cross-sign has the same public key as the self-signed root
   (`openssl x509 -noout -pubkey` of both, identical).
3. **Generate**, pinning the checked fingerprints (the generator refuses to write on a mismatch):

        mkdir -p /tmp/newroots && cp isrg-root-ye.pem /tmp/newroots/ISRG_Root_YE.pem && cp isrg-root-yr.pem /tmp/newroots/ISRG_Root_YR.pem
        NP_HTTPS_ROOTS_DAY=<today> python3 -I tools/net/gen_https_roots.py /etc/ssl/certs /tmp/newroots --new-isrg-roots \
            --pin ISRG_Root_YE=<sha256> --pin ISRG_Root_YR=<sha256> > net/src/np_https_roots.h

4. **Test**: `tests/mp/tls_chain_test.sh share/tls/served-chain.pem galaxy.psvita.dev <today>` must print `ACCEPT`
   for the served chain, and also for the chain WITHOUT the cross-signs (only leaf + YE1: cut the PEM after the second certificate)
   against the bundled roots alone; the 24 synthetic cases must still pass. Then the per-file Vita compile check
   (`tools/cc.sh <tree> net/src/np_https_tls_mbedtls.cpp vita`) and the console self-test below.

## Tests

- `tests/mp/tls_chain_test.sh [REAL.pem [HOST] [DATE]]` (host64 Mbed TLS at `/tls/mbedtls/host64`, openssl, ~10 s, no network beyond
  127.0.0.1): builds `tests/mp/tls_chain_test.cpp` with the client's own `np_https_tls_mbedtls.cpp` (test hooks: `tls::VerifyChain`,
  `tls::SetExtraTestRoots`), makes synthetic chains of the new hierarchy and its variants with openssl, and asserts each verdict both
  offline and through a real TLS 1.2 and 1.3 handshake plus one request by the game's code against `openssl s_server`. Synthetic anchors
  are added only through `--extra-root`; the game's anchors are `np_https_roots.h` alone.
- `tests/mp/tls_chain_test` binary alone: `verify CHAIN.pem HOST DATE` prints `ACCEPT` or `REFUSE flags=0x.. (NAMES) client=NAMES`.
- `net/tests/https_test.cpp` / `https_server.py` (from Strikers) have no Galaxy runner yet.

## Console self-test

`SMG_NET_SELFTEST=https://galaxy.psvita.dev/<path>` in `ux0:data/smg/env.txt`: one GET a few seconds after start-up; the
log shows the clock, each stage, TLS version, suite, the server certificate and, for a refusal, every certificate with its verdict.
