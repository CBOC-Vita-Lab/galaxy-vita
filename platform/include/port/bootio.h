#pragma once
// bootio: opens of the files a start-up needs, retried for a moment when they fail, and logged with the real error.
//
// Why: about one launch in 20-200 died ~1.6 s after the start with "io: a seek on file handle -1 ... dvd: game data not found ... abort()" although
// the game data was there (the next launch worked). Whatever the cause of one failed open on the memory card (a transient SCE error just after another
// process was killed, a full descriptor table, a stdio slot race), a start-up must not die of it: the open is repeated every 50 ms for up to 3 s, the
// first failure is logged with the SCE code, errno and the number of open descriptors, and the success with the number of tries.
// Only the error path runs any of this; a file that opens at once costs one call. Header only (host-testable: the clock, the sleep, the log and the
// descriptor count are replaceable).
//
// GXV_BOOTIO_INJECT=<n> (default 0, TEST ONLY): the first n opens of the file table (tag "fst.bin") fail artificially (EIO), to show the retry on a console.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((format(printf, 1, 2), weak));  // (port/diagnostics.h; weak: host tests need not define it)

#if defined(__vita__)
#include <psp2/io/fcntl.h>
extern "C" void* __vita_fdmap[256];  // (newlib's descriptor table: 256 slots, 0..2 reserved; a NULL slot is free)
#endif

namespace bootio {

struct Options {
    unsigned budgetMs = 3000;   // how long a failing open is repeated
    unsigned stepMs = 50;       // the wait between two tries
    unsigned missingMs = 3000;  // the same for "no such file" (an optional file that is normally absent sets it small or 0)
};

struct Attempt {
    bool ok = false;
    int err = 0;          // errno of the failed call
    unsigned sce = 0;     // SCE error code when known (0: unknown)
};

struct Env {
    int64_t (*nowMs)() = nullptr;
    void (*sleepMs)(unsigned) = nullptr;
    void (*log)(const char* line) = nullptr;
    int (*fdCount)() = nullptr;
};

struct Outcome {
    bool ok = false;
    unsigned tries = 0;
    int err = 0;
    unsigned sce = 0;
    int64_t elapsedMs = 0;
};

inline int64_t DefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void DefaultSleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void DefaultLog(const char* line) {
    if (port_log_summary) port_log_summary("%s", line);
    else fprintf(stderr, "%s\n", line);
}

// Descriptors in use in the C library's table (Vita: slots 3..255 that are not free), -1 where that is not known.
inline int DescriptorCount() {
#if defined(__vita__)
    int n = 0;
    for (int i = 3; i < 256; ++i) n += __vita_fdmap[i] != nullptr;
    return n;
#else
    return -1;
#endif
}

// The SCE code the system gives an open of `path` right now (0 = it opens; the handle is closed again). Vita only; 0 elsewhere.
inline unsigned ProbeSce(const char* path) {
#if defined(__vita__)
    const int r = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (r >= 0) { sceIoClose(r); return 0; }
    return static_cast<unsigned>(r);
#else
    (void)path;
    return 0;
#endif
}

inline Env Complete(Env e) {
    if (!e.nowMs) e.nowMs = &DefaultNowMs;
    if (!e.sleepMs) e.sleepMs = &DefaultSleepMs;
    if (!e.log) e.log = &DefaultLog;
    if (!e.fdCount) e.fdCount = &DescriptorCount;
    return e;
}

// Repeats attempt() (returns Attempt) until it succeeds or the budget for its error is spent. "No such file" has its own budget (missingMs);
// with 0 there is no retry and no log (an optional file that is not there is normal).
template <class F>
Outcome Retry(const char* tag, const char* path, const Options& opt, F&& attempt, Env env = Env()) {
    env = Complete(env);
    Outcome out;
    const int64_t t0 = env.nowMs();
    char line[400];
    bool logged = false;
    for (;;) {
        const Attempt a = attempt();
        ++out.tries;
        out.elapsedMs = env.nowMs() - t0;
        if (a.ok) {
            out.ok = true;
            if (logged) {
                snprintf(line, sizeof line, "io: %s: %s opened after %u tries (%lld ms)", tag, path, out.tries, static_cast<long long>(out.elapsedMs));
                env.log(line);
            }
            return out;
        }
        out.err = a.err;
        out.sce = a.sce;
        const unsigned budget = a.err == ENOENT ? opt.missingMs : opt.budgetMs;
        if (budget == 0 && a.err == ENOENT) return out;  // (absent, and that is normal: silent)
        if (!logged) {
            logged = true;
            snprintf(line, sizeof line, "io: %s: open of %s failed: SCE 0x%08X, errno %d (%s), %d descriptors open; trying again every %u ms for up to %u ms", tag,
                     path, a.sce, a.err, strerror(a.err), env.fdCount(), opt.stepMs, budget);
            env.log(line);
        }
        if (out.elapsedMs + static_cast<int64_t>(opt.stepMs) > static_cast<int64_t>(budget)) {
            snprintf(line, sizeof line, "io: %s: gave up on %s after %u tries, %lld ms: SCE 0x%08X, errno %d (%s), %d descriptors open", tag, path, out.tries,
                     static_cast<long long>(out.elapsedMs), a.sce, a.err, strerror(a.err), env.fdCount());
            env.log(line);
            return out;
        }
        env.sleepMs(opt.stepMs);
    }
}

// GXV_BOOTIO_INJECT: true while the first n opens of `tag` ("fst.bin" only) are to fail artificially.
inline bool Inject(const char* tag) {
    static std::atomic<int> left{-1};
    if (strcmp(tag, "fst.bin") != 0) return false;
    int v = left.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = getenv("GXV_BOOTIO_INJECT");
        int n = e && *e ? atoi(e) : 0;
        if (n < 0) n = 0;
        int expected = -1;
        left.compare_exchange_strong(expected, n);
        v = left.load(std::memory_order_relaxed);
    }
    while (v > 0) {
        if (left.compare_exchange_weak(v, v - 1)) return true;
    }
    return false;
}

// stdio: fopen with the retry. NULL when it gave up (errno is the last failure's).
inline FILE* FopenRetry(const char* path, const char* mode, const char* tag, const Options& opt = Options(), Env env = Env()) {
    FILE* f = nullptr;
    bool probed = false;
    const Outcome o = Retry(tag, path, opt, [&]() {
        Attempt a;
        if (Inject(tag)) { a.err = EIO; a.sce = 0x80010005u; return a; }
        errno = 0;
        f = fopen(path, mode);
        if (f) { a.ok = true; return a; }
        a.err = errno ? errno : EIO;
        if (!probed) { probed = true; a.sce = ProbeSce(path); }
        return a;
    }, env);
    if (!o.ok) errno = o.err;
    return o.ok ? f : nullptr;
}

// open(2) with the retry; -1 when it gave up.
inline int OpenRetry(const char* path, int flags, const char* tag, const Options& opt = Options(), Env env = Env()) {
    int fd = -1;
    bool probed = false;
    const Outcome o = Retry(tag, path, opt, [&]() {
        Attempt a;
        if (Inject(tag)) { a.err = EIO; a.sce = 0x80010005u; return a; }
        errno = 0;
        fd = open(path, flags);
        if (fd >= 0) { a.ok = true; return a; }
        a.err = errno ? errno : EIO;
        if (!probed) { probed = true; a.sce = ProbeSce(path); }
        return a;
    }, env);
    if (!o.ok) errno = o.err;
    return o.ok ? fd : -1;
}

}  // namespace bootio
