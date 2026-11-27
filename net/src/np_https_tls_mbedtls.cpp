// np_https_tls_mbedtls.cpp: the mbedtls backend of np_https (interface: np_https_tls.h). Build profile: dependencies/mbedtls/np_user_config.h.
#include "np_https_tls.h"

#if defined(NP_HAVE_MBEDTLS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"   // (only its error codes: the library's sockets are not used)
#include "mbedtls/base64.h"
#include "mbedtls/oid.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "np_https.h"
#include "np_https_roots.h"
#include "np_https_sys.h"

extern "C" void port_host_heap_log(const char* when) __attribute__((weak));   // memfix: the port layer's C heap line (absent in host tests)

namespace nph {
namespace tls {

namespace {

struct Global {
    int state;   // 0 untried, 1 ready, -1 failed
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    int roots;
    char why[80];
};
Global g;
char* g_testPem;
size_t g_testLen;
char* g_extraPem;   // test hooks only: anchors added AFTER the bundled roots (tls_chain_test's synthetic roots); never set in the game
size_t g_extraLen;

// The certificate profile of every handshake (Init) and of VerifyChain: the library's default, named here so both use the same object.
// Signature hashes SHA-256/384/512 (SHA-1 and MD5 are refused: BADCERT_BAD_MD; this build has no SHA-1 code at all), RSA keys of 2048
// bits or more (BADCERT_BAD_KEY), EC keys on P-256 / P-384 (the only curves this build has; the profile would also allow P-521 and the
// Brainpool curves, which do not parse here).
const mbedtls_x509_crt_profile& kCertProfile = mbedtls_x509_crt_profile_default;

bool Failed(const char* fmt, int a) {
    snprintf(g.why, sizeof g.why, fmt, a);
    Log("tls init failed: %s", g.why);
    return false;
}

}  // namespace

struct Conn {
    mbedtls_ssl_context ssl;
    Io io;
    Details details;
    uint32_t verify_flags;
    bool handshaken;
};

namespace {

int BioSend(void* c, const unsigned char* buf, size_t len) {
    Conn* conn = (Conn*)c;
    const int n = conn->io.send(conn->io.ctx, buf, len);
    if (n > 0) return n;
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}
int BioRecv(void* c, unsigned char* buf, size_t len) {
    Conn* conn = (Conn*)c;
    const int n = conn->io.recv(conn->io.ctx, buf, len);
    if (n > 0) return n;
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == -2) return 0;   // the peer closed
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

void TimeText(const mbedtls_x509_time& t, char* out, size_t cap) {
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02dZ", t.year, t.mon, t.day, t.hour, t.min, t.sec);
}

// Called once per certificate of the chain (leaf = depth 0 ... the root last) with the library's verdict; records it for the log and
// never changes the verdict.
int VerifyHook(void* c, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    Conn* conn = (Conn*)c;
    if (depth < 0 || depth >= MAX_CHAIN) return 0;
    CertInfo& ci = conn->details.chain[depth];
    memset(&ci, 0, sizeof ci);
    if (mbedtls_x509_dn_gets(ci.subject, sizeof ci.subject, &crt->subject) < 0) strcpy(ci.subject, "?");
    if (mbedtls_x509_dn_gets(ci.issuer, sizeof ci.issuer, &crt->issuer) < 0) strcpy(ci.issuer, "?");
    TimeText(crt->valid_from, ci.not_before, sizeof ci.not_before);
    TimeText(crt->valid_to, ci.not_after, sizeof ci.not_after);
    ci.raw_flags = *flags;
    if (depth + 1 > conn->details.chain_len) conn->details.chain_len = depth + 1;
    return 0;
}

Result MapError(int rc, int* code) {
    *code = rc;
    switch (rc) {
    case MBEDTLS_ERR_SSL_WANT_READ: return T_WANT_READ;
    case MBEDTLS_ERR_SSL_WANT_WRITE: return T_WANT_WRITE;
    case MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET: return T_WANT_READ;   // (tickets are ignored; read on)
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY: return T_CLOSED;
    case MBEDTLS_ERR_SSL_CONN_EOF: return T_EOF;
    case MBEDTLS_ERR_X509_CERT_VERIFY_FAILED: return T_CERT;
    // The handshake's secondary checks (the server certificate's keyUsage / extKeyUsage, a TLS 1.2 EC key on a curve we do not have) and
    // an unparsable certificate message end with this code, not the one above: they are certificate refusals too (CertFlags says which;
    // KEY_USAGE / EXT_KEY_USAGE map to NP_HTTPS_CERT_OTHER). Before tls (Nov 11) they read as TLS_HANDSHAKE.
    case MBEDTLS_ERR_SSL_BAD_CERTIFICATE: return T_CERT;
    // A chain longer than MBEDTLS_X509_MAX_INTERMEDIATE_CA intermediates stops the library's path search with this code and every flag
    // set (verify_result = 0xFFFFFFFF): a refused certificate as well (CertFlags reports it as NP_HTTPS_CERT_OTHER only).
    case MBEDTLS_ERR_X509_FATAL_ERROR: return T_CERT;
    case MBEDTLS_ERR_SSL_ALLOC_FAILED: return T_MEMORY;
    default: return T_FAIL;
    }
}

}  // namespace

const char* BackendName() { return "mbedtls " MBEDTLS_VERSION_STRING; }
int State() { return g.state; }

void Reset() {
    if (g.state != 0) {
        mbedtls_ssl_config_free(&g.conf);
        mbedtls_ctr_drbg_free(&g.drbg);
        mbedtls_entropy_free(&g.entropy);
        mbedtls_x509_crt_free(&g.ca);
    }
    memset(&g, 0, sizeof g);
}

bool Init(char* why, size_t why_cap, int* roots_loaded) {
    if (g.state == 0) {
        mbedtls_x509_crt_init(&g.ca);
        mbedtls_entropy_init(&g.entropy);
        mbedtls_ctr_drbg_init(&g.drbg);
        mbedtls_ssl_config_init(&g.conf);
        g.state = -1;
        [&]() {
            int rc;
            if (g_testPem) {
                rc = mbedtls_x509_crt_parse(&g.ca, (const unsigned char*)g_testPem, g_testLen);
                if (rc != 0) { Failed("test roots did not parse (%d)", rc); return; }
            } else {
                for (int i = 0; i < NP_HTTPS_ROOT_COUNT; i++) {
                    const NpHttpsRootEntry& r = np_https_roots[i];
                    unsigned char sum[32];
                    if (mbedtls_sha256(r.der, r.len, sum, 0) != 0 || memcmp(sum, r.sha256, 32) != 0) { Failed("root %d does not match its fingerprint", i); return; }
                    rc = mbedtls_x509_crt_parse_der(&g.ca, r.der, r.len);
                    if (rc != 0) { Failed("root %d did not parse", i); return; }
                }
            }
            int n = 0;
            for (const mbedtls_x509_crt* c = &g.ca; c && c->raw.p; c = c->next) n++;
            if (n == 0 || (!g_testPem && n != NP_HTTPS_ROOT_COUNT)) { Failed("%d roots loaded", n); return; }
            if (g_extraPem) {   // (test hooks only)
                rc = mbedtls_x509_crt_parse(&g.ca, (const unsigned char*)g_extraPem, g_extraLen);
                if (rc != 0) { Failed("extra test roots did not parse (%d)", rc); return; }
                n = 0;
                for (const mbedtls_x509_crt* c = &g.ca; c && c->raw.p; c = c->next) n++;
            }
            g.roots = n;
            if (psa_crypto_init() != PSA_SUCCESS) { Failed("psa_crypto_init failed (%d)", 0); return; }
            static const char kPers[] = "np_https";
            rc = mbedtls_ctr_drbg_seed(&g.drbg, mbedtls_entropy_func, &g.entropy, (const unsigned char*)kPers, sizeof kPers - 1);
            if (rc != 0) { Failed("random source failed (%d)", rc); return; }
            rc = mbedtls_ssl_config_defaults(&g.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            if (rc != 0) { Failed("ssl config failed (%d)", rc); return; }
            mbedtls_ssl_conf_authmode(&g.conf, MBEDTLS_SSL_VERIFY_REQUIRED);   // always. There is no other setting in this file.
            mbedtls_ssl_conf_cert_profile(&g.conf, &kCertProfile);             // (the default one, named: VerifyChain uses the same)
            mbedtls_ssl_conf_ca_chain(&g.conf, &g.ca, nullptr);
            mbedtls_ssl_conf_rng(&g.conf, mbedtls_ctr_drbg_random, &g.drbg);
            mbedtls_ssl_conf_min_tls_version(&g.conf, MBEDTLS_SSL_VERSION_TLS1_2);
            mbedtls_ssl_conf_max_tls_version(&g.conf, MBEDTLS_SSL_VERSION_TLS1_3);
            g.state = 1;
            Log("tls ready: %s, %d roots", BackendName(), n);
        }();
    }
    if (why && why_cap) snprintf(why, why_cap, "%s", g.why);
    if (roots_loaded) *roots_loaded = g.roots;
    return g.state > 0;
}

Conn* Open(const char* host, const Io& io) {
    Conn* c = (Conn*)calloc(1, sizeof *c);
    if (!c) return nullptr;
    c->io = io;
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &g.conf) != 0 || mbedtls_ssl_set_hostname(&c->ssl, host) != 0) {   // SNI and the name the certificate must carry
        mbedtls_ssl_free(&c->ssl);
        free(c);
        return nullptr;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, BioSend, BioRecv, nullptr);
    mbedtls_ssl_set_verify(&c->ssl, VerifyHook, c);
    return c;
}

Result Handshake(Conn* c, int* code) {
    *code = 0;
    const int rc = mbedtls_ssl_handshake(&c->ssl);
    if (rc == 0) {
        {   // memfix: the C heap once, after the first TLS handshake of the process (the buffers of a live connection are in it)
            static bool noted = false;
            if (!noted) {
                noted = true;
                if (port_host_heap_log) port_host_heap_log("mp first tls");
            }
        }
        c->handshaken = true;
        c->details.version = mbedtls_ssl_get_version_number(&c->ssl) == MBEDTLS_SSL_VERSION_TLS1_3 ? 0x0304u : 0x0303u;
        const char* cs = mbedtls_ssl_get_ciphersuite(&c->ssl);
        snprintf(c->details.cipher, sizeof c->details.cipher, "%s", cs ? cs : "?");
        return T_OK;
    }
    return MapError(rc, code);
}

Result Read(Conn* c, uint8_t* buf, size_t cap, size_t* got, int* code) {
    *code = 0;
    *got = 0;
    const int rc = mbedtls_ssl_read(&c->ssl, buf, cap);
    if (rc > 0) { *got = (size_t)rc; return T_OK; }
    if (rc == 0) return T_EOF;
    return MapError(rc, code);
}

Result Write(Conn* c, const uint8_t* buf, size_t len, size_t* done, int* code) {
    *code = 0;
    *done = 0;
    const int rc = mbedtls_ssl_write(&c->ssl, buf, len);
    if (rc > 0) { *done = (size_t)rc; return T_OK; }
    return MapError(rc, code);
}

void CloseNotify(Conn* c) { mbedtls_ssl_close_notify(&c->ssl); }
size_t Pending(Conn* c) { return mbedtls_ssl_get_bytes_avail(&c->ssl); }

namespace {
uint32_t MapCertFlags(uint32_t f) {
    if (f == 0xFFFFFFFFu) return NP_HTTPS_CERT_OTHER;   // the library gave up (chain too long): not "every reason at once"
    uint32_t out = 0;
    if (f & MBEDTLS_X509_BADCERT_EXPIRED) out |= NP_HTTPS_CERT_EXPIRED;
    if (f & MBEDTLS_X509_BADCERT_FUTURE) out |= NP_HTTPS_CERT_NOT_YET_VALID;
    if (f & MBEDTLS_X509_BADCERT_NOT_TRUSTED) out |= NP_HTTPS_CERT_UNTRUSTED;
    if (f & MBEDTLS_X509_BADCERT_CN_MISMATCH) out |= NP_HTTPS_CERT_HOST_MISMATCH;
    if (f & (MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK | MBEDTLS_X509_BADCERT_BAD_KEY)) out |= NP_HTTPS_CERT_BAD_SIGNATURE;
    if (f & ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH |
                        MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK | MBEDTLS_X509_BADCERT_BAD_KEY))
        out |= NP_HTTPS_CERT_OTHER;
    return out;
}
}  // namespace

uint32_t CertFlags(Conn* c, uint32_t* raw) {
    const uint32_t f = mbedtls_ssl_get_verify_result(&c->ssl);
    if (raw) *raw = f;
    return MapCertFlags(f);
}

void GetDetails(Conn* c, Details* out) { *out = c->details; }

void Free(Conn* c) {
    if (!c) return;
    mbedtls_ssl_free(&c->ssl);
    free(c);
}

#ifdef NP_HTTPS_TEST_HOOKS
void SetExtraTestRoots(const char* pem, size_t len) {
    free(g_extraPem);
    g_extraPem = nullptr;
    g_extraLen = 0;
    if (pem) {
        g_extraPem = (char*)malloc(len);
        if (g_extraPem) {   // lanfix: (test hook) a failed allocation leaves no extra roots
            memcpy(g_extraPem, pem, len);
            g_extraLen = len;
        }
    }
}

Result VerifyChain(const char* pem, size_t len, const char* host, uint32_t* raw, Details* out) {
    *raw = 0;
    memset(out, 0, sizeof *out);
    if (g.state != 1) return T_FAIL;
    // The served chain, in the order the server sends it, parsed one DER at a time as the handshake parses its Certificate message
    // (ssl_tls.c ssl_parse_certificate_chain / ssl_tls13_generic.c): a certificate whose signature algorithm this build does not have
    // (SHA-1, MD5 ...: UNKNOWN_SIG_ALG + OID_NOT_FOUND) is skipped, any other parse error ends the handshake.
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    static const char kBegin[] = "-----BEGIN CERTIFICATE-----", kEnd[] = "-----END CERTIFICATE-----";
    const char* at = pem;
    const char* const stop = pem + (len && pem[len - 1] == 0 ? len - 1 : len);
    int prc = 0;
    for (;;) {
        const char* b = strstr(at, kBegin);
        if (!b || b >= stop) break;
        b += sizeof kBegin - 1;
        const char* e = strstr(b, kEnd);
        if (!e || e >= stop) { prc = -1; break; }
        static unsigned char b64[16384], der[12288];
        size_t n = 0, dn = 0;
        for (const char* q = b; q < e && n < sizeof b64; q++)
            if (*q != '\r' && *q != '\n' && *q != ' ' && *q != '\t') b64[n++] = (unsigned char)*q;
        if (n >= sizeof b64 || mbedtls_base64_decode(der, sizeof der, &dn, b64, n) != 0) { prc = -1; break; }
        const int r = mbedtls_x509_crt_parse_der(&chain, der, dn);
        if (r != 0 && r != MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG + MBEDTLS_ERR_OID_NOT_FOUND) { prc = r; break; }
        if (r != 0) Log("verify: a served certificate with a signature algorithm this build does not have was skipped (as the handshake does)");
        at = e + sizeof kEnd - 1;
    }
    if (prc != 0 || !chain.raw.p) {   // (a handshake would end with the parse error: TLS_HANDSHAKE)
        mbedtls_x509_crt_free(&chain);
        return T_FAIL;
    }
    Conn c;
    memset(&c, 0, sizeof c);
    // Primary check: the call mbedtls_ssl_verify_certificate makes (ssl_tls.c), with what Init / Open give it: our roots, no CRL, the
    // same profile, the host name of mbedtls_ssl_set_hostname, the same per-certificate hook. Dates come from np_https_wallclock.
    uint32_t flags = 0;
    const int vrc = mbedtls_x509_crt_verify_with_profile(&chain, &g.ca, nullptr, &kCertProfile, host, &flags, VerifyHook, &c);
    // Secondary checks of the same function, for the suites this profile has (all ECDHE, TLS 1.2 and 1.3): the server certificate must
    // allow digitalSignature (keyUsage) and serverAuth (extKeyUsage). (Its TLS 1.2-only EC curve check needs a connection's group list;
    // with only P-256 / P-384 parsable here it cannot fail for a chain that parsed.)
    if (mbedtls_x509_crt_check_key_usage(&chain, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) flags |= MBEDTLS_X509_BADCERT_KEY_USAGE;
    if (mbedtls_x509_crt_check_extended_key_usage(&chain, MBEDTLS_OID_SERVER_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) != 0)
        flags |= MBEDTLS_X509_BADCERT_EXT_KEY_USAGE;
    mbedtls_x509_crt_free(&chain);
    *out = c.details;
    *raw = flags;
    if (vrc != 0 && vrc != MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) return MapError(vrc, &prc) == T_CERT ? T_CERT : T_FAIL;   // (as Handshake)
    return flags ? T_CERT : T_OK;
}

uint32_t MapFlags(uint32_t f) { return MapCertFlags(f); }

void SetTestRoots(const char* pem, size_t len) {
    free(g_testPem);
    g_testPem = nullptr;
    if (pem) {
        g_testPem = (char*)malloc(len);
        if (g_testPem) {   // lanfix: (test hook) a failed allocation leaves no test roots
            memcpy(g_testPem, pem, len);
            g_testLen = len;
        }
    }
}
int RootCount() { return g.roots; }
#endif

}  // namespace tls
}  // namespace nph

#endif  // NP_HAVE_MBEDTLS
