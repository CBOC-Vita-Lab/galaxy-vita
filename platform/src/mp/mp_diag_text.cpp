// Galaxy 3.0 multiplayer (beta diagnostics): the pure text side of the problem report and the session summary. No file, no network, no
// globals: tests/mp/diag_test.cpp runs it. See port/mp_diag.h.
#include "port/mp_diag.h"
#include "port/mp_bridge.h"

#include "mp_report.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct W {   // a bounded writer: never past cap - 1, remembers a cut
    char* p;
    size_t cap, n;
    bool cut;
    void put(char c) { if (n + 1 < cap) p[n++] = c; else cut = true; }
    void puts(const char* s) { while (*s) put(*s++); }
    void fmt(const char* f, ...) __attribute__((format(printf, 2, 3)));
};
void W::fmt(const char* f, ...) {
    char t[96];
    va_list ap;
    va_start(ap, f);
    const int k = vsnprintf(t, sizeof t, f, ap);
    va_end(ap);
    if (k > 0) puts(t);
}

float Fin(float v) { return (v != v || v < 0.0f) ? 0.0f : (v > 1e9f ? 1e9f : v); }   // (never NaN / inf / negative in the JSON)

// Length of the valid UTF-8 sequence at s[0..n) (1..4), 0 when it is not valid.
size_t Utf8Len(const unsigned char* s, size_t n) {
    if (n == 0) return 0;
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) return n >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xE0 && s[1] < 0xA0) return 0;    // overlong
        if (s[0] == 0xED && s[1] >= 0xA0) return 0;   // surrogate
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xF0 && s[1] < 0x90) return 0;
        if (s[0] == 0xF4 && s[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

// The escaped length and (when w is given) the text.
size_t Escape(const char* in, size_t n, W* w) {
    size_t need = 0;
    auto emit = [&](const char* s, size_t k) { need += k; if (w) for (size_t i = 0; i < k; i++) w->put(s[i]); };
    const unsigned char* u = reinterpret_cast<const unsigned char*>(in);
    size_t i = 0;
    while (i < n) {
        const unsigned char c = u[i];
        if (c == '"') { emit("\\\"", 2); ++i; }
        else if (c == '\\') { emit("\\\\", 2); ++i; }
        else if (c == '\n') { emit("\\n", 2); ++i; }
        else if (c == '\r') { emit("\\r", 2); ++i; }
        else if (c == '\t') { emit("\\t", 2); ++i; }
        else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); emit(b, 6); ++i; }
        else if (c < 0x80) { emit(in + i, 1); ++i; }
        else {
            const size_t l = Utf8Len(u + i, n - i);
            if (l) { emit(in + i, l); i += l; } else { emit("?", 1); ++i; }
        }
    }
    return need;
}

bool Safe(const char* s) {   // a short code (kind, where, nat, ...): letters, digits, underscore, colon, dash, dot
    if (!s) return false;
    for (; *s; ++s) {
        const char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '-' || c == '.')) return false;
    }
    return true;
}

void Str(W& w, const char* key, const char* v) {
    w.fmt("\"%s\":\"", key);
    char t[160];
    W e{t, sizeof t, 0, false};
    Escape(v, strlen(v), &e);
    t[e.n] = 0;
    w.puts(t);
    w.put('"');
}

}  // namespace

extern "C" size_t mp_diag_json_escape(const char* in, size_t n, char* out, size_t cap) {
    if (!out || cap == 0) return in ? Escape(in, n, nullptr) : 0;
    W w{out, cap, 0, false};
    const size_t need = in ? Escape(in, n, &w) : 0;
    out[w.n] = 0;
    return need;
}

extern "C" size_t mp_diag_summary_json(const MpDiagSummary* s, char* out, size_t cap) {
    if (!s || !out || cap == 0) return 0;
    W w{out, cap, 0, false};
    const char* tr = !strcmp(s->transport, "relay") ? "relay" : "udp";
    w.fmt("{\"transport\":\"%s\",\"dur_s\":%u,\"members\":%u,", tr, (unsigned)s->durS, (unsigned)s->members);
    Str(w, "ended", s->ended[0] ? s->ended : "leave");
    w.fmt(",\"rx_rate\":%.1f,\"gap_mean_ms\":%.1f,\"gap_max_ms\":%.1f,\"buf_mean_ms\":%.1f,\"buf_max_ms\":%.1f,", Fin(s->rxRate), Fin(s->gapMeanMs),
          Fin(s->gapMaxMs), Fin(s->bufMeanMs), Fin(s->bufMaxMs));
    w.fmt("\"extrap\":%u,\"hold\":%u,\"snap\":%u,\"late\":%u,\"stall100\":%u,\"tx_fail\":%u,\"relay_used\":%s,", (unsigned)s->extrap, (unsigned)s->hold,
          (unsigned)s->snap, (unsigned)s->late, (unsigned)s->stall100, (unsigned)s->txFail, s->relayUsed ? "true" : "false");
    Str(w, "app", s->app[0] ? s->app : "3.0.0");
    w.put('}');
    out[w.n] = 0;
    if (w.cut) { out[0] = 0; return 0; }
    return w.n;
}

extern "C" size_t mp_diag_check_json(const MpDiagCheck* c, char* out, size_t cap) {
    if (!c || !out || cap == 0) return 0;
    W w{out, cap, 0, false};
    const char* nat = c->nat[0] ? c->nat : "unknown";
    const char* relay = !strcmp(c->relay, "ok") || !strcmp(c->relay, "fail") ? c->relay : "skipped";
    w.puts("{");
    Str(w, "nat", nat);
    w.fmt(",\"stun_ms\":%d,", c->stunMs < 0 ? -1 : c->stunMs);
    Str(w, "relay", relay);
    w.fmt(",\"relay_rtt_ms\":%d,", c->relayRttMs < 0 ? -1 : c->relayRttMs);
    Str(w, "err", c->err);
    w.put('}');
    out[w.n] = 0;
    if (w.cut) { out[0] = 0; return 0; }
    return w.n;
}

extern "C" size_t mp_diag_build_body(const MpDiagBodyIn* in, char* out, size_t cap, size_t limit, size_t* logBytes) {
    if (logBytes) *logBytes = 0;
    if (!in || !out || cap < 256 || !Safe(in->kind) || !Safe(in->where)) return 0;
    if (limit >= cap) limit = cap - 1;
    // the note: at most MP_DIAG_NOTE_MAX characters, cut at a character boundary
    char note[4 * MP_DIAG_NOTE_MAX + 4];
    size_t nl = 0;
    if (in->note) {
        const unsigned char* u = reinterpret_cast<const unsigned char*>(in->note);
        const size_t n = strlen(in->note);
        size_t i = 0;
        int chars = 0;
        while (i < n && chars < MP_DIAG_NOTE_MAX) {
            size_t l = Utf8Len(u + i, n - i);
            if (!l) l = 1;   // (the escape turns it into '?')
            if (nl + l >= sizeof note) break;
            memcpy(note + nl, u + i, l);
            nl += l;
            i += l;
            ++chars;
        }
    }
    char tmp[1024];
    W w{out, cap, 0, false};
    w.fmt("{\"v\":1,\"kind\":\"%s\",\"where\":\"%s\",\"note\":\"", in->kind, in->where);
    {
        char e[8 * MP_DIAG_NOTE_MAX + 8];
        W ew{e, sizeof e, 0, false};
        Escape(note, nl, &ew);
        e[ew.n] = 0;
        w.puts(e);
    }
    w.puts("\",\"check\":");
    if (in->check && in->check->valid && mp_diag_check_json(in->check, tmp, sizeof tmp)) w.puts(tmp); else w.puts("null");
    w.puts(",\"stats\":");
    if (in->stats && in->stats->valid && mp_diag_summary_json(in->stats, tmp, sizeof tmp)) w.puts(tmp); else w.puts("null");
    w.puts(",\"log\":\"");
    const size_t head = w.n;
    const char* tail = "\"}";
    // the log: filtered and scrubbed, the newest lines kept; cut further until the escaped body fits the limit
    size_t logCap = MP_DIAG_LOG_MAX;
    char* payload = nullptr;
    size_t pl = 0;
    for (int round = 0; round < 8; round++) {
        free(payload);
        payload = static_cast<char*>(malloc(logCap + 1));
        if (!payload) return 0;
        pl = mp_report_log_payload(in->logPrev, in->logPrevLen, in->logPrevIsTail, in->logCur, in->logCurLen, in->logCurIsTail, payload, logCap + 1);
        const size_t need = Escape(payload, pl, nullptr);
        if (head + need + strlen(tail) <= limit) break;
        const size_t budget = limit > head + strlen(tail) ? limit - head - strlen(tail) : 0;
        if (budget < 256 || round == 7) { logCap = 0; break; }
        // the escaped text is longer than the raw: shrink the raw in the same proportion (and a little more), keeping the newest lines
        size_t next = static_cast<size_t>(static_cast<double>(pl) * static_cast<double>(budget) / static_cast<double>(need) * 0.96);
        if (next >= logCap) next = logCap - logCap / 16;
        logCap = next;
        if (logCap < 128) { logCap = 0; break; }
    }
    if (logCap == 0) pl = 0;
    w.n = head;
    w.cut = false;
    Escape(payload, pl, &w);
    free(payload);
    w.puts(tail);
    out[w.n] = 0;
    if (w.cut || w.n > limit) { out[0] = 0; return 0; }
    if (logBytes) *logBytes = pl;
    return w.n;
}

// Which session endings are worth the "Something went wrong last time. Send a report?" offer: unexpected faults only. Every MpUiError is named (no default
// in the switch: a new code makes the compiler warn, and tests/mp/diag_test.cpp lists them all), anything outside the enum counts as internal.
extern "C" bool mp_diag_error_offers_report(int err) {
    switch (static_cast<MpUiError>(err)) {
    case MP_ERR_TIMEOUT: case MP_ERR_NAT: case MP_ERR_SERVICE_DOWN: case MP_ERR_INTERNAL: return true;
    case MP_ERR_NONE: case MP_ERR_NO_WIFI: case MP_ERR_UPDATE_REQUIRED: case MP_ERR_ROOM_NOT_FOUND: case MP_ERR_ROOM_FULL: case MP_ERR_DIFFERENT_VERSION:
    case MP_ERR_HOST_LEFT: case MP_ERR_KICKED: case MP_ERR_NAME_TAKEN: case MP_ERR_BAD_NAME: case MP_ERR_RATE_LIMITED: case MP_ERR_CLOCK: return false;
    }
    return true;   // (an unknown code maps to internal)
}

// The relay loopback's probes leave from a second socket: 0 = its public address is unknown (STUN gave no answer: keep the permission for A's IP only),
// 1 = same public IP as the first socket, 2 = a different one (the relay needs a permission for that IP too).
extern "C" int mp_diag_probe_source(uint32_t aIp, uint32_t bIp, bool bAnswered) {
    if (!bAnswered || bIp == 0) return 0;
    return aIp == bIp ? 1 : 2;
}
