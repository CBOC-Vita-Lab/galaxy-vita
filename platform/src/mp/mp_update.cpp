// Galaxy 3.0 multiplayer (release3): the "newer version available" notice. See mp_update.h.
#include "mp_update.h"

#include <atomic>
#include <ctype.h>
#include <string.h>

extern "C" const char* mp_service_app_version(void) __attribute__((weak));   // (mp_service.cpp; a test links without it)
extern "C" void mp_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));

namespace {

struct Ver {
    unsigned n[4];
};

// "3.0.0" / "3.1" / "3.0.0-rc1": false if it is anything else (letters in the numbers, empty parts, too long, too many parts).
bool Parse(const char* s, Ver* v) {
    if (!s || !*s) return false;
    memset(v, 0, sizeof *v);
    int part = 0, digits = 0;
    unsigned cur = 0;
    bool any = false;
    for (const char* p = s;; ++p) {
        const char c = *p;
        if (c >= '0' && c <= '9') {
            if (++digits > 4) return false;
            cur = cur * 10 + static_cast<unsigned>(c - '0');
            any = true;
        } else if (c == '.' || c == '-' || c == 0) {
            if (!any) return false;   // "", ".1", "3..0", "-rc1"
            if (part >= 4) return false;
            v->n[part++] = cur;
            cur = 0;
            digits = 0;
            any = false;
            if (c == 0) return true;
            if (c == '-') {   // the suffix: [a-z0-9.]{1,12}, ignored for the order
                const char* q = p + 1;
                size_t k = 0;
                for (; *q; ++q, ++k)
                    if (!(islower(static_cast<unsigned char>(*q)) || isdigit(static_cast<unsigned char>(*q)) || *q == '.')) return false;
                return k >= 1 && k <= 12;
            }
        } else {
            return false;
        }
    }
}

std::atomic<int> sState{0};        // 0 nothing known, 1 newer known (notice not taken yet), 2 newer known, notice taken
char sLatest[24] = {};             // written once (state 0 -> 1 by the compare_exchange winner), read after the state is seen
std::atomic<bool> sLatestSet{false};
char sCurrentOverride[24] = {};

const char* Current() {
    if (sCurrentOverride[0]) return sCurrentOverride;
    if (mp_service_app_version) return mp_service_app_version();
    return "3.0.0";
}

// Finds `"key"` followed by `:` and a string value in [p, end); copies at most cap-1 bytes of it. Plain scan: no escapes are valid
// in a version, so a backslash ends the search for this key.
bool FindString(const char* p, const char* end, const char* key, char* out, size_t cap) {
    const size_t kl = strlen(key);
    for (const char* q = p; q + kl + 2 < end; ++q) {
        if (*q != '"' || memcmp(q + 1, key, kl) != 0 || q[1 + kl] != '"') continue;
        const char* r = q + 2 + kl;
        while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) ++r;
        if (r >= end || *r != ':') continue;
        ++r;
        while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) ++r;
        if (r >= end || *r != '"') continue;
        ++r;
        size_t n = 0;
        while (r < end && *r != '"' && *r != '\\' && n + 1 < cap) out[n++] = *r++;
        if (r >= end || *r != '"') continue;   // too long, escaped or unterminated: not a version
        out[n] = 0;
        return true;
    }
    return false;
}

}  // namespace

extern "C" {

int mp_version_compare(const char* a, const char* b) {
    Ver x, y;
    if (!Parse(a, &x) || !Parse(b, &y)) return 2;
    for (int i = 0; i < 4; i++) {
        if (x.n[i] != y.n[i]) return x.n[i] < y.n[i] ? -1 : 1;
    }
    return 0;
}

bool mp_update_note_latest(const char* latest) {
    if (mp_version_compare(latest, Current()) != 1) return false;   // not newer (or not a version at all)
    int expect = 0;
    if (sState.compare_exchange_strong(expect, 1)) {
        strncpy(sLatest, latest, sizeof sLatest - 1);
        sLatest[sizeof sLatest - 1] = 0;
        sLatestSet.store(true);
        if (mp_log) mp_log("update: a newer version is available (this build %s, service says %s)", Current(), sLatest);
    }
    return true;
}

bool mp_update_scan_body(const char* body, size_t len) {
    if (!body || len == 0 || len > 64 * 1024) return false;
    char v[24];
    const char* end = body + len;
    // an error answer: {"error": {"code": ..., "latestVersion": "3.1.0"}}
    if (FindString(body, end, "latestVersion", v, sizeof v) && mp_update_note_latest(v)) return true;
    // an info answer: {..., "app": {"min": "3.0.0", "latest": "3.1.0"}}: "latest" after the key "app"
    for (const char* q = body; q + 6 < end; ++q) {
        if (memcmp(q, "\"app\"", 5) != 0) continue;
        if (FindString(q + 5, end, "latest", v, sizeof v)) return mp_update_note_latest(v);
        break;
    }
    return false;
}

bool mp_update_available(void) { return sState.load() != 0; }

const char* mp_update_latest(void) { return sLatestSet.load() ? sLatest : ""; }

bool mp_update_take_notice(void) {
    int expect = 1;
    return sState.compare_exchange_strong(expect, 2);   // 1 -> 2 once; 0 and 2 give false
}

void mp_update_set_current(const char* version) {
    sCurrentOverride[0] = 0;
    if (version) {
        strncpy(sCurrentOverride, version, sizeof sCurrentOverride - 1);
        sCurrentOverride[sizeof sCurrentOverride - 1] = 0;
    }
}

void mp_update_reset(void) {
    sState.store(0);
    sLatestSet.store(false);
    sLatest[0] = 0;
    sCurrentOverride[0] = 0;
}

}  // extern "C"
