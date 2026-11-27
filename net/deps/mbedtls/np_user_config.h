/* np_user_config.h -- the TLS client profile of the Strikers port's HTTPS client (platform/sdk/net/np_https.*).
 *
 * Included at the end of mbedtls' stock mbedtls_config.h (the library is built, and the headers are installed,
 * with MBEDTLS_USER_CONFIG_FILE pointing here). It turns the stock configuration into a client that does
 * TLS 1.2 and 1.3 over sockets the caller owns, verifies certificates against roots it is given, and nothing else:
 * no server side, no DTLS, no session tickets or caches, no PSK, no static key exchange, no file system, no
 * threading, no library networking or timing (the game talks through sceNet, the library never touches a socket).
 *
 * Two things come from the application, not the library: entropy (mbedtls_hardware_poll: sceKernelGetRandomNumber on
 * the console, getrandom on the host) and the wall clock for certificate dates (np_https_wallclock: the Vita's RTC,
 * overridable in the host tests). Neither needs a patch to mbedtls, which is why this build does not apply the vitasdk
 * package's patch (it only repairs net_sockets, timing and the platform entropy source, none of which is built here).
 *
 * Cipher suites left in (TLS 1.2): ECDHE-ECDSA / ECDHE-RSA with AES-128/256-GCM and ChaCha20-Poly1305, SHA-256/384.
 * TLS 1.3: TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256 with ECDHE (x25519,
 * secp256r1, secp384r1). Signatures: ECDSA P-256/P-384, RSA PKCS#1 v1.5 and PSS. */
#ifndef NP_MBEDTLS_USER_CONFIG_H
#define NP_MBEDTLS_USER_CONFIG_H

/* ---- application-provided pieces ---------------------------------------------------------------------------------- */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT          /* np_https provides mbedtls_ms_time() (monotonic) */
#define MBEDTLS_PLATFORM_TIME_MACRO           np_https_wallclock
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO      long long
#ifdef __cplusplus
extern "C" {
#endif
long long np_https_wallclock(long long *t);   /* defined in np_https_sys.cpp */
#ifdef __cplusplus
}
#endif
#define MBEDTLS_PLATFORM_GMTIME_R_ALT         /* np_https provides mbedtls_platform_gmtime_r() (no libc time zone code) */

/* ---- drop what a client does not use ------------------------------------------------------------------------------- */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_VERSION_FEATURES
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_THREADING_PTHREAD
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_SESSION_TICKETS
#undef MBEDTLS_SSL_PROTO_TLS1_2_DEPRECATED
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
#undef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_SSL_ALPN
#undef MBEDTLS_SSL_EXPORT_KEYS
#undef MBEDTLS_SSL_DTLS_BADMAC_LIMIT
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED
#undef MBEDTLS_ECJPAKE_C
#undef MBEDTLS_DHM_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_ECDSA_DETERMINISTIC
#undef MBEDTLS_HMAC_DRBG_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_LMS_PRIVATE
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_MD5_C
#undef MBEDTLS_SHA1_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_SHA224_C
#undef MBEDTLS_SHA512_NO_SHA384
#undef MBEDTLS_ERROR_C
#undef MBEDTLS_ERROR_STRERROR_DUMMY
#undef MBEDTLS_PSA_CRYPTO_CLIENT
#undef MBEDTLS_PSA_CRYPTO_SE_C
#undef MBEDTLS_PSA_INJECT_ENTROPY
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#undef MBEDTLS_USE_PSA_CRYPTO
#undef MBEDTLS_MEMORY_BUFFER_ALLOC_C
#undef MBEDTLS_CHECK_RETURN_WARNING
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

/* ---- keep, explicitly (the stock file enables these; a stock change must not silently drop them) ---------------- */
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_PSA_CRYPTO_C

/* ---- size / speed ------------------------------------------------------------------------------------------------- */
/* The server may send full 16 KiB records; we send small requests. */
#define MBEDTLS_SSL_IN_CONTENT_LEN              16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN             4096
#define MBEDTLS_MPI_WINDOW_SIZE                 2
#define MBEDTLS_ECP_WINDOW_SIZE                 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM           0
#define MBEDTLS_X509_MAX_INTERMEDIATE_CA        6

#endif /* NP_MBEDTLS_USER_CONFIG_H */
