// np_https: the game's HTTPS client (login, rankings, lobby, result checks). Not used by the game yet.
//
// One request is a short life on a worker thread of its own: np_https_start() queues it and returns at once, the game
// thread asks np_https_poll() once a frame, np_https_cancel() ends it early, np_https_release() frees its answer. Nothing
// here ever blocks the caller (the calls take a spin lock for a few instructions).
//
//   NpHttpsRequest rq = {}; rq.url = "https://example.workers.dev/v1/login"; rq.body = json; rq.body_len = n;
//   NpHttpsError err; NpHttpsHandle h = np_https_start(&rq, &err);   // h == 0: err says why (BAD_REQUEST, BUSY)
//   ... each frame:  NpHttpsResult r; if (np_https_poll(h, &r) == 1) { use r; np_https_release(h); }
//
// What it does, and refuses to do:
//  - TLS 1.2 or 1.3 (mbedtls), SNI, certificate verification ALWAYS on, against the roots compiled in (np_https_roots.h).
//    There is no switch that turns verification off. If the roots do not load, every request fails with UNAVAILABLE.
//  - A console clock earlier than this build is reported as CLOCK ("set the date and time"), not as a certificate fault.
//  - DNS (sceNetResolver / getaddrinfo), TCP connect, the handshake, the request and the answer each have a time limit; a
//    total limit covers the whole request. Redirects are never followed (REDIRECT). Compressed answers are refused.
//  - HTTP/1.1 GET, POST, PUT and DELETE (DELETE without a body); the answer is framed by Content-Length, by chunked coding, or by the close of the connection.
//    The answer is limited (max_response, default 64 KiB, at most 1 MiB; TOO_LARGE past it). At most NP_HTTPS_SLOTS
//    requests exist at a time; memory is bounded by their limits (< 4.5 MiB in the worst case, ~100 KiB for one typical request).
//  - keep_alive (optional) keeps one TLS connection to the last host for 10 s: a login followed by a ranking call pays one handshake.
//    A reused connection that turns out dead is replaced once, silently (so make POSTs safe to repeat).
//  - Strings: urls, header lines and bodies are copied at start; the caller's memory may go away at once.
#ifndef NP_HTTPS_H
#define NP_HTTPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// s1117b (Strikers 1117b): 8 slots, was 4. The worker runs one request at a time; a held poll, a login and the fire-and-forget leave / usage
// reports could fill 4 and the report was refused (and dropped without a word). Costs about 2.7 KB of static memory a slot.
#define NP_HTTPS_SLOTS 8
#define NP_HTTPS_MAX_URL 1023
#define NP_HTTPS_MAX_REQUEST_BODY (64u * 1024u)
#define NP_HTTPS_MAX_RESPONSE_LIMIT (1024u * 1024u)
#define NP_HTTPS_DEFAULT_MAX_RESPONSE (64u * 1024u)
#define NP_HTTPS_MAX_HEADERS 8
#define NP_HTTPS_MAX_HEADER_BYTES 1024

typedef enum NpHttpsError {
    NP_HTTPS_OK = 0,
    NP_HTTPS_E_UNAVAILABLE,    // built without TLS, or the root certificates / random source did not load: nothing was sent
    NP_HTTPS_E_BAD_REQUEST,    // url / method / header / body refused before any network use (np_https_start)
    NP_HTTPS_E_BUSY,           // all NP_HTTPS_SLOTS requests are in use (np_https_start)
    NP_HTTPS_E_OFFLINE,        // the console has no network connection
    NP_HTTPS_E_CLOCK,          // the console clock is earlier than this build: certificates cannot be checked
    NP_HTTPS_E_DNS,            // the name did not resolve
    NP_HTTPS_E_CONNECT,        // TCP connect failed (refused, unreachable, reset)
    NP_HTTPS_E_TLS_HANDSHAKE,  // the TLS handshake failed for a reason that is not the certificate (tls_code has the library's code)
    NP_HTTPS_E_CERT,           // the server's certificate was refused: cert_flags says why
    NP_HTTPS_E_IO,             // send or receive failed, or the connection closed before the answer was complete
    NP_HTTPS_E_PROTOCOL,       // the answer is not valid HTTP/1.x (or uses a coding we refuse)
    NP_HTTPS_E_REDIRECT,       // a 3xx answer; never followed. http_status is set
    NP_HTTPS_E_HTTP_STATUS,    // any other non-2xx status. http_status and the body (the server's explanation) are set
    NP_HTTPS_E_TIMEOUT,        // a time limit ran out; stage says in which step
    NP_HTTPS_E_TOO_LARGE,      // the answer is over max_response
    NP_HTTPS_E_CANCELLED,
    NP_HTTPS_E_MEMORY
} NpHttpsError;

// Why a certificate was refused (NpHttpsResult::cert_flags; several may be set).
enum {
    NP_HTTPS_CERT_EXPIRED = 1,        // a certificate in the chain is past its end date (and the clock is later than this build)
    NP_HTTPS_CERT_NOT_YET_VALID = 2,  // a certificate in the chain starts later than the clock says
    NP_HTTPS_CERT_UNTRUSTED = 4,      // the chain does not end in one of our roots (self-signed, unknown issuer, not a CA)
    NP_HTTPS_CERT_HOST_MISMATCH = 8,  // the certificate is not for the host name asked for
    NP_HTTPS_CERT_BAD_SIGNATURE = 16, // a signature in the chain does not check out, or uses a refused algorithm or key
    NP_HTTPS_CERT_OTHER = 32          // anything else (revoked flags, key usage, chain too long ...)
};

// The clock floor: a console clock earlier than this day is certainly wrong. A fixed day in the source (UTC midnight; 2026-11-15), raised by hand when a
// release is made, so the same source always builds the same program (it was the build's __DATE__). np_https_roots.h's NP_HTTPS_ROOTS_DAY counts too when it
// is later; the floor is a day before the later of the two (time zones).
#define NP_HTTPS_CLOCK_FLOOR_UTC 1794700800LL

// A certificate refused ONLY for its dates (expired or not yet valid, no other fault) is, for a player, a wrong date and time on the Vita far more often than a
// service whose certificate ran out: the request then fails as NP_HTTPS_E_CLOCK ("The Vita's date and time look wrong. Set them in Settings > Date & Time") and not as
// "the server's certificate was not accepted", which nobody can act on. The flags stay in the result for the log.
static inline int np_https_cert_is_date_fault(unsigned flags) {
    const unsigned dates = NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_NOT_YET_VALID;
    return (flags & dates) != 0 && (flags & ~dates) == 0;
}

// Where a request is / was (NpHttpsResult::stage; also visible while pending).
typedef enum NpHttpsStage {
    NP_HTTPS_S_QUEUED = 0, NP_HTTPS_S_DNS, NP_HTTPS_S_CONNECT, NP_HTTPS_S_TLS, NP_HTTPS_S_SEND, NP_HTTPS_S_RESPONSE, NP_HTTPS_S_DONE
} NpHttpsStage;

typedef struct NpHttpsRequest {
    const char* url;           // "https://host[:port]/path?query"; https only; host name (or IPv4 address); required
    const char* method;        // "GET" or "POST"; NULL = POST when there is a body, else GET
    const void* body;          // optional; copied. Sent with Content-Length
    size_t body_len;           // <= NP_HTTPS_MAX_REQUEST_BODY
    const char* content_type;  // default "application/json" (when there is a body)
    const char* const* headers;  // optional, NULL-terminated array of "Name: value" lines (at most NP_HTTPS_MAX_HEADERS, 1 KiB in all, no CR/LF).
                                 // Host, Content-Length, Connection, Transfer-Encoding, Accept-Encoding, Upgrade are set by us; a "Content-Type: x"
                                 // line is taken as content_type (netport31)
    uint32_t connect_timeout_ms;  // DNS + TCP connect + TLS handshake together; default 10000
    uint32_t io_timeout_ms;       // longest wait for any byte once connected; default 10000. A request the server holds open on purpose (a held
                                  // poll) passes its hold plus a margin (np_http.h HeldIoMs)
    uint32_t total_timeout_ms;    // the whole request; default 30000, at most 120000
    uint32_t max_response;        // bytes of body accepted; default NP_HTTPS_DEFAULT_MAX_RESPONSE, at most NP_HTTPS_MAX_RESPONSE_LIMIT
    int keep_alive;               // nonzero: ask to keep the connection (see above)
} NpHttpsRequest;

typedef struct NpHttpsResult {
    NpHttpsError error;        // NP_HTTPS_OK or why not
    NpHttpsStage stage;        // the step the request was in when it ended (or is in, while pending)
    int http_status;           // 0 until a status line was read
    uint32_t cert_flags;       // NP_HTTPS_CERT_* when error == CERT
    uint32_t cert_raw;         // the TLS library's own verification flags (mbedtls MBEDTLS_X509_BADCERT_*), for logs
    int tls_code;              // the TLS library's raw (negative) code of a failure, 0 if none. For logs
    int sys_code;              // the system's raw code (errno / SceNet error) of a failure, 0 if none. For logs
    uint32_t tls_version;      // 0x0303 (1.2) or 0x0304 (1.3) once a handshake finished, else 0
    int reused_connection;     // 1 if a kept-alive connection carried this request
    uint32_t elapsed_ms;
    uint32_t dns_ms, connect_ms, tls_ms;  // time spent in each connection step (0 on a reused connection)
    char cipher[48];           // the negotiated suite, once a handshake finished
    char cert_detail[200];     // the server certificate: subject, issuer, validity window (and, after a refusal, the console clock in UTC)
    size_t bytes_received;     // body bytes so far (final once done)
    const uint8_t* body;       // NUL-terminated, valid until np_https_release; never NULL once done (empty string if none)
    size_t body_len;
    char message[96];          // one short English line for the log ("certificate is not for this host name")
} NpHttpsResult;

typedef uint32_t NpHttpsHandle;  // 0 = none

// Queues a request. Returns its handle, or 0 with *err set (BAD_REQUEST, BUSY, UNAVAILABLE when TLS is not built in).
NpHttpsHandle np_https_start(const NpHttpsRequest* request, NpHttpsError* err);
// 0 = still running (*out has stage / bytes so far; out may be NULL), 1 = finished (*out complete; read it, then release), -1 = not a live handle.
int np_https_poll(NpHttpsHandle handle, NpHttpsResult* out);
// Asks the request to stop (the worker notices within ~50 ms; DNS may take until its own short limit). It then finishes as CANCELLED.
void np_https_cancel(NpHttpsHandle handle);
// Frees the request and its body (cancelling it first if it still runs). The handle is dead afterwards.
void np_https_release(NpHttpsHandle handle);
// Cancels everything, stops the worker (waiting up to timeout_ms for it) and frees all memory. A later np_https_start restarts it.
void np_https_shutdown(uint32_t timeout_ms);

// 1 if this build has TLS compiled in and (after the first request ran) the roots loaded; 0 if not. Cheap, no network use.
int np_https_available(void);
// 1 when the last completed certificate verification refused the chain only because of its dates (expired / not yet valid, nothing else
// wrong), or the console clock is now earlier than this build (live); 0 after any verification that passed or failed for another reason.
// Global across hosts; failures before a verification (DNS, connect) leave it as it was.
int np_https_last_clock_fault(void);
// 1 if the clock is not earlier than this build (np_https_start would not report CLOCK now), 0 if it is, -1 if the platform has no clock.
int np_https_clock_ok(void);

// "mbedtls 3.6.8" (the TLS backend compiled in) or "none".
const char* np_https_backend_name(void);
// The console clock in UTC and the earliest time this build accepts, for the log: "2026-10-30 12:00:00Z (accepted from 2026-10-29 00:00:00Z)".
void np_https_clock_text(char* out, size_t cap);

const char* np_https_error_name(NpHttpsError e);   // "CERT", "DNS" ... for logs
const char* np_https_error_text(NpHttpsError e);   // a line a player can act on: "Set your Vita's date and time."
const char* np_https_stage_name(NpHttpsStage s);

// netport31: requests waiting to start (queued: the worker runs ONE request at a time), not counting `except` (0 = count all). A caller
// holding a long request open on purpose (the room's held poll) gives it up when another waits, so the other is not held up for the hold.
int np_https_waiting(NpHttpsHandle except);
// netport31: the client's sockets (np_https_sys.h: one connection at a time, a new one refused while two are open). Any pointer may be NULL.
// live = open now (0 or 1; 1 only while a request runs or a kept connection waits, at most 10 s), opened / closed = totals of this run,
// refused = connections not started because the budget was used up (always 0 unless something leaks).
void np_https_socket_stats(int* live, int* opened, int* closed, int* refused);

// SMG_NET_SELFTEST=https://host/path (environment / env file): one GET a few seconds after start-up, the result in the log
// ("[net-https] selftest ..."). See np_https_selftest.cpp. Nothing to call.

#ifdef NP_HTTPS_TEST_HOOKS
// Host tests only (never defined in the game's builds): replace the roots by PEM text, the clock by a function, and the
// time limit of an idle kept-alive connection. Pass NULL to restore.
void np_https_test_set_roots_pem(const char* pem, size_t len);   // len includes the terminating NUL (mbedtls' rule)
void np_https_test_set_clock(int64_t (*unix_seconds)(void));
void np_https_test_set_build_floor(int64_t unix_seconds);        // 0 = the real one
void np_https_test_reset_tls(void);                               // forget the parsed roots and the kept connection
int np_https_test_root_count(void);
void np_https_test_set_default_io_ms(uint32_t ms);               // the no-data limit of a request that gives none (default 10000); 0 = restore
#endif

#ifdef __cplusplus
}
#endif
#endif
