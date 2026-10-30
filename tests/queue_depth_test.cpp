// Host test of the queue-depth adapter (platform/include/port/queue_depth.h): it decides like the inline code it replaced (an oracle
// copy of that code runs beside it), and it shows the paced-display trap and the way out. Synthetic times only.
#include "port/queue_depth.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// The code that was in RenderMain, verbatim in behaviour.
struct Oracle {
    int64_t sumNs = 0;
    unsigned n = 0;
    bool tight = false;
    unsigned maxInFlight = 2;
    unsigned Feed(int64_t t) {
        unsigned newLimit = 0;
        sumNs += t < 60000000 ? t : 60000000;
        if (++n >= 24) {
            const int64_t avg = sumNs / n;
            sumNs = 0;
            n = 0;
            if (!tight && avg >= 26000000) {
                tight = true;
                newLimit = 1;
            } else if (tight && avg <= 20000000) {
                tight = false;
                newLimit = maxInFlight + 1;
            }
        }
        return newLimit;
    }
};

int main() {
    // ---- same decisions as the code it replaced, over many pseudo-random sequences
    {
        unsigned seed = 12345;
        auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        for (int run = 0; run < 200; ++run) {
            port::depth::Adapter a;
            a.Configure(3);
            Oracle o;
            const int64_t base = 5000000 + static_cast<int64_t>(rnd() % 40000000);
            for (int i = 0; i < 2400; ++i) {
                // a level that drifts, plus jitter, plus an occasional 200 ms stall
                int64_t t = base + static_cast<int64_t>(rnd() % 15000000) - 7000000;
                if (rnd() % 97 == 0) t = 200000000;
                if (rnd() % 400 == 0) t = 0;
                Require(a.Feed(t) == o.Feed(t), "adapter and the old inline logic decide the same on every frame");
                Require(a.Tight() == o.tight, "same state");
            }
        }
    }

    // ---- thresholds, hysteresis, cap
    {
        port::depth::Adapter a;
        a.Configure(3);
        unsigned r = 0;
        for (int i = 0; i < 24; ++i) r = a.Feed(25900000);
        Require(r == 0 && !a.Tight(), "a mean just under 26 ms keeps the queue open");
        for (int i = 0; i < 24; ++i) r = a.Feed(26000000);
        Require(r == 1 && a.Tight(), "a mean of 26 ms cuts the queue to one frame");
        for (int i = 0; i < 24; ++i) r = a.Feed(23000000);
        Require(r == 0 && a.Tight(), "between 20 and 26 ms it stays cut (hysteresis)");
        for (int i = 0; i < 24; ++i) r = a.Feed(20000000);
        Require(r == 3 && !a.Tight(), "a mean of 20 ms opens the queue to the in-flight depth + 1");
        for (int i = 0; i < 23; ++i) a.Feed(1000000);
        Require(a.Feed(24 * 60000000LL * 10) == 0, "nothing is decided before 24 samples");
        port::depth::Adapter b;
        b.Configure(3);
        for (int i = 0; i < 23; ++i) b.Feed(20000000);
        Require(b.Feed(1000000000) == 0 && b.LastMean() == (23 * 20000000LL + 60000000) / 24, "one huge sample counts as 60 ms, no more");
    }

    // ---- the paced-display trap: wall time of a fast back end under a 30 FPS display wait
    {
        port::depth::Adapter wall, cpu;
        wall.Configure(3);
        cpu.Configure(3);
        unsigned cuts = 0, opens = 0;
        for (int i = 0; i < 24 * 100; ++i) {
            const int64_t jitter = (i * 7919) % 2000000;           // up to 2 ms
            const int64_t paced = 33300000 + jitter;               // waits for the display: about one 30 FPS period
            const int64_t own = 9000000 + jitter;                  // its own work: 9 to 11 ms
            const unsigned w = wall.Feed(paced), c = cpu.Feed(own);
            cuts += (w == 1) + (c == 1);
            opens += (w == 3) + (c == 3);
        }
        Require(wall.Tight(), "the old measure: a back end that needs 10 ms is cut to one frame and stays there under a 30 FPS display wait");
        Require(!cpu.Tight() && cuts == 1 && opens == 0, "own-work time: the queue stays open (only the wall measure cut it, once, and it never reopened)");
    }
    // ---- a back end that really is slower than the display is still cut when its own work is measured
    {
        port::depth::Adapter cpu;
        cpu.Configure(3);
        unsigned r = 0;
        for (int i = 0; i < 24; ++i) r = cpu.Feed(34000000);
        Require(r == 1 && cpu.Tight(), "34 ms of its own work a frame: cut to one frame, as before");
    }
    std::puts("PASS: queue-depth adapter: equal to the inline logic, thresholds, and the paced-display trap");
    return 0;
}
