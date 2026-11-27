// core3-38 (FABLE-IDEAS 13): the rule for every thread of ours that runs on the Vita's fourth core (the system's: its network stack and
// other services live there; MH3's port found that a thread which stays busy there starves them, and our freeze at a stage load / the
// start of play is likely that). The rule: nothing there spins, and nothing there runs for long without giving the core back.
//
//   GXV_CORE3_YIELD=1        (default ON: a safety fix; =0 the old behaviour, no yield at all) a core-3 worker that has run for GXV_CORE3_BUDGET_US
//                            (default 1000) in one piece sleeps GXV_CORE3_NAP_US (default 100, never under 50; sceKernelDelayThread, never 0)
//                            at its next job boundary (between items of a batch, between the finishing thread's batches of 32 operations).
//                            Inside one job nothing is cut: a job that is one function call (par-dl, the draw pass) is only metered.
//   GXV_CORE3_AUDIO_PRIO=<0x41..0xBF> (default 0 = off) the audio output thread, when it is on core 3, runs at this priority instead of 0x40,
//                            the DSP mixer 0x10 below it (never above 0xBF; never above what it has now when that is already lower).
//
//   GXV_MP_CORE3=1          (mp3c24; default 0 = off, nothing changes) while an online session is on (mp_entered(): the Multiplayer menu opened,
//                            which includes the login, until the session is over and nothing of it is on screen) the DISCRETIONARY core-3 workers
//                            (par-view, par-anim/smg_spec, par-ptcl ...: the ones that built a Worker with discretionary = true) use a smaller
//                            budget and a longer nap: GXV_MP_CORE3_BUDGET_US (default 600, 200..20000) / GXV_MP_CORE3_NAP_US (default 150, 50..1000),
//                            i.e. at most 80% of the core instead of 91%. Same mechanism as the yield cap above (a sleep between jobs of a worker that
//                            is already running): no thread changes core or priority, so switching on join and off at leave is a variable read at
//                            the next job boundary, with no hitch. vgx-finish, the audio output thread and the DSP mixer are NOT discretionary: untouched.
//                            Single player (mp_entered() false) is exactly as without the switch.
//
// And the meter: every core-3 thread adds its busy time to a one-second ring; a stage load starts a 5 s timer and then one log line says
// who was busy in the 10 s around the load ("core3: stage load ..."), so the next freeze log shows who held the core.
//
// Pure policy and bookkeeping: the kernel (sleep, clock) is the two functions at the bottom, so tests/core3_budget_test.cpp runs on a PC.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <thread>
#endif

namespace port {
namespace core3 {

constexpr uint32_t kMinNapUs = 50, kMaxNapUs = 1000, kMinBudgetUs = 200, kMaxBudgetUs = 20000;

struct Policy {
    bool yield = true;         // GXV_CORE3_YIELD (anything but "0")
    uint32_t budgetUs = 1000;  // GXV_CORE3_BUDGET_US
    uint32_t napUs = 100;      // GXV_CORE3_NAP_US
    int audioPrio = 0;         // GXV_CORE3_AUDIO_PRIO: 0 off, else 0x41..0xBF
    bool mpCore3 = false;      // GXV_MP_CORE3=1 (mp3c24)
    uint32_t mpBudgetUs = 600; // GXV_MP_CORE3_BUDGET_US (g30 Wave 8: 600/150 us = 80% duty, protects Wi-Fi without starving par workers)
    uint32_t mpNapUs = 150;    // GXV_MP_CORE3_NAP_US
};

inline long ParseNum(const char* text, long fallback) {
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 0);
    return (end && !*end) ? v : fallback;
}

template <class Get> Policy ReadPolicy(Get get) {
    Policy p;
    const char* y = get("GXV_CORE3_YIELD");
    p.yield = !(y && *y == '0');
    long b = ParseNum(get("GXV_CORE3_BUDGET_US"), 1000);
    p.budgetUs = static_cast<uint32_t>(b < kMinBudgetUs ? kMinBudgetUs : b > kMaxBudgetUs ? kMaxBudgetUs : b);
    long n = ParseNum(get("GXV_CORE3_NAP_US"), 100);
    p.napUs = static_cast<uint32_t>(n < kMinNapUs ? kMinNapUs : n > kMaxNapUs ? kMaxNapUs : n);  // (never 0: sceKernelDelayThread(0) is a bare yield the scheduler can hand straight back)
    const long a = ParseNum(get("GXV_CORE3_AUDIO_PRIO"), 0);
    p.audioPrio = (a >= 0x41 && a <= 0xBF) ? static_cast<int>(a) : 0;
    const char* m = get("GXV_MP_CORE3");
    p.mpCore3 = m && *m == '1' && !m[1];
    const long mb = ParseNum(get("GXV_MP_CORE3_BUDGET_US"), 600);
    p.mpBudgetUs = static_cast<uint32_t>(mb < kMinBudgetUs ? kMinBudgetUs : mb > kMaxBudgetUs ? kMaxBudgetUs : mb);
    const long mn = ParseNum(get("GXV_MP_CORE3_NAP_US"), 150);
    p.mpNapUs = static_cast<uint32_t>(mn < kMinNapUs ? kMinNapUs : mn > kMaxNapUs ? kMaxNapUs : mn);
    return p;
}

// The budget / nap a worker uses now: the online pair for a discretionary worker while the switch is on and a session is on.
inline void EffectiveCap(const Policy& p, bool discretionary, bool online, uint32_t* budgetUs, uint32_t* napUs) {
    const bool mp = p.mpCore3 && discretionary && online;
    *budgetUs = mp ? p.mpBudgetUs : p.budgetUs;
    *napUs = mp ? p.mpNapUs : p.napUs;
}

// The priority for the audio output thread (mixer = false) or the DSP mixer (true) on core 3: 0 = leave it. Never raised, only lowered.
constexpr int AudioPriority(const Policy& p, bool mixer, bool onCore3, int current) {
    if (!onCore3 || p.audioPrio == 0) return 0;
    const int want = mixer ? (p.audioPrio + 0x10 > 0xBF ? 0xBF : p.audioPrio + 0x10) : p.audioPrio;
    return want > current ? want : 0;
}

// One thread's window of continuous work. Times are microseconds from any steady clock.
struct Budget {
    uint64_t winStart = 0;  // when the current stretch of continuous work began (wake-up, or the end of the last nap)
    uint64_t mark = 0;      // the last time busy time was counted
    void Begin(uint64_t now) { winStart = mark = now; }
    // At a job boundary. Returns the busy microseconds since the last call; *napUs = the sleep to take now (0: none); *runUs = the continuous stretch so far.
    uint32_t Step(uint64_t now, const Policy& p, uint32_t* napUs, uint32_t* runUs) {
        const uint32_t busy = static_cast<uint32_t>(now > mark ? now - mark : 0);
        mark = now;
        const uint64_t run = now > winStart ? now - winStart : 0;
        *runUs = static_cast<uint32_t>(run);
        *napUs = (p.yield && run >= p.budgetUs) ? p.napUs : 0;
        return busy;
    }
    void Resumed(uint64_t now) { winStart = mark = now; }  // (after the nap: a new stretch, the nap is not busy time)
};

// ---- the meter: 16 slots (one per named thread), a ring of 1 s buckets each
constexpr int kSlots = 16, kBuckets = 16;
constexpr uint32_t kSlotNameLen = 20;

struct Slot {
    char name[kSlotNameLen] = {};
    std::atomic<bool> onCore3{false};  // the thread is on the fourth core now
    std::atomic<bool> everOn{false};
    std::atomic<bool> discretionary{false};  // mp3c24: a worker the online policy may slow
    std::atomic<uint32_t> stamp[kBuckets] = {};  // the second (since the clock's start, +1) each bucket holds
    std::atomic<uint32_t> busyUs[kBuckets] = {};
    std::atomic<uint32_t> yields{0};     // since the stage load
    std::atomic<uint32_t> longestUs{0};  // the longest continuous stretch since the stage load
};

struct Meter {
    Slot slots[kSlots];
    std::mutex regMutex;
    int count = 0;
    std::atomic<uint64_t> loadAtUs{0};  // 0: no stage load is waiting for its line
    std::atomic<uint64_t> loadSeq{0};
    std::atomic<bool> online{false};  // mp3c24: an online session is on (set by the game thread each turn: SetOnline)
    uint64_t mpLogAtUs = 0;           // (game thread only) when the last core3-mp line was due; 0: not online

    int Register(const char* name) {
        std::lock_guard<std::mutex> l(regMutex);
        for (int i = 0; i < count; ++i)
            if (!std::strncmp(slots[i].name, name, kSlotNameLen - 1)) return i;
        if (count >= kSlots) return kSlots - 1;  // (shared overflow slot: only the name is wrong)
        std::strncpy(slots[count].name, name, kSlotNameLen - 1);
        return count++;
    }
    void SetOn(int s, bool on) {
        if (s < 0 || s >= kSlots) return;
        slots[s].onCore3.store(on, std::memory_order_relaxed);
        if (on) slots[s].everOn.store(true, std::memory_order_relaxed);
    }
    void SetDiscretionary(int s) { if (s >= 0 && s < kSlots) slots[s].discretionary.store(true, std::memory_order_relaxed); }
    bool On(int s) const { return s >= 0 && s < kSlots && slots[s].onCore3.load(std::memory_order_relaxed); }
    void AddAtSec(int s, uint32_t us, uint32_t sec) {
        if (s < 0 || s >= kSlots || !us) return;
        Slot& sl = slots[s];
        const int b = static_cast<int>(sec % kBuckets);
        if (sl.stamp[b].load(std::memory_order_relaxed) != sec) {
            sl.stamp[b].store(sec, std::memory_order_relaxed);
            sl.busyUs[b].store(0, std::memory_order_relaxed);
        }
        sl.busyUs[b].fetch_add(us, std::memory_order_relaxed);
    }
    void Add(int s, uint32_t us, uint64_t nowUs) {
        if (s < 0 || s >= kSlots || !us) return;
        AddAtSec(s, us, static_cast<uint32_t>(nowUs / 1000000u) + 1);
    }
    void NoteRun(int s, uint32_t runUs, bool yielded) {
        if (s < 0 || s >= kSlots) return;
        Slot& sl = slots[s];
        if (yielded) sl.yields.fetch_add(1, std::memory_order_relaxed);
        uint32_t mx = sl.longestUs.load(std::memory_order_relaxed);
        while (runUs > mx && !sl.longestUs.compare_exchange_weak(mx, runUs, std::memory_order_relaxed)) {}
    }
    // Busy microseconds of a slot in the seconds [fromSec, toSec] (clock seconds, as nowUs / 1e6).
    uint64_t Sum(int s, int64_t fromSec, int64_t toSec) const {
        uint64_t total = 0;
        for (int64_t sec = fromSec; sec <= toSec; ++sec) {
            if (sec < 0) continue;
            const uint32_t want = static_cast<uint32_t>(sec) + 1;
            const int b = static_cast<int>(want % kBuckets);
            if (slots[s].stamp[b].load(std::memory_order_relaxed) == want) total += slots[s].busyUs[b].load(std::memory_order_relaxed);
        }
        return total;
    }
    // mp3c24: the game thread, once a turn. online = a session is on. Returns true when a line is due (the switch's moment, then every 10 s while
    // online); at the switch's moment the line says so. Duty = busy time of the threads on core 3 in the last 10 s over 10 s.
    bool PollMp(bool isOnline, uint64_t nowUs, const Policy& pol, char* out, size_t cap) {
        online.store(isOnline, std::memory_order_relaxed);
        if (!isOnline) { mpLogAtUs = 0; return false; }
        const bool first = mpLogAtUs == 0;
        if (first) mpLogAtUs = nowUs ? nowUs : 1;
        else if (nowUs < mpLogAtUs + 10000000u) return false;
        else mpLogAtUs = nowUs;
        const int64_t sec = static_cast<int64_t>(nowUs / 1000000u);
        uint64_t all = 0, disc = 0, other = 0;
        for (int i = 0; i < count; ++i) {
            if (!slots[i].onCore3.load(std::memory_order_relaxed)) continue;
            const uint64_t b = Sum(i, sec - 10, sec - 1);
            all += b;
            if (slots[i].discretionary.load(std::memory_order_relaxed)) disc += b; else other += b;
        }
        const bool mode = pol.mpCore3;
        const uint32_t bu = mode ? pol.mpBudgetUs : pol.budgetUs, nu = mode ? pol.mpNapUs : pol.napUs;
        snprintf(out, cap, "core3-mp: %s online, mode %s, discretionary workers run <= %u us then nap %u us (max duty %u%%); core 3 busy in the last 10 s: %.0f%% (discretionary %.0f%%, vgx-finish/audio/other %.0f%%)",
                 first ? "session on:" : "still", mode ? "ON (GXV_MP_CORE3=1)" : "off (full speed)", bu, nu, bu * 100u / (bu + nu),
                 all / 100000.0, disc / 100000.0, other / 100000.0);
        return true;
    }
    // A stage load: a line is due 5 s from now, and the per-load counters start again.
    void StageLoad(uint64_t nowUs) {
        for (int i = 0; i < kSlots; ++i) {
            slots[i].yields.store(0, std::memory_order_relaxed);
            slots[i].longestUs.store(0, std::memory_order_relaxed);
        }
        loadSeq.fetch_add(1, std::memory_order_relaxed);
        loadAtUs.store(nowUs ? nowUs : 1, std::memory_order_release);
    }
    // Called often by any thread (cheap: one atomic load). When the line is due, formats it into `out` and returns true (once per load).
    bool Poll(uint64_t nowUs, const Policy& pol, char* out, size_t cap) {
        const uint64_t at = loadAtUs.load(std::memory_order_acquire);
        if (!at || nowUs < at + 5000000u) return false;
        uint64_t expect = at;
        if (!loadAtUs.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) return false;
        const int64_t loadSec = static_cast<int64_t>(at / 1000000u);
        size_t n = static_cast<size_t>(snprintf(out, cap, "core3: stage load %llu, busy ms in the 10 s around it:", static_cast<unsigned long long>(loadSeq.load(std::memory_order_relaxed))));
        bool any = false;
        for (int i = 0; i < count && n < cap; ++i) {
            const Slot& sl = slots[i];
            if (!sl.everOn.load(std::memory_order_relaxed)) continue;
            const uint64_t before = Sum(i, loadSec - 5, loadSec - 1), after = Sum(i, loadSec, loadSec + 5);
            n += static_cast<size_t>(snprintf(out + n, cap - n, " %s%s %.1f (before %.1f, after %.1f; yields %u, longest run %.2f ms)", any ? "|" : "", sl.name,
                                                   (before + after) / 1000.0, before / 1000.0, after / 1000.0, sl.yields.load(std::memory_order_relaxed),
                                                   sl.longestUs.load(std::memory_order_relaxed) / 1000.0));
            any = true;
            if (n >= cap) break;
        }
        if (n < cap) snprintf(out + n, cap - n, "%s; yield cap %s (budget %u us, nap %u us)", any ? "" : " no thread of ours is on core 3", pol.yield ? "on" : "off (GXV_CORE3_YIELD=0)", pol.budgetUs, pol.napUs);
        return true;
    }
};

// ---- the process-wide meter and policy, and the kernel
inline Meter& TheMeter() { static Meter* m = new Meter; return *m; }  // (never destroyed: threads outlive main)
inline const Policy& ThePolicy() {
    static const Policy p = ReadPolicy([](const char* k) { return std::getenv(k); });
    return p;
}
inline uint64_t NowUs() {
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)
    return sceKernelGetProcessTimeWide();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline void SleepUs(uint32_t us) {
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)
    sceKernelDelayThread(us);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
}

inline int RegisterThread(const char* name) { return TheMeter().Register(name ? name : "unnamed"); }
inline void MarkOnCore3(int slot, bool on) { TheMeter().SetOn(slot, on); }

// One per core-3 worker thread (on its stack). Wake() when the thread leaves its blocking wait, Checkpoint() at every job boundary,
// Sleep() just before it blocks again. Does nothing (no clock read) while the thread is not on core 3.
class Worker {
public:
    // discretionary (mp3c24): the online policy (GXV_MP_CORE3=1) may give this worker a smaller budget and a longer nap while a session is on.
    // fineGrained: only per-item loops (par_spec / par_exec) may stride clock checks to 4 on tiny (<40 us) SP items; coarse callers (vgx-finish, thp-dec3) always poll every call.
    explicit Worker(int slot, bool discretionary = false, bool fineGrained = false) : slot_(slot), disc_(discretionary), fine_(fineGrained) { if (discretionary) TheMeter().SetDiscretionary(slot); }
    void Wake() {
        on_ = TheMeter().On(slot_);
        ticks_ = 0;
        stride_ = 1;
        if (on_) {
            const uint64_t t = NowUs();
            b_.Begin(t);
            SyncSec(t);
        }
    }
    void Checkpoint() {
        if (!on_) return;
        if (++ticks_ < stride_) return;
        ticks_ = 0;
        uint32_t nap = 0, run = 0;
        const uint64_t t = NowUs();
        Policy eff = ThePolicy();
        const bool online = TheMeter().online.load(std::memory_order_relaxed);
        EffectiveCap(eff, disc_, online, &eff.budgetUs, &eff.napUs);
        const uint32_t busy = b_.Step(t, eff, &nap, &run);
        SyncSec(t);
        TheMeter().AddAtSec(slot_, busy, curSec_);
        TheMeter().NoteRun(slot_, run, nap != 0);
        // Adaptive syscall throttle: fine-grained SP loops only, never under tight online MP Core 3 cap.
        const bool mpTight = online && eff.mpCore3;
        stride_ = (fine_ && !mpTight && nap == 0 && busy < 40u && run < eff.budgetUs / 2u) ? 4u : 1u;
        if (nap) {
            SleepUs(nap);
            const uint64_t t2 = NowUs();
            b_.Resumed(t2);
            SyncSec(t2);
        }
    }
    void Sleep() {
        if (!on_) return;
        uint32_t nap = 0, run = 0;
        const uint64_t t = NowUs();
        const uint32_t busy = b_.Step(t, ThePolicy(), &nap, &run);
        SyncSec(t);
        TheMeter().AddAtSec(slot_, busy, curSec_);
        TheMeter().NoteRun(slot_, run, false);
        on_ = false;
    }

private:
    void SyncSec(uint64_t t) {
        if (t < nextSecUs_ && curSec_ != 0) return;
        if (curSec_ != 0 && t < nextSecUs_ + 1000000u) {
            ++curSec_;
            nextSecUs_ += 1000000u;
            return;
        }
        const uint64_t s = t / 1000000u;
        curSec_ = static_cast<uint32_t>(s) + 1u;
        nextSecUs_ = (s + 1u) * 1000000u;
    }
    int slot_;
    bool disc_ = false, fine_ = false;
    uint32_t ticks_ = 0, stride_ = 1, curSec_ = 0;
    uint64_t nextSecUs_ = 0;
    bool on_ = false;
    Budget b_;
};

// For the stage load hook (perf.cpp): the game thread calls StageLoad() at a stage's load and Poll() every turn.
inline void StageLoad() { TheMeter().StageLoad(NowUs()); }
inline bool Poll(char* out, size_t cap) { return TheMeter().Poll(NowUs(), ThePolicy(), out, cap); }
inline bool PollMp(bool online, char* out, size_t cap) { return TheMeter().PollMp(online, NowUs(), ThePolicy(), out, cap); }  // (mp3c24: the game thread, once a turn)

}  // namespace core3
}  // namespace port
