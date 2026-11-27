// np_https_tls.h: the TLS backend interface of np_https. np_https.cpp (requests, HTTP, time limits) knows nothing of the TLS library;
// everything it needs from one is below. Today there is one backend, np_https_tls_mbedtls.cpp (NP_HAVE_MBEDTLS). mbedtls is unproven on a
// Vita, and OpenSSL 1.0.2 from VitaSDK is known to work there (Monster Hunter Tri's port), so a second backend with these same
// functions (np_https_tls_openssl.cpp) can replace it without touching anything else. The rules every backend must keep:
//  - certificate verification is always on, against the roots in np_https_roots.h (or the test roots), and a failed verification is T_CERT;
//  - if Init() cannot load every root or seed the random generator it returns false and the client sends nothing (fail closed);
//  - the library never touches a socket or a clock of its own: bytes move through Io, certificate dates are checked against nph::WallSeconds();
//  - nothing blocks: Handshake / Read / Write return T_WANT_* and np_https waits for the socket.
#ifndef NP_HTTPS_TLS_H
#define NP_HTTPS_TLS_H

#include <stddef.h>
#include <stdint.h>

#if defined(NP_HAVE_MBEDTLS)
#define NP_HTTPS_HAVE_TLS 1
#endif

namespace nph {
namespace tls {

enum Result {
    T_OK = 0,
    T_WANT_READ,
    T_WANT_WRITE,
    T_CLOSED,   // the peer sent close_notify (a clean end)
    T_EOF,      // the transport closed without close_notify
    T_CERT,     // certificate verification failed (CertFlags / Details say why)
    T_FAIL,     // anything else (code = the library's own)
    T_MEMORY
};

// Moves bytes for the library. Return > 0 bytes moved, 0 = would block, -1 = error, -2 = the peer closed (recv only).
struct Io {
    int (*send)(void* ctx, const uint8_t* buf, size_t len);
    int (*recv)(void* ctx, uint8_t* buf, size_t len);
    void* ctx;
};

struct CertInfo {
    char subject[100];
    char issuer[100];
    char not_before[24];   // "2026-10-30 12:00:00Z"
    char not_after[24];
    uint32_t raw_flags;    // the library's own verdict for this certificate
};
enum { MAX_CHAIN = 6 };
struct Details {
    uint32_t version;      // 0x0303 / 0x0304 once the handshake finished
    char cipher[48];
    int chain_len;
    CertInfo chain[MAX_CHAIN];   // [0] = the server's certificate, then its issuers as sent
};

struct Conn;   // one connection's TLS state

const char* BackendName();                                          // "mbedtls 3.6.8"
// Loads the roots and the random generator, once. False = permanent failure (why = a short reason); every later call returns the same.
bool Init(char* why, size_t why_cap, int* roots_loaded);
int State();                                                        // 0 = not tried, 1 = ready, -1 = failed
void Reset();                                                       // forgets everything (tests; shutdown)
Conn* Open(const char* host, const Io& io);                         // host: SNI and the name the certificate must carry. null = no memory
Result Handshake(Conn*, int* code);
Result Read(Conn*, uint8_t* buf, size_t cap, size_t* got, int* code);
Result Write(Conn*, const uint8_t* buf, size_t len, size_t* done, int* code);
void CloseNotify(Conn*);                                            // one best-effort try
size_t Pending(Conn*);                                              // decrypted bytes already buffered
uint32_t CertFlags(Conn*, uint32_t* raw);                           // NP_HTTPS_CERT_* of the failed verification, and the library's raw flags
void GetDetails(Conn*, Details* out);
void Free(Conn*);

#ifdef NP_HTTPS_TEST_HOOKS
void SetTestRoots(const char* pem, size_t len);                     // len includes the NUL; NULL = the bundled roots
int RootCount();
// tls_chain_test (tests/mp): anchors added after the bundled roots (synthetic test roots; NULL = none). Takes effect at the next Init.
void SetExtraTestRoots(const char* pem, size_t len);
// Verifies a served chain (PEM text, leaf first, len includes the NUL) offline, as Handshake would for a server presenting it to a client
// that asked for `host`: the roots Init loaded, the same certificate profile, host-name rule, clock (WallSeconds) and the handshake's
// key-usage checks. T_OK = accepted, T_CERT = refused (*raw = the library's MBEDTLS_X509_BADCERT_* flags), T_FAIL = the PEM did not parse
// or Init has not succeeded. *out: the verified path, leaf first, with each certificate's own verdict.
Result VerifyChain(const char* pem, size_t len, const char* host, uint32_t* raw, Details* out);
uint32_t MapFlags(uint32_t raw);                                    // the library's flags -> NP_HTTPS_CERT_* (what CertFlags reports)
#endif

}  // namespace tls
}  // namespace nph

#endif
