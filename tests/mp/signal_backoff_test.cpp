// The room signaling (net/src/online/np_signal_http.cpp) against a failing service: the local fake behind tests/mp/flaky_proxy.mjs.
// Proves that a room's polling backs off exponentially WITH JITTER, honours a 429's retryAfterSec, and GIVES UP (so thousands of consoles
// cannot keep polling an exhausted or broken service), and that the room-deciding calls (info, create, join) give up after 3 tries.
// Virtual time (Signal::SetClock) for the schedule, real HTTP for the requests; the proxy logs each request it sees and the test notes
// the virtual time at which it showed up (+- one loop step of 50 ms).
//   tests/mp/signal_backoff_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "np_http.h"
#include "np_signal_http.h"

using namespace NpNet;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static uint64_t g_virt = 1000000;
static uint64_t Clock(void*) { return g_virt; }

static std::string g_proxy, g_log, g_token, g_token2, g_fake;

static std::vector<std::string> LogText() {
    std::vector<std::string> out;
    FILE* f = fopen(g_log.c_str(), "r");
    if (!f) return out;
    char line[512];
    while (fgets(line, sizeof line, f)) out.push_back(line);
    fclose(f);
    return out;
}
static int LogLines() { return int(LogText().size()); }

static void Mode(const char* m) {
    std::string cmd = "curl -s -o /dev/null " + g_proxy + "/__mode/" + m;
    CHECK(system(cmd.c_str()) == 0);
}

static Signal* NewSignal() {
    HttpSignalConfig cfg;
    snprintf(cfg.baseUrl, sizeof cfg.baseUrl, "%s", g_proxy.c_str());
    snprintf(cfg.token, sizeof cfg.token, "%s", g_token.c_str());
    snprintf(cfg.appVersion, sizeof cfg.appVersion, "3.0.0");
    cfg.requestTimeoutMs = 2000;
    ResetServiceInfoCache();   // (svcfix2: /v1/info is asked once per session; these tests count the requests of a fresh console)
    Signal* s = CreateHttpSignal(cfg);
    s->SetClock(Clock, 0);
    return s;
}

// Runs the signal for `virtMs` of virtual time (50 ms per step, 1 ms of real time per step); returns the virtual times at which the
// proxy saw requests.
static std::vector<uint64_t> Run(Signal* s, uint64_t virtMs, Signal::Status stopAt = Signal::IDLE) {
    std::vector<uint64_t> at;
    int seen = LogLines();
    const uint64_t end = g_virt + virtMs;
    while (g_virt < end) {
        s->Poll();
        HttpPump();
        usleep(1000);
        const int n = LogLines();
        for (; seen < n; ++seen) at.push_back(g_virt);
        if (stopAt != Signal::IDLE && s->GetStatus() == stopAt) break;
        g_virt += 50;
    }
    return at;
}

static void Gaps(const char* what, const std::vector<uint64_t>& at, std::vector<double>* out) {
    fprintf(stderr, "%s: %zu requests, gaps (s):", what, at.size());
    for (size_t i = 1; i < at.size(); ++i) {
        const double g = double(at[i] - at[i - 1]) / 1000.0;
        out->push_back(g);
        fprintf(stderr, " %.1f", g);
    }
    fprintf(stderr, "\n");
}

int main() {
    const char* p = getenv("PROXY");
    const char* l = getenv("PROXY_LOG");
    const char* t = getenv("TOKEN");
    if (!p || !l || !t) { fprintf(stderr, "PROXY, PROXY_LOG, TOKEN (signal_backoff_test.sh sets them)\n"); return 2; }
    g_proxy = p; g_log = l; g_token = t;
    if (const char* t2 = getenv("TOKEN2")) g_token2 = t2;
    if (const char* f = getenv("FAKE_URL")) g_fake = f;

    // 0. held polls (request budget), on the REAL clock: a host alone in its room asks once per ~8 s instead of every 1.7 s, sees a
    // guest who joins within about a second, and a queued request (its endpoint) is not held back by a pending poll
    {
        Mode("pass");
        HttpSignalConfig cfg;
        snprintf(cfg.baseUrl, sizeof cfg.baseUrl, "%s", g_proxy.c_str());
        snprintf(cfg.token, sizeof cfg.token, "%s", g_token.c_str());
        snprintf(cfg.appVersion, sizeof cfg.appVersion, "3.0.0");
        cfg.pollWaitSec = 8;
        Signal* h = CreateHttpSignal(cfg);
        h->Host("Held");
        const uint64_t t0 = SteadyNowMs(0);
        while (h->GetStatus() != Signal::IN_ROOM && SteadyNowMs(0) - t0 < 5000) { h->Poll(); HttpPump(); usleep(5000); }
        CHECK(h->GetStatus() == Signal::IN_ROOM);
        // one plain poll first (version 0), then held ones
        const int before = LogLines();
        const uint64_t t1 = SteadyNowMs(0);
        while (SteadyNowMs(0) - t1 < 20000) { h->Poll(); HttpPump(); usleep(5000); }
        const int n = LogLines() - before;
        fprintf(stderr, "held polls: %d requests in 20 s waiting alone (plain polls: about 12)\n", n);
        CHECK(n >= 0 && n <= 5);   // (svcfix2: the service holds a forming room's poll 25 s; before, 8 s)
        // our endpoint while a poll is held: sent at once
        SigCandidates mine;
        mine.valid = true;
        ParseAddr("203.0.113.5:41000", 0, &mine.pub);    // (the service refuses loopback claims; the fake does not check the source)
        ParseAddr("192.168.1.5:41000", 0, &mine.local);
        const int b2 = LogLines();
        const uint64_t t2 = SteadyNowMs(0);
        h->PublishCandidates(mine);
        bool sent = false;
        while (!sent && SteadyNowMs(0) - t2 < 3000) {
            h->Poll(); HttpPump(); usleep(5000);
            const std::vector<std::string> lines = LogText();
            for (size_t i = size_t(b2); i < lines.size(); ++i) if (lines[i].find("/endpoint") != std::string::npos) sent = true;
        }
        fprintf(stderr, "held polls: endpoint sent after %u ms\n", unsigned(SteadyNowMs(0) - t2));
        CHECK(sent);
        for (int i = 0; i < 20; ++i) { h->Poll(); HttpPump(); usleep(10000); }
        CHECK(h->GetStatus() == Signal::IN_ROOM);
        // a guest joins (straight at the fake service): the host's held poll answers with the new member
        if (!g_token2.empty() && !g_fake.empty()) {
            for (int i = 0; i < 40; ++i) { h->Poll(); HttpPump(); usleep(50000); }   // (a held poll is pending again)
            const uint64_t t3 = SteadyNowMs(0);
            std::string cmd = "curl -s -o /dev/null -X POST " + g_fake + "/v1/rooms/join -H 'X-Msc-App: 3.0.0' -H 'X-Msc-Game: smg' "
                              "-H 'Content-Type: application/json' -H 'Authorization: Bearer " + g_token2 + "' -d '{\"code\":\"" +
                              std::string(h->Room().invite) + "\"}'";
            CHECK(system(cmd.c_str()) == 0);
            bool seen2 = false;
            while (!seen2 && SteadyNowMs(0) - t3 < 6000) {
                h->Poll(); HttpPump(); usleep(5000);
                for (int s2 = 0; s2 < 4; ++s2) if (s2 != h->Room().mySlot && h->Room().members[s2].present) seen2 = true;
            }
            fprintf(stderr, "held polls: the host saw the guest %u ms after the join\n", unsigned(SteadyNowMs(0) - t3));
            if (getenv("DEBUG_HELD")) { const std::vector<std::string> L = LogText(); for (size_t i = L.size() > 8 ? L.size() - 8 : 0; i < L.size(); ++i) fprintf(stderr, "  %s", L[i].c_str()); fprintf(stderr, "  (t3 real ms ~ now-%u)\n", unsigned(SteadyNowMs(0) - t3)); }
            CHECK(seen2 && SteadyNowMs(0) - t3 < 2500);
        } else CHECK(false);
        delete h;
    }

    // 1. a host waiting in its room polls the healthy service every 1.7 s
    Mode("pass");
    Signal* a = NewSignal();
    a->Host("Waiter");
    Run(a, 3000, Signal::IN_ROOM);
    CHECK(a->GetStatus() == Signal::IN_ROOM);
    std::vector<double> g0;
    Gaps("healthy, waiting", Run(a, 10000), &g0);
    for (double g : g0) CHECK(g >= 1.5 && g <= 2.1);

    // 2. the service starts failing (503 JSON): exponential back-off 2, 4, 8, 16, 32, 60, 60, 60 s (each +-25 %), then the room gives up
    Signal* b = NewSignal();
    b->Host("Second");
    Run(b, 3000, Signal::IN_ROOM);
    CHECK(b->GetStatus() == Signal::IN_ROOM);
    Mode("s503");
    std::vector<uint64_t> atA, atB;
    {
        // both rooms together, so their schedules can be compared (jitter: they must not stay in step); each request is credited to
        // its room by the code in its path
        const std::string codeA = a->Room().invite, codeB = b->Room().invite;
        size_t seen = LogText().size();
        const uint64_t end = g_virt + 8 * 60000;
        while (g_virt < end && (a->GetStatus() == Signal::IN_ROOM || b->GetStatus() == Signal::IN_ROOM)) {
            a->Poll();
            b->Poll();
            HttpPump();
            usleep(1000);
            const std::vector<std::string> lines = LogText();
            for (; seen < lines.size(); ++seen) {
                if (lines[seen].find(codeA) != std::string::npos) atA.push_back(g_virt);
                else if (lines[seen].find(codeB) != std::string::npos) atB.push_back(g_virt);
            }
            g_virt += 50;
        }
    }
    std::vector<double> ga, gb;
    Gaps("503, room A", atA, &ga);
    Gaps("503, room B", atB, &gb);
    CHECK(a->GetStatus() == Signal::FAILED && b->GetStatus() == Signal::FAILED);
    fprintf(stderr, "room A gave up with: %s\n", a->Error());
    CHECK(atA.size() >= 8 && atA.size() <= 11);           // a bounded number of tries
    const double base[] = {2, 4, 8, 16, 32, 60, 60, 60};
    bool differ = false;
    for (size_t i = 0; i < ga.size() && i < 8; ++i) {
        const double want = base[i];
        CHECK(ga[i] >= want * 0.75 - 0.6 && ga[i] <= want * 1.25 + 0.6);
        if (i < gb.size() && (ga[i] - gb[i] > 0.3 || gb[i] - ga[i] > 0.3)) differ = true;
    }
    CHECK(differ);                                        // jitter: two consoles do not retry in step
    double total = 0;
    for (double g : ga) total += g;
    CHECK(total > 150 && total < 330);                    // given up after about 4 minutes, not polling forever
    delete a;
    delete b;

    // 3. a 429 with retryAfterSec 120 (JSON) is waited out; a Cloudflare-style HTML 429 (no JSON) backs off like any failure
    Mode("pass");
    Signal* c = NewSignal();
    c->Host("Third");
    Run(c, 3000, Signal::IN_ROOM);
    Mode("s429-120");
    std::vector<double> gc;
    Gaps("429 retryAfter 120", Run(c, 300000, Signal::FAILED), &gc);
    CHECK(gc.size() >= 1 && gc.size() <= 4);
    for (size_t i = 1; i < gc.size(); ++i) CHECK(gc[i] >= 119);
    delete c;
    Mode("pass");
    Signal* d = NewSignal();
    d->Host("Fourth");
    Run(d, 3000, Signal::IN_ROOM);
    Mode("html429");
    std::vector<double> gd;
    Gaps("HTML 429 (1015 page)", Run(d, 8 * 60000, Signal::FAILED), &gd);
    CHECK(d->GetStatus() == Signal::FAILED && gd.size() >= 7 && gd.size() <= 10);
    delete d;

    // 4. the room-deciding calls (info, then join) give up after 3 attempts with growing, jittered gaps
    Mode("s500");
    Signal* e = NewSignal();
    e->Join("K7QM4D", "Joiner");
    std::vector<double> ge;
    Gaps("join against 500", Run(e, 120000, Signal::FAILED), &ge);
    CHECK(e->GetStatus() == Signal::FAILED);
    CHECK(ge.size() == 2 && ge[0] >= 0.7 && ge[1] >= 1.4);   // 3 tries of GET /v1/info: 1 s, then 2 s (+- 25 %)
    delete e;

    // 5. a service that never answers: each request times out, the gaps still grow, and the room-deciding call gives up
    Mode("hang");
    Signal* f = NewSignal();
    f->Join("K7QM4D", "Hanger");
    std::vector<double> gf;
    Gaps("join against silence", Run(f, 180000, Signal::FAILED), &gf);
    CHECK(f->GetStatus() == Signal::FAILED && gf.size() == 2);
    delete f;
    Mode("pass");


    // 6. svc3 (outside review S7): a 503 over_budget (the service has no requests left for the day) is honoured, with a cap.
    {
        // 6a. the room-deciding calls do not retry (retrying within seconds cannot succeed, and every try is a request the free plan counts);
        // the console remembers it was told to wait and asks nothing more until then (at most 10 minutes), then asks again
        ResetServiceBusy();
        Mode("over-1800");
        Signal* g = NewSignal();
        g->Host("Busy");
        std::vector<uint64_t> atG = Run(g, 60000, Signal::FAILED);
        fprintf(stderr, "over_budget: Host() sent %zu request(s), error: %s\n", atG.size(), g->Error());
        CHECK(g->GetStatus() == Signal::FAILED);
        CHECK(atG.size() == 1);                                   // one GET /v1/info (it was 3 tries of it)
        CHECK(strstr(g->Error(), "very busy") != nullptr);
        CHECK(strcmp(g->ErrorCode(), "over_budget") == 0);
        delete g;
        Mode("pass");
        g_virt += 60000;                                          // a minute later the service would answer, but we were told 30 minutes
        const int b1 = LogLines();
        Signal* g2 = NewSignal();
        g2->Host("Again");
        Run(g2, 2000);
        fprintf(stderr, "over_budget: a minute later Host() sent %d request(s)\n", LogLines() - b1);
        CHECK(g2->GetStatus() == Signal::FAILED && LogLines() == b1);   // answered locally, no request
        delete g2;
        g_virt += 600000;                                         // past the cap (10 minutes): it asks again
        Signal* g3 = NewSignal();
        g3->Host("Later");
        Run(g3, 4000, Signal::IN_ROOM);
        CHECK(g3->GetStatus() == Signal::IN_ROOM);
        // 6b. a running room's poll waits what the service said (here 45 s; at most 60 s), not the 2, 4, 8 s of the ordinary back-off
        Mode("over-45");
        std::vector<double> gp;
        Gaps("503 over_budget retryAfter 45", Run(g3, 200000), &gp);
        CHECK(gp.size() >= 3);
        for (double d : gp) CHECK(d >= 44 && d <= 61);
        delete g3;
        Mode("over-3000");                                        // a huge retryAfterSec is capped at 60 s for a poll
        Mode("pass");
        Signal* g4 = NewSignal();
        g4->Host("Capped");
        Run(g4, 4000, Signal::IN_ROOM);
        Mode("over-3000");
        std::vector<double> gq;
        Gaps("503 over_budget retryAfter 3000 (capped)", Run(g4, 200000), &gq);
        CHECK(gq.size() >= 2);
        for (double d : gq) CHECK(d >= 59 && d <= 76);
        delete g4;
        Mode("pass");
        ResetServiceBusy();
    }

    // 6c. quota32: Cloudflare's own "1027 daily limit" answer (HTTP 429, plain text): the player gets the calm busy sentence and the code
    // "service_busy" after ONE request (no retries of a day's limit), later calls are answered locally, and a running room's poll backs off
    {
        ResetServiceBusy();
        Mode("cf1027");
        Signal* g = NewSignal();
        g->Host("Quota");
        std::vector<uint64_t> atG = Run(g, 60000, Signal::FAILED);
        fprintf(stderr, "cf1027: Host() sent %zu request(s), error: %s\n", atG.size(), g->Error());
        CHECK(g->GetStatus() == Signal::FAILED);
        CHECK(atG.size() == 1);
        CHECK(strstr(g->Error(), "busy") != nullptr);
        CHECK(strcmp(g->ErrorCode(), "service_busy") == 0);
        delete g;
        Mode("pass");
        g_virt += 20000;
        const int b1 = LogLines();
        Signal* g2 = NewSignal();
        g2->Host("Again");
        Run(g2, 2000);
        CHECK(g2->GetStatus() == Signal::FAILED && LogLines() == b1);   // still inside the 30 s the console keeps quiet: no request
        delete g2;
        ResetServiceBusy();
        Signal* g3 = NewSignal();
        g3->Host("Room");
        Run(g3, 4000, Signal::IN_ROOM);
        Mode("cf1027");
        std::vector<double> gq;
        Gaps("cf1027 on a running room's poll", Run(g3, 8 * 60000, Signal::FAILED), &gq);
        CHECK(g3->GetStatus() == Signal::FAILED && gq.size() >= 7 && gq.size() <= 10);   // backs off 2, 4, ... 60 s and gives up after about 4 minutes
        CHECK(strstr(g3->Error(), "busy") != nullptr);
        delete g3;
        Mode("pass");
        ResetServiceBusy();
    }

    // 7. svc3: the service asks the clients to stretch their polls (pollScale in the room answers): a held poll asks for 11 s instead of 8
    // (REAL clock, about 12 s)
    {
        ResetServiceBusy();
        Mode("pace");
        HttpSignalConfig cfg;
        snprintf(cfg.baseUrl, sizeof cfg.baseUrl, "%s", g_proxy.c_str());
        snprintf(cfg.token, sizeof cfg.token, "%s", g_token.c_str());
        snprintf(cfg.appVersion, sizeof cfg.appVersion, "3.0.0");
        cfg.pollWaitSec = 8;
        Signal* h = CreateHttpSignal(cfg);
        const int before = LogLines();
        h->Host("Paced");
        const uint64_t t0 = SteadyNowMs(0);
        bool w8 = false, w11 = false;
        while (SteadyNowMs(0) - t0 < 16000 && !w11) {
            h->Poll(); HttpPump(); usleep(5000);
            const std::vector<std::string> L = LogText();
            for (size_t i = size_t(before); i < L.size(); ++i) {
                if (L[i].find("wait=8") != std::string::npos) w8 = true;
                if (L[i].find("wait=11") != std::string::npos) w11 = true;
            }
        }
        fprintf(stderr, "pace: first poll wait=8 seen %d, the stretched poll wait=11 seen %d (%u ms)\n", w8, w11, unsigned(SteadyNowMs(0) - t0));
        CHECK(w8 && w11);
        delete h;
        Mode("pass");
    }

    // 8. svcfix2: the service's poll plan is obeyed (25 s holds while forming, the direct-link report, an immediate ask on a loss) and
    // /v1/info is asked once per session. REAL clock, about 25 s.
    {
        ResetServiceBusy();
        ResetServiceInfoCache();
        Mode("pass");
        auto cfgFor = [&](const std::string& tok) {
            HttpSignalConfig c;
            snprintf(c.baseUrl, sizeof c.baseUrl, "%s", g_proxy.c_str());
            snprintf(c.token, sizeof c.token, "%s", tok.c_str());
            snprintf(c.appVersion, sizeof c.appVersion, "3.0.0");
            c.pollWaitSec = 25;
            return c;
        };
        auto spin = [&](Signal* a, Signal* b, uint64_t ms) {
            const uint64_t t0 = SteadyNowMs(0);
            while (SteadyNowMs(0) - t0 < ms) { a->Poll(); if (b) b->Poll(); HttpPump(); usleep(5000); }
        };
        auto count = [&](int from, const char* needle) {
            int n = 0;
            const std::vector<std::string> L = LogText();
            for (size_t i = size_t(from); i < L.size(); ++i) if (L[i].find(needle) != std::string::npos) ++n;
            return n;
        };
        const int base = LogLines();
        Signal* h = CreateHttpSignal(cfgFor(g_token));
        h->Host("PlanHost");
        spin(h, 0, 3000);
        CHECK(h->GetStatus() == Signal::IN_ROOM);
        CHECK(count(base, "/v1/info") == 1);
        // forming: the held poll asks for 25 s and says it wants the plan
        spin(h, 0, 1500);
        CHECK(count(base, "wait=25&p=1") >= 1);
        // a second Host() in the same session (the first room left): no /v1/info again
        if (!g_token2.empty()) {
            Signal* j = CreateHttpSignal(cfgFor(g_token2));
            j->Join(h->Room().invite, "PlanGuest");
            spin(h, j, 3000);
            CHECK(j->GetStatus() == Signal::IN_ROOM);
            CHECK(count(base, "/v1/info") == 2);   // (the guest is another account: it asked once more)
            SigCandidates hm, jm;
            hm.valid = jm.valid = true;
            ParseAddr("203.0.113.5:41000", 0, &hm.pub); ParseAddr("192.168.1.5:41000", 0, &hm.local);
            ParseAddr("203.0.113.6:41000", 0, &jm.pub); ParseAddr("192.168.1.6:41000", 0, &jm.local);
            h->PublishCandidates(hm);
            j->PublishCandidates(jm);
            spin(h, j, 1500);
            h->Started();
            j->Started();
            h->SetDirectLinks(1u << 1);   // the console reports the heartbeats of its peer link (np_online.cpp)
            j->SetDirectLinks(1u << 0);
            const int b2 = LogLines();
            spin(h, j, 6000);
            fprintf(stderr, "plan: polls with a link report: host &l=2 %d, guest &l=1 %d; held 55 s: %d\n", count(b2, "&l=2"), count(b2, "&l=1"), count(b2, "wait=55"));
            CHECK(count(b2, "&l=2") >= 1 && count(b2, "&l=1") >= 1);
            CHECK(count(b2, "wait=55") >= 1);               // the host's settled hold: obeyed
            // a peer is lost: the console asks the service AT ONCE with the new report (the held poll is dropped)
            const int b3 = LogLines();
            const uint64_t t3 = SteadyNowMs(0);
            h->SetDirectLinks(0);
            while (SteadyNowMs(0) - t3 < 3000 && count(b3, "&l=0") == 0) spin(h, j, 50);
            fprintf(stderr, "plan: the lost link was reported after %u ms\n", unsigned(SteadyNowMs(0) - t3));
            CHECK(count(b3, "&l=0") >= 1 && SteadyNowMs(0) - t3 < 2500);
            j->Leave();
            delete j;
        }
        h->Leave();
        delete h;
        // the cache is for the session (this console's account): hosting again asks nothing but the room itself
        const int b4 = LogLines();
        Signal* h2 = CreateHttpSignal(cfgFor(g_token2.empty() ? g_token : g_token2));   // (the cache holds the last account that asked)
        h2->Host("PlanAgain");
        spin(h2, 0, 2000);
        CHECK(h2->GetStatus() == Signal::IN_ROOM);
        CHECK(count(b4, "/v1/info") == 0);
        h2->Leave();
        delete h2;
        ResetServiceInfoCache();
    }

    // every request carried the game's own User-Agent (a library default or none can be refused before the Worker runs)
    {
        const std::vector<std::string> lines = LogText();
        int without = 0;
        for (const std::string& ln : lines) if (ln.find("ua=GalaxyVita") == std::string::npos) ++without;
        fprintf(stderr, "user-agent: %zu requests, %d without ua=GalaxyVita\n", lines.size(), without);
        CHECK(!lines.empty() && without == 0);
    }
    fprintf(stderr, g_fail ? "signal_backoff_test: %d check(s) failed\n" : "signal_backoff_test: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
