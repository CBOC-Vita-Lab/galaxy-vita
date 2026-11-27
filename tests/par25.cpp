// par25 (12 Nov 2026): host test of platform/include/port/par25.h. The Judge's verdicts (thresholds, windows in a row, backoff, probation, the mismatch latch,
// verify runs not judged, bounded estimates), the Honeyhive windows of the console logs (c76-tourK2, c77-s6K), the report line, and the Gate under two real
// threads (run under ThreadSanitizer by tools/test_par25.sh). PAR25_NEG=1: negative controls, each must fail its property.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include "port/par25.h"

using namespace port::par25;

static int sFail = 0, sChecks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++sChecks;                                                                \
        if (!(c)) {                                                               \
            ++sFail;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
        }                                                                         \
    } while (0)

// a 5 s window at 60 turns a second whose net is `netPerSec` ms/s
static Summary Win(double netPerSec, bool bound = false, bool judged = true, unsigned mism = 0) {
    Summary s;
    s.secs = 5.0;
    s.turns = 300;
    s.jobs = 1000;
    s.workerMs = 100;
    s.grossMs = netPerSec > 0 ? netPerSec * 5.0 + 10.0 : 10.0;
    s.costMs = s.grossMs - netPerSec * 5.0;
    s.bound = bound;
    s.judged = judged;
    s.mismatches = mism;
    return s;
}

static void TestThresholds() {
    Params p;
    Judge j;
    for (int i = 0; i < 50; ++i) CHECK(!j.Window(Win(+5.0), p));
    CHECK(j.Allowed());
    // a small loss (0.04 ms a turn = 2.4 ms/s) is inside the margin: never off
    for (int i = 0; i < 50; ++i) CHECK(!j.Window(Win(-2.4), p));
    CHECK(j.Allowed() && j.lowerings == 0);
    // exactly the margin is not below it
    for (int i = 0; i < 10; ++i) CHECK(!j.Window(Win(-3.0), p));
    CHECK(j.Allowed());
    // one negative window is not enough, a good one in between resets the run
    CHECK(!j.Window(Win(-10.0), p));
    CHECK(!j.Window(Win(+1.0), p));
    CHECK(!j.Window(Win(-10.0), p));
    CHECK(j.Allowed());
    // two in a row
    CHECK(j.Window(Win(-10.0), p));
    CHECK(j.st == kOff && j.lowerings == 1);
}

static void TestBackoffAndProbation() {
    Params p;
    Judge j;
    j.Window(Win(-10), p);
    j.Window(Win(-10), p);
    CHECK(j.st == kOff && j.offLeft == 12);
    for (int i = 0; i < 11; ++i) CHECK(!j.Tick(p) && j.st == kOff);
    CHECK(j.Tick(p) && j.st == kOn && j.probation);
    // a negative window on probation: off again at once, twice as long
    CHECK(j.Window(Win(-10), p));
    CHECK(j.st == kOff && j.offLeft == 24);
    for (int i = 0; i < 23; ++i) j.Tick(p);
    CHECK(j.Tick(p) && j.probation);
    CHECK(j.Window(Win(-10), p) && j.offLeft == 48);
    for (int i = 0; i < 47; ++i) j.Tick(p);
    CHECK(j.Tick(p));
    CHECK(j.Window(Win(-10), p) && j.offLeft == 96);
    for (int i = 0; i < 95; ++i) j.Tick(p);
    CHECK(j.Tick(p));
    CHECK(j.Window(Win(-10), p) && j.offLeft == 120);  // the cap
    for (int i = 0; i < 119; ++i) j.Tick(p);
    CHECK(j.Tick(p));
    CHECK(j.Window(Win(-10), p) && j.offLeft == 120);
    // good windows on probation clear the backoff
    for (int i = 0; i < 119; ++i) j.Tick(p);
    CHECK(j.Tick(p) && j.probation);
    CHECK(!j.Window(Win(+2), p) && !j.Window(Win(+2), p));
    CHECK(j.probation);
    CHECK(!j.Window(Win(+2), p) && !j.probation && j.backoff == 0);
    j.Window(Win(-10), p);
    CHECK(j.st == kOn);
    j.Window(Win(-10), p);
    CHECK(j.st == kOff && j.offLeft == 12);
    // windows posted while off change nothing (a worker that is off posts nothing, but a late one may arrive)
    CHECK(!j.Window(Win(+50), p) && j.st == kOff && j.offLeft == 12);
}

static void TestLatchAndUnjudged() {
    Params p;
    Judge j;
    CHECK(j.Window(Win(+5, false, true, 1), p));
    CHECK(j.st == kLatched && !j.Allowed());
    CHECK(!j.Window(Win(+50), p) && !j.Tick(p) && j.st == kLatched);
    Judge k;
    // a verify run is not judged (its merge holds the verify's own computation), but its mismatches latch
    for (int i = 0; i < 20; ++i) CHECK(!k.Window(Win(-100, false, false), p));
    CHECK(k.Allowed());
    CHECK(k.Window(Win(-100, false, false, 3), p) && k.st == kLatched);
    // a bounded net that is negative is negative for certain: it counts
    Judge b;
    b.Window(Win(-10, true), p);
    CHECK(b.Window(Win(-10, true), p) && b.st == kOff);
}

static void TestNoLimitCycle() {
    // a worker that is negative for an hour (720 ticks): with the backoff it is on probation about a dozen times; without it, every 14 ticks (51 times)
    Params p;
    Judge j;
    unsigned lowerings = 0;
    for (int tick = 0; tick < 720; ++tick) {
        if (j.Allowed()) j.Window(Win(-10), p);
        j.Tick(p);
    }
    lowerings = j.lowerings;
    CHECK(lowerings >= 6 && lowerings <= 14);
    std::printf("par25 test: a worker negative for an hour went off %u times (an unbacked-off one: about 51)\n", lowerings);
    if (std::getenv("PAR25_NEG")) {
        // negative control: the same without the doubling must fail the property above
        Judge n;
        for (int tick = 0; tick < 720; ++tick) {
            if (n.Allowed()) n.Window(Win(-10), p);
            n.Tick(p);
            n.backoff = 0;
        }
        CHECK(n.lowerings >= 6 && n.lowerings <= 14);
    }
}

static void TestHoneyhive() {
    // c76-tourK2, 04:09:38 (Honeyhive, par-anim default): a batch: saved 0.142 ms (walks 0.174 - copies 0.032), dispatch 0.230; 276 batches in 5.0 s
    Params p;
    {
        Summary s;
        s.secs = 5.0;
        s.turns = 276;
        s.jobs = 1655;
        s.workerMs = 0.366 * 276;
        s.grossMs = 0.174 * 276;
        s.costMs = (0.032 + 0.230) * 276;
        CHECK(NetPerSec(s) < -p.negMsPerSec);  // -4.9 ms/s: -0.088 ms a turn: a loss
        Judge j;
        j.Window(s, p);
        CHECK(j.Window(s, p) && j.st == kOff);
    }
    // c77-s6K, 06:00:16 (Honeyhive, PAR_ANIM=1): saved 0.212 (walks 0.267 - copies 0.055), dispatch 0.251; 301 batches
    {
        Summary s;
        s.secs = 5.0;
        s.turns = 301;
        s.jobs = 2184;
        s.workerMs = 0.344 * 301;
        s.grossMs = 0.267 * 301;
        s.costMs = (0.055 + 0.251) * 301;
        CHECK(NetPerSec(s) > -p.negMsPerSec && NetPerSec(s) < 0);  // -2.4 ms/s: break-even, kept
        Judge j;
        for (int i = 0; i < 10; ++i) j.Window(s, p);
        CHECK(j.Allowed());
    }
    // c77-s6K, 06:00:26: saved 0.298 (walks 0.361 - copies 0.064), dispatch 0.255: a small gain
    {
        Summary s;
        s.secs = 5.0;
        s.turns = 300;
        s.grossMs = 0.361 * 300;
        s.costMs = (0.064 + 0.255) * 300;
        CHECK(NetPerSec(s) > 0);
    }
    // par-dl, GXV_PAR_DL=1 (no verify), c77-hB3 / c77-s6A Honeyhive windows: "saved" = worker - wait - dispatch - merge, per overlapped drawn turn
    struct DlWin { double turns, worker, wait, dispatch, merge; };
    const DlWin dl[] = {
        {187, 1.736, 1.159, 0.077, 0.017},  // 07:11:26 hB3: saved +0.483 a drawn turn (+18 ms/s)
        {186, 0.952, 0.701, 0.088, 0.047},  // 07:11:56 hB3: saved +0.115 (+4.3 ms/s): the game thread took the job itself 97 times of 186
        {300, 0.000, 0.000, 0.021, 0.005},  // 07:09:52 hB3: a light window, 1 entry a turn: saved -0.026 (-1.6 ms/s, inside the margin)
    };
    const double expectNet[] = {18.0, 4.3, -1.6};
    for (int i = 0; i < 3; ++i) {
        Summary s;
        s.secs = 5.0;
        s.turns = dl[i].turns;
        s.workerMs = dl[i].worker * dl[i].turns;
        s.grossMs = s.workerMs;
        s.costMs = (dl[i].wait + dl[i].dispatch + dl[i].merge) * dl[i].turns;
        CHECK(NetPerSec(s) > expectNet[i] - 0.3 && NetPerSec(s) < expectNet[i] + 0.3);
        Judge j;
        for (int k = 0; k < 20; ++k) j.Window(s, p);
        CHECK(j.Allowed());  // none of the three is a reason to switch par-dl off
    }
}

static void TestFormat() {
    Summary s[kWorkers];
    Judge j[kWorkers];
    bool fresh[kWorkers] = {true, true, true, false, false, false};
    s[kAnim] = Win(+4.0);
    s[kPtcl] = Win(+1.5, true);
    s[kDl] = Win(-8.0);
    j[kDl].Window(s[kDl], Params());
    j[kDl].Window(s[kDl], Params());
    char buf[1200];
    const int n = Format(buf, sizeof buf, 5.0, 300.0, 1, s, j, fresh);
    CHECK(n > 0 && n < static_cast<int>(sizeof buf));
    std::printf("%s\n", buf);
    CHECK(std::strstr(buf, "par25: 5.0 s, 300 turns (mode 1: guard)") != nullptr);
    CHECK(std::strstr(buf, "mismatches 0") != nullptr);
    CHECK(std::strstr(buf, "dl OFF") != nullptr);
    CHECK(std::strstr(buf, "ptcl on +1.50<=") != nullptr);
    CHECK(std::strstr(buf, "view idle") != nullptr);
    // the proven total counts anim (+4.0) only: dl is off, ptcl is a bound
    CHECK(std::strstr(buf, "saved est +4.00 ms/s (+0.067 ms a turn) proven, up to +1.50 ms/s more") != nullptr);
    char tiny[40];
    CHECK(Format(tiny, sizeof tiny, 5.0, 300.0, 2, s, j, fresh) >= 0);  // truncation is safe
}

static void TestGateThreads() {
    Gate g;
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0}, bad{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&, t] {
            long n = 0;
            while (!stop.load(std::memory_order_relaxed) || n == 0) {
                for (int w = 0; w < kWorkers; ++w) {
                    const bool a = g.Allowed(w);
                    (void)a;
                    ++n;
                }
            }
            reads += n;
            (void)t;
        });
    }
    Params p;
    Judge j[kWorkers];
    for (int round = 0; round < 20000; ++round) {
        const int w = round % kWorkers;
        if (j[w].Allowed()) j[w].Window(Win(round % 7 == 0 ? -20 : +1), p);
        j[w].Tick(p);
        g.Set(w, j[w].Allowed());
        if (g.Allowed(w) != j[w].Allowed()) ++bad;
    }
    stop = true;
    for (auto& r : readers) r.join();
    CHECK(bad == 0);
    CHECK(reads > 0);
}

int main() {
    TestThresholds();
    TestBackoffAndProbation();
    TestLatchAndUnjudged();
    TestNoLimitCycle();
    TestHoneyhive();
    TestFormat();
    TestGateThreads();
    std::printf("par25 test: %d checks, %d failed\n", sChecks, sFail);
    return sFail == 0 ? 0 : 1;
}
