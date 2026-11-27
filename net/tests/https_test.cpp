// Host tests of platform/sdk/net/np_https against a local HTTPS server with a private test CA (tools/net/https_server.py).
// Run by tools/https/run_https_test.sh. Each CHECK prints on failure; the exit code is the number of failures.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../src/np_https.h"
#include "../src/np_https_roots.h"   // (NP_HTTPS_ROOTS_DAY: the clock floor test)
#include "../src/online/np_http.h"    // netport31: the callers' request path (the room's held poll passes its hold on as the no-data limit)

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) { g_pass++; }                                             \
        else { g_fail++; printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static std::string g_dir;
static int Port(const char* name) {
    static std::string json;
    if (json.empty()) {
        FILE* f = fopen((g_dir + "/ports.json").c_str(), "rb");
        if (!f) { printf("no ports.json\n"); exit(2); }
        char buf[2048];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        json = buf;
    }
    const std::string key = std::string("\"") + name + "\":";
    const size_t at = json.find(key);
    if (at == std::string::npos) { printf("no port %s\n", name); exit(2); }
    return atoi(json.c_str() + at + key.size());
}
static std::string Url(const char* scenario, const char* path, const char* host = "localhost") {
    char b[256];
    snprintf(b, sizeof b, "https://%s:%d%s", host, Port(scenario), path);
    return b;
}

static int64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static double g_maxCallMs;   // the longest np_https_start / poll / cancel call of the whole run: the game thread must never wait
struct Timed {
    int64_t t;
    Timed() : t(NowMs()) {}
    ~Timed() { const double d = (double)(NowMs() - t); if (d > g_maxCallMs) g_maxCallMs = d; }
};

struct Answer {
    NpHttpsError start_err;
    NpHttpsResult r;
    std::string body;
    bool started;
};

// Starts, polls like a game would (every 4 ms), copies the result out.
static Answer Do(const NpHttpsRequest& rq, uint32_t limit_ms = 45000) {
    Answer a;
    memset(&a, 0, sizeof a.r);
    a.started = false;
    a.start_err = NP_HTTPS_OK;
    NpHttpsHandle h;
    { Timed t; h = np_https_start(&rq, &a.start_err); }
    if (!h) return a;
    a.started = true;
    const int64_t t0 = NowMs();
    for (;;) {
        int rc;
        { Timed t; rc = np_https_poll(h, &a.r); }
        if (rc != 0) break;
        if (NowMs() - t0 > limit_ms) { np_https_cancel(h); }
        if (NowMs() - t0 > limit_ms + 3000) { printf("  (test harness gave up)\n"); break; }
        usleep(4000);
    }
    a.body.assign((const char*)a.r.body, a.r.body_len);
    a.r.body = nullptr;
    np_https_release(h);
    return a;
}

static Answer Get(const std::string& url, uint32_t io = 0, uint32_t total = 0, uint32_t maxresp = 0, int keep = 0) {
    NpHttpsRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url.c_str();
    rq.io_timeout_ms = io;
    rq.total_timeout_ms = total;
    rq.max_response = maxresp;
    rq.keep_alive = keep;
    return Do(rq);
}

static void Show(const char* what, const Answer& a) {
    printf("  %-34s -> %s%s%s status=%d stage=%s tls=%x cert=0x%x %ums %zuB\n", what, np_https_error_name(a.r.error), a.r.message[0] ? " (" : "", a.r.message[0] ? a.r.message : "",
           a.r.http_status, np_https_stage_name(a.r.stage), a.r.tls_version, a.r.cert_flags, a.r.elapsed_ms, a.body.size());
}

static std::string ReadFile(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", path.c_str()); exit(2); }
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static int64_t g_clock;
static int64_t FakeClock() { return g_clock; }

static void UseTestRoots() {
    const std::string pem = ReadFile(g_dir + "/pki/trusted.pem");
    np_https_test_set_roots_pem(pem.c_str(), pem.size() + 1);
    np_https_test_reset_tls();
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: https_test DIR\n"); return 2; }
    g_dir = argv[1];
    printf("np_https host tests (dir %s)\n", argv[1]);
    UseTestRoots();

    printf("[good certificates]\n");
    {
        Answer a = Get(Url("good", "/hello"));
        Show("TLS 1.3 or 1.2, EC leaf", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.http_status == 200 && a.body == "hello from the test server\n", "body '%s'", a.body.c_str());
        CHECK(a.r.tls_version == 0x0304, "tls %x", a.r.tls_version);
        CHECK(np_https_test_root_count() == 2, "roots %d", np_https_test_root_count());
        a = Get(Url("tls12", "/hello"));
        Show("TLS 1.2 only server", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.tls_version == 0x0303, "err %s tls %x", np_https_error_name(a.r.error), a.r.tls_version);
        a = Get(Url("tls13", "/hello"));
        Show("TLS 1.3 only server", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.tls_version == 0x0304, "err %s", np_https_error_name(a.r.error));
        a = Get(Url("rsa", "/hello"));
        Show("RSA root -> intermediate -> leaf", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.http_status == 200, "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
    }

    printf("[certificate failures]\n");
    {
        // linkfix: np_https_last_clock_fault (the Connection Test's "set the date and time"): only a handshake refused for the dates sets it,
        // the next successful handshake clears it
        CHECK(np_https_last_clock_fault() == 0, "no fault before any request");
        Answer a = Get(Url("wronghost", "/hello"));
        Show("wrong host name", a);
        CHECK(a.r.error == NP_HTTPS_E_CERT && (a.r.cert_flags & NP_HTTPS_CERT_HOST_MISMATCH) && a.body.empty(), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        CHECK(np_https_last_clock_fault() == 0, "a wrong host name is not the clock");
        a = Get(Url("expired", "/hello"));
        Show("expired certificate", a);
        CHECK(a.r.error == NP_HTTPS_E_CLOCK && (a.r.cert_flags & NP_HTTPS_CERT_EXPIRED), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        CHECK(np_https_last_clock_fault() == 1, "expired: the clock is blamed");
        a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_OK && np_https_last_clock_fault() == 0, "the next good handshake clears it (err %s)", np_https_error_name(a.r.error));
        a = Get(Url("notyet", "/hello"));
        Show("certificate not yet valid", a);
        CHECK(a.r.error == NP_HTTPS_E_CLOCK && (a.r.cert_flags & NP_HTTPS_CERT_NOT_YET_VALID), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        CHECK(np_https_last_clock_fault() == 1, "not yet valid: the clock is blamed");
        a = Get(Url("expired", "/hello"));
        CHECK(np_https_last_clock_fault() == 1, "expired again");
        a = Get(Url("untrusted", "/hello"));
        Show("untrusted root", a);
        CHECK(a.r.error == NP_HTTPS_E_CERT && (a.r.cert_flags & NP_HTTPS_CERT_UNTRUSTED), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        CHECK(np_https_last_clock_fault() == 0, "a refusal for another reason (untrusted) clears it: each verification decides");
        a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_OK && np_https_last_clock_fault() == 0, "cleared again");
        // the bundled production roots do not know the test CA: a good test certificate is refused (also proves the bundled roots load and verification is on)
        np_https_test_set_roots_pem(nullptr, 0);
        np_https_test_reset_tls();
        a = Get(Url("good", "/hello"));
        Show("production roots vs test CA", a);
        CHECK(a.r.error == NP_HTTPS_E_CERT && (a.r.cert_flags & NP_HTTPS_CERT_UNTRUSTED), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        CHECK(np_https_test_root_count() == 8, "bundled roots %d", np_https_test_root_count());
        UseTestRoots();
    }

    printf("[fail closed]\n");
    {
        np_https_test_set_roots_pem("not a certificate", 18);
        np_https_test_reset_tls();
        Answer a = Get(Url("good", "/hello"));
        Show("garbage roots", a);
        CHECK(a.r.error == NP_HTTPS_E_UNAVAILABLE && a.r.stage != NP_HTTPS_S_TLS && a.body.empty(), "err %s", np_https_error_name(a.r.error));
        CHECK(np_https_available() == 0, "available after a failed init");
        np_https_test_set_roots_pem("", 1);
        np_https_test_reset_tls();
        a = Get(Url("good", "/hello"));
        Show("empty roots", a);
        CHECK(a.r.error == NP_HTTPS_E_UNAVAILABLE, "err %s", np_https_error_name(a.r.error));
        UseTestRoots();
        a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_OK, "recovered after reset: %s", np_https_error_name(a.r.error));
    }

    printf("[certificate date faults are a wrong clock]\n");
    {   // np_https_cert_is_date_fault: dates alone are a clock problem, any other fault next to them (or instead of them) is not
        CHECK(np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED), "expired");
        CHECK(np_https_cert_is_date_fault(NP_HTTPS_CERT_NOT_YET_VALID), "not yet valid");
        CHECK(np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_NOT_YET_VALID), "both");
        CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_HOST_MISMATCH), "expired + host");
        CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_UNTRUSTED), "expired + untrusted");
        CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_NOT_YET_VALID | NP_HTTPS_CERT_BAD_SIGNATURE), "not yet + signature");
        CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_UNTRUSTED), "untrusted");
        CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_HOST_MISMATCH), "host");
        CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_OTHER), "other");
        CHECK(!np_https_cert_is_date_fault(0), "none");
    }

    printf("[clock]\n");
    {
        g_clock = 1577836800;   // 2020-01-01: before this build
        np_https_test_set_clock(FakeClock);
        Answer a = Get(Url("good", "/hello"));
        Show("clock in 2020", a);
        CHECK(a.r.error == NP_HTTPS_E_CLOCK && a.r.stage == NP_HTTPS_S_QUEUED && a.r.http_status == 0, "err %s stage %s", np_https_error_name(a.r.error), np_https_stage_name(a.r.stage));
        CHECK(np_https_last_clock_fault() == 1, "a clock earlier than this build is a wrong date too");
        CHECK(np_https_clock_ok() == 0, "clock_ok %d", np_https_clock_ok());
        CHECK(strstr(np_https_error_text(NP_HTTPS_E_CLOCK), "date and time") != nullptr, "text");
        {   // the floor is the source's own fixed day (not the build date): a day before the later of it and the root list's day, to the second
            const int64_t later = NP_HTTPS_CLOCK_FLOOR_UTC > NP_HTTPS_ROOTS_DAY ? NP_HTTPS_CLOCK_FLOOR_UTC : NP_HTTPS_ROOTS_DAY;
            g_clock = later - 86400 - 1;
            CHECK(np_https_clock_ok() == 0, "one second before the floor");
            g_clock = later - 86400;
            CHECK(np_https_clock_ok() == 1, "at the floor");
            g_clock = 1577836800;
        }
        g_clock = 2208988800LL;   // the clock fixed (no network): the live check clears the flag
        CHECK(np_https_last_clock_fault() == 0, "clock right again clears it with no request");
        g_clock = 1577836800;
        CHECK(np_https_last_clock_fault() == 1, "and wrong again");
        g_clock = 0;   // no clock at all
        a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_E_CLOCK, "err %s (no clock)", np_https_error_name(a.r.error));
        g_clock = 2208988800LL;   // 2040: after the end of the test leaf (10 years from now are not over, but the 30-day expired/notyet ones are) and of nothing else
        a = Get(Url("expired", "/hello"));
        Show("clock in 2040, expired leaf", a);
        CHECK(a.r.error == NP_HTTPS_E_CLOCK && (a.r.cert_flags & NP_HTTPS_CERT_EXPIRED), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        g_clock = 4102444800LL;   // 2100: every test certificate has ended
        a = Get(Url("good", "/hello"));
        Show("clock in 2100, good leaf", a);
        CHECK(a.r.error == NP_HTTPS_E_CLOCK && (a.r.cert_flags & NP_HTTPS_CERT_EXPIRED), "err %s flags 0x%x", np_https_error_name(a.r.error), a.r.cert_flags);
        CHECK(np_https_last_clock_fault() == 1, "forced clock in 2100: every certificate is expired, the clock is blamed");
        np_https_test_set_clock(nullptr);
        a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_OK, "real clock again: %s", np_https_error_name(a.r.error));
        CHECK(np_https_last_clock_fault() == 0, "real clock again: cleared");
        CHECK(np_https_clock_ok() == 1, "clock_ok");
    }

    printf("[network failures]\n");
    {
        Answer a = Get("https://no-such-host.invalid/x", 0, 0, 0);
        {
            NpHttpsRequest rq;
            memset(&rq, 0, sizeof rq);
            rq.url = "https://no-such-host.invalid/x";
            rq.connect_timeout_ms = 4000;
            a = Do(rq);
        }
        Show("unknown host name", a);
        CHECK(a.r.error == NP_HTTPS_E_DNS || a.r.error == NP_HTTPS_E_TIMEOUT, "err %s", np_https_error_name(a.r.error));
        if (a.r.error == NP_HTTPS_E_TIMEOUT) printf("  NOTE: this machine's resolver did not answer within 4 s; the DNS error code path was not exercised here\n");
        CHECK(a.r.stage == NP_HTTPS_S_DNS, "stage %s", np_https_stage_name(a.r.stage));
        a = Get(Url("closed", "/hello"));
        Show("nothing listening", a);
        CHECK(a.r.error == NP_HTTPS_E_CONNECT && a.r.sys_code != 0, "err %s sys %d", np_https_error_name(a.r.error), a.r.sys_code);
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        std::string u = Url("blackhole", "/hello");
        rq.url = u.c_str();
        rq.connect_timeout_ms = 1200;
        a = Do(rq);
        Show("TCP accepted, no TLS answer", a);
        CHECK(a.r.error == NP_HTTPS_E_TIMEOUT && a.r.stage == NP_HTTPS_S_TLS && a.r.elapsed_ms < 2200, "err %s stage %s %ums", np_https_error_name(a.r.error), np_https_stage_name(a.r.stage), a.r.elapsed_ms);
        a = Get(Url("plain", "/hello"));
        Show("server that does not speak TLS", a);
        CHECK(a.r.error == NP_HTTPS_E_TLS_HANDSHAKE && a.r.tls_code != 0, "err %s tls %d", np_https_error_name(a.r.error), a.r.tls_code);
    }

    printf("[timeouts]\n");
    {
        Answer a = Get(Url("good", "/slow?ms=5000"), 1500, 20000);
        Show("silent server (io 1.5 s)", a);
        CHECK(a.r.error == NP_HTTPS_E_TIMEOUT && a.r.stage == NP_HTTPS_S_RESPONSE && a.r.elapsed_ms >= 1400 && a.r.elapsed_ms < 3000, "err %s %ums", np_https_error_name(a.r.error), a.r.elapsed_ms);
        a = Get(Url("good", "/drip"), 5000, 2000);
        Show("one byte per 200 ms, total 2 s", a);
        CHECK(a.r.error == NP_HTTPS_E_TIMEOUT && a.r.elapsed_ms >= 1900 && a.r.elapsed_ms < 3500 && a.body.empty(), "err %s %ums", np_https_error_name(a.r.error), a.r.elapsed_ms);
        a = Get(Url("good", "/slow?ms=1000"), 5000, 30000);
        CHECK(a.r.error == NP_HTTPS_OK && a.body == "slow hello\n", "a slow server inside the limits is fine: %s", np_https_error_name(a.r.error));
    }

    printf("[held polls: the no-data limit follows the hold]\n");
    {
        // netport31 (Strikers heldpoll23). Test scale: the 10 s default no-data limit is 1 s here and the "hold" 2.2 s (the room's 25 s forming hold
        // under 10 s, scaled). Through the callers' own entry point (NpNet::HttpStart, as the signaling's held room poll uses it).
        np_https_test_set_default_io_ms(1000);
        auto viaHttp = [](const std::string& url, uint32_t holdMs, NpNet::HttpResponse* out, uint32_t* ms) {
            std::string err;
            std::vector<std::string> h;
            const int64_t t0 = NowMs();
            const int id = holdMs ? NpNet::HttpStart("GET", url, "", h, 15000 + holdMs, NpNet::HeldIoMs(holdMs), &err) : NpNet::HttpStart("GET", url, "", h, 15000, &err);
            if (!id) { out->error = -1; return; }
            for (;;) { if (NpNet::HttpPoll(id, out) != 0) break; usleep(4000); if (NowMs() - t0 > 40000) { out->error = -2; break; } }
            *ms = (uint32_t)(NowMs() - t0);
        };
        NpNet::HttpResponse r;
        uint32_t ms = 0;
        viaHttp(Url("good", "/slow?ms=2200"), 2200, &r, &ms);
        printf("  held poll, answer after 2.2 s       -> error=%d status=%d %ums\n", r.error, r.status, ms);
        CHECK(r.error == 0 && r.status == 200 && ms >= 2100, "a held poll may go without data for its hold: error %d status %d %ums (%s)", r.error, r.status, ms, r.text.c_str());
        r = NpNet::HttpResponse();
        viaHttp(Url("good", "/slow?ms=2200"), 0, &r, &ms);
        printf("  ordinary request, same silence      -> error=%d status=%d %ums (%s)\n", r.error, r.status, ms, r.text.c_str());
        CHECK(r.error == NP_HTTPS_E_TIMEOUT && ms >= 900 && ms < 1900, "an ordinary request still fails at the no-data limit: error %d %ums", r.error, ms);
        np_https_test_set_default_io_ms(0);
        Answer a = Get(Url("good", "/slow?ms=1200"));
        CHECK(a.r.error == NP_HTTPS_OK, "the default (10 s) is back: %s", np_https_error_name(a.r.error));
    }

    printf("[one request at a time: a held request can see that another waits]\n");
    {
        // netport31: the worker runs one request at a time; np_https_waiting tells a held poll that a menu's request queued behind it
        // (NpNet::HttpOthersWaiting, np_signal_http.cpp gives the poll up then).
        std::string err;
        std::vector<std::string> h;
        const int held = NpNet::HttpStart("GET", Url("good", "/slow?ms=1500"), "", h, 20000, NpNet::HeldIoMs(1500), &err);
        CHECK(held > 0, "held started: %s", err.c_str());
        usleep(300000);   // (the held one is running now)
        CHECK(np_https_waiting(0) == 0 && !NpNet::HttpOthersWaiting(held), "nothing waits yet (%d)", np_https_waiting(0));
        const std::string u = Url("good", "/hello");
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = u.c_str();
        NpHttpsError e = NP_HTTPS_OK;
        const NpHttpsHandle other = np_https_start(&rq, &e);
        CHECK(other != 0 && np_https_waiting(0) == 1 && np_https_waiting(other) == 0, "one waits (%d / %d)", np_https_waiting(0), np_https_waiting(other));
        CHECK(NpNet::HttpOthersWaiting(held), "the held request sees it");
        NpNet::HttpCancel(held);   // what the signaling does: give the hold up, ask again afterwards
        NpHttpsResult res;
        const int64_t t0 = NowMs();
        int rc = 0;
        while ((rc = np_https_poll(other, &res)) == 0 && NowMs() - t0 < 10000) usleep(4000);
        const uint32_t waited = (uint32_t)(NowMs() - t0);
        printf("  the waiting request finished %u ms after the held one gave way\n", waited);
        CHECK(rc == 1 && res.error == NP_HTTPS_OK && waited < 1000, "the other request did not wait for the hold: rc %d %s %ums", rc, np_https_error_name(res.error), waited);
        np_https_release(other);
        // a queued poll does not count itself
        const int again = NpNet::HttpStart("GET", Url("good", "/hello"), "", h, 20000, NpNet::HeldIoMs(1500), &err);
        CHECK(again > 0 && !NpNet::HttpOthersWaiting(again), "a lone request: nobody else waits");
        NpNet::HttpResponse hr;
        for (int i = 0; i < 2500 && NpNet::HttpPoll(again, &hr) == 0; i++) usleep(4000);
    }

    printf("[HTTP framing]\n");
    {
        Answer a = Get(Url("good", "/chunked"));
        Show("chunked, extensions, trailers", a);
        std::string want = "Hello, chunked ";
        for (int i = 0; i < 40; i++) want += "world! ";
        want += "end";
        CHECK(a.r.error == NP_HTTPS_OK && a.body == want, "chunked body (%zu vs %zu)", a.body.size(), want.size());
        a = Get(Url("good", "/close"));
        Show("body ends with the connection", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.body == "until the connection closes\n", "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/closerude"));
        Show("body ends with a bare TCP close", a);
        CHECK(a.r.error == NP_HTTPS_E_IO && a.body.empty(), "an unauthenticated end of data must not count as complete (%s)", np_https_error_name(a.r.error));
        a = Get(Url("good", "/status?code=404"));
        Show("404 with a JSON body", a);
        CHECK(a.r.error == NP_HTTPS_E_HTTP_STATUS && a.r.http_status == 404 && a.body.find("nope") != std::string::npos, "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/status?code=500"));
        CHECK(a.r.error == NP_HTTPS_E_HTTP_STATUS && a.r.http_status == 500, "500");
        a = Get(Url("good", "/redirect"));
        Show("302 is not followed", a);
        CHECK(a.r.error == NP_HTTPS_E_REDIRECT && a.r.http_status == 302 && a.body.empty(), "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/gzip"));
        Show("Content-Encoding: gzip", a);
        CHECK(a.r.error == NP_HTTPS_E_PROTOCOL, "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/badlen"));
        Show("two different Content-Length", a);
        CHECK(a.r.error == NP_HTTPS_E_PROTOCOL, "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/truncated"));
        Show("body shorter than Content-Length", a);
        CHECK(a.r.error == NP_HTTPS_E_IO && a.body.empty(), "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/nonexistent"));
        CHECK(a.r.error == NP_HTTPS_E_HTTP_STATUS && a.r.http_status == 404 && a.body.empty() && a.r.body_len == 0, "empty 404");
    }

    printf("[sizes]\n");
    {
        Answer a = Get(Url("good", "/huge?n=2097152"), 0, 0, 0);
        Show("2 MiB, default limit 64 KiB", a);
        CHECK(a.r.error == NP_HTTPS_E_TOO_LARGE && a.r.elapsed_ms < 1500 && a.body.empty(), "err %s %ums", np_https_error_name(a.r.error), a.r.elapsed_ms);
        a = Get(Url("good", "/hugechunked?n=2097152"), 0, 0, 0);
        Show("2 MiB chunked, default limit", a);
        CHECK(a.r.error == NP_HTTPS_E_TOO_LARGE && a.body.empty(), "err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/hugechunked?n=2097152"), 0, 0, 5000000);
        Show("2 MiB chunked, limit asked 5 MB", a);
        CHECK(a.r.error == NP_HTTPS_E_TOO_LARGE, "the hard limit is 1 MiB: err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/exact?n=1048576"), 0, 0, 1048576);
        Show("exactly 1 MiB, limit 1 MiB", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.body.size() == 1048576, "err %s %zu", np_https_error_name(a.r.error), a.body.size());
        a = Get(Url("good", "/exact?n=1048577"), 0, 0, 1048576);
        CHECK(a.r.error == NP_HTTPS_E_TOO_LARGE, "one byte over: err %s", np_https_error_name(a.r.error));
        a = Get(Url("good", "/exact?n=1000"), 0, 0, 1000);
        CHECK(a.r.error == NP_HTTPS_OK && a.body.size() == 1000, "exact small limit");
        a = Get(Url("good", "/exact?n=1001"), 0, 0, 1000);
        CHECK(a.r.error == NP_HTTPS_E_TOO_LARGE, "small limit +1");
        a = Get(Url("good", "/big404"), 0, 0, 1000);
        CHECK(a.r.error == NP_HTTPS_E_TOO_LARGE && a.r.http_status == 404, "big error body: err %s status %d", np_https_error_name(a.r.error), a.r.http_status);
    }

    printf("[POST]\n");
    {
        std::string json = "{\"player\":\"CBOC\",\"score\":12,\"note\":\"\xc3\xa9 and \\\"quotes\\\"\"}";
        for (int i = 0; i < 2000; i++) json += " ";   // spans several TLS records
        const char* hdrs[] = {"X-Test: token-123", nullptr};
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        std::string u = Url("good", "/echo?x=1&y=two");
        rq.url = u.c_str();
        rq.body = json.data();
        rq.body_len = json.size();
        rq.headers = hdrs;
        Answer a = Do(rq);
        Show("POST JSON with a header", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.body.find("\"method\": \"POST\"") != std::string::npos, "%s", a.body.c_str());
        CHECK(a.body.find("\"ctype\": \"application/json\"") != std::string::npos, "content type");
        char len[64];
        snprintf(len, sizeof len, "\"len\": %zu", json.size());
        CHECK(a.body.find(len) != std::string::npos, "length %s", len);
        CHECK(a.body.find("\"path\": \"/echo?x=1&y=two\"") != std::string::npos, "path and query");
        CHECK(a.body.find("\"x_test\": \"token-123\"") != std::string::npos, "extra header");
        CHECK(a.body.find("\"accept_enc\": \"identity\"") != std::string::npos, "identity");
        char hostp[64];
        snprintf(hostp, sizeof hostp, "\"host\": \"localhost:%d\"", Port("good"));
        CHECK(a.body.find(hostp) != std::string::npos, "Host header with port");
        // netport31 (Strikers onmatch23): a "Content-Type: x" line names the content type (it was refused as a reserved header: the whole
        // request failed on the console only, where the plain desktop client had sent it); written once, by us
        {
            const char* hdrs2[] = {"Content-Type: text/plain; charset=utf-8", "X-Test: ct", nullptr};
            NpHttpsRequest r2 = rq;
            r2.headers = hdrs2;
            Answer b = Do(r2);
            CHECK(b.started && b.r.error == NP_HTTPS_OK && b.body.find("\"ctype\": \"text/plain; charset=utf-8\"") != std::string::npos, "Content-Type line taken: %s %s",
                  np_https_error_name(b.start_err), b.body.c_str());
            const char* bad[] = {"Content-Type: a\x01b", nullptr};
            r2.headers = bad;
            b = Do(r2);
            CHECK(!b.started && b.start_err == NP_HTTPS_E_BAD_REQUEST, "a Content-Type line with a control character is still refused");
            const char* empty[] = {"Content-Type:   ", nullptr};
            r2.headers = empty;
            b = Do(r2);
            CHECK(!b.started && b.start_err == NP_HTTPS_E_BAD_REQUEST, "an empty Content-Type line is refused");
            // PUT (the rename) carries its body; DELETE (the account's own delete) none
            const std::string eu = Url("good", "/echo");
            NpHttpsRequest r3;
            memset(&r3, 0, sizeof r3);
            r3.url = eu.c_str();
            r3.method = "PUT";
            r3.body = "{\"name\":\"x\"}";
            r3.body_len = 12;
            b = Do(r3);
            CHECK(b.r.error == NP_HTTPS_OK && b.body.find("\"method\": \"PUT\"") != std::string::npos && b.body.find("\"len\": 12") != std::string::npos, "PUT: %s", b.body.c_str());
            r3.method = "DELETE";
            r3.body = nullptr;
            r3.body_len = 0;
            b = Do(r3);
            CHECK(b.r.error == NP_HTTPS_OK && b.body.find("\"method\": \"DELETE\"") != std::string::npos, "DELETE: %s", b.body.c_str());
        }
        // 60 KiB body
        std::string big(60 * 1024, 'b');
        rq.body = big.data();
        rq.body_len = big.size();
        rq.headers = nullptr;
        a = Do(rq);
        snprintf(len, sizeof len, "\"len\": %zu", big.size());
        CHECK(a.r.error == NP_HTTPS_OK && a.body.find(len) != std::string::npos, "60 KiB body: %s", np_https_error_name(a.r.error));
        // POST without a body, custom content type
        rq.body = nullptr;
        rq.body_len = 0;
        rq.method = "POST";
        rq.content_type = "text/plain";
        a = Do(rq);
        CHECK(a.r.error == NP_HTTPS_OK && a.body.find("\"len\": 0") != std::string::npos && a.body.find("text/plain") != std::string::npos, "empty POST");
        // DELETE (the account's own delete, DELETE /v1/me): no body, sent as DELETE
        rq.method = "DELETE";
        rq.content_type = nullptr;
        a = Do(rq);
        CHECK(a.r.error == NP_HTTPS_OK && a.body.find("\"method\": \"DELETE\"") != std::string::npos && a.body.find("\"len\": 0") != std::string::npos, "DELETE: %s", np_https_error_name(a.r.error));
        // PUT with a body (the rename, PUT /v1/me): sent as PUT, body intact
        static const char kPut[] = "{\"name\":\"Luma\"}";
        rq.method = "PUT";
        rq.body = kPut;
        rq.body_len = sizeof kPut - 1;
        a = Do(rq);
        snprintf(len, sizeof len, "\"len\": %zu", sizeof kPut - 1);
        CHECK(a.r.error == NP_HTTPS_OK && a.body.find("\"method\": \"PUT\"") != std::string::npos && a.body.find(len) != std::string::npos, "PUT: %s", np_https_error_name(a.r.error));
    }

    printf("[refused before any network use]\n");
    {
        struct Bad { const char* what; const char* url; const char* method; const char* body; size_t body_len; const char* hdr; };
        std::string toolong = std::string("https://localhost/") + std::string(2000, 'a');
        const Bad bad[] = {
            {"http scheme", "http://localhost/x", nullptr, nullptr, 0, nullptr},
            {"no scheme", "localhost/x", nullptr, nullptr, 0, nullptr},
            {"ftp scheme", "ftp://localhost/x", nullptr, nullptr, 0, nullptr},
            {"userinfo", "https://user:pw@localhost/x", nullptr, nullptr, 0, nullptr},
            {"space in url", "https://localhost/a b", nullptr, nullptr, 0, nullptr},
            {"CRLF in url", "https://localhost/a\r\nX: y", nullptr, nullptr, 0, nullptr},
            {"fragment", "https://localhost/a#frag", nullptr, nullptr, 0, nullptr},
            {"port 0", "https://localhost:0/", nullptr, nullptr, 0, nullptr},
            {"port 70000", "https://localhost:70000/", nullptr, nullptr, 0, nullptr},
            {"empty host", "https:///x", nullptr, nullptr, 0, nullptr},
            {"ipv6 literal", "https://[::1]/x", nullptr, nullptr, 0, nullptr},
            {"underscore host", "https://bad_host.test/x", nullptr, nullptr, 0, nullptr},
            {"url too long", toolong.c_str(), nullptr, nullptr, 0, nullptr},
            {"GET with body", "https://localhost/x", "GET", "abc", 3, nullptr},
            {"PATCH", "https://localhost/x", "PATCH", nullptr, 0, nullptr},
            {"DELETE with body", "https://localhost/x", "DELETE", "abc", 3, nullptr},
            {"body too large", "https://localhost/x", "POST", "x", 70000, nullptr},
            {"header injection", "https://localhost/x", nullptr, nullptr, 0, "X-A: b\r\nX-Evil: 1"},
            {"reserved header", "https://localhost/x", nullptr, nullptr, 0, "Host: evil.example"},
            {"header without colon", "https://localhost/x", nullptr, nullptr, 0, "NoColon"},
            {"transfer-encoding header", "https://localhost/x", nullptr, nullptr, 0, "Transfer-Encoding: chunked"},
        };
        std::string big(70000, 'x');
        for (const Bad& b : bad) {
            const char* hdrs[2] = {b.hdr, nullptr};
            NpHttpsRequest rq;
            memset(&rq, 0, sizeof rq);
            rq.url = b.url;
            rq.method = b.method;
            rq.body = b.body_len == 70000 ? big.data() : b.body;
            rq.body_len = b.body_len;
            rq.headers = b.hdr ? hdrs : nullptr;
            NpHttpsError e;
            NpHttpsHandle h = np_https_start(&rq, &e);
            CHECK(h == 0 && e == NP_HTTPS_E_BAD_REQUEST, "%s: handle %u err %s", b.what, h, np_https_error_name(e));
            if (h) np_https_release(h);
        }
        NpHttpsError e;
        CHECK(np_https_start(nullptr, &e) == 0 && e == NP_HTTPS_E_BAD_REQUEST, "null request");
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        CHECK(np_https_start(&rq, &e) == 0 && e == NP_HTTPS_E_BAD_REQUEST, "no url");
        NpHttpsResult r;
        CHECK(np_https_poll(0, &r) == -1 && np_https_poll(12345, &r) == -1, "dead handles");
        np_https_cancel(0);
        np_https_release(77);
    }

    printf("[cancel, busy, release]\n");
    {
        std::string slow = Url("good", "/slow?ms=8000");
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = slow.c_str();
        NpHttpsError e;
        NpHttpsHandle h1 = np_https_start(&rq, &e);
        usleep(300000);
        NpHttpsResult r;
        CHECK(np_https_poll(h1, &r) == 0 && r.stage >= NP_HTTPS_S_SEND, "running, stage %s", np_https_stage_name(r.stage));
        const int64_t t0 = NowMs();
        { Timed t; np_https_cancel(h1); }
        int rc;
        while ((rc = np_https_poll(h1, &r)) == 0) usleep(2000);
        const int64_t took = NowMs() - t0;
        CHECK(rc == 1 && r.error == NP_HTTPS_E_CANCELLED && took < 400, "cancel took %lld ms, err %s", (long long)took, np_https_error_name(r.error));
        np_https_release(h1);
        CHECK(np_https_poll(h1, &r) == -1, "released handle is dead");

        // all NP_HTTPS_SLOTS (8) busy, the next refused; cancelling queued ones completes them at once
        CHECK(NP_HTTPS_SLOTS >= 8, "s1117b: at least 8 request slots (a held poll, a login and the fire-and-forget reports must not fill them), have %d", NP_HTTPS_SLOTS);
        NpHttpsHandle hs[NP_HTTPS_SLOTS];
        for (int i = 0; i < NP_HTTPS_SLOTS; i++) { hs[i] = np_https_start(&rq, &e); CHECK(hs[i] != 0, "slot %d", i); }
        NpHttpsHandle h5 = np_https_start(&rq, &e);
        CHECK(h5 == 0 && e == NP_HTTPS_E_BUSY, "fifth request: %s", np_https_error_name(e));
        usleep(100000);
        { Timed t; np_https_cancel(hs[3]); }
        CHECK(np_https_poll(hs[3], &r) == 1 && r.error == NP_HTTPS_E_CANCELLED && r.stage == NP_HTTPS_S_QUEUED, "queued request cancelled at once");
        np_https_release(hs[3]);
        NpHttpsHandle again = np_https_start(&rq, &e);
        CHECK(again != 0, "a freed slot is usable");
        np_https_release(again);   // released while queued/running
        for (int i = 0; i < NP_HTTPS_SLOTS; i++) if (i != 3) np_https_release(hs[i]);   // released while running / queued
        usleep(500000);
        Answer a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_OK, "after releasing everything: %s", np_https_error_name(a.r.error));

        // several at once: each gets its own answer
        std::string u[4] = {Url("good", "/hello"), Url("tls12", "/hello"), Url("good", "/status?code=404"), Url("rsa", "/hello")};
        NpHttpsHandle hh[4];
        for (int i = 0; i < 4; i++) { rq.url = u[i].c_str(); hh[i] = np_https_start(&rq, &e); }
        int done = 0;
        NpHttpsResult rr[4];
        std::string bodies[4];
        while (done < 4) {
            done = 0;
            for (int i = 0; i < 4; i++) if (np_https_poll(hh[i], &rr[i]) == 1) done++;
            usleep(4000);
        }
        for (int i = 0; i < 4; i++) { bodies[i].assign((const char*)rr[i].body, rr[i].body_len); np_https_release(hh[i]); }
        CHECK(rr[0].error == NP_HTTPS_OK && rr[1].tls_version == 0x0303 && rr[2].http_status == 404 && rr[3].error == NP_HTTPS_OK && bodies[0] == bodies[3], "four requests in flight");
    }

    printf("[keep-alive]\n");
    {
        Answer a = Get(Url("good", "/count"));
        const int c0 = atoi(a.body.c_str());
        a = Get(Url("good", "/hello"), 0, 0, 0, 1);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0, "first request opens");
        const uint32_t first_ms = a.r.elapsed_ms;
        a = Get(Url("good", "/count"), 0, 0, 0, 1);
        Show("second request, kept connection", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 1 && atoi(a.body.c_str()) == c0 + 1, "reused %d, count %s vs %d", a.r.reused_connection, a.body.c_str(), c0 + 1);
        printf("  (first %u ms, second %u ms)\n", first_ms, a.r.elapsed_ms);
        // another host/port: the kept connection is closed, a new one opened
        a = Get(Url("tls12", "/hello"), 0, 0, 0, 1);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0 && a.r.tls_version == 0x0303, "different port");
        // a request without keep_alive after a kept one: closes the old, opens a new one, works
        a = Get(Url("tls12", "/hello"), 0, 0, 0, 0);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0, "a non-keep-alive request opens its own connection (%d)", a.r.reused_connection);
        // the server drops an idle connection: the next request notices and reconnects
        a = Get(Url("good", "/stale"), 0, 0, 0, 1);
        usleep(700000);
        a = Get(Url("good", "/hello"), 0, 0, 0, 1);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0 && a.body == "hello from the test server\n", "after the server closed: err %s", np_https_error_name(a.r.error));
        // a kept connection that died silently: one retry on a fresh one
        a = Get(Url("good", "/flaky"), 0, 0, 0, 1);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0, "flaky first");
        a = Get(Url("good", "/flaky"), 0, 0, 0, 1);
        Show("kept connection dropped without a word", a);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0 && a.body == "first request on this connection\n", "retried on a fresh connection: err %s", np_https_error_name(a.r.error));
        // an error status keeps the connection too; a redirect does not
        a = Get(Url("good", "/status?code=404"), 0, 0, 0, 1);
        a = Get(Url("good", "/redirect"), 0, 0, 0, 1);
        CHECK(a.r.error == NP_HTTPS_E_REDIRECT && a.r.reused_connection == 1, "redirect on a kept connection");
        a = Get(Url("good", "/hello"), 0, 0, 0, 1);
        CHECK(a.r.error == NP_HTTPS_OK && a.r.reused_connection == 0, "after a redirect the connection was closed");
    }

    printf("[sockets: many failing polls leave nothing open]\n");
    {
        // netport31 (from Strikers onmatch23, unfinished there): the console's table of sockets is small and shared with the system. Sixty polls
        // that fail in every way a held poll can fail on the console (no data in time, the server silent after the connection, nothing listening,
        // a refused certificate, a cancel in the middle, a release while running) must leave no socket open: the client's own count AND the
        // process's file descriptors, with never more than one open at once.
        auto Fds = [] {
            int n = 0;
            if (DIR* d = opendir("/proc/self/fd")) { while (readdir(d)) n++; closedir(d); }
            return n;
        };
        usleep(300000);
        np_https_shutdown(5000);   // the kept connection of the tests above is closed; the worker starts again with the first request
        int live = -1, opened0 = 0, closed0 = 0, refused = -1;
        np_https_socket_stats(&live, &opened0, &closed0, &refused);
        CHECK(live == 0 && opened0 == closed0 && refused == 0, "before: live %d opened %d closed %d refused %d", live, opened0, closed0, refused);
        const int fds0 = Fds();
        int worstLive = 0, bad = 0;
        const std::string slow = Url("good", "/slow?ms=3000"), hole = Url("blackhole", "/hello"), dead = Url("closed", "/hello"), cert = Url("untrusted", "/hello");
        for (int i = 0; i < 60; i++) {
            NpHttpsRequest rq;
            memset(&rq, 0, sizeof rq);
            rq.keep_alive = 1;
            const int kind = i % 6;
            NpHttpsError e = NP_HTTPS_OK;
            if (kind == 0) { rq.url = slow.c_str(); rq.io_timeout_ms = 60; rq.total_timeout_ms = 5000; }          // a held poll with no data in time
            else if (kind == 1) { rq.url = hole.c_str(); rq.connect_timeout_ms = 60; }                              // TCP accepted, then silence
            else if (kind == 2) { rq.url = dead.c_str(); }                                                           // nothing listening
            else if (kind == 3) { rq.url = cert.c_str(); }                                                           // certificate refused
            else { rq.url = slow.c_str(); rq.total_timeout_ms = 5000; }                                              // 4: cancelled, 5: released while running
            if (kind <= 3) {
                Answer a = Do(rq);
                if (a.r.error == NP_HTTPS_OK || a.r.error == NP_HTTPS_E_HTTP_STATUS) bad++;
            } else {
                NpHttpsHandle h = np_https_start(&rq, &e);
                usleep(40000 + (i % 5) * 15000);   // somewhere in the connect, the handshake or the wait for the answer
                if (kind == 4) {
                    np_https_cancel(h);
                    NpHttpsResult r;
                    memset(&r, 0, sizeof r);
                    for (int t = 0; t < 500 && np_https_poll(h, &r) == 0; t++) usleep(4000);
                    if (r.error != NP_HTTPS_E_CANCELLED) bad++;
                }
                np_https_release(h);
                // (a released request still runs to its end on the worker: wait for it through one that is queued behind it)
                Answer a = Get(dead);
                if (a.r.error != NP_HTTPS_E_CONNECT) bad++;
            }
            np_https_socket_stats(&live, nullptr, nullptr, nullptr);
            if (live > worstLive) worstLive = live;
        }
        int opened = 0, closed = 0;
        np_https_socket_stats(&live, &opened, &closed, &refused);
        printf("  60 failing polls: %d sockets opened, %d closed, %d open now (most after a poll: %d), %d refused; file descriptors %d -> %d\n", opened - opened0,
               closed - closed0, live, worstLive, refused, fds0, Fds());
        CHECK(bad == 0, "%d polls did not fail the way they were meant to", bad);
        CHECK(opened - opened0 >= 50, "the polls opened sockets (%d)", opened - opened0);
        CHECK(live == 0 && worstLive == 0, "no socket is left open after a failed poll (now %d, worst %d)", live, worstLive);
        CHECK(opened == closed, "every socket opened was closed (%d / %d)", opened, closed);
        CHECK(refused == 0, "the budget was never reached (%d refused)", refused);
        CHECK(Fds() == fds0, "the process has as many file descriptors as before (%d -> %d)", fds0, Fds());
        // a kept connection is the one socket that may stay, and only for its 10 s life
        Answer a = Get(Url("good", "/hello"), 0, 0, 0, 1);
        np_https_socket_stats(&live, nullptr, nullptr, nullptr);
        CHECK(a.r.error == NP_HTTPS_OK && live == 1, "a kept connection is one socket (%d)", live);
        a = Get(Url("good", "/hello"), 0, 0, 0, 1);
        np_https_socket_stats(&live, &opened0, nullptr, nullptr);
        CHECK(a.r.reused_connection == 1 && live == 1 && opened0 == opened + 1, "reused: still one socket (%d), one opened (%d)", live, opened0 - opened);
    }

    printf("[shutdown and restart]\n");
    {
        np_https_shutdown(5000);
        {
            int live = -1;
            np_https_socket_stats(&live, nullptr, nullptr, nullptr);
            CHECK(live == 0, "shutdown closes the kept connection (%d open)", live);
        }
        Answer a = Get(Url("good", "/hello"));
        CHECK(a.r.error == NP_HTTPS_OK, "restarted: %s", np_https_error_name(a.r.error));
        np_https_shutdown(5000);
        np_https_shutdown(5000);
    }

    printf("[never blocks the caller]\n");
    printf("  longest start/poll/cancel call: %.1f ms\n", g_maxCallMs);
    CHECK(g_maxCallMs < 25.0, "a call blocked for %.1f ms", g_maxCallMs);

    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
