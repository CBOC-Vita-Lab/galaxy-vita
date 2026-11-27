// quota32: telling "the service (or Cloudflare in front of it) is out of quota / overloaded" from the service's own refusals.
//
// On the Workers Free plan the account has 100,000 Worker requests and 100,000 Durable Object requests a day. Past the first, Cloudflare does not
// run the Worker and answers itself (the "1027 Worker exceeded free tier daily request limit" page, HTTP 429, an HTML or plain-text body); past
// the second, the Worker's call to the object throws (the Worker answers its own 503 "over_budget" since quota32, or Cloudflare's 1101 page, HTTP
// 500, on an older Worker). Cloudflare's rate-limit rules answer 429 with their own page too. None of those bodies is the service's JSON envelope
// {"error":{"code":...}}. The service's own refusals always are, so the rule is:
//   * a 429 or 5xx whose body carries no {"error":{"code":"..."}} is "busy" (an edge page, not ours);
//   * the service's own 503 over_budget / service_busy is "busy";
//   * any other answer carrying a service code (rate_limited, banned, internal, ...) is the service speaking: not "busy".
// Pure functions, no game headers: host-tested by tests/mp/svc_busy_test.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mp_svc {

// The "code" of a {"error":{"code":"x"}} envelope in the first 1 KiB of `body` (a light scan, not a parser: it only has to tell the service's
// envelope from an HTML / text page). Returns the length written to `out` (0 = no envelope).
inline size_t EnvelopeCode(const char* body, size_t len, char* out, size_t cap) {
    if (!body || !out || cap == 0) return 0;
    out[0] = 0;
    if (len > 1024) len = 1024;
    // the body must start (after white space) with '{': an HTML page or "error code: 1027" never does
    size_t i = 0;
    while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) ++i;
    if (i >= len || body[i] != '{') return 0;
    auto find = [&](const char* needle, size_t from) -> size_t {
        const size_t n = strlen(needle);
        for (size_t k = from; k + n <= len; ++k)
            if (memcmp(body + k, needle, n) == 0) return k;
        return size_t(-1);
    };
    size_t e = find("\"error\"", i);
    if (e == size_t(-1)) return 0;
    size_t c = find("\"code\"", e);
    if (c == size_t(-1)) return 0;
    c += 6;
    while (c < len && (body[c] == ' ' || body[c] == ':' || body[c] == '\t')) ++c;
    if (c >= len || body[c] != '"') return 0;
    ++c;
    size_t n = 0;
    while (c < len && body[c] != '"' && n + 1 < cap) out[n++] = body[c++];
    out[n] = 0;
    return n;
}

// True when this answer means "busy right now, try later" (see the top of the file). transport must be 0 (an HTTP answer arrived).
inline bool IsBusyAnswer(int transport, int status, const char* body, size_t len) {
    if (transport != 0) return false;
    char code[40];
    const bool ours = EnvelopeCode(body, len, code, sizeof code) > 0;
    if (ours) return !strcmp(code, "over_budget") || !strcmp(code, "service_busy");
    return status == 429 || status >= 500;
}

}  // namespace mp_svc
