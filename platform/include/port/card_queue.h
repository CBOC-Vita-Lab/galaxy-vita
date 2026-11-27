// hitch30 (13 Nov 2026): the card-write queue (header-only; the thread and the C API are platform/src/os/os_alarm.cpp; host proof tests/card_queue.cpp).
//
// By the owner's measurements a file create or fsync on the console's card costs 190-350 ms, a rename or remove a directory walk, an append with
// flush 12-15 ms. A file written by the game thread, the front-end thread or the render thread while the player has control is a hitch of that
// size. The caller hands the bytes to Enqueue (a copy into a fixed pool: no heap, no lock, no card call, never a wait; a full pool or an
// oversize payload is DROPPED and counted) and a low-priority thread does the file work, in the order the writes were made:
//   kReplace  the whole file: written to <path>.tmp, then <path> removed and the tmp renamed onto it (a rename onto an existing file fails on
//             some of the console's file systems; the callers' old code did the same), all on the writer; of two queued replaces of the same
//             path the older is skipped (coalesced);
//   kAppend   the bytes appended to the file (fopen "ab", fwrite, fclose on the writer).
// Slots: kSmall x 4 KiB, kMid x 20 KiB, kBig x 64 KiB (a payload goes to the smallest free slot that holds it; 232 KiB of static memory).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace port {
namespace card {

enum Mode : int { kReplace = 0, kAppend = 1 };
constexpr uint32_t kPathMax = 160;
constexpr uint32_t kSmallN = 6, kMidN = 4, kBigN = 2, kSlots = kSmallN + kMidN + kBigN;
constexpr uint32_t kSmallBytes = 4u << 10, kMidBytes = 20u << 10, kBigBytes = 64u << 10;

struct Stats {
    std::atomic< uint32_t > queued{0};      // writes accepted
    std::atomic< uint32_t > bytes{0};       // bytes accepted
    std::atomic< uint32_t > done{0};        // writes finished (written)
    std::atomic< uint32_t > failed{0};      // writes the file system refused
    std::atomic< uint32_t > dropped{0};     // refused at the door: no free slot, path too long
    std::atomic< uint32_t > oversize{0};    // refused: more than kBigBytes (the caller writes it itself)
    std::atomic< uint32_t > coalesced{0};   // older replaces of a path skipped
    std::atomic< uint32_t > callerThread{0};  // file work the calling thread had to do itself (no writer thread could be made)
    std::atomic< uint32_t > maxMs{0};       // longest single write
    std::atomic< uint32_t > maxMsWindow{0};
};

class Queue {
public:
    struct Job {
        int slot = -1;
        int mode = 0;
        uint32_t len = 0;
        const char* path = nullptr;
        const uint8_t* data = nullptr;
    };

    // Any thread. true: queued. false: dropped (counted), the caller goes on.
    bool Enqueue(const char* path, int mode, const void* data, uint32_t n) {
        if (path == nullptr || data == nullptr || n == 0 || (mode != kReplace && mode != kAppend)) {
            return false;
        }
        const size_t pl = std::strlen(path);
        if (pl == 0 || pl >= kPathMax - 8) {  // (room for ".tmp")
            st.dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (n > kBigBytes) {
            st.oversize.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (uint32_t i = FirstFor(n); i < kSlots; ++i) {
            uint32_t expect = kFree;
            if (!slots_[i].state.compare_exchange_strong(expect, kFilling, std::memory_order_acquire)) {
                continue;
            }
            Slot& s = slots_[i];
            std::memcpy(s.path, path, pl + 1);
            std::memcpy(Buf(i), data, n);
            s.len = n;
            s.mode = mode;
            s.seq = seq_.fetch_add(1, std::memory_order_relaxed);
            pending_.fetch_add(1, std::memory_order_relaxed);
            st.queued.fetch_add(1, std::memory_order_relaxed);
            st.bytes.fetch_add(n, std::memory_order_relaxed);
            s.state.store(kReady, std::memory_order_release);
            return true;
        }
        st.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // The writer (one at a time: the caller holds the writer lock): the oldest ready write, or false. A replace that a newer replace of the
    // same path follows is skipped (its slot is freed, counted) and the search goes on.
    bool Take(Job* out) {
        for (;;) {
            // (the oldest ready write. A scan is not one instant: a producer's older write can become ready in a slot scanned before
            // its newer write is seen in a slot scanned after. Once a write was seen ready, every older write of the same producer is
            // visible to a new scan: scan again until two scans in a row give the same slot.)
            int best = -1;
            for (int again = 0; again < 8; ++again) {
                int pick = -1;
                for (uint32_t i = 0; i < kSlots; ++i) {
                    if (slots_[i].state.load(std::memory_order_acquire) != kReady) {
                        continue;
                    }
                    if (pick < 0 || static_cast< int32_t >(slots_[i].seq - slots_[pick].seq) < 0) {
                        pick = static_cast< int >(i);
                    }
                }
                if (pick == best) {
                    break;
                }
                best = pick;
                if (best < 0) {
                    return false;
                }
            }
            if (best < 0) {
                return false;
            }
            Slot& b = slots_[best];
            if (b.mode == kReplace) {
                bool newer = false;
                for (uint32_t i = 0; i < kSlots && !newer; ++i) {
                    if (static_cast< int >(i) != best && slots_[i].state.load(std::memory_order_acquire) == kReady && slots_[i].mode == kReplace &&
                        static_cast< int32_t >(slots_[i].seq - b.seq) > 0 && std::strcmp(slots_[i].path, b.path) == 0) {
                        newer = true;
                    }
                }
                if (newer) {
                    st.coalesced.fetch_add(1, std::memory_order_relaxed);
                    Release(best);
                    continue;
                }
            }
            b.state.store(kWriting, std::memory_order_relaxed);
            out->slot = best;
            out->mode = b.mode;
            out->len = b.len;
            out->path = b.path;
            out->data = Buf(static_cast< uint32_t >(best));
            return true;
        }
    }
    void Release(int slot) {
        slots_[slot].state.store(kFree, std::memory_order_release);
        pending_.fetch_sub(1, std::memory_order_relaxed);
    }
    uint32_t Pending() const { return pending_.load(std::memory_order_relaxed); }

    // The file work of one job (the writer's thread). Returns true when the file system took it. Standard C file calls only.
    static bool Write(const Job& j) {
        char tmp[kPathMax];
        if (j.mode == kAppend) {
            FILE* f = std::fopen(j.path, "ab");
            if (!f) {
                return false;
            }
            const bool ok = std::fwrite(j.data, 1, j.len, f) == j.len;
            return std::fclose(f) == 0 && ok;
        }
        snprintf(tmp, sizeof tmp, "%s.tmp", j.path);
        FILE* f = std::fopen(tmp, "wb");
        if (!f) {
            return false;
        }
        const bool ok = std::fwrite(j.data, 1, j.len, f) == j.len;
        const bool closed = std::fclose(f) == 0;
        if (!ok || !closed) {
            std::remove(tmp);
            return false;
        }
        std::remove(j.path);  // (a rename onto an existing file fails on some of the console's file systems)
        if (std::rename(tmp, j.path) != 0) {
            std::remove(tmp);
            return false;
        }
        return true;
    }

    // The writer: runs every ready write (the caller holds the writer lock). Returns how many it did.
    uint32_t RunAll() {
        uint32_t n = 0;
        Job j;
        while (Take(&j)) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = Write(j);
            const uint32_t ms = static_cast< uint32_t >(std::chrono::duration_cast< std::chrono::milliseconds >(std::chrono::steady_clock::now() - t0).count());
            (ok ? st.done : st.failed).fetch_add(1, std::memory_order_relaxed);
            NoteMs(ms);
            Release(j.slot);
            ++n;
        }
        return n;
    }

    // "card: N async writes, M bytes, longest write T ms, dropped K, on the caller's thread 0 (...)". Resets the window's longest write.
    int Line(char* buf, unsigned cap) {
        const unsigned w = st.done.load(std::memory_order_relaxed) + st.failed.load(std::memory_order_relaxed);
        return snprintf(buf, cap,
                             "card: %u async writes, %u bytes, longest write %u ms, dropped %u, on the caller's thread %u (window longest %u ms; failed %u, "
                             "coalesced %u, oversize %u, waiting %u)",
                             w, static_cast< unsigned >(st.bytes.load(std::memory_order_relaxed)), static_cast< unsigned >(st.maxMs.load(std::memory_order_relaxed)),
                             static_cast< unsigned >(st.dropped.load(std::memory_order_relaxed)), static_cast< unsigned >(st.callerThread.load(std::memory_order_relaxed)),
                             static_cast< unsigned >(st.maxMsWindow.exchange(0, std::memory_order_relaxed)), static_cast< unsigned >(st.failed.load(std::memory_order_relaxed)),
                             static_cast< unsigned >(st.coalesced.load(std::memory_order_relaxed)), static_cast< unsigned >(st.oversize.load(std::memory_order_relaxed)), Pending());
    }
    // A number that changes whenever the census would read differently (for "print on change").
    uint32_t Mark() const {
        return st.queued.load(std::memory_order_relaxed) * 31u + st.done.load(std::memory_order_relaxed) * 17u + st.failed.load(std::memory_order_relaxed) * 13u +
               st.dropped.load(std::memory_order_relaxed) * 7u + st.oversize.load(std::memory_order_relaxed) * 5u + st.coalesced.load(std::memory_order_relaxed) * 3u +
               st.callerThread.load(std::memory_order_relaxed);
    }

    Stats st;

private:
    enum : uint32_t { kFree = 0, kFilling = 1, kReady = 2, kWriting = 3 };
    struct Slot {
        std::atomic< uint32_t > state{kFree};
        uint32_t seq = 0, len = 0;
        int mode = 0;
        char path[kPathMax] = {};
    };
    static uint32_t FirstFor(uint32_t n) { return n <= kSmallBytes ? 0 : (n <= kMidBytes ? kSmallN : kSmallN + kMidN); }
    uint8_t* Buf(uint32_t i) {
        if (i < kSmallN) {
            return small_[i];
        }
        if (i < kSmallN + kMidN) {
            return mid_[i - kSmallN];
        }
        return big_[i - kSmallN - kMidN];
    }
    void NoteMs(uint32_t ms) {
        uint32_t m = st.maxMs.load(std::memory_order_relaxed);
        while (ms > m && !st.maxMs.compare_exchange_weak(m, ms, std::memory_order_relaxed)) {
        }
        uint32_t w = st.maxMsWindow.load(std::memory_order_relaxed);
        while (ms > w && !st.maxMsWindow.compare_exchange_weak(w, ms, std::memory_order_relaxed)) {
        }
    }
    Slot slots_[kSlots];
    std::atomic< uint32_t > seq_{0};
    std::atomic< uint32_t > pending_{0};
    uint8_t small_[kSmallN][kSmallBytes];
    uint8_t mid_[kMidN][kMidBytes];
    uint8_t big_[kBigN][kBigBytes];
};

}  // namespace card
}  // namespace port
