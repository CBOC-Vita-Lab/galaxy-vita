// Host test of MpListRefresh::Pacer (uxwait): the online list's self-refresh stays inside the quota rules.
#include "Game/MP/MpListRefreshLogic.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
using namespace MpListRefresh;

// Runs the pacer one frame (16 ms) at a time for `ms`, pressing every `pressEvery` ms (0 = never), answering each request after 400 ms
// (ok or not). Returns the number of requests it started.
static int run(Pacer& p, int64_t from, int64_t ms, int64_t pressEvery, bool ok, int64_t* answerAt, bool* running) {
    int n = 0;
    for (int64_t t = from; t < from + ms; t += 16) {
        if (pressEvery && (t - from) % pressEvery < 16) p.press(t);
        if (*running && t >= *answerAt) { *running = false; p.answered(t, ok); }
        if (p.due(t, *running, false)) { ++n; *running = true; *answerAt = t + 400; }
    }
    return n;
}

int main() {
    CHECK(kIntervalMs == 30000 && kMaxWaitMs == 240000 && kMaxFails == 4 && kIdleStopMs == 180000 && kVisitCapRequests == 20);
    { // the first request is a full interval after the screen's own; then every 30 s (2 a minute)
        Pacer p; p.enter(0);
        int64_t at = 0; bool run_ = false;
        CHECK(run(p, 0, 29000, 1000, true, &at, &run_) == 0);
        Pacer q; q.enter(0); at = 0; run_ = false;
        const int n = run(q, 0, 60000, 1000, true, &at, &run_);
        CHECK(n == 1 || n == 2);   // 30 s, 60.4 s
        std::printf("on screen with presses: %d requests in 60 s\n", n);
    }
    { // untouched: stops after 3 minutes without a press: 11 or 12 requests, and none afterwards
        Pacer p; p.enter(0);
        int64_t at = 0; bool r = false;
        const int n = run(p, 0, 5 * 60 * 1000, 0, true, &at, &r);
        CHECK(n >= 5 && n <= 6);
        CHECK(p.parked);
        std::printf("idle on screen: %d requests, then stopped\n", n);
        // a press wakes it: one request at once, then paced again
        p.press(310000);
        CHECK(p.due(310016, false, false));
    }
    { // never closer than the interval, whatever the presses
        Pacer p; p.enter(0);
        p.press(29000);
        CHECK(!p.due(29500, false, false));
        CHECK(p.due(30000, false, false));
        CHECK(!p.due(30016, false, false));
    }
    { // errors back off 30, 60, 120, then stop after 4 in a row
        Pacer p; p.enter(0);
        int64_t t = 30000;
        CHECK(p.due(t, false, false));
        p.answered(t, false);                         // 1st error: wait 60 s
        CHECK(!p.due(t + 59999, false, false)); CHECK(p.due(t + 60000, false, false));
        t += 60000; p.press(t); p.answered(t, false); // 2nd: 120 s
        CHECK(!p.due(t + 119999, false, false)); CHECK(p.due(t + 120000, false, false));
        t += 120000; p.press(t); p.answered(t, false);// 3rd: 240 s
        p.press(t + 100000);
        CHECK(!p.due(t + 239999, false, false));
        p.press(t + 239999);
        CHECK(p.due(t + 240000, false, false));
        t += 240000; p.press(t); p.answered(t, false);// 4th: stop
        CHECK(!p.due(t + 500000, false, false));
        CHECK(p.parked);
        p.press(t + 500016);                          // a press does not undo an error stop
        CHECK(!p.due(t + 500032, false, false));
        p.manual(t + 600000);                         // a manual refresh does
        CHECK(!p.parked && p.fails == 0);
        CHECK(p.due(t + 600000 + kIntervalMs, false, false));
    }
    { // a success clears the errors
        Pacer p; p.enter(0);
        p.answered(30000, false); p.answered(90000, false);
        p.answered(210000, true);
        CHECK(p.fails == 0 && p.nextAtMs == 210000 + kIntervalMs);
    }
    { // the visit cap (20 requests of its own) stops it even with presses
        Pacer p; p.enter(0);
        int64_t at = 0; bool r = false;
        const int n = run(p, 0, 12 * 60 * 1000, 1000, true, &at, &r);
        CHECK(n == 20);
        CHECK(p.parked);
        p.press(12 * 60 * 1000 + 1000);
        CHECK(!p.due(12 * 60 * 1000 + 40000, false, false));
        p.manual(12 * 60 * 1000 + 50000);                // by hand: a new count
        CHECK(p.requests == 0 && !p.parked);
        std::printf("pressing all the time: %d requests in 12 min, then stopped\n", n);
    }
    { // blocked (a window / the keyboard) or a request running: not due, and the request is not lost
        Pacer p; p.enter(0);
        CHECK(!p.due(40000, false, true));
        CHECK(!p.due(40016, true, false));
        CHECK(p.due(40032, false, false));
    }
    { // leaving the screen: nothing is due; entering again starts a fresh visit
        Pacer p; p.enter(0);
        p.leave();
        CHECK(!p.due(99999, false, false));
        p.enter(100000);
        CHECK(!p.due(100001, false, false));
        CHECK(p.due(130000, false, false));
    }
    { // a manual refresh: not twice within 10 s
        Pacer p; p.enter(0);
        CHECK(p.manual(100000));
        CHECK(!p.manual(105000));
        CHECK(!p.manual(109999));
        CHECK(p.manual(110000));
    }
    { // the hourly ceiling (30) holds for manual and self-made requests together and survives leaving and entering the screen
        Pacer p; p.enter(0);
        int64_t t = 0; int n = 0;
        for (int i = 0; i < 40; i++) {
            t += 11000;                              // (11 s apart: the gap is no obstacle, 40 presses in 7 minutes)
            if (p.manual(t)) ++n;
            p.leave(); p.enter(t + 1);               // a new visit each time (Friends > Blocked > back)
        }
        CHECK(n < kHourCap && n > 10);                // (each entry is a request of the screen's own and counts too: 40 entries + n manual = 30 + the entries after the ceiling)
        CHECK(!p.manual(t + 20000));
        CHECK(!p.due(t + 5 * 60 * 1000, false, false));   // self-made ones too (the idle stop may also fire: no request either way)
        p.enter(t + kHourMs + 100000);                     // an hour after the first ones: they have aged out
        CHECK(p.manual(t + kHourMs + 100001));
    }
    { // self-made requests count toward the ceiling too
        Pacer p;
        int64_t t = 0; int n = 0;
        for (int v = 0; v < 12; v++) {                // 12 visits of 6 requests (each below the 20-request visit cap)
            p.enter(t);
            for (int i = 0; i < 6; i++) { t += kIntervalMs; p.press(t); if (p.due(t, false, false)) ++n; p.answered(t, true); }
            p.leave();
        }
        CHECK(n <= kHourCap);
        std::printf("12 short visits in a row: %d requests\n", n);
    }
    { // entering the screen again and again (Friends > Blocked > back): the ceiling holds, the entries count
        Pacer p;
        int64_t t = 0;
        for (int i = 0; i < 29; i++) { p.enter(t); p.leave(); t += 60000; }   // 29 entries in 29 minutes
        CHECK(p.underCeiling(t));                  // 29 of 30
        p.enter(t); p.leave();                     // the 30th
        CHECK(!p.underCeiling(t + 1000));
        CHECK(!p.manual(t + 20000));
        p.enter(t + 30000);
        CHECK(!p.due(t + 30000 + kIntervalMs, false, false));   // nor does it ask by itself
        CHECK(p.underCeiling(t + kHourMs));        // the first entries age out after an hour
    }
    { // above the ceiling an entry is not noted again (the window is not stretched)
        Pacer p;
        for (int i = 0; i < kHourCap; i++) p.enter(i * 1000);
        CHECK(!p.underCeiling(40000));
        const int at = p.stampAt;
        p.enter(41000);
        CHECK(p.stampAt == at);
        CHECK(p.underCeiling(kHourMs + 1000 * (kHourCap - 1) + 1));   // all aged out one hour after the last real one
    }
    std::printf("list_refresh_test OK\n");
    return 0;
}
