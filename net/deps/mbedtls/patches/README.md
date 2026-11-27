# Mbed TLS patches

None. Up to 3.6.7 this folder held `0001-tls13-transform-cleanup.patch`, the one-file backport of upstream
afa648aa44c698f6f01b1dd653edc3dfbe140355 ("Fix non-PSA TLS 1.3 transform cleanup"). Mbed TLS 3.6.8
(released 2026-11-16, pinned in `net/deps/lock.json`) contains that fix upstream (changelog: "Fix resource leaks in
TLS 1.3 transform setup ...", CVE-2026-93843; the four `mbedtls_ssl_transform_free` hunks are present in 3.6.8's
`library/ssl_tls13_keys.c`), so the pinned checkout is built unmodified and `"patches": []` in the lock means
"no overlay" to `tools/mp/build-mbedtls.py`.
