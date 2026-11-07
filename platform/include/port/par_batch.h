// GXV_PAR_VIEW (par13, 5 Nov): the list of a parallel batch's jobs while the game thread walks its loop (header-only; used by
// decomp/src/Game/LiveActor/ModelManager.cpp and by the host test tests/par_exec_view.cpp).
//
// The walk stays on the game thread, in its order; a job that can be computed later is recorded instead of computed (Add), and
// the batch is run when the walk ends (platform/src/port/par_exec.h). The rule that keeps the result that of the serial walk:
// a job is keyed by the one object all of its inputs and outputs belong to (a J3DMtxBuffer for the view pass), and anything on
// the game thread that is about to read or write that object while its job is pending first runs that job, in place (Touch; Add
// does the same for a second job of the same key). So every job runs before anything that follows it in the serial order and
// touches what it touches, and after everything that precedes it (it is recorded at its place in the walk).
// No allocation per job: the arrays are made once (capacity jobs; a full batch makes Add return nullptr, and the caller computes
// that job at once, which is the serial order too).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace port {
namespace par {

template < class Job >
class PendingBatch {
public:
    typedef void (*RunFn)(Job&);

    bool Init(uint32_t capacity, RunFn fn) {
        if (jobs != nullptr) {
            return true;
        }
        if (capacity < 16) {
            capacity = 16;
        }
        uint32_t slots = 1;
        while (slots < capacity * 2) {
            slots <<= 1;
        }
        jobs = static_cast< Job* >(malloc(sizeof(Job) * capacity));
        keys = static_cast< const void** >(malloc(sizeof(const void*) * capacity));
        done = static_cast< uint8_t* >(malloc(capacity));
        table = static_cast< Slot* >(calloc(slots, sizeof(Slot)));
        if (jobs == nullptr || keys == nullptr || done == nullptr || table == nullptr) {
            free(jobs);
            free(keys);
            free(done);
            free(table);
            jobs = nullptr;
            keys = nullptr;
            done = nullptr;
            table = nullptr;
            return false;
        }
        cap = capacity;
        mask = slots - 1;
        run = fn;
        return true;
    }
    bool Ready() const { return jobs != nullptr; }
    bool IsOpen() const { return open; }

    void Begin() {
        open = true;
        count = 0;
        inlineRuns = 0;
        overflow = 0;
        if (++stamp == 0) {  // (a new stamp empties the table; on the wrap, really empty it)
            memset(table, 0, sizeof(Slot) * (mask + 1));
            stamp = 1;
        }
    }
    // A slot for the job of `key` (the caller fills it), or nullptr: the batch is not open or is full (compute at once).
    // A pending job of the same key runs first.
    Job* Add(const void* key) {
        if (!open) {
            return nullptr;
        }
        Slot* s = Find(key);
        if (s->stamp == stamp && !done[s->index]) {
            RunNow(s->index);
        }
        if (count == cap) {
            ++overflow;  // (a slot of this key stays, its job done: a probe chain through it must not break)
            return nullptr;
        }
        s->key = key;
        s->stamp = stamp;
        s->index = count;
        keys[count] = key;
        done[count] = 0;
        return &jobs[count++];
    }
    // Something on the game thread is about to read or write what `key`'s pending job reads or writes: the job runs now.
    void Touch(const void* key) {
        if (!open) {
            return;
        }
        Slot* s = Find(key);
        if (s->stamp == stamp && !done[s->index]) {
            RunNow(s->index);
        }
    }
    // The walk is over: no more Add or Touch. The pending jobs are Count() items for ItemFn(this, i).
    void Close() { open = false; }
    uint32_t Count() const { return count; }
    static void Item(void* self, uint32_t i) {
        PendingBatch* b = static_cast< PendingBatch* >(self);
        if (!b->done[i]) {
            b->run(b->jobs[i]);  // (done[] is not written by the item: the batch is read-only while it runs)
        }
    }
    // (after Close, until the next Begin) the job i, and whether it is the last of its key in this batch (no later job of the same
    // key superseded what it wrote): GXV_PAR_VERIFY checks those.
    const Job& At(uint32_t i) const { return jobs[i]; }
    bool IsLatest(uint32_t i) {
        const Slot* s = Find(keys[i]);
        return s->stamp == stamp && s->index == i;
    }
    uint32_t InlineRuns() const { return inlineRuns; }
    uint32_t Overflow() const { return overflow; }

private:
    struct Slot {
        const void* key;
        uint32_t stamp;
        uint32_t index;
    };
    Slot* Find(const void* key) {
        uintptr_t h = reinterpret_cast< uintptr_t >(key);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        uint32_t i = static_cast< uint32_t >(h) & mask;
        while (table[i].stamp == stamp && table[i].key != key) {
            i = (i + 1) & mask;  // (at most half full: count <= cap, slots >= 2 cap)
        }
        return &table[i];
    }
    void RunNow(uint32_t i) {
        run(jobs[i]);
        done[i] = 1;
        ++inlineRuns;
    }

    Job* jobs = nullptr;
    const void** keys = nullptr;
    uint8_t* done = nullptr;
    Slot* table = nullptr;
    RunFn run = nullptr;
    uint32_t cap = 0, mask = 0, count = 0, stamp = 0, inlineRuns = 0, overflow = 0;
    bool open = false;
};

}  // namespace par
}  // namespace port
