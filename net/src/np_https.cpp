// np_https.cpp: the game's HTTPS client. See np_https.h for the contract; docs/ONLINE-TLS.md for the design and the root set.
//
// Threads: callers (the game thread) only touch the slot table under a spin lock held for a few instructions. One worker
// thread runs the requests one at a time: DNS, TCP, TLS handshake, request, answer. It owns the TLS state (roots, random
// generator, the one kept-alive connection) so none of that needs a lock.
#include "np_https.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "np_https_sys.h"

#include "np_https_tls.h"
#if defined(NP_HTTPS_HAVE_TLS)
#include "np_https_roots.h"   // (NP_HTTPS_ROOTS_DAY; the certificates themselves are the backend's)
#endif

using namespace nph;

namespace {

// ---------------------------------------------------------------------------------------------------------------------------
// names and texts
// ---------------------------------------------------------------------------------------------------------------------------

const char* const kErrorNames[] = {"OK", "UNAVAILABLE", "BAD_REQUEST", "BUSY", "OFFLINE", "CLOCK", "DNS", "CONNECT", "TLS_HANDSHAKE", "CERT",
                                   "IO", "PROTOCOL", "REDIRECT", "HTTP_STATUS", "TIMEOUT", "TOO_LARGE", "CANCELLED", "MEMORY"};
const char* const kErrorTexts[] = {
    "OK.",
    "Secure connections are not available in this build.",
    "The request was not valid.",
    "Too many requests at once. Try again in a moment.",
    "The Vita is not connected to a network. Turn on Wi-Fi and try again.",
    "The Vita's date and time look wrong. Set them in Settings > Date & Time, then try again.",
    "The server's address could not be found. Check the Wi-Fi connection.",
    "Could not connect to the server. Check the Wi-Fi connection.",
    "The secure connection could not be set up.",
    "The server's certificate was not accepted, so nothing was sent.",
    "The connection to the server broke.",
    "The server's answer was not understood.",
    "The server asked to go somewhere else, which is not followed.",
    "The server answered with an error.",
    "The server took too long to answer.",
    "The server's answer is too large.",
    "Cancelled.",
    "Out of memory."};
const char* const kStageNames[] = {"queued", "dns", "connect", "tls", "send", "response", "done"};

// ---------------------------------------------------------------------------------------------------------------------------
// the slot table
// ---------------------------------------------------------------------------------------------------------------------------

enum SlotState { SS_FREE = 0, SS_QUEUED, SS_RUNNING, SS_DONE };

struct Slot {
    int state;
    uint32_t gen;
    uint32_t seq;               // arrival order
    volatile int cancel;
    bool released;              // released while running: the worker frees the slot at the end
    // the request, parsed and copied (read-only for the worker until it ends)
    char host[256];
    uint16_t port;
    char path[NP_HTTPS_MAX_URL + 1];
    char method[8];
    char ctype[64];
    char headers[NP_HTTPS_MAX_HEADER_BYTES + 8];   // "Name: value\r\n" lines
    uint8_t* body;
    size_t body_len;
    uint32_t t_connect, t_io, t_total, max_response;
    bool keep_alive;
    // the answer
    volatile int stage;
    volatile size_t got;
    int64_t t_start;
    NpHttpsResult res;
    uint8_t* resp;              // malloc'd, NUL-terminated
    size_t resp_len, resp_cap;
};

Slot g_slots[NP_HTTPS_SLOTS];
SpinLock g_lock;                // the slot table (state, gen, seq, released, the DONE hand-over)
SpinLock g_workerLock;          // starting / stopping the worker
Event* g_event;
Thread* g_thread;
volatile int g_running;         // the worker exists
volatile int g_quit;
uint32_t g_genCounter, g_seqCounter;
const uint8_t kEmpty[1] = {0};

inline NpHttpsHandle MakeHandle(uint32_t gen, int index) { return (gen << 8) | (uint32_t)(index + 1); }
Slot* SlotOf(NpHttpsHandle h) {   // caller holds g_lock
    const uint32_t idx = (h & 0xFF);
    if (idx < 1 || idx > NP_HTTPS_SLOTS) return nullptr;
    Slot* s = &g_slots[idx - 1];
    return (s->state != SS_FREE && s->gen == (h >> 8)) ? s : nullptr;
}

void FreeSlotMemory(Slot* s) {
    free(s->body);
    free(s->resp);
    s->body = nullptr;
    s->resp = nullptr;
    s->body_len = s->resp_len = s->resp_cap = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// the clock check
// ---------------------------------------------------------------------------------------------------------------------------

int64_t g_floorOverride;

// netport31 (Strikers heldpoll23): the no-data limit of a request that names none. Ordinary requests keep it short (a dead connection fails
// fast); a request the service holds open on purpose (the room's held poll, np_signal_http.cpp) passes io_timeout_ms = its hold + a
// margin (np_http.h HeldIoMs). Host tests shrink the default (np_https_test_set_default_io_ms).
uint32_t g_defaultIoOverride;
uint32_t DefaultIoMs() { return g_defaultIoOverride ? g_defaultIoOverride : 10000; }

// linkfix: "the last handshake failed only because of the certificate dates" (the console's date and time are wrong), for the Connection Test
// (np_https_last_clock_fault). Set by a handshake refused ONLY for expired / not-yet-valid certificates (the library's flags are EXPIRED
// and/or FUTURE and nothing else: np_https_cert_is_date_fault) and by the pre-handshake refusal of a clock earlier than this build (a clock
// reset to 2000 or 2011: the same wrong date, read live from the clock, not stored). The stored part is set from EACH completed certificate
// verification: 1 for a date-only refusal, 0 for success or any other refusal. GLOBAL: not per host. Worker thread writes, any thread reads.
int g_clockFault;
void NoteClockFault(int on) { __atomic_store_n(&g_clockFault, on, __ATOMIC_RELAXED); }

#if defined(NP_HTTPS_HAVE_TLS)
// The earliest time the console clock can truthfully show: the later of the day the root list was made and NP_HTTPS_CLOCK_FLOOR_UTC (np_https.h: a
// fixed day in the source, not the build's date, so builds are reproducible), minus a day for time zones. A Vita whose clock was reset (battery gone,
// never set) shows 2011 or 2000: certificates would all look "not yet valid", which a player cannot act on; "set your clock" is what they can do.
int64_t BuildFloor() {
    if (g_floorOverride) return g_floorOverride;
    int64_t t = NP_HTTPS_ROOTS_DAY;
    if (NP_HTTPS_CLOCK_FLOOR_UTC > t) t = NP_HTTPS_CLOCK_FLOOR_UTC;
    return t - 86400;
}
#else
int64_t BuildFloor() { return g_floorOverride; }
#endif

// ---------------------------------------------------------------------------------------------------------------------------
// TLS state (worker thread only)
// ---------------------------------------------------------------------------------------------------------------------------

#if defined(NP_HTTPS_HAVE_TLS)

struct Conn {
    bool open;
    Sock sock;
    tls::Conn* tls;
    char host[256];
    uint16_t port;
    int64_t last_used;
    int sys;                    // the last system error seen by the socket callbacks
    uint32_t version;
};
Conn g_conn;

void ConnClose(bool graceful) {
    if (!g_conn.open) return;
    if (graceful && g_conn.tls) tls::CloseNotify(g_conn.tls);   // one try; a full send buffer or a dead peer is not waited for
    tls::Free(g_conn.tls);
    g_conn.tls = nullptr;
    SockClose(&g_conn.sock);
    g_conn.open = false;
}

int IoSend(void* ctx, const uint8_t* buf, size_t len) {
    Conn* c = (Conn*)ctx;
    int sys = 0;
    const int n = SockSend(&c->sock, buf, len, &sys);
    if (n < 0) c->sys = sys;
    return n;
}
int IoRecv(void* ctx, uint8_t* buf, size_t len) {
    Conn* c = (Conn*)ctx;
    int sys = 0;
    const int n = SockRecv(&c->sock, buf, len, &sys);
    if (n == -1) c->sys = sys;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------------
// one request
// ---------------------------------------------------------------------------------------------------------------------------

struct Ctx {
    Slot* s;
    int64_t total_end;          // the request's overall deadline
    int64_t limit;              // the current step's deadline (connect, then "no progress for io_timeout")
    bool connecting;
    bool progress;              // some byte moved since the last check
};

void SetStage(Ctx& c, NpHttpsStage st) { c.s->stage = (int)st; }

// Ends the request with an error. Keeps what is already known (status, version).
int Fail(Ctx& c, NpHttpsError e, const char* message, int tls_code = 0, int sys = 0) {
    Slot* s = c.s;
    s->res.error = e;
    s->res.stage = (NpHttpsStage)s->stage;
    s->res.tls_code = tls_code;
    s->res.sys_code = sys;
    if (message) snprintf(s->res.message, sizeof s->res.message, "%s", message);
    return -1;
}

// Waits (in slices, watching the cancel flag and the deadlines) until the socket may move bytes. 0 = go on; -1 = the request failed (set).
int WaitIo(Ctx& c, bool for_write) {
    Slot* s = c.s;
    for (;;) {
        if (s->cancel) return Fail(c, NP_HTTPS_E_CANCELLED, "cancelled");
        const int64_t now = MonoMs();
        if (now >= c.total_end) return Fail(c, NP_HTTPS_E_TIMEOUT, "total time limit");
        if (now >= c.limit) return Fail(c, NP_HTTPS_E_TIMEOUT, c.connecting ? "connect time limit" : "no data for too long");
        int64_t slice = c.limit - now;
        if (c.total_end - now < slice) slice = c.total_end - now;
        if (slice > 50) slice = 50;
        const int r = SockWait(&g_conn.sock, for_write, (uint32_t)slice);
        if (r < 0) return Fail(c, NP_HTTPS_E_IO, "socket wait failed", 0, g_conn.sys);
        if (r > 0) return 0;
    }
}

void Progress(Ctx& c) {
    if (!c.connecting) c.limit = MonoMs() + c.s->t_io;
}

const char* CertMessage(uint32_t f) {
    if (f & NP_HTTPS_CERT_HOST_MISMATCH) return "certificate is not for this host name";
    if (f & NP_HTTPS_CERT_EXPIRED) return "certificate has expired (or the console clock is far ahead)";
    if (f & NP_HTTPS_CERT_NOT_YET_VALID) return "certificate is not valid yet (or the console clock is behind)";
    if (f & NP_HTTPS_CERT_UNTRUSTED) return "certificate is not signed by a trusted root";
    if (f & NP_HTTPS_CERT_BAD_SIGNATURE) return "certificate uses a refused key or signature";
    return "certificate refused";
}

// "2026-10-30 12:00:00Z" for a unix time
void UtcText(int64_t t, char* out, size_t cap) {
    if (t <= 0) { snprintf(out, cap, "unknown"); return; }
    int64_t days = t / 86400;
    const int64_t rem = t % 86400;
    days += 719468;
    const int64_t era = days / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, cap, "%04d-%02u-%02u %02d:%02d:%02dZ", (int)(y + (m <= 2)), m, d, (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60));
}

// What a player or a tester needs to see when a certificate is refused (the idea of Monster Hunter Tri's LogRefusedCertificate): every
// certificate the server sent with the library's verdict on it, its validity window, and the console's own clock in UTC.
void LogCertificates(const Slot* s, const tls::Details& d, uint32_t flags, uint32_t raw) {
    char now[24], floor_[24];
    UtcText(WallSeconds(), now, sizeof now);
    UtcText(BuildFloor(), floor_, sizeof floor_);
    Log("certificate for %s: %s (flags 0x%x, library 0x%x); console clock %s, this build is not older than %s", s->host,
        flags ? "REFUSED" : "accepted", (unsigned)flags, (unsigned)raw, now, floor_);
    for (int i = 0; i < d.chain_len && i < tls::MAX_CHAIN; i++) {
        const tls::CertInfo& ci = d.chain[i];
        Log("  [%d] subject \"%s\" issuer \"%s\" valid %s .. %s verdict 0x%x", i, ci.subject, ci.issuer, ci.not_before, ci.not_after, (unsigned)ci.raw_flags);
    }
}

void SetCertDetail(Slot* s, const tls::Details& d, bool with_clock) {
    if (d.chain_len < 1) return;
    char now[24];
    UtcText(WallSeconds(), now, sizeof now);
    const tls::CertInfo& ci = d.chain[0];
    snprintf(s->res.cert_detail, sizeof s->res.cert_detail, "%s; issuer %s; valid %s .. %s%s%s", ci.subject, ci.issuer, ci.not_before, ci.not_after,
             with_clock ? "; console clock " : "", with_clock ? now : "");
}

// DNS, TCP and the TLS handshake. 0 = a connection is open in g_conn; -1 = failed (set in the slot).
int Connect(Ctx& c) {
    Slot* s = c.s;
    c.connecting = true;
    c.limit = MonoMs() + s->t_connect;
    if (c.limit > c.total_end) c.limit = c.total_end;
    g_conn.open = false;
    g_conn.tls = nullptr;
    g_conn.sys = 0;
    SockInit(&g_conn.sock);

    SetStage(c, NP_HTTPS_S_DNS);
    int64_t t0 = MonoMs();
    uint32_t ip = 0;
    int sys = 0;
    int64_t left = c.limit - t0;
    int r = Resolve(s->host, &ip, left > 0 ? (uint32_t)left : 1, &s->cancel, &sys);
    s->res.dns_ms = (uint32_t)(MonoMs() - t0);
    if (r == NET_CANCELLED) return Fail(c, NP_HTTPS_E_CANCELLED, "cancelled");
    if (r == NET_TIMEOUT) return Fail(c, NP_HTTPS_E_TIMEOUT, "name lookup took too long", 0, sys);
    if (r != 0) return Fail(c, NP_HTTPS_E_DNS, "name did not resolve", 0, sys);

    SetStage(c, NP_HTTPS_S_CONNECT);
    t0 = MonoMs();
    left = c.limit - t0;
    r = SockConnect(&g_conn.sock, ip, s->port, left > 0 ? (uint32_t)left : 1, &s->cancel, &sys);
    s->res.connect_ms = (uint32_t)(MonoMs() - t0);
    if (r == NET_CANCELLED) { SockClose(&g_conn.sock); return Fail(c, NP_HTTPS_E_CANCELLED, "cancelled"); }
    if (r == NET_TIMEOUT) { SockClose(&g_conn.sock); return Fail(c, NP_HTTPS_E_TIMEOUT, "connect took too long", 0, sys); }
    if (r != 0) { SockClose(&g_conn.sock); return Fail(c, NP_HTTPS_E_CONNECT, "TCP connect failed", 0, sys); }

    SetStage(c, NP_HTTPS_S_TLS);
    t0 = MonoMs();
    tls::Io io = {IoSend, IoRecv, &g_conn};
    g_conn.tls = tls::Open(s->host, io);   // SNI and the name the certificate must carry
    if (!g_conn.tls) { SockClose(&g_conn.sock); return Fail(c, NP_HTTPS_E_MEMORY, "TLS setup failed"); }
    g_conn.open = true;   // from here ConnClose releases both
    int code = 0;
    tls::Result tr;
    for (;;) {
        tr = tls::Handshake(g_conn.tls, &code);
        if (tr == tls::T_WANT_READ || tr == tls::T_WANT_WRITE) {
            if (WaitIo(c, tr == tls::T_WANT_WRITE) != 0) { ConnClose(false); return -1; }
            continue;
        }
        break;
    }
    s->res.tls_ms = (uint32_t)(MonoMs() - t0);
    tls::Details d;
    tls::GetDetails(g_conn.tls, &d);
    if (tr != tls::T_OK) {
        const int saved = g_conn.sys;
        if (tr == tls::T_CERT) {
            uint32_t raw = 0;
            const uint32_t flags = tls::CertFlags(g_conn.tls, &raw);
            s->res.cert_raw = raw;
            s->res.cert_flags = flags ? flags : (uint32_t)NP_HTTPS_CERT_OTHER;
            SetCertDetail(s, d, true);
            LogCertificates(s, d, s->res.cert_flags, raw);
            NoteClockFault(np_https_cert_is_date_fault(s->res.cert_flags) ? 1 : 0);   // each completed verification decides: dates only = 1, any other refusal = 0
            ConnClose(false);
            return Fail(c, np_https_cert_is_date_fault(s->res.cert_flags) ? NP_HTTPS_E_CLOCK : NP_HTTPS_E_CERT, CertMessage(s->res.cert_flags), code);
        }
        ConnClose(false);
        if (tr == tls::T_MEMORY) return Fail(c, NP_HTTPS_E_MEMORY, "out of memory in the TLS handshake", code);
        if (tr == tls::T_EOF || saved != 0) return Fail(c, NP_HTTPS_E_TLS_HANDSHAKE, "connection closed during the TLS handshake", code, saved);
        return Fail(c, NP_HTTPS_E_TLS_HANDSHAKE, "TLS handshake failed", code, saved);
    }
    NoteClockFault(0);   // a handshake went through: the dates are fine
    g_conn.version = d.version;
    snprintf(s->res.cipher, sizeof s->res.cipher, "%s", d.cipher);
    SetCertDetail(s, d, false);
    strncpy(g_conn.host, s->host, sizeof g_conn.host - 1);
    g_conn.host[sizeof g_conn.host - 1] = 0;
    g_conn.port = s->port;
    g_conn.last_used = MonoMs();
    c.connecting = false;
    return 0;
}

// Writes all of buf through TLS.
int SendAll(Ctx& c, const uint8_t* p, size_t n) {
    while (n > 0) {
        size_t done = 0;
        int code = 0;
        const tls::Result r = tls::Write(g_conn.tls, p, n, &done, &code);
        if (r == tls::T_OK) { p += done; n -= done; Progress(c); continue; }
        if (r == tls::T_WANT_WRITE || r == tls::T_WANT_READ) {
            if (WaitIo(c, r == tls::T_WANT_WRITE) != 0) return -1;
            continue;
        }
        return Fail(c, NP_HTTPS_E_IO, "send failed", code, g_conn.sys);
    }
    return 0;
}

// ---- the answer: header parse and body framing -------------------------------------------------------------------------------------

enum BodyMode { BM_NONE, BM_LENGTH, BM_CHUNKED, BM_CLOSE };
enum ChunkState { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_TRAIL_START, CH_TRAIL_LINE, CH_END_LF, CH_DONE };

struct Body {
    BodyMode mode;
    uint64_t remaining;         // BM_LENGTH
    ChunkState cs;              // BM_CHUNKED
    uint64_t chunk_size;
    uint64_t chunk_left;
    int digits;
    uint32_t ext_bytes, trailer_bytes;
    bool complete;
};

// Appends decoded body bytes. Returns -1 and sets the error when over the limit / out of memory.
int Append(Ctx& c, const uint8_t* p, size_t n) {
    Slot* s = c.s;
    if (n == 0) return 0;
    if (s->resp_len + n > s->max_response) return Fail(c, NP_HTTPS_E_TOO_LARGE, "answer is larger than the limit");
    if (s->resp_len + n + 1 > s->resp_cap) {
        size_t cap = s->resp_cap ? s->resp_cap : 1024;
        while (cap < s->resp_len + n + 1) cap *= 2;
        if (cap > (size_t)s->max_response + 1) cap = (size_t)s->max_response + 1;
        uint8_t* nb = (uint8_t*)realloc(s->resp, cap);
        if (!nb) return Fail(c, NP_HTTPS_E_MEMORY, "out of memory");
        s->resp = nb;
        s->resp_cap = cap;
    }
    memcpy(s->resp + s->resp_len, p, n);
    s->resp_len += n;
    s->resp[s->resp_len] = 0;
    s->got = s->resp_len;
    return 0;
}

// Feeds raw bytes of the body. 0 = fine (check b.complete), -1 = error set. *used = bytes consumed (less than n only when the body ended).
int Feed(Ctx& c, Body& b, const uint8_t* p, size_t n, size_t* used) {
    size_t i = 0;
    if (b.mode == BM_NONE) { b.complete = true; *used = 0; return 0; }
    if (b.mode == BM_CLOSE) {
        *used = n;
        return Append(c, p, n);
    }
    if (b.mode == BM_LENGTH) {
        const size_t take = (uint64_t)n > b.remaining ? (size_t)b.remaining : n;
        if (Append(c, p, take) != 0) return -1;
        b.remaining -= take;
        if (b.remaining == 0) b.complete = true;
        *used = take;
        return 0;
    }
    while (i < n && !b.complete) {
        const uint8_t ch = p[i];
        switch (b.cs) {
        case CH_SIZE:
            if (isxdigit(ch)) {
                const unsigned d = ch <= '9' ? (unsigned)(ch - '0') : (unsigned)((ch | 0x20) - 'a' + 10);
                if (b.chunk_size > 0x07FFFFFFull) return Fail(c, NP_HTTPS_E_PROTOCOL, "chunk size too large");
                b.chunk_size = b.chunk_size * 16 + d;
                b.digits++;
                i++;
            } else if (b.digits > 0 && ch == ';') { b.cs = CH_EXT; i++; }
            else if (b.digits > 0 && ch == '\r') { b.cs = CH_SIZE_LF; i++; }
            else return Fail(c, NP_HTTPS_E_PROTOCOL, "bad chunk size line");
            break;
        case CH_EXT:
            if (++b.ext_bytes > 256) return Fail(c, NP_HTTPS_E_PROTOCOL, "chunk extension too long");
            if (ch == '\r') b.cs = CH_SIZE_LF;
            i++;
            break;
        case CH_SIZE_LF:
            if (ch != '\n') return Fail(c, NP_HTTPS_E_PROTOCOL, "bad chunk size line");
            i++;
            if (b.chunk_size == 0) b.cs = CH_TRAIL_START;
            else { b.cs = CH_DATA; b.chunk_left = b.chunk_size; }
            break;
        case CH_DATA: {
            const size_t take = (uint64_t)(n - i) > b.chunk_left ? (size_t)b.chunk_left : n - i;
            if (Append(c, p + i, take) != 0) return -1;
            i += take;
            b.chunk_left -= take;
            if (b.chunk_left == 0) b.cs = CH_DATA_CR;
            break;
        }
        case CH_DATA_CR:
            if (ch != '\r') return Fail(c, NP_HTTPS_E_PROTOCOL, "chunk not followed by CRLF");
            b.cs = CH_DATA_LF;
            i++;
            break;
        case CH_DATA_LF:
            if (ch != '\n') return Fail(c, NP_HTTPS_E_PROTOCOL, "chunk not followed by CRLF");
            b.cs = CH_SIZE;
            b.chunk_size = 0;
            b.digits = 0;
            b.ext_bytes = 0;
            i++;
            break;
        case CH_TRAIL_START:
            if (++b.trailer_bytes > 4096) return Fail(c, NP_HTTPS_E_PROTOCOL, "trailers too long");
            b.cs = ch == '\r' ? CH_END_LF : CH_TRAIL_LINE;
            i++;
            break;
        case CH_TRAIL_LINE:
            if (++b.trailer_bytes > 4096) return Fail(c, NP_HTTPS_E_PROTOCOL, "trailers too long");
            if (ch == '\n') b.cs = CH_TRAIL_START;
            i++;
            break;
        case CH_END_LF:
            if (ch != '\n') return Fail(c, NP_HTTPS_E_PROTOCOL, "bad end of chunked body");
            b.cs = CH_DONE;
            b.complete = true;
            i++;
            break;
        case CH_DONE:
            b.complete = true;
            break;
        }
    }
    *used = i;
    return 0;
}

// Case-insensitive header lookup helpers over one header line [p, p+n).
bool NameIs(const char* p, size_t n, const char* name) {
    const size_t l = strlen(name);
    if (n != l) return false;
    for (size_t i = 0; i < l; i++)
        if (tolower((unsigned char)p[i]) != name[i]) return false;
    return true;
}
bool ValueIs(const char* v, size_t n, const char* token) {   // whole value equals token, ignoring case
    const size_t l = strlen(token);
    if (n != l) return false;
    for (size_t i = 0; i < l; i++)
        if (tolower((unsigned char)v[i]) != token[i]) return false;
    return true;
}
bool ValueHasToken(const char* v, size_t n, const char* token) {   // comma list contains token
    const size_t l = strlen(token);
    size_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t j = i;
        while (j < n && v[j] != ',' && v[j] != ' ' && v[j] != '\t') j++;
        if (j - i == l && ValueIs(v + i, l, token)) return true;
        i = j;
    }
    return false;
}

struct Head {
    int status;
    bool http10;
    bool conn_close;
    bool has_length, chunked;
    uint64_t length;
};

// Parses the header block [buf, buf+len) (without the final blank line). 0 = ok.
int ParseHead(Ctx& c, const char* buf, size_t len, Head& h) {
    memset(&h, 0, sizeof h);
    // status line
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0 || (buf[7] != '0' && buf[7] != '1') || buf[8] != ' ' || !isdigit((unsigned char)buf[9]) ||
        !isdigit((unsigned char)buf[10]) || !isdigit((unsigned char)buf[11]) || (len > 12 && buf[12] != ' ' && buf[12] != '\r'))
        return Fail(c, NP_HTTPS_E_PROTOCOL, "bad status line");
    h.http10 = buf[7] == '0';
    h.status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    size_t i = 0;
    while (i < len && buf[i] != '\n') i++;
    i++;   // past the status line's LF
    while (i < len) {
        size_t e = i;
        while (e < len && buf[e] != '\n') e++;
        size_t le = e;
        if (le > i && buf[le - 1] == '\r') le--;
        if (le == i) break;
        if (buf[i] == ' ' || buf[i] == '\t') return Fail(c, NP_HTTPS_E_PROTOCOL, "folded header");
        const char* colon = (const char*)memchr(buf + i, ':', le - i);
        if (!colon) return Fail(c, NP_HTTPS_E_PROTOCOL, "bad header line");
        const size_t nl = (size_t)(colon - (buf + i));
        const char* v = colon + 1;
        size_t vn = (size_t)(buf + le - v);
        while (vn > 0 && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
        if (NameIs(buf + i, nl, "content-length")) {
            if (vn == 0 || vn > 10) return Fail(c, NP_HTTPS_E_PROTOCOL, "bad Content-Length");
            uint64_t n = 0;
            for (size_t k = 0; k < vn; k++) {
                if (!isdigit((unsigned char)v[k])) return Fail(c, NP_HTTPS_E_PROTOCOL, "bad Content-Length");
                n = n * 10 + (uint64_t)(v[k] - '0');
            }
            if (h.has_length && h.length != n) return Fail(c, NP_HTTPS_E_PROTOCOL, "conflicting Content-Length");
            h.has_length = true;
            h.length = n;
        } else if (NameIs(buf + i, nl, "transfer-encoding")) {
            if (!ValueIs(v, vn, "chunked")) return Fail(c, NP_HTTPS_E_PROTOCOL, "unsupported Transfer-Encoding");
            h.chunked = true;
        } else if (NameIs(buf + i, nl, "content-encoding")) {
            if (!ValueIs(v, vn, "identity")) return Fail(c, NP_HTTPS_E_PROTOCOL, "compressed answer");
        } else if (NameIs(buf + i, nl, "connection")) {
            if (ValueHasToken(v, vn, "close")) h.conn_close = true;
        }
        i = e + 1;
    }
    return 0;
}

// Sends the request and reads the answer. 0 = answer complete (error may still be HTTP_STATUS/REDIRECT, set here); -1 = failed.
// *any_bytes is set once any byte of the answer arrived (a reused connection that died before that may be retried).
int Exchange(Ctx& c, bool* any_bytes, bool* keep) {
    Slot* s = c.s;
    *any_bytes = false;
    *keep = false;
    SetStage(c, NP_HTTPS_S_SEND);
    c.connecting = false;
    Progress(c);

    // request head
    char head[NP_HTTPS_MAX_URL + NP_HTTPS_MAX_HEADER_BYTES + 512];
    const bool has_body = s->body_len > 0 || strcmp(s->method, "POST") == 0 || strcmp(s->method, "PUT") == 0;
    // snprintf returns the length it WOULD have written: n can pass sizeof head, and `sizeof head - n` would then wrap to a huge size. Every
    // append goes through Append, which stops at the end of the buffer; the check below turns an overflow into BAD_REQUEST.
    size_t n = 0;
    auto Append = [&](const char* fmt, ...) {
        if (n >= sizeof head) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = vsnprintf(head + n, sizeof head - n, fmt, ap);
        va_end(ap);
        if (w < 0) n = sizeof head;
        else n += (size_t)w;
    };
    Append("%s %s HTTP/1.1\r\nHost: %s", s->method, s->path, s->host);
    if (s->port != 443) Append(":%u", (unsigned)s->port);
    Append("\r\nUser-Agent: GalaxyVita\r\nAccept: application/json, */*\r\nAccept-Encoding: identity\r\nConnection: %s\r\n",
           s->keep_alive ? "keep-alive" : "close");
    if (has_body) Append("Content-Type: %s\r\nContent-Length: %u\r\n", s->ctype, (unsigned)s->body_len);
    Append("%s\r\n", s->headers);
    if (n == 0 || n >= sizeof head) return Fail(c, NP_HTTPS_E_BAD_REQUEST, "request head too long");
    if (SendAll(c, (const uint8_t*)head, (size_t)n) != 0) return -1;
    if (s->body_len && SendAll(c, s->body, s->body_len) != 0) return -1;

    // answer head: read until the blank line (interim 1xx answers are skipped)
    SetStage(c, NP_HTTPS_S_RESPONSE);
    static uint8_t buf[8192 + 4096];
    size_t have = 0, head_end = 0;
    Head h;
    for (;;) {
        for (;;) {   // look for the end of the head
            const char* end = nullptr;
            for (size_t k = 0; k + 3 < have; k++)
                if (buf[k] == '\r' && buf[k + 1] == '\n' && buf[k + 2] == '\r' && buf[k + 3] == '\n') { end = (const char*)buf + k; break; }
            if (end) { head_end = (size_t)(end - (const char*)buf); break; }
            if (have >= 8192) return Fail(c, NP_HTTPS_E_PROTOCOL, "answer head too large");
            if (s->cancel) return Fail(c, NP_HTTPS_E_CANCELLED, "cancelled");
            size_t got = 0;
            int code = 0;
            const tls::Result r = tls::Read(g_conn.tls, buf + have, 2048, &got, &code);
            if (r == tls::T_OK) { have += got; *any_bytes = true; Progress(c); continue; }
            if (r == tls::T_WANT_READ || r == tls::T_WANT_WRITE) {
                if (WaitIo(c, r == tls::T_WANT_WRITE) != 0) return -1;
                continue;
            }
            if (r == tls::T_CLOSED || r == tls::T_EOF) return Fail(c, NP_HTTPS_E_IO, "connection closed before the answer", code, g_conn.sys);
            return Fail(c, NP_HTTPS_E_IO, "receive failed", code, g_conn.sys);
        }
        if (ParseHead(c, (const char*)buf, head_end, h) != 0) return -1;
        if (h.status >= 100 && h.status < 200) {
            if (h.status == 101) return Fail(c, NP_HTTPS_E_PROTOCOL, "protocol switch refused");
            memmove(buf, buf + head_end + 4, have - (head_end + 4));
            have -= head_end + 4;
            continue;
        }
        break;
    }
    s->res.http_status = h.status;
    const size_t body_start = head_end + 4;
    if (h.status >= 300 && h.status < 400) {
        s->res.error = NP_HTTPS_E_REDIRECT;
        snprintf(s->res.message, sizeof s->res.message, "redirect (%d) refused", h.status);
        return 0;   // (the connection is closed by the caller: the body is not read)
    }
    Body b;
    memset(&b, 0, sizeof b);
    const bool no_body = h.status == 204 || h.status == 304;
    if (no_body) b.mode = BM_NONE;
    else if (h.chunked) { b.mode = BM_CHUNKED; b.cs = CH_SIZE; }   // (chunked wins over a Content-Length)
    else if (h.has_length) {
        if (h.length > s->max_response) return Fail(c, NP_HTTPS_E_TOO_LARGE, "answer is larger than the limit");
        b.mode = h.length == 0 ? BM_NONE : BM_LENGTH;
        b.remaining = h.length;
        if (h.length > 0) {   // exact allocation
            s->resp = (uint8_t*)malloc((size_t)h.length + 1);
            if (!s->resp) return Fail(c, NP_HTTPS_E_MEMORY, "out of memory");
            s->resp_cap = (size_t)h.length + 1;
            s->resp[0] = 0;
        }
    } else b.mode = BM_CLOSE;
    if (b.mode == BM_NONE) b.complete = true;

    size_t used = 0;
    bool extra = false;   // bytes after the end of the body: the framing cannot be trusted, so the connection is not kept
    if (body_start < have) {
        if (Feed(c, b, buf + body_start, have - body_start, &used) != 0) return -1;
        if (b.complete && used < have - body_start) extra = true;
    }
    while (!b.complete) {
        if (s->cancel) return Fail(c, NP_HTTPS_E_CANCELLED, "cancelled");
        size_t got = 0;
        int code = 0;
        const tls::Result r = tls::Read(g_conn.tls, buf, 4096, &got, &code);
        if (r == tls::T_OK) {
            Progress(c);
            if (Feed(c, b, buf, got, &used) != 0) return -1;
            if (b.complete && used < got) extra = true;
            continue;
        }
        if (r == tls::T_WANT_READ || r == tls::T_WANT_WRITE) {
            if (WaitIo(c, r == tls::T_WANT_WRITE) != 0) return -1;
            continue;
        }
        if (r == tls::T_CLOSED && b.mode == BM_CLOSE) { b.complete = true; break; }
        return Fail(c, NP_HTTPS_E_IO, b.mode == BM_CLOSE ? "connection closed without close_notify" : "connection closed before the answer was complete", code,
                    g_conn.sys);
    }
    if (!s->resp) {   // an empty body is an empty string, not NULL
        s->resp = (uint8_t*)malloc(1);
        if (!s->resp) return Fail(c, NP_HTTPS_E_MEMORY, "out of memory");
        s->resp[0] = 0;
        s->resp_cap = 1;
    }
    // keep the connection only if the framing was exact and nobody asked to close
    *keep = s->keep_alive && !h.http10 && !h.conn_close && b.mode != BM_CLOSE && !extra;
    if (h.status < 200 || h.status >= 300) {
        s->res.error = NP_HTTPS_E_HTTP_STATUS;
        snprintf(s->res.message, sizeof s->res.message, "HTTP status %d", h.status);
    }
    return 0;
}

// Runs one request to its end (the answer is in the slot).
void RunRequest(Slot* s) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.s = s;
    s->t_start = MonoMs();
    c.total_end = s->t_start + s->t_total;
    SetStage(c, NP_HTTPS_S_QUEUED);
    memset(&s->res, 0, sizeof s->res);
    s->res.error = NP_HTTPS_OK;

    if (s->cancel) { Fail(c, NP_HTTPS_E_CANCELLED, "cancelled"); goto done; }
    {
        char why[96];
        if (!tls::Init(why, sizeof why, nullptr)) { Fail(c, NP_HTTPS_E_UNAVAILABLE, why[0] ? why : "TLS not available"); goto done; }
    }
    if (!NetworkUp()) { Fail(c, NP_HTTPS_E_OFFLINE, "no network connection"); goto done; }
    {
        const int64_t now = WallSeconds();
        if (now <= 0 || now < BuildFloor()) {
            Fail(c, NP_HTTPS_E_CLOCK, "the console clock is earlier than this build");
            goto done;
        }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = false;
        if (g_conn.open) {
            // a kept connection serves only the same host and port, while fresh, and only if the server has not spoken (a close) meanwhile
            const bool fresh = s->keep_alive && (MonoMs() - g_conn.last_used) < 10000 && g_conn.port == s->port && strcmp(g_conn.host, s->host) == 0;
            if (fresh && attempt == 0 && SockWait(&g_conn.sock, false, 0) == 0 && tls::Pending(g_conn.tls) == 0) reused = true;
            else ConnClose(true);
        }
        if (!reused) {
            memset(&s->res, 0, sizeof s->res);
            if (Connect(c) != 0) goto done;
        } else {
            c.connecting = false;
        }
        s->res.reused_connection = reused ? 1 : 0;
        s->res.tls_version = g_conn.version;
        bool any = false, keep = false;
        const int rc = Exchange(c, &any, &keep);
        if (rc != 0) {
            const bool dead_reuse = reused && !any && s->res.error == NP_HTTPS_E_IO && !s->cancel;
            ConnClose(false);
            if (dead_reuse && attempt == 0) {   // the server had closed the idle connection: once more on a fresh one
                free(s->resp);
                s->resp = nullptr;
                s->resp_len = s->resp_cap = 0;
                s->got = 0;
                memset(&s->res, 0, sizeof s->res);
                continue;
            }
            goto done;
        }
        if (keep && s->res.error != NP_HTTPS_E_REDIRECT) { g_conn.last_used = MonoMs(); }
        else ConnClose(true);
        break;
    }
done:
    {
        const NpHttpsStage last = (NpHttpsStage)s->stage;   // a failure keeps the stage it happened in; success reads DONE
        SetStage(c, NP_HTTPS_S_DONE);
        s->res.stage = s->res.error == NP_HTTPS_OK ? NP_HTTPS_S_DONE : last;
    }
    s->res.elapsed_ms = (uint32_t)(MonoMs() - s->t_start);
    s->res.bytes_received = s->resp_len;
    // (on a failure the half-read body is dropped; only HTTP_STATUS keeps the server's explanation)
    if (s->res.error != NP_HTTPS_OK && s->res.error != NP_HTTPS_E_HTTP_STATUS) {
        free(s->resp);
        s->resp = nullptr;
        s->resp_len = s->resp_cap = 0;
        s->res.bytes_received = 0;
    }
    Log("%s %s:%u -> %s%s%s%s status=%d tls=%s%s %ums %uB", s->method, s->host, (unsigned)s->port, kErrorNames[s->res.error],
        s->res.message[0] ? " (" : "", s->res.message[0] ? s->res.message : "", s->res.message[0] ? ")" : "", s->res.http_status,
        s->res.tls_version == 0x0304 ? "1.3" : (s->res.tls_version == 0x0303 ? "1.2" : "-"), s->res.reused_connection ? " reused" : "",
        (unsigned)s->res.elapsed_ms, (unsigned)s->res.bytes_received);   // (%u: the Vita libc has no %zu)
    // netport31: the socket budget (np_https_sys.h). A failed request keeps no connection, so none may be open after it; and a running
    // count for the console log (every 20 sockets, and after each failure that opened one).
    {
        const SockCounts k = SockStats(SOCK_POOL_HTTPS);
        const bool failed = s->res.error != NP_HTTPS_OK && s->res.error != NP_HTTPS_E_HTTP_STATUS;
        static int said = 0;
        if (failed && k.live != 0) Log("socket budget: %d socket(s) still open after a failed request (opened %d, closed %d)", k.live, k.opened, k.closed);
        else if (k.opened >= said + 20 || (failed && k.opened != said)) {
            said = k.opened;
            Log("sockets: %d open (%d epoll), %d opened and %d closed so far, most at once %d, refused %d", k.live, k.epolls, k.opened, k.closed, k.most,
                k.refused);
        }
    }
}

#else  // no TLS backend

void RunRequest(Slot* s) {
    memset(&s->res, 0, sizeof s->res);
    s->res.error = NP_HTTPS_E_UNAVAILABLE;
    s->res.stage = NP_HTTPS_S_DONE;
    snprintf(s->res.message, sizeof s->res.message, "built without TLS");
}
void ConnClose(bool) {}

#endif  // NP_HTTPS_HAVE_TLS

// ---------------------------------------------------------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------------------------------------------------------

void WorkerMain(void*) {
    for (;;) {
        Slot* next = nullptr;
        g_lock.lock();
        uint32_t best = 0;
        for (int i = 0; i < NP_HTTPS_SLOTS; i++) {
            Slot* s = &g_slots[i];
            if (s->state == SS_QUEUED && (!next || (int32_t)(s->seq - best) < 0)) { next = s; best = s->seq; }
        }
        if (next) next->state = SS_RUNNING;
        g_lock.unlock();
        if (g_quit) {
            if (next) { g_lock.lock(); next->state = SS_QUEUED; g_lock.unlock(); }
            break;
        }
        if (!next) {
            EventWait(g_event, 1000);
#if defined(NP_HTTPS_HAVE_TLS)
            if (g_conn.open && MonoMs() - g_conn.last_used > 10000) ConnClose(true);   // an idle kept connection is not held longer than its life
#endif
            continue;
        }
        RunRequest(next);
        g_lock.lock();
        if (next->released) {
            FreeSlotMemory(next);
            next->state = SS_FREE;
            next->released = false;
        } else {
            next->state = SS_DONE;
        }
        g_lock.unlock();
    }
#if defined(NP_HTTPS_HAVE_TLS)
    ConnClose(true);
#endif
}

bool EnsureWorker() {
    if (g_running) return true;
    g_workerLock.lock();
    bool ok = true;
    if (!g_running) {
        g_quit = 0;
        if (!g_event) g_event = EventCreate();
        if (g_event) g_thread = ThreadStart(WorkerMain, nullptr);
        ok = g_event && g_thread;
        if (ok) g_running = 1;
    }
    g_workerLock.unlock();
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------
// request validation
// ---------------------------------------------------------------------------------------------------------------------------

bool ValidHeaderLine(const char* line) {
    const char* colon = strchr(line, ':');
    if (!colon || colon == line) return false;
    for (const char* p = line; p < colon; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return false;
    for (const char* p = colon + 1; *p; p++)
        if ((unsigned char)*p < 0x20 && *p != '\t') return false;   // no CR, LF, NUL or controls
    const size_t nl = (size_t)(colon - line);
    static const char* const kReserved[] = {"host", "content-length", "content-type", "connection", "transfer-encoding", "accept-encoding", "upgrade",
                                            "te", "trailer", "expect", "user-agent"};
    for (const char* r : kReserved)
        if (strlen(r) == nl && strncasecmp(line, r, nl) == 0) return false;
    return true;
}

// "https://host[:port]/path?query" -> host, port, path
bool ParseUrl(const char* url, Slot* s) {
    if (!url || strlen(url) > NP_HTTPS_MAX_URL) return false;
    if (strncasecmp(url, "https://", 8) != 0) return false;
    const char* a = url + 8;
    const char* end = a;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    if (*end == '#') return false;
    for (const char* p = url; *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7F || *p == '#') return false;
    const char* colon = nullptr;
    for (const char* p = a; p < end; p++) {
        if (*p == '@' || *p == '[' || *p == ']') return false;   // no userinfo, no IPv6 literals
        if (*p == ':') colon = p;
    }
    const char* hostEnd = colon ? colon : end;
    const size_t hl = (size_t)(hostEnd - a);
    if (hl == 0 || hl > 253) return false;
    for (const char* p = a; p < hostEnd; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '.')) return false;
    if (a[0] == '.' || a[0] == '-' || hostEnd[-1] == '-') return false;
    memcpy(s->host, a, hl);
    s->host[hl] = 0;
    s->port = 443;
    if (colon) {
        const char* q = colon + 1;
        if (q == end || end - q > 5) return false;
        uint32_t port = 0;
        for (; q < end; q++) {
            if (!isdigit((unsigned char)*q)) return false;
            port = port * 10 + (uint32_t)(*q - '0');
        }
        if (port == 0 || port > 65535) return false;
        s->port = (uint16_t)port;
    }
    if (*end == 0) strcpy(s->path, "/");
    else if (*end == '?') { s->path[0] = '/'; strcpy(s->path + 1, end); }
    else strcpy(s->path, end);
    return true;
}

}  // namespace

// =============================================================================================================================
// public API
// =============================================================================================================================

extern "C" {

NpHttpsHandle np_https_start(const NpHttpsRequest* rq, NpHttpsError* err) {
    NpHttpsError dummy;
    if (!err) err = &dummy;
    *err = NP_HTTPS_OK;
#if !defined(NP_HTTPS_HAVE_TLS)
    (void)rq;
    *err = NP_HTTPS_E_UNAVAILABLE;
    return 0;
#else
    if (!rq || !rq->url) { *err = NP_HTTPS_E_BAD_REQUEST; return 0; }
    // Validate into a scratch copy so a refused request costs the table nothing.
    Slot* t = (Slot*)calloc(1, sizeof(Slot));
    if (!t) { *err = NP_HTTPS_E_MEMORY; return 0; }
    const char* method = rq->method ? rq->method : (rq->body_len ? "POST" : "GET");
    bool ok = ParseUrl(rq->url, t);
    if (strcmp(method, "GET") == 0) ok = ok && rq->body_len == 0;
    else if (strcmp(method, "POST") == 0) ok = ok;
    else if (strcmp(method, "DELETE") == 0) ok = ok && rq->body_len == 0;   // (the account's own delete: no body, like GET)
    else if (strcmp(method, "PUT") == 0) ok = ok;   // (the rename, PUT /v1/me: a body like POST)
    else ok = false;
    if (ok) snprintf(t->method, sizeof t->method, "%s", method);
    if (rq->body_len > NP_HTTPS_MAX_REQUEST_BODY || (rq->body_len && !rq->body)) ok = false;
    const char* ctype = rq->content_type ? rq->content_type : "application/json";
    if (ok) {
        if (strlen(ctype) >= sizeof t->ctype) ok = false;
        for (const char* p = ctype; ok && *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7F) ok = false;
        if (ok) strcpy(t->ctype, ctype);
    }
    size_t hl = 0;
    if (ok && rq->headers) {
        int count = 0;
        for (const char* const* h = rq->headers; *h; h++) {
            const size_t l = strlen(*h);
            // netport31 (Strikers onmatch23): a "Content-Type: x" line is the request's content type (as rq->content_type), not an extra
            // header. A caller written against the desktop's plain client may name it that way; refusing the line refused the whole request
            // on the console only. It is still written once, by us, and only with a body.
            if (strncasecmp(*h, "Content-Type:", 13) == 0) {
                const char* v = *h + 13;
                while (*v == ' ' || *v == '\t') v++;
                bool fine = *v != 0 && strlen(v) < sizeof t->ctype;
                for (const char* p = v; fine && *p; p++)
                    if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7F) fine = false;
                if (!fine) { ok = false; break; }
                strcpy(t->ctype, v);
                continue;
            }
            if (++count > NP_HTTPS_MAX_HEADERS || !ValidHeaderLine(*h) || hl + l + 2 > NP_HTTPS_MAX_HEADER_BYTES) { ok = false; break; }
            memcpy(t->headers + hl, *h, l);
            hl += l;
            t->headers[hl++] = '\r';
            t->headers[hl++] = '\n';
        }
        t->headers[hl] = 0;
    }
    if (!ok) { free(t); *err = NP_HTTPS_E_BAD_REQUEST; return 0; }
    uint8_t* body = nullptr;
    if (rq->body_len) {
        body = (uint8_t*)malloc(rq->body_len);
        if (!body) { free(t); *err = NP_HTTPS_E_MEMORY; return 0; }
        memcpy(body, rq->body, rq->body_len);
    }
    t->t_connect = rq->connect_timeout_ms ? rq->connect_timeout_ms : 10000;
    t->t_io = rq->io_timeout_ms ? rq->io_timeout_ms : DefaultIoMs();   // 10 s; a held poll asks for its hold + a margin (io_timeout_ms)
    t->t_total = rq->total_timeout_ms ? rq->total_timeout_ms : 30000;
    if (t->t_total > 120000) t->t_total = 120000;
    if (t->t_connect > t->t_total) t->t_connect = t->t_total;
    t->max_response = rq->max_response ? rq->max_response : NP_HTTPS_DEFAULT_MAX_RESPONSE;
    if (t->max_response > NP_HTTPS_MAX_RESPONSE_LIMIT) t->max_response = NP_HTTPS_MAX_RESPONSE_LIMIT;
    t->keep_alive = rq->keep_alive != 0;

    if (!EnsureWorker()) { free(body); free(t); *err = NP_HTTPS_E_MEMORY; return 0; }
    g_lock.lock();
    Slot* s = nullptr;
    int index = -1;
    for (int i = 0; i < NP_HTTPS_SLOTS; i++)
        if (g_slots[i].state == SS_FREE) { s = &g_slots[i]; index = i; break; }
    if (!s) {
        g_lock.unlock();
        free(body);
        free(t);
        *err = NP_HTTPS_E_BUSY;
        return 0;
    }
    uint32_t gen = ++g_genCounter & 0xFFFFFFu;
    if (gen == 0) gen = ++g_genCounter & 0xFFFFFFu;
    *s = *t;                     // (copies the parsed request; the rest of t is zero)
    s->gen = gen;
    s->seq = ++g_seqCounter;
    s->body = body;
    s->body_len = rq->body_len;
    s->cancel = 0;
    s->released = false;
    s->stage = NP_HTTPS_S_QUEUED;
    s->got = 0;
    s->resp = nullptr;
    s->resp_len = s->resp_cap = 0;
    memset(&s->res, 0, sizeof s->res);
    s->state = SS_QUEUED;
    g_lock.unlock();
    free(t);
    EventSignal(g_event);
    return MakeHandle(gen, index);
#endif
}

int np_https_poll(NpHttpsHandle h, NpHttpsResult* out) {
    g_lock.lock();
    Slot* s = SlotOf(h);
    if (!s || s->released) { g_lock.unlock(); return -1; }
    if (s->state != SS_DONE) {
        if (out) {
            memset(out, 0, sizeof *out);
            out->stage = (NpHttpsStage)s->stage;
            out->bytes_received = s->got;
            out->body = kEmpty;
            out->elapsed_ms = s->t_start ? (uint32_t)(MonoMs() - s->t_start) : 0;
        }
        g_lock.unlock();
        return 0;
    }
    if (out) {
        *out = s->res;
        out->body = s->resp ? s->resp : kEmpty;
        out->body_len = s->resp ? s->resp_len : 0;
    }
    g_lock.unlock();
    return 1;
}

void np_https_cancel(NpHttpsHandle h) {
    bool abort_dns = false;
    g_lock.lock();
    Slot* s = SlotOf(h);
    if (s && !s->released) {
        if (s->state == SS_QUEUED) {
            memset(&s->res, 0, sizeof s->res);
            s->res.error = NP_HTTPS_E_CANCELLED;
            s->res.stage = NP_HTTPS_S_QUEUED;
            snprintf(s->res.message, sizeof s->res.message, "cancelled");
            s->state = SS_DONE;
        } else if (s->state == SS_RUNNING) {
            s->cancel = 1;
            abort_dns = true;
        }
    }
    g_lock.unlock();
    if (abort_dns) ResolveAbort();
}

void np_https_release(NpHttpsHandle h) {
    bool abort_dns = false;
    g_lock.lock();
    Slot* s = SlotOf(h);
    if (s && !s->released) {
        if (s->state == SS_RUNNING) {
            s->cancel = 1;
            s->released = true;   // the worker frees it
            abort_dns = true;
        } else {
            FreeSlotMemory(s);
            s->state = SS_FREE;
        }
    }
    g_lock.unlock();
    if (abort_dns) ResolveAbort();
}

void np_https_shutdown(uint32_t timeout_ms) {
    g_workerLock.lock();
    if (g_running) {
        g_quit = 1;
        g_lock.lock();
        for (int i = 0; i < NP_HTTPS_SLOTS; i++)
            if (g_slots[i].state == SS_RUNNING) g_slots[i].cancel = 1;
        g_lock.unlock();
        ResolveAbort();
        EventSignal(g_event);
        if (ThreadJoin(g_thread, timeout_ms)) {
            g_thread = nullptr;
            g_running = 0;
            EventDestroy(g_event);
            g_event = nullptr;
            g_lock.lock();
            for (int i = 0; i < NP_HTTPS_SLOTS; i++) {
                FreeSlotMemory(&g_slots[i]);
                g_slots[i].state = SS_FREE;
                g_slots[i].released = false;
            }
            g_lock.unlock();
        }
    }
    g_workerLock.unlock();
}

int np_https_available(void) {
#if defined(NP_HTTPS_HAVE_TLS)
    return tls::State() >= 0;
#else
    return 0;
#endif
}

int np_https_last_clock_fault(void) {
    // (the earlier-than-this-build case is read live, so setting the clock right clears it with no network)
    return __atomic_load_n(&g_clockFault, __ATOMIC_RELAXED) || np_https_clock_ok() == 0;
}

int np_https_clock_ok(void) {
#if defined(NP_HTTPS_HAVE_TLS)
    const int64_t now = WallSeconds();
    if (now <= 0) return -1;
    return now >= BuildFloor() ? 1 : 0;
#else
    return -1;
#endif
}

const char* np_https_backend_name(void) {
#if defined(NP_HTTPS_HAVE_TLS)
    return tls::BackendName();
#else
    return "none";
#endif
}

void np_https_clock_text(char* out, size_t cap) {
#if defined(NP_HTTPS_HAVE_TLS)
    char now[32], floor_[32];
    UtcText(WallSeconds(), now, sizeof now);
    UtcText(BuildFloor(), floor_, sizeof floor_);
    snprintf(out, cap, "%s (accepted from %s)", now, floor_);
#else
    snprintf(out, cap, "n/a");
#endif
}

const char* np_https_error_name(NpHttpsError e) { return (unsigned)e < sizeof kErrorNames / sizeof *kErrorNames ? kErrorNames[e] : "?"; }
const char* np_https_error_text(NpHttpsError e) { return (unsigned)e < sizeof kErrorTexts / sizeof *kErrorTexts ? kErrorTexts[e] : "?"; }
int np_https_waiting(NpHttpsHandle except) {
    int n = 0;
    g_lock.lock();
    const Slot* own = except ? SlotOf(except) : nullptr;
    for (int i = 0; i < NP_HTTPS_SLOTS; i++)
        if (g_slots[i].state == SS_QUEUED && &g_slots[i] != own && !g_slots[i].released) n++;
    g_lock.unlock();
    return n;
}

void np_https_socket_stats(int* live, int* opened, int* closed, int* refused) {
    const SockCounts k = SockStats(SOCK_POOL_HTTPS);
    if (live) *live = k.live;
    if (opened) *opened = k.opened;
    if (closed) *closed = k.closed;
    if (refused) *refused = k.refused;
}

const char* np_https_stage_name(NpHttpsStage s) { return (unsigned)s < sizeof kStageNames / sizeof *kStageNames ? kStageNames[s] : "?"; }

#ifdef NP_HTTPS_TEST_HOOKS
void np_https_test_set_roots_pem(const char* pem, size_t len) {
#if defined(NP_HTTPS_HAVE_TLS)
    tls::SetTestRoots(pem, len);
#else
    (void)pem; (void)len;
#endif
}
void np_https_test_set_clock(int64_t (*unix_seconds)(void)) { nph::g_wall_override = unix_seconds; }
void np_https_test_set_build_floor(int64_t unix_seconds) { g_floorOverride = unix_seconds; }
void np_https_test_set_default_io_ms(uint32_t ms) { g_defaultIoOverride = ms; }
void np_https_test_reset_tls(void) {
    // only between requests: the worker must be idle (tests wait for every handle first), so shut it down for the reset
    np_https_shutdown(5000);
#if defined(NP_HTTPS_HAVE_TLS)
    tls::Reset();
#endif
}
int np_https_test_root_count(void) {
#if defined(NP_HTTPS_HAVE_TLS)
    return tls::RootCount();
#else
    return 0;
#endif
}
#endif

}  // extern "C"
