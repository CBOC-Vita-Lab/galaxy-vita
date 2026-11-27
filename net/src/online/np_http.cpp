// Request table of the signaling's HTTP client: routes https:// to np_https and http:// to the plain desktop client (np_http_plain.cpp).
#include "np_http.h"
#include "../np_log.h"

#if defined(__has_include)
#if __has_include("../np_https.h")
#define NP_HAVE_NPHTTPS 1
#include "../np_https.h"
#endif
#endif

#include <stdio.h>
#include <string.h>

namespace NpNet {

#if !defined(NP_TARGET_VITA)
void* PlainStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs,
                 uint32_t ioTimeoutMs, std::string* err);
int PlainPoll(void* request, HttpResponse* out);
void PlainCancel(void* request);
#endif

namespace {

struct Slot {
    int handle;       // 0 = free
    bool tls;
    bool fired;       // nobody asks about it: HttpPump reaps it
    void* plain;
    uint32_t tlsHandle;
    Slot() : handle(0), tls(false), fired(false), plain(0), tlsHandle(0) {}
};
const int kSlots = 12;
Slot g_slots[kSlots];
int g_next = 1;
unsigned g_fireDropped = 0;   // s1117b: fire-and-forget requests that could not start (each also logged)
// The game thread is the only caller (a frame's worth of network work); no locks.

Slot* Find(int handle) {
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].handle == handle) return &g_slots[i];
    return 0;
}

int PollSlot(Slot* s, HttpResponse* out) {
#if defined(NP_HAVE_NPHTTPS)
    if (s->tls) {
        NpHttpsResult r;
        memset(&r, 0, sizeof r);
        const int rc = np_https_poll(s->tlsHandle, &r);
        if (rc == 0) return 0;
        if (rc < 0) {   // not a live handle any more: the slot would never be freed (nobody polls a dead handle again)
            np_https_release(s->tlsHandle);
            s->handle = 0;
            return -1;
        }
        if (r.error == NP_HTTPS_OK || r.error == NP_HTTPS_E_HTTP_STATUS) {
            out->error = 0;
            out->status = r.http_status;
            out->body.assign(reinterpret_cast<const char*>(r.body), r.body_len);
        } else {
            out->error = int(r.error);
            out->status = r.http_status;
            out->text = np_https_error_text(r.error);
            out->body.clear();
            NpLogf("np-net", "https request failed: %s (%s; stage %s, %s)\n", np_https_error_name(r.error), r.message, np_https_stage_name(r.stage),
                    r.error == NP_HTTPS_E_CERT || r.error == NP_HTTPS_E_CLOCK ? r.cert_detail : "");
        }
        np_https_release(s->tlsHandle);
        s->handle = 0;
        return 1;
    }
#endif
#if !defined(NP_TARGET_VITA)
    if (!s->tls) {
        const int rc = PlainPoll(s->plain, out);
        if (rc == 1) s->handle = 0;
        return rc;
    }
#endif
    (void)out;
    s->handle = 0;
    return -1;
}

}  // namespace

int HttpStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs, std::string* err) {
    return HttpStart(method, url, body, headers, timeoutMs, 0, err);
}

int HttpStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs,
              uint32_t ioTimeoutMs, std::string* err) {
    Slot* s = 0;
    for (int i = 0; i < kSlots && !s; ++i)
        if (!g_slots[i].handle) s = &g_slots[i];
    if (!s) { *err = "Too many requests at once."; return 0; }
    const bool tls = url.compare(0, 8, "https://") == 0;
    if (tls) {
#if defined(NP_HAVE_NPHTTPS)
        std::vector<const char*> hv;
        for (size_t i = 0; i < headers.size(); ++i) hv.push_back(headers[i].c_str());
        hv.push_back(0);
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = url.c_str();
        rq.method = method;
        rq.body = body.empty() ? 0 : body.data();
        rq.body_len = body.size();
        rq.headers = &hv[0];
        rq.total_timeout_ms = timeoutMs;
        rq.io_timeout_ms = ioTimeoutMs;   // 0: np_https' 10 s; a held poll: its hold + a margin
        rq.max_response = 32 * 1024;
        rq.keep_alive = 1;
        NpHttpsError e = NP_HTTPS_OK;
        const uint32_t h = np_https_start(&rq, &e);
        if (!h) { *err = np_https_error_text(e); return 0; }
        s->tls = true;
        s->tlsHandle = h;
#else
        *err = "Secure connections are not available in this build.";
        return 0;
#endif
    } else {
#if !defined(NP_TARGET_VITA)
        s->plain = PlainStart(method, url, body, headers, timeoutMs, ioTimeoutMs, err);
        if (!s->plain) return 0;
        s->tls = false;
#else
        *err = "Only secure (https) addresses are allowed.";
        return 0;
#endif
    }
    s->fired = false;
    s->handle = g_next++;
    if (g_next > 0x3fffffff) g_next = 1;
    return s->handle;
}

int HttpPoll(int handle, HttpResponse* out) {
    Slot* s = Find(handle);
    if (!s) return -1;
    return PollSlot(s, out);
}

void HttpCancel(int handle) {
    Slot* s = Find(handle);
    if (!s) return;
#if defined(NP_HAVE_NPHTTPS)
    if (s->tls) np_https_release(s->tlsHandle);
#endif
#if !defined(NP_TARGET_VITA)
    if (!s->tls) PlainCancel(s->plain);
#endif
    s->handle = 0;
}

#if !defined(NP_TARGET_VITA)
namespace { bool (*g_othersWaitingTest)(void) = 0; }
void HttpTestSetOthersWaiting(bool (*fn)(void)) { g_othersWaitingTest = fn; }
#endif

bool HttpOthersWaiting(int handle) {
    Slot* s = Find(handle);
    if (!s) return false;
#if !defined(NP_TARGET_VITA)
    if (g_othersWaitingTest) return g_othersWaitingTest();
#endif
#if defined(NP_HAVE_NPHTTPS)
    if (s->tls) return np_https_waiting(s->tlsHandle) > 0;
#endif
    return false;
}

unsigned HttpFireDropped() { return g_fireDropped; }

void HttpFire(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs) {
    std::string err;
    const int h = HttpStart(method, url, body, headers, timeoutMs, &err);
    if (!h) {   // s1117b (Strikers 1117b): a fire-and-forget request (room leave, relay usage) that cannot start is dropped, but never silently
        ++g_fireDropped;
        NpLogf("np-net", "fire-and-forget %s dropped (not started: %s)\n", method ? method : "?", err.c_str());
        return;
    }
    Find(h)->fired = true;
}

void HttpPump() {
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.handle && s.fired) {
            HttpResponse r;
            PollSlot(&s, &r);
        }
    }
}

}  // namespace NpNet
