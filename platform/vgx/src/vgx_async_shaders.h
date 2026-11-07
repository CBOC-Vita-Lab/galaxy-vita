// Asynchronous shaders (GXV_ASYNC_SHADERS): the bookkeeping of "a draw whose program is not compiled yet does not wait for the compiler".
//
// The problem. The renderer builds a program's GLSL at run time and vitaGL compiles it (libshacccg) inside glLinkProgram unless the compiled
// shader is in its cache (the memory card's, and the package's read-only seed behind it). A miss costs 50-300 ms on the render thread, in
// play, and the player sees a stall ("shader compiling interrupting mid game", the "draws' new programs" part of the stutter lines).
//
// The remedy. A draw (or the front end's CMD_PROGRAM / CMD_VARIANT announcement) that needs a program whose shaders are not in the cache
// hands the pair to ONE worker thread and goes on without it: the draw is skipped, as a draw of a program that failed to link always has
// been (Draw() returns when the program is not ok). The worker compiles the pair into the cache files (vitaGL's own file layout, written
// by vglCompileShaderPairToCache, tools/vitagl/patches/0020); when it is done the entry is Ready and the ordinary path links the program
// on the render thread, which then costs a file read and a registration, not a compile. No GL call is ever made off the render thread:
// the worker touches only the compiler, the translator and the files (the reasoning is in the patch and in the notes of this change).
//
// Never skipped (the back end decides, AsyncMustBlock in vgx_backend_gl.cpp): the player's draws (the HD pack's character list), the HUD and
// every 2D / screen-space draw (no depth test), render-to-texture passes, and opaque cached geometry (the map's static runs). Such a draw
// asks with `blocking`: it gets the old path, the render thread compiles it itself (the entry's worker job, if still queued, is dropped, if
// running, awaited, so the same shader is never compiled twice at once).
//
// This header is the pure part (no GL, no kernel): the per-pair state machine, the queue the worker takes from (most recently asked for
// first), the statistics, and a worker loop that calls a compile function it is given. tests/vgx_async_shaders.cpp drives it with a fake
// compiler that takes time. The Vita's compile function, cache probe and thread are in vgx_host_galaxy_vita.cpp (vgxvita_asyncShader*).
//
//   GXV_ASYNC_SHADERS=1 (default) on; 0 off, exactly the blocking behaviour of before (no thread, no probe, no state, no lock in vitaGL).
//   GXV_ASYNC_SHADERS_MAXWAIT_MS=<ms> (default 15000, 0 = never) a skippable draw that has waited this long for its program stops being
//                         skipped and takes the blocking path (a worker that is starved of CPU must not leave the picture without its
//                         effects for ever).
//   GXV_ASYNC_SMALL_VERTS=<n> (default 0 = off) an opaque cached draw of at most n vertices may be skipped too (small actors); off because
//                         the map's pieces are cached draws as well and the back end cannot tell a coin from a tile.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vgx::asyncsh {

// What a program needs compiled: the two shader texts exactly as glShaderSource receives them joined (that text is vitaGL's cache key),
// and whether the fragment shader is translated with half precision (GXV_FS_HALF=2, part of how vitaGL translates it).
struct Job {
    uint32_t key = 0;
    std::string vs, fs;
    bool fsHalf = false;
};

// The platform. Plain function pointers: nothing here is on a hot path, but the tests need no std::function and the console build none.
struct Host {
    // Render thread: are both compiled entries (the card's cache, then the package's seed) there? Called at most once per pair.
    bool (*inCache)(void* user, const Job& job) = nullptr;
    // Worker thread: compile what the cache lacks and write it there. 0: both entries are in the cache afterwards; else a failure.
    // *shaders gets how many shaders this call compiled (0..2).
    int (*compile)(void* user, const Job& job, int* shaders) = nullptr;
    // Milliseconds on a monotonic clock.
    double (*nowMs)(void* user) = nullptr;
    // Worker thread, optional: the render thread is about to wait for the compiler (on = true) / has stopped waiting; the worker thread's
    // priority is raised to the render thread's meanwhile (the compiler's lock has no priority inheritance, and the worker runs at the lowest).
    void (*boost)(void* user, bool on) = nullptr;
    void* user = nullptr;
};

enum class State : uint8_t {
    Unknown,   // never asked for
    Queued,    // not in the cache; the worker has the job in its queue
    Compiling, // the worker is compiling it
    Ready,     // in the cache: the ordinary path links it cheaply
    Failed,    // the worker's compile failed: the ordinary (blocking) path decides, as it always did
};

enum class Verdict : uint8_t {
    Go,   // go on to the ordinary path now (cache hit, a draw that must not wait, a failed job, a wait that ran out)
    Wait, // skip the draw: the worker has the shaders
};

// Totals of one 5 s window (Engine::TakeWindow), or since the start (Engine::Totals).
struct Stats {
    uint64_t queued = 0;         // pairs handed to the worker
    uint64_t compiledPairs = 0;  // jobs the worker finished (cache complete)
    uint64_t compiledShaders = 0; // shaders it compiled for them
    uint64_t failed = 0;         // jobs that ended in a failure
    uint64_t cacheHits = 0;      // pairs asked for that were in the cache after all (no wait)
    uint64_t skippedDraws = 0;   // draws (and variants) skipped because of a wait (NoteSkipped)
    uint64_t forcedBlocking = 0; // asks of a draw that must not wait, for a pair that was not ready: it took the blocking path
    uint64_t forcedByAge = 0;    // asks of a skippable draw that had waited MAXWAIT: blocking path
    uint64_t raced = 0;          // forced asks that found the worker compiling that very pair and waited for it (instead of compiling it twice)
    double longestWaitMs = 0;    // the longest a pair took from the ask to Ready (or Failed), or is still waiting
    double longestCompileMs = 0; // the longest single job
    double compileMs = 0;        // the worker's compile time in the window
    double probeMs = 0;          // the render thread's cache probes in the window
    uint32_t queueDepth = 0;     // jobs waiting now (not part of the window: a snapshot)
    bool Any() const { return queued || compiledPairs || failed || skippedDraws || forcedBlocking || forcedByAge || queueDepth; }
};

class Engine {
public:
    explicit Engine(const Host& host, double maxWaitMs = 15000.0) : host_(host), maxWaitMs_(maxWaitMs) {}

    // What a draw asks of a program that is not linked yet. `make` builds the Job when the pair has to be queued (it copies the texts).
    // `blocking`: the draw must not be skipped. Render thread only.
    template <class MakeJob> Verdict Ask(uint32_t key, bool blocking, MakeJob&& make) {
        const double now = host_.nowMs(host_.user);
        std::unique_lock<std::mutex> lk(m_);
        Entry& e = map_[key];
        switch (e.st) {
        case State::Ready:
        case State::Failed:
            return Verdict::Go;
        case State::Unknown: {
            if (blocking) return Verdict::Go; // (the ordinary path, without a probe: the entry stays Unknown for a later skippable ask)
            lk.unlock();
            Job job = make();
            job.key = key;
            const double t0 = host_.nowMs(host_.user);
            const bool have = host_.inCache(host_.user, job);
            const double dt = host_.nowMs(host_.user) - t0;
            lk.lock();
            Entry& f = map_[key]; // (the map may have grown while unlocked; only the render thread adds or changes Unknown entries)
            win_.probeMs += dt;
            if (have) {
                f.st = State::Ready;
                ++win_.cacheHits;
                ++tot_.cacheHits;
                return Verdict::Go;
            }
            f.st = State::Queued;
            f.tQueued = now;
            f.lastAsk = now;
            f.asks = 1;
            queue_.push_back(std::move(job));
            ++win_.queued;
            ++tot_.queued;
            cv_.notify_one();
            return Verdict::Wait;
        }
        case State::Queued:
        case State::Compiling:
            break;
        }
        if (blocking) return TakeOver(lk, key, e, true);
        if (maxWaitMs_ > 0 && now - e.tQueued >= maxWaitMs_) return TakeOver(lk, key, e, false);
        e.lastAsk = now;
        ++e.asks;
        return Verdict::Wait;
    }

    // A draw (or an announcement) was skipped because Ask said Wait.
    void NoteSkipped() { skipped_.fetch_add(1, std::memory_order_relaxed); }

    // True while the worker is inside a compile (the render thread then boosts it before it takes the compiler's lock itself).
    bool WorkerBusy() const { return busy_.load(std::memory_order_relaxed); }
    void Boost(bool on) { if (host_.boost) host_.boost(host_.user, on); }

    // The worker thread's body: returns when Stop() is called. (The caller makes the thread; vgx_host_galaxy_vita.cpp places it on a core.)
    void WorkerLoop() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            RunOne(lk);
        }
    }
    // Tests: the worker's step on the calling thread, without waiting. False when the queue is empty.
    bool PumpOne() {
        std::unique_lock<std::mutex> lk(m_);
        if (queue_.empty()) return false;
        RunOne(lk);
        return true;
    }
    void Stop() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
    }

    State StateOf(uint32_t key) const {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = map_.find(key);
        return it == map_.end() ? State::Unknown : it->second.st;
    }
    size_t QueueDepth() const {
        std::lock_guard<std::mutex> lk(m_);
        return queue_.size();
    }

    // The window since the last call (and the snapshot parts), then the window starts again.
    Stats TakeWindow() {
        const double now = host_.nowMs(host_.user);
        std::lock_guard<std::mutex> lk(m_);
        Stats s = win_;
        s.skippedDraws = skipped_.exchange(0, std::memory_order_relaxed);
        tot_.skippedDraws += s.skippedDraws;
        s.queueDepth = static_cast<uint32_t>(queue_.size());
        for (const auto& kv : map_)
            if (kv.second.st == State::Queued || kv.second.st == State::Compiling) s.longestWaitMs = std::max(s.longestWaitMs, now - kv.second.tQueued);
        win_ = Stats{};
        return s;
    }
    Stats Totals() const {
        std::lock_guard<std::mutex> lk(m_);
        Stats s = tot_;
        s.skippedDraws += skipped_.load(std::memory_order_relaxed);
        s.queueDepth = static_cast<uint32_t>(queue_.size());
        return s;
    }

private:
    struct Entry {
        State st = State::Unknown;
        double tQueued = 0, lastAsk = 0;
        uint32_t asks = 0;
    };

    // A draw that cannot wait (blocking) or has waited too long (age) is going to link the pair itself. Queued: the job leaves the queue.
    // Compiling: wait for the worker to finish that very job (a second compile of the same shaders would only take longer). Either way
    // the entry is Ready afterwards, or Failed, and the draw goes on to the ordinary path, whose own cache load finds the files.
    Verdict TakeOver(std::unique_lock<std::mutex>& lk, uint32_t key, Entry& e, bool blocking) {
        if (blocking) ++win_.forcedBlocking; else ++win_.forcedByAge;
        if (blocking) ++tot_.forcedBlocking; else ++tot_.forcedByAge;
        if (e.st == State::Queued) {
            for (auto it = queue_.begin(); it != queue_.end(); ++it)
                if (it->key == key) { queue_.erase(it); break; }
            e.st = State::Ready; // (the ordinary path will compile and cache it; a failure there is its business, as ever)
            return Verdict::Go;
        }
        ++win_.raced;
        ++tot_.raced;
        Boost(true); // (the render thread waits for a thread that runs at the lowest priority: it runs at the render thread's meanwhile)
        done_.wait(lk, [&] { return map_[key].st != State::Compiling; });
        Boost(false);
        return Verdict::Go;
    }

    // The most recently asked-for job first (what is on screen now), the one asked for most often next, the oldest last.
    std::deque<Job>::iterator Pick() {
        auto best = queue_.begin();
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            const Entry& a = map_[it->key];
            const Entry& b = map_[best->key];
            const long ba = static_cast<long>(a.lastAsk / 100.0), bb = static_cast<long>(b.lastAsk / 100.0); // (100 ms buckets)
            if (ba != bb ? ba > bb : a.asks != b.asks ? a.asks > b.asks : a.tQueued < b.tQueued) best = it;
        }
        return best;
    }

    void RunOne(std::unique_lock<std::mutex>& lk) {
        auto it = Pick();
        Job job = std::move(*it);
        queue_.erase(it);
        Entry& e = map_[job.key];
        e.st = State::Compiling;
        busy_.store(true, std::memory_order_relaxed);
        lk.unlock();
        const double t0 = host_.nowMs(host_.user);
        int shaders = 0;
        const int rc = host_.compile(host_.user, job, &shaders);
        const double t1 = host_.nowMs(host_.user);
        lk.lock();
        busy_.store(false, std::memory_order_relaxed);
        Entry& f = map_[job.key];
        f.st = rc == 0 ? State::Ready : State::Failed;
        const double wait = t1 - f.tQueued, took = t1 - t0;
        for (Stats* s : {&win_, &tot_}) {
            if (rc == 0) { ++s->compiledPairs; s->compiledShaders += static_cast<uint64_t>(shaders); } else ++s->failed;
            s->longestWaitMs = std::max(s->longestWaitMs, wait);
            s->longestCompileMs = std::max(s->longestCompileMs, took);
            s->compileMs += took;
        }
        done_.notify_all();
    }

    Host host_;
    double maxWaitMs_;
    mutable std::mutex m_;
    std::condition_variable cv_, done_;
    std::unordered_map<uint32_t, Entry> map_;
    std::deque<Job> queue_;
    Stats win_, tot_;
    std::atomic<uint64_t> skipped_{0};
    std::atomic<bool> busy_{false};
    bool stop_ = false;
};

} // namespace vgx::asyncsh
