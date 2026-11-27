// hitch30: host proof of port/wait_bound.h and of the two waits that use it (par_plant.h PlantGameOwn, par_spec.h ForkJoin).
// g++ -std=c++17 -Iplatform/include tests/wait_bound.cpp -pthread -o /tmp/wait_bound && /tmp/wait_bound
#include "port/par_plant.h"
#include "port/par_spec.h"
#include "port/wait_bound.h"

#include <cstdio>
#include <unistd.h>
#include <thread>
#include <vector>

using namespace port::par;
static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);             \
            ++fails;                                                     \
        }                                                                \
    } while (0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 1. no wait: nothing is counted, no clock read needed.
    {
        WaitCensus c;
        std::atomic< int > flag{0};
        uint32_t us = BoundedWait([&] { return flag.load() != 0; }, c, 100, 50);
        CHECK(us == 0 && c.waits.load() == 0);
    }
    // 2. a short wait ends inside the spin bound: no nap, spunOut 0.
    {
        WaitCensus c;
        std::atomic< int > flag{1};
        std::thread w([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            flag.store(0);
        });
        uint32_t us = BoundedWait([&] { return flag.load() != 0; }, c, 20000 /* generous: a slow CI box */, 50);
        w.join();
        CHECK(c.waits.load() == 1 && c.spunOut.load() == 0);
        CHECK(us < 20000);
    }
    // 3. a long wait: spins at most spinUs, then the polls are at least ~napUs apart (the thread sleeps), counters say so.
    {
        WaitCensus c;
        std::atomic< int > flag{1};
        std::vector< uint64_t > stamps;
        std::thread w([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            flag.store(0);
        });
        const uint64_t t0 = NowUs();
        uint32_t us = BoundedWait(
            [&] {
                stamps.push_back(NowUs());
                return flag.load() != 0;
            },
            c, 100, 200);
        w.join();
        CHECK(c.waits.load() == 1 && c.spunOut.load() == 1);
        CHECK(us >= 29000);
        CHECK(c.maxUs.load() == us && c.maxEverUs.load() == us);
        // polls after the spin phase: gaps >= 150 us (a 200 us nap); count the busy polls: those inside the first ~100 us (+ one batch of 16)
        size_t spinPolls = 0, napGaps = 0, smallGapsAfterSpin = 0;
        uint64_t spinEnd = 0;
        for (size_t i = 0; i < stamps.size(); ++i) {
            if (stamps[i] - t0 < 100 + 200) {
                ++spinPolls;
                spinEnd = stamps[i];
            }
        }
        for (size_t i = 1; i < stamps.size(); ++i) {
            if (stamps[i - 1] > spinEnd + 400) {
                ++napGaps;
                if (stamps[i] - stamps[i - 1] < 150) {
                    ++smallGapsAfterSpin;
                }
            }
        }
        CHECK(napGaps > 10);
        CHECK(smallGapsAfterSpin == 0);
        // total polls in a 30 ms wait: a pure spin would make millions; the bounded one stays in the thousands at most
        CHECK(stamps.size() < 2000);
        std::printf("test3: wait %u us, %zu polls (%zu in the spin window), %zu nap gaps\n", us, stamps.size(), spinPolls, napGaps);
    }
    // 4. spinUs == 0 is the old pure spin (never naps): the poll count of a 5 ms wait is large.
    {
        WaitCensus c;
        std::atomic< int > flag{1};
        std::atomic< uint64_t > polls{0};
        std::thread w([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            flag.store(0);
        });
        BoundedWait(
            [&] {
                polls.fetch_add(1);
                return flag.load() != 0;
            },
            c, 0, 50);
        w.join();
        CHECK(c.waits.load() == 1 && c.spunOut.load() == 0);
        CHECK(polls.load() > 10000);
    }
    // 5. PlantGameOwn: kPlBuilding then kPlBuilt by the worker after 3 ms: the game thread gets kPlBuilt, waited >= 3 ms, counted.
    {
        std::atomic< uint8_t > phase{kPlBuilding};
        std::thread w([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            PlantWorkerBuilt(phase);
        });
        uint64_t waited = 0;
        const uint8_t r = PlantGameOwn(phase, &waited);
        w.join();
        CHECK(r == kPlBuilt);
        CHECK(waited >= 3000);
        CHECK(PlantWaitCensus().waits.load() == 1 && PlantWaitCensus().spunOut.load() == 1);
        // free -> kPlGame, no wait, nothing counted
        std::atomic< uint8_t > p2{kPlFree};
        uint64_t w2 = 99;
        CHECK(PlantGameOwn(p2, &w2) == kPlGame && w2 == 0 && PlantWaitCensus().waits.load() == 1);
    }
    // 6. ForkJoin on a real SpecWorker with a slow job: the results are the serial loop's, the wait is counted.
    {
        SpecWorker& sw = *new SpecWorker;
        const bool ok = sw.Start(1, 0);
        (void)ok;
        if (sw.Ready()) {
            struct Ctx {
                std::atomic< int > out[8];
            } ctx;
            for (auto& o : ctx.out) o.store(0);
            std::atomic< uint8_t > items[8];
            ItemFn fn = [](void* c, uint32_t i) {
                if (i == 0) std::this_thread::sleep_for(std::chrono::milliseconds(4));
                static_cast< Ctx* >(c)->out[i].store(static_cast< int >(i * i + 1));
            };
            uint32_t waitUs = 0;
            const bool joined = ForkJoin(sw, items, fn, nullptr, &ctx, 8, &waitUs);
            CHECK(joined);
            for (int i = 0; i < 8; ++i) CHECK(ctx.out[i].load() == i * i + 1);
            std::printf("test6: join waited %u us; spec census waits %u spunOut %u\n", waitUs, SpecWaitCensus().waits.load(),
                        SpecWaitCensus().spunOut.load());
        } else {
            std::printf("test6: SpecWorker not available on this host (skipped)\n");
        }
    }
    char line[256];
    PlantWaitCensus().Line("plant", line, sizeof line);
    std::printf("%s\n", line);
    std::printf(fails ? "FAILED %d\n" : "wait_bound: all passed\n", fails);
    std::fflush(stdout);
    _exit(fails ? 1 : 0);  // (a SpecWorker has no Stop: its thread waits for work for ever)
}
