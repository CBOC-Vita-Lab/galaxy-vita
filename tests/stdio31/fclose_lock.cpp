// strk1118 host proof: fclose under the fopen lock (vita/vgx-game/fd_guard.cpp, the real wrappers). The C library is a MODEL of newlib's FILE slot
// table: __sfp claims a free slot (flags == 0) with no lock, and a release is two steps (the slot is marked free, then its fields are cleared,
// like _fclose_r's flag clear and buffer free with the lock that should cover them being empty on VitaSDK). With the lock off a thread in fopen
// claims a slot between the two steps and the release then wipes the new owner's fields; with the wrappers' lock on it cannot.
// Build and run: tools/test_fclose_lock.sh. Exit 0 = no slot was ever damaged (mode "locked") / damage seen (mode "unlocked", the control).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>
#include <sched.h>

namespace {
struct Slot { std::atomic<int> flags{0}; std::atomic<long> owner{0}; };   // flags 0 = free
Slot gSlots[8];
std::atomic<long> gDamage{0};
std::atomic<bool> gStop{false};
}
// the Vita symbols fd_guard.cpp's seek wrapper names (not used here)
extern "C" void* __vita_fd_grab(int) { return nullptr; }
extern "C" void __vita_fd_drop(void*) {}
extern "C" void port_log_summary(const char*, ...) {}
extern "C" off_t __real__lseek_r(struct _reent*, int, off_t, int) { return 0; }

extern "C" FILE* __real_fopen(const char*, const char* mode) {   // the model's __sfp: a lock-free search, then the open
    static thread_local long me = 0;
    if (!me) me = reinterpret_cast<long>(&me);
    for (auto& s : gSlots) {
        int free = 0;
        if (s.flags.compare_exchange_strong(free, 1)) {   // claimed (the library has no lock; this is as atomic as its test-and-set gets)
            s.owner.store(me);
            sched_yield();
            if (s.owner.load() != me) gDamage++;   // the release below wiped it
            s.flags.store(2);
            return reinterpret_cast<FILE*>(&s);
        }
    }
    (void)mode;
    return nullptr;
}
extern "C" FILE* __real_freopen(const char* p, const char* m, FILE*) { return __real_fopen(p, m); }
extern "C" int __real_fclose(FILE* f) {   // step 1: marked free; step 2 (after a pause): the fields are cleared
    Slot* s = reinterpret_cast<Slot*>(f);
    s->flags.store(0);
    sched_yield();
    s->owner.store(0);
    return 0;
}
extern "C" FILE* __wrap_fopen(const char*, const char*);
extern "C" int __wrap_fclose(FILE*);
extern "C" void port_fd_guard_configure(const char*);

int main(int argc, char** argv) {
    const bool locked = argc > 1 && !strcmp(argv[1], "locked");
    const bool lockOff = argc > 1 && !strcmp(argv[1], "unlocked");
    // "unlocked" = fopen wrapped (as before) and fclose NOT: the pre-change state
    if (lockOff) port_fd_guard_configure("0");
    std::vector<std::thread> th;
    for (int t = 0; t < 4; t++) th.emplace_back([&] {
        for (int i = 0; i < 20000; i++) {
            FILE* f = __wrap_fopen("x", "r");
            if (f) __wrap_fclose(f);
        }
    });
    for (auto& t : th) t.join();
    printf("%s: slot damage events %ld\n", locked ? "locked" : "unlocked", gDamage.load());
    if (locked) return gDamage.load() == 0 ? 0 : 1;
    return gDamage.load() > 0 ? 0 : 2;   // the control must show damage, or the test proves nothing
}
