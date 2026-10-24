// Virtual clock (see port/vclock.h).  armhf/qemu harness only.
#include "port/vclock.h"

#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "port/port.h"

namespace {

struct Sleeper {
    std::condition_variable cv;
    int64_t wake = INT64_MAX;  // virtual ns (sNow domain)
    bool queued = false, fired = false;
    bool owesSettle = false;  // woken by the clock; counts as outstanding until it sleeps again
    char name[16] = {};
    Sleeper();
    ~Sleeper();
};

constexpr int64_t kBase = 1000000000ll;  // port_vclock_now() starts at 1 s so that "time 0" never occurs
std::atomic<int> sOn{-1};
std::atomic<int64_t> sNow{0};
std::mutex sM;
std::condition_variable sSettleCv;
std::vector<Sleeper*> sQueue;
int sOutstanding = 0;  // woken sleepers still running
int sExternal = 0;     // outstanding asynchronous host work
uint64_t sPokeSeq = 0;
uint64_t sSteps = 0;
std::vector<Sleeper*> sAll;
Sleeper::Sleeper() {
    std::lock_guard<std::mutex> lk(sM);
    sAll.push_back(this);
}
Sleeper::~Sleeper() {
    std::lock_guard<std::mutex> lk(sM);
    for (size_t i = 0; i < sAll.size(); ++i)
        if (sAll[i] == this) {
            sAll[i] = sAll.back();
            sAll.pop_back();
            break;
        }
}
thread_local Sleeper tlsSleeper;

void settle(Sleeper& s) {  // sM held
    if (s.owesSettle) {
        s.owesSettle = false;
        --sOutstanding;
        sSettleCv.notify_all();
    }
}

void fire(Sleeper* s) {  // sM held; s is queued
    s->fired = true;
    s->queued = false;
    s->owesSettle = true;
    ++sOutstanding;
    for (size_t i = 0; i < sQueue.size(); ++i) {
        if (sQueue[i] == s) {
            sQueue[i] = sQueue.back();
            sQueue.pop_back();
            break;
        }
    }
    s->cv.notify_one();
}

}  // namespace

extern "C" {

int port_vclock_enabled(void) {
    int v = sOn.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = getenv("PETARI_VCLOCK");
        v = (e && atoi(e)) ? 1 : 0;
        sOn.store(v);
    }
    return v;
}

int64_t port_vclock_now(void) { return kBase + sNow.load(std::memory_order_relaxed); }

void port_vclock_sleep_until(int64_t absNs, uint64_t pokeSeq) {
    Sleeper& s = tlsSleeper;
    std::unique_lock<std::mutex> lk(sM);
    settle(s);
    if (pokeSeq != ~0ull && pokeSeq != sPokeSeq) return;
    const int64_t wake = absNs == INT64_MAX ? INT64_MAX : absNs - kBase;
    if (wake <= sNow.load(std::memory_order_relaxed)) return;
    s.wake = wake;
    s.fired = false;
    s.queued = true;
    sQueue.push_back(&s);
    s.cv.wait(lk, [&] { return s.fired; });
}

void port_vclock_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    port_vclock_sleep_until(port_vclock_now() + ns, ~0ull);
}

uint64_t port_vclock_poke_seq(void) {
    std::lock_guard<std::mutex> lk(sM);
    return sPokeSeq;
}

void* port_vclock_self(void) { return &tlsSleeper; }

void port_vclock_release(void) {  // the caller is about to block on something other than the clock (join)
    std::lock_guard<std::mutex> lk(sM);
    settle(tlsSleeper);
}

void port_vclock_name_thread(const char* name) {
    Sleeper& s = tlsSleeper;
    std::lock_guard<std::mutex> lk(sM);
    snprintf(s.name, sizeof s.name, "%s", name);
}

void port_vclock_poke(void* handle) {
    Sleeper* s = static_cast<Sleeper*>(handle);
    std::lock_guard<std::mutex> lk(sM);
    ++sPokeSeq;
    if (s && s->queued && !s->fired) fire(s);
}

void port_vclock_external_begin(void) {
    std::lock_guard<std::mutex> lk(sM);
    ++sExternal;
}

void port_vclock_external_end(void) {
    std::lock_guard<std::mutex> lk(sM);
    --sExternal;
    sSettleCv.notify_all();
}

void port_vclock_idle_step(int (*irqPending)(void)) {
    std::unique_lock<std::mutex> lk(sM);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (sOutstanding > 0 || sExternal > 0) {
        if (irqPending()) return;
        if (sSettleCv.wait_until(lk, limit) == std::cv_status::timeout) {
            port_log("vclock: sleepers did not settle (outstanding %d external %d); continuing", sOutstanding, sExternal);
            for (const Sleeper* t : sAll)
                port_log("vclock:   thread '%s' owes=%d queued=%d wake=%lld", t->name, (int)t->owesSettle, (int)t->queued, (long long)t->wake);
            sOutstanding = 0;
            sExternal = 0;
            break;
        }
    }
    if (irqPending()) return;
    int64_t next = INT64_MAX;
    for (const Sleeper* s : sQueue)
        if (s->wake < next) next = s->wake;
    if (next == INT64_MAX) {
        lk.unlock();
        port_host_sleep_ns(0);  // nothing scheduled: the caller waits for an interrupt in real time
        return;
    }
    if (next > sNow.load(std::memory_order_relaxed)) sNow.store(next, std::memory_order_relaxed);
    ++sSteps;
    const int64_t now = sNow.load(std::memory_order_relaxed);
    for (size_t i = 0; i < sQueue.size();) {
        Sleeper* s = sQueue[i];
        if (s->wake <= now) fire(s);  // swaps the last element into slot i
        else ++i;
    }
}

}  // extern "C"
