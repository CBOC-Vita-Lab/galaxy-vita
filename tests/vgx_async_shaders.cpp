// GXV_ASYNC_SHADERS, the state machine and the worker (platform/vgx/src/vgx_async_shaders.h), driven with a fake compiler that takes time.
// Host build, no GL: tools/test_vgx_async_shaders.sh. What it checks:
//   * a pair that is not in the cache is queued and its draws are told to wait until the worker has finished it; then they go on;
//   * a pair that is in the cache is never queued and never waits (no compile at all);
//   * a draw that must not be skipped (the "blocking" ask) never gets Wait: on an unknown pair it goes at once (no probe, no job), on a
//     queued one the job is taken out of the queue, on one the worker is compiling it waits for that job and the pair is compiled ONCE;
//   * the worker takes the most recently asked-for job first;
//   * a skippable draw that has waited GXV_ASYNC_SHADERS_MAXWAIT_MS goes on to the blocking path; a failed job goes on too;
//   * the window statistics (compiled, skipped, longest wait) and the priority boost around a wait.
#include "../platform/vgx/src/vgx_async_shaders.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>
#include <thread>

using namespace vgx::asyncsh;

namespace {
void Require(bool good, const char* why) {
    if (good) return;
    std::fprintf(stderr, "FAIL: %s\n", why);
    std::exit(1);
}

// The fake platform: a set of cached texts, a compile that takes `compileMs` of real time (or of the virtual clock when virt is set).
struct Fake {
    std::mutex m;
    std::set<std::string> cache;
    std::map<uint32_t, int> compiles;       // compile calls by key
    std::map<uint32_t, int> concurrent;     // calls running now, by key (must never exceed 1)
    int maxConcurrentKey = 0, probes = 0, boosts = 0, unboosts = 0, running = 0, maxRunning = 0;
    double compileMs = 20;
    bool failNext = false;
    bool virt = false;
    double virtualNow = 0;
    std::vector<uint32_t> order;            // keys in the order the worker compiled them
    static double Now(void* u) {
        Fake* f = static_cast<Fake*>(u);
        if (f->virt) return f->virtualNow;
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static bool InCache(void* u, const Job& j) {
        Fake* f = static_cast<Fake*>(u);
        std::lock_guard<std::mutex> lk(f->m);
        ++f->probes;
        return f->cache.count(j.vs) && f->cache.count(j.fs);
    }
    static int Compile(void* u, const Job& j, int* shaders) {
        Fake* f = static_cast<Fake*>(u);
        {
            std::lock_guard<std::mutex> lk(f->m);
            ++f->compiles[j.key];
            f->maxConcurrentKey = std::max(f->maxConcurrentKey, ++f->concurrent[j.key]);
            f->maxRunning = std::max(f->maxRunning, ++f->running);
            f->order.push_back(j.key);
        }
        if (!f->virt) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(f->compileMs * 1000)));
        else f->virtualNow += f->compileMs;
        std::lock_guard<std::mutex> lk(f->m);
        --f->concurrent[j.key];
        --f->running;
        if (f->failNext) { f->failNext = false; return 7; }
        int n = 0;
        if (f->cache.insert(j.vs).second) ++n;
        if (f->cache.insert(j.fs).second) ++n;
        if (shaders) *shaders = n;
        return 0;
    }
    static void Boost(void* u, bool on) {
        Fake* f = static_cast<Fake*>(u);
        std::lock_guard<std::mutex> lk(f->m);
        (on ? f->boosts : f->unboosts)++;
    }
    Host MakeHost() {
        Host h;
        h.inCache = &InCache; h.compile = &Compile; h.nowMs = &Now; h.boost = &Boost; h.user = this;
        return h;
    }
};
Job MakeJob(uint32_t key) {
    Job j;
    j.key = key;
    j.vs = "vs" + std::to_string(key >> 16);
    j.fs = "fs" + std::to_string(key & 0xFFFF);
    return j;
}
constexpr uint32_t K(uint32_t vs, uint32_t fs) { return (vs << 16) | fs; }
Verdict AskKey(Engine& e, uint32_t key, bool blocking) {
    return e.Ask(key, blocking, [&] { return MakeJob(key); });
}
void WaitReady(Engine& e, uint32_t key) {
    for (int i = 0; i < 2000 && e.StateOf(key) != State::Ready && e.StateOf(key) != State::Failed; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
} // namespace

int main() {
    // ---- 1. a missing pair: draws wait until the worker has written it, then go on; the render thread never blocks in between
    {
        Fake f;
        f.compileMs = 40;
        Engine e(f.MakeHost());
        std::thread worker([&] { e.WorkerLoop(); });
        const uint32_t key = K(1, 2);
        const double t0 = Fake::Now(&f);
        Require(AskKey(e, key, false) == Verdict::Wait, "a pair that is not in the cache: Wait");
        const double askMs = Fake::Now(&f) - t0;
        Require(askMs < 30.0, "the ask itself is quick (a probe and a queue push, not the compile)");
        unsigned waits = 0;
        while (AskKey(e, key, false) == Verdict::Wait) { ++waits; e.NoteSkipped(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); Require(waits < 2000, "the worker finishes"); }
        Require(waits >= 3, "the draws waited for several asks (a compile takes time)");
        Require(e.StateOf(key) == State::Ready, "Ready afterwards");
        Require(AskKey(e, key, false) == Verdict::Go, "Go afterwards");
        Require(f.compiles[key] == 1, "compiled once");
        const Stats s = e.TakeWindow();
        Require(s.queued == 1 && s.compiledPairs == 1 && s.compiledShaders == 2 && s.failed == 0, "window: one pair, two shaders");
        Require(s.skippedDraws == waits, "window: the skipped draws are counted");
        Require(s.longestWaitMs >= 35.0 && s.longestCompileMs >= 35.0, "window: the longest wait is at least the compile");
        Require(!e.TakeWindow().Any(), "the window starts again");
        e.Stop();
        worker.join();
    }
    // ---- 2. a pair in the cache never waits and never reaches the worker
    {
        Fake f;
        f.cache = {"vs3", "fs4"};
        Engine e(f.MakeHost());
        Require(AskKey(e, K(3, 4), false) == Verdict::Go, "in the cache: Go at once");
        Require(e.QueueDepth() == 0 && f.compiles.empty(), "nothing queued, nothing compiled");
        Require(e.Totals().cacheHits == 1, "counted as a cache hit");
        Require(AskKey(e, K(3, 4), false) == Verdict::Go && f.probes == 1, "asked again: no second probe");
        // one shader of two missing is a miss
        f.cache = {"vs5"};
        Require(AskKey(e, K(5, 6), false) == Verdict::Wait, "only the vertex shader cached: still a miss");
        Require(e.PumpOne(), "the worker's step");
        Require(AskKey(e, K(5, 6), false) == Verdict::Go && f.compiles[K(5, 6)] == 1, "compiled by the pump, then Go");
    }
    // ---- 3. draws that must not wait
    {
        Fake f;
        Engine e(f.MakeHost());
        // unknown pair: the old path at once, no probe, no job, and the pair stays unknown for a later skippable ask
        Require(AskKey(e, K(1, 1), true) == Verdict::Go, "blocking ask on an unknown pair: Go");
        Require(f.probes == 0 && e.QueueDepth() == 0 && e.StateOf(K(1, 1)) == State::Unknown, "...without a probe, a job or a state");
        // queued pair (an announcement queued it): the blocking ask takes the job out of the queue
        Require(AskKey(e, K(2, 2), false) == Verdict::Wait && e.QueueDepth() == 1, "an announcement queues a pair");
        Require(AskKey(e, K(2, 2), true) == Verdict::Go, "the first draw that must not wait: Go");
        Require(e.QueueDepth() == 0 && !e.PumpOne(), "the job left the queue (nothing for the worker to compile)");
        Require(f.compiles.empty(), "no compile was made by the worker for it");
        Require(e.TakeWindow().forcedBlocking == 1, "counted");
    }
    // ---- 3b. the worker is compiling that very pair: the blocking ask waits for it, the pair is compiled once
    {
        Fake f;
        f.compileMs = 80;
        Engine e(f.MakeHost());
        std::thread worker([&] { e.WorkerLoop(); });
        const uint32_t key = K(7, 8);
        Require(AskKey(e, key, false) == Verdict::Wait, "queued");
        for (int i = 0; i < 500 && e.StateOf(key) != State::Compiling; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(e.StateOf(key) == State::Compiling && e.WorkerBusy(), "the worker is in it");
        const double t0 = Fake::Now(&f);
        Require(AskKey(e, key, true) == Verdict::Go, "blocking ask during the compile: Go");
        const double waited = Fake::Now(&f) - t0;
        Require(e.StateOf(key) == State::Ready, "...but only once the worker had finished (Ready)");
        Require(waited > 10.0 && waited < 400.0, "...it waited for the worker's compile, not for a second one");
        Require(f.compiles[key] == 1 && f.maxConcurrentKey == 1, "the pair was compiled once");
        Require(f.boosts == 1 && f.unboosts == 1, "the worker was boosted for the wait, and set back");
        Require(e.TakeWindow().raced == 1, "counted");
        e.Stop();
        worker.join();
    }
    // ---- 4. the order: the most recently asked-for first, then the most asked for, the oldest last
    {
        Fake f;
        f.virt = true;
        Engine e(f.MakeHost());
        f.virtualNow = 1000;
        Require(AskKey(e, K(1, 1), false) == Verdict::Wait, "a");
        f.virtualNow = 1010;
        Require(AskKey(e, K(1, 2), false) == Verdict::Wait, "b");
        f.virtualNow = 1020;
        Require(AskKey(e, K(1, 3), false) == Verdict::Wait, "c");
        f.virtualNow = 2000; // (a second later only b and c are still on screen; c asked three times)
        AskKey(e, K(1, 2), false);
        AskKey(e, K(1, 3), false);
        AskKey(e, K(1, 3), false);
        f.virtualNow = 2050;
        AskKey(e, K(1, 3), false);
        while (e.PumpOne()) {}
        Require(f.order.size() == 3 && f.order[0] == K(1, 3) && f.order[1] == K(1, 2) && f.order[2] == K(1, 1),
                "order: c (asked last and most), b, a (not asked since it was queued)");
    }
    // ---- 5. a wait that runs out, a failed job
    {
        Fake f;
        f.virt = true;
        Engine e(f.MakeHost(), 15000.0);
        f.virtualNow = 100;
        Require(AskKey(e, K(9, 9), false) == Verdict::Wait, "queued");
        f.virtualNow = 10000;
        Require(AskKey(e, K(9, 9), false) == Verdict::Wait, "10 s: still waiting");
        f.virtualNow = 16000;
        Require(AskKey(e, K(9, 9), false) == Verdict::Go, "after MAXWAIT: the blocking path");
        Require(e.QueueDepth() == 0 && e.StateOf(K(9, 9)) == State::Ready, "...the job left the queue");
        Require(e.TakeWindow().forcedByAge == 1, "counted");
        Engine e2(f.MakeHost(), 0.0); // 0: never
        f.virtualNow = 20000;
        AskKey(e2, K(1, 1), false);
        f.virtualNow = 9e9;
        Require(AskKey(e2, K(1, 1), false) == Verdict::Wait, "MAXWAIT 0: never forced");
        // failure
        Engine e3(f.MakeHost());
        f.failNext = true;
        Require(AskKey(e3, K(4, 4), false) == Verdict::Wait, "queued");
        e3.PumpOne();
        Require(e3.StateOf(K(4, 4)) == State::Failed, "failed job: Failed");
        Require(AskKey(e3, K(4, 4), false) == Verdict::Go, "Failed: Go (the ordinary path decides, as it always did)");
        Require(e3.TakeWindow().failed == 1, "counted");
    }
    // ---- 6. an empty cache: many pairs, a few hundred asks per "frame", nothing but the first ask of a pair costs the render thread anything
    {
        Fake f;
        f.compileMs = 5;
        Engine e(f.MakeHost());
        std::thread worker([&] { e.WorkerLoop(); });
        double worst = 0;
        unsigned skipped = 0, went = 0;
        for (int frame = 0; frame < 400; ++frame) {
            for (uint32_t i = 0; i < 40; ++i) {
                const double t0 = Fake::Now(&f);
                const Verdict v = AskKey(e, K(i / 8, i), false);
                worst = std::max(worst, Fake::Now(&f) - t0);
                if (v == Verdict::Wait) { e.NoteSkipped(); ++skipped; } else ++went;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Require(worst < 25.0, "no ask took long (the compiles are the worker's)");
        Require(skipped > 0 && went > 0, "draws were skipped at first and went on later");
        for (uint32_t i = 0; i < 40; ++i) WaitReady(e, K(i / 8, i));
        for (uint32_t i = 0; i < 40; ++i) Require(f.compiles[K(i / 8, i)] == 1, "every pair compiled exactly once");
        Require(f.maxRunning == 1, "one compile at a time");
        e.Stop();
        worker.join();
        std::printf("empty cache: 40 pairs, %u skips, %u goes, slowest ask %.2f ms\n", skipped, went, worst);
    }
    std::printf("PASS async shaders: state machine, worker, blocking classes, order, ageing, failure, statistics\n");
    return 0;
}
