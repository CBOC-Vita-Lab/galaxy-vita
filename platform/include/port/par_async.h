// GXV_PAR_VIEW=2 (drawpar, 6 Nov): a recorded batch of per-object jobs that is finished asynchronously, while the game thread goes
// on with the work that reads the results, and that every reader "touches" first (header-only; used by
// decomp/src/Game/LiveActor/ModelManager.cpp and by the host test tests/par_view_async.cpp).
//
// Recording is par_batch.h's PendingBatch, unchanged in meaning: the game thread walks its loop in order and records a job instead
// of computing it (Add); a job is keyed by the one object all of its inputs belong to (a J3DMtxBuffer), and anything about to read
// or write that object while the job is pending first runs it in place (Touch; a second Add of the same key does the same).
// Unlike PendingBatch, the batch has no join. Dispatch() hands the pending jobs to a worker (port::par::SpecWorker, par_spec.h, on
// another core; the caller owns it) and returns at once. From then until Retire():
//   - each job also has up to two OUTPUT keys (the draw-matrix half and the normal-matrix half it writes). Anything about to read an
//     output calls TouchOut(output): if its job is still pending, the calling thread takes it and computes it (an atomic claim, so
//     a job is computed exactly once, by the worker or by a toucher); if the worker is in the middle of it, the toucher waits for it;
//     when every job is done, a touch is one acquire load;
//   - Touch(key) does the same for anything about to write a job's inputs or outputs;
//   - Retire() makes sure every job is done (taking the ones the worker has not reached), after which the caller quiesces the worker.
// The states are par_spec.h's: 0 pending -> 1 running (the worker) -> 2 done; 0 pending -> 3 taken (a toucher) -> 2 done.
// A result is read only after an acquire of its state (or of the count of jobs left, which every completion decrements with a
// release), so the reader sees every byte the job wrote, whoever computed it. The output and key tables are written only while
// recording (game thread) and published by the dispatch's release; afterwards they are read-only, so any thread may touch.
// No allocation per job or per batch (malloc'd once at Init; never operator new: on game threads it takes the game heap).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C++" {  // (platform/src/port/perf.cpp includes this inside an extern "C" block)
namespace port {
namespace par {

namespace async_detail {
enum : uint8_t { kPending = 0, kRunning = 1, kDone = 2, kTaken = 3 };  // (the values of par_spec.h's kJob*: the worker uses those)
inline void Relax() {
#if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}
}  // namespace async_detail

template < class Job >
class AsyncBatch {
public:
    typedef void (*RunFn)(Job&);

    struct Stats {
        uint32_t jobs = 0;        // recorded
        uint32_t inlineRuns = 0;  // run in the walk (a touch or a second job of the same key while recording)
        uint32_t overflow = 0;    // past the capacity: the caller computed them at once
        uint32_t dispatched = 0;  // still pending at the dispatch
        uint32_t byToucher = 0;   // taken by a touch after the dispatch (the draw reached them first)
        uint32_t byRetire = 0;    // taken at the retire (nobody read them, and the worker had not reached them)
        uint32_t waits = 0;       // touches that found the worker in the middle of the job and waited for it
        uint32_t spins = 0;       // the polls of those waits
    };

    bool Init(uint32_t capacity, RunFn fn) {
        if (jobs != nullptr) {
            return true;
        }
        if (capacity < 16) {
            capacity = 16;
        }
        uint32_t slots = 1;
        while (slots < capacity * 4) {  // (the output table holds two keys a job: at most half full)
            slots <<= 1;
        }
        jobs = static_cast< Job* >(malloc(sizeof(Job) * capacity));
        keys = static_cast< const void** >(malloc(sizeof(const void*) * capacity));
        touched = static_cast< uint8_t* >(malloc(capacity));
        items = static_cast< std::atomic< uint8_t >* >(malloc(sizeof(std::atomic< uint8_t >) * capacity));
        keyTable = static_cast< Slot* >(calloc(slots, sizeof(Slot)));
        outTable = static_cast< Slot* >(calloc(slots, sizeof(Slot)));
        if (jobs == nullptr || keys == nullptr || touched == nullptr || items == nullptr || keyTable == nullptr || outTable == nullptr) {
            free(jobs);
            free(keys);
            free(touched);
            free(items);
            free(keyTable);
            free(outTable);
            jobs = nullptr;
            return false;
        }
        for (uint32_t i = 0; i < capacity; ++i) {
            new (&items[i]) std::atomic< uint8_t >(async_detail::kDone);
        }
        cap = capacity;
        mask = slots - 1;
        run = fn;
        return true;
    }
    bool Ready() const { return jobs != nullptr; }
    bool Recording() const { return recording; }
    // (the game thread's view) jobs were dispatched and the batch is not retired yet
    bool Outstanding() const { return outstanding; }

    // A new batch (the last one must be retired).
    void Begin() {
        recording = true;
        count = 0;
        st = Stats();
        if (++stamp == 0) {  // (a new stamp empties the tables; on the wrap, really empty them)
            memset(keyTable, 0, sizeof(Slot) * (mask + 1));
            memset(outTable, 0, sizeof(Slot) * (mask + 1));
            stamp = 1;
        }
    }
    // A slot for the job of `key` writing out0 / out1 (nullptr: none), or nullptr: not recording, or full (compute at once). A pending
    // job of the same key runs first.
    Job* Add(const void* key, const void* out0, const void* out1) {
        if (!recording) {
            return nullptr;
        }
        Slot* s = Find(keyTable, key);
        if (s->stamp == stamp) {
            RunInline(s->index);
        }
        if (count == cap) {
            ++st.overflow;  // (the key's slot stays, its job done: a probe chain through it must not break)
            return nullptr;
        }
        s->key = key;
        s->stamp = stamp;
        s->index = count;
        Put(out0, count);
        Put(out1, count);
        keys[count] = key;
        touched[count] = 0;
        items[count].store(async_detail::kPending, std::memory_order_relaxed);
        ++st.jobs;
        return &jobs[count++];
    }
    // The recording is over: the jobs still pending are handed out. Returns how many there are (0: nothing outstanding; the caller
    // need not dispatch nor retire). The caller then publishes [0, Count()) of Items() to its worker with a release (SpecWorker::
    // Dispatch), with Item() as the function and this as the context.
    uint32_t Dispatch() {
        recording = false;
        uint32_t pending = 0;
        for (uint32_t i = 0; i < count; ++i) {
            pending += items[i].load(std::memory_order_relaxed) == async_detail::kPending ? 1u : 0u;
        }
        st.dispatched = pending;
        left.store(pending, std::memory_order_relaxed);  // (published with the batch by the worker dispatch's release)
        outstanding = pending != 0;
        return pending;
    }
    // The worker's function (SpecWorker calls it on the jobs it claimed: pending -> running; it marks them done afterwards).
    static void Item(void* self, uint32_t i) {
        AsyncBatch* b = static_cast< AsyncBatch* >(self);
        b->run(b->jobs[i]);
        b->left.fetch_sub(1, std::memory_order_release);
    }
    std::atomic< uint8_t >* Items() { return items; }
    uint32_t Count() const { return count; }

    // Something is about to read or write what `key`'s job reads or writes: the job is done when this returns.
    void Touch(const void* key) {
        if (recording) {
            const Slot* s = Find(keyTable, key);
            if (s->stamp == stamp) {
                RunInline(s->index);
            }
            return;
        }
        if (!outstanding) {
            return;
        }
        const Slot* s = Find(keyTable, key);
        if (s->stamp == stamp) {
            touched[s->index] = 1;  // (its outputs may be written again after this: the verify leaves it out)
            Ensure(s->index, false);
        }
    }
    // Something is about to read `out` (a job's output): its job is done when this returns. Cheap once all are done. (The claims are
    // atomic for any thread; the counters and the touched flags assume the game thread, the only one that draws.)
    void TouchOut(const void* out) {
        if (left.load(std::memory_order_acquire) == 0) {
            return;
        }
        const Slot* s = Find(outTable, out);
        if (s->stamp == stamp) {
            Ensure(s->index, false);
        }
    }
    // Every job done (the ones nobody has started are run here, in order). The caller then quiesces its worker (SpecWorker::Quiesce)
    // before the next Begin: the worker may still be walking the states, finding nothing to take.
    void Retire() {
        if (!outstanding) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            Ensure(i, true);
        }
        outstanding = false;
    }

    // (after Retire, until the next Begin) the job i; whether it is the last of its key (no later job of the key superseded it); and
    // whether something touched its key after the dispatch (it may have written its outputs again since).
    const Job& At(uint32_t i) const { return jobs[i]; }
    bool IsLatest(uint32_t i) const {
        const Slot* s = Find(keyTable, keys[i]);
        return s->stamp == stamp && s->index == i;
    }
    bool TouchedAfterDispatch(uint32_t i) const { return touched[i] != 0; }
    const Stats& Counters() const { return st; }

private:
    struct Slot {
        const void* key;
        uint32_t stamp;
        uint32_t index;
    };
    Slot* Find(Slot* table, const void* key) const {
        uintptr_t h = reinterpret_cast< uintptr_t >(key);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        uint32_t i = static_cast< uint32_t >(h) & mask;
        while (table[i].stamp == stamp && table[i].key != key) {
            i = (i + 1) & mask;
        }
        return &table[i];
    }
    void Put(const void* out, uint32_t index) {
        if (out == nullptr) {
            return;
        }
        Slot* s = Find(outTable, out);
        s->key = out;
        s->stamp = stamp;
        s->index = index;  // (a later job of the same output: the earlier one has run, as Add ran it)
    }
    // (recording, game thread only: no worker yet)
    void RunInline(uint32_t i) {
        if (items[i].load(std::memory_order_relaxed) == async_detail::kPending) {
            run(jobs[i]);
            items[i].store(async_detail::kDone, std::memory_order_relaxed);
            ++st.inlineRuns;
        }
    }
    void Ensure(uint32_t i, bool retire) {
        uint8_t v = items[i].load(std::memory_order_acquire);
        if (v == async_detail::kDone) {
            return;
        }
        if (v == async_detail::kPending && items[i].compare_exchange_strong(v, async_detail::kTaken, std::memory_order_acq_rel)) {
            run(jobs[i]);
            items[i].store(async_detail::kDone, std::memory_order_release);
            left.fetch_sub(1, std::memory_order_release);
            ++(retire ? st.byRetire : st.byToucher);
            return;
        }
        // (the worker is in it: wait until it is done)
        ++st.waits;
        while (items[i].load(std::memory_order_acquire) != async_detail::kDone) {
            ++st.spins;
            async_detail::Relax();
        }
    }

    Job* jobs = nullptr;
    const void** keys = nullptr;
    uint8_t* touched = nullptr;
    std::atomic< uint8_t >* items = nullptr;
    Slot* keyTable = nullptr;
    Slot* outTable = nullptr;
    RunFn run = nullptr;
    uint32_t cap = 0, mask = 0, count = 0, stamp = 0;
    bool recording = false, outstanding = false;
    Stats st;
    alignas(64) std::atomic< uint32_t > left{0};
};

}  // namespace par
}  // namespace port
}  // extern "C++"
