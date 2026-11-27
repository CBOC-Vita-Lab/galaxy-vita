// stdio31 host proof (tools/test_stdio31.sh). Several threads print to one console stream while a writer thread flushes it, the way the Vita's
// diagnostic build does (the game, render / back-end, shader, net and audio threads print; the log writer flushes stderr every 100 ms).
//
// Backends:
//   -DNL_BACKEND   the stream is a VitaSDK newlib FILE: the SDK's own fvwrite.o / fflush.o / fwrite.o / fputs.o / wbuf.o / wsetup.o / makebuf.o /
//                  putc.o (renamed nl_*), cross-built for armhf and run under qemu-arm. Like the console, these take no lock.
//   (default)      the stream is a model with no lock at all (one buffer, one index); built natively with -fsanitize=thread, any two threads in it
//                  at once is a reported race.
// Modes:
//   old     every thread calls the C library directly (the pattern before stdio31). Expect corruption or a crash with the newlib backend.
//   safe    every thread goes through port::console::Sink (platform/include/port/console_stdio.h), the code the Vita build uses.
//   stress  safe, plus the deadlock checks: an "alarm" thread printing while it holds the guest-CPU lock, a model of the log writer's own
//           locks taken in os_alarm.cpp's order, a write hook that prints (and logs) from inside the write, a slow write, fflush(NULL),
//           long lines, a reopen; PORT_CONSOLE_LOCK_CHECK (lock order) and a watchdog (no progress for 15 s = deadlock, exit 3).
// Usage: stdio_race <old|safe|stress> <fbf|lbf|nbf> [seconds=3] [writers=4] [stress pause after each line, us=100]
// Exit: 0 every line intact, exactly once, in order per thread; 1 lost / torn / repeated lines or a bad FILE state; 3 deadlock; a crash is a crash.
#define PORT_CONSOLE_LOCK_CHECK 1
#include "port/console_stdio.h"

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// ---- what reaches the "system" (the write callback): appended at an atomic offset, checked at the end
constexpr size_t kOutCap = 48u << 20;
char* gOut;
std::atomic<size_t> gOutLen{0};
std::atomic<unsigned> gBadWrites{0};
std::atomic<unsigned> gHookPrints{0};
std::atomic<bool> gHook{false}, gSlow{false};
std::atomic<unsigned> gWrites{0};

void Fail(const char* what) {
    char line[256];
    const int n = snprintf(line, sizeof line, "LOCK ORDER: %s\n", what);
    if (write(2, line, n) < 0) {}
    _exit(4);
}
void OnSignal(int sig) {
    char line[64];
    const int n = snprintf(line, sizeof line, "CRASH: signal %d\n", sig);
    if (write(2, line, n) < 0) {}
    _exit(128 + sig);
}

void Sink(const char* data, int n);
port::console::Sink* gSink;
void LogModel(const char* text);

int OnWrite(void*, const char* data, int n) {
    if (n < 0 || n > (8 << 20)) { gBadWrites.fetch_add(1); return n < 0 ? -1 : 0; }
    Sink(data, n);
    const unsigned w = gWrites.fetch_add(1);
    if (gSlow.load(std::memory_order_relaxed) && w % 7 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));  // (a card write)
    if (gHook.load(std::memory_order_relaxed)) {
        // The cold-trace build's sceIoWrite observer, or any hook on the write path, printing (and logging) from INSIDE the write: the re-entry
        // guard must neither wait for the stream lock it already holds nor corrupt the stream.
        if (w % 50 == 0) {
            char line[96];
            const int m = snprintf(line, sizeof line, "hook %u\n", w);
            gSink->Put(1, line, static_cast<size_t>(m));
            gHookPrints.fetch_add(1);
        }
        if (w % 200 == 0) LogModel("hook: a log line from inside the console write");
    }
    return n;
}
void Sink(const char* data, int n) {
    const size_t at = gOutLen.fetch_add(static_cast<size_t>(n));
    if (at + static_cast<size_t>(n) <= kOutCap) memcpy(gOut + at, data, static_cast<size_t>(n));
}

// ---- the stream
#if defined(NL_BACKEND)
extern "C" {
size_t nl_fwrite(const void*, size_t, size_t, FILE*);
int nl_fputs(const char*, FILE*);
int nl_fflush(FILE*);
FILE* shim_make_file(int mode, unsigned char* buf, int size, void* cookie, int (*cb)(void*, const char*, int));
void shim_file_state(FILE* f, unsigned char** p, int* w, unsigned char** base, int* size);
}
FILE* gFile;
unsigned char* gBuf;
int gBufSize;
constexpr int kGuard = 4096;
size_t StreamWrite(void* s, const void* d, size_t n) { return nl_fwrite(d, 1, n, static_cast<FILE*>(s)); }
int StreamPuts(void* s, const char* t) { return nl_fputs(t, static_cast<FILE*>(s)); }
int StreamFlush(void* s) { return nl_fflush(static_cast<FILE*>(s)); }
int StreamFlushAll() { return nl_fflush(nullptr); }
void* MakeStream(int mode) {
    gBufSize = 16384;  // (GXV_STDERR_FILE's setvbuf; line-buffered stdout gets the same size here)
    unsigned char* room = static_cast<unsigned char*>(malloc(gBufSize + 2 * kGuard));
    memset(room, 0xA5, gBufSize + 2 * kGuard);
    gBuf = room + kGuard;
    gFile = shim_make_file(mode, gBuf, gBufSize, nullptr, &OnWrite);
    return gFile;
}
// The FILE's write state and the guard bytes around its buffer. Only meaningful when no thread is in the stream (the end of the run).
bool StreamStateOk(char* why, size_t n) {
    unsigned char *p, *base;
    int w, size;
    shim_file_state(gFile, &p, &w, &base, &size);
    for (int i = 0; i < kGuard; ++i)
        if (gBuf[-1 - i] != 0xA5 || (base == gBuf && gBuf[gBufSize + i] != 0xA5)) { snprintf(why, n, "guard bytes around the buffer overwritten"); return false; }
    if (base == gBuf && (p < base || p > base + size)) { snprintf(why, n, "_p outside the buffer (%ld)", static_cast<long>(p - base)); return false; }
    if (w < -size || w > size) { snprintf(why, n, "_w = %d (buffer %d)", w, size); return false; }
    return true;
}
#else
// A stream with no lock at all: one buffer and an index. TSan reports any two threads in it at once.
struct Model {
    char buf[16384];
    size_t len = 0;
    int mode = 0;
} gModel;
void ModelPush() {
    if (gModel.len) { OnWrite(nullptr, gModel.buf, static_cast<int>(gModel.len)); gModel.len = 0; }
}
size_t StreamWrite(void*, const void* d, size_t n) {
    const char* p = static_cast<const char*>(d);
    for (size_t i = 0; i < n; ++i) {
        if (gModel.len == sizeof gModel.buf) ModelPush();
        gModel.buf[gModel.len++] = p[i];
        if ((gModel.mode == 1 && p[i] == '\n') || gModel.mode == 2) ModelPush();
    }
    return n;
}
int StreamPuts(void* s, const char* t) { StreamWrite(s, t, strlen(t)); return 0; }
int StreamFlush(void*) { ModelPush(); return 0; }
int StreamFlushAll() { ModelPush(); return 0; }
void* MakeStream(int mode) { gModel.mode = mode; return &gModel; }
bool StreamStateOk(char*, size_t) { return gModel.len <= sizeof gModel.buf; }
#endif

int StreamVPrint(void* s, const char* fmt, va_list ap) {  // (the long-line path: format, then the stream's own write)
    char big[8192];
    const int n = vsnprintf(big, sizeof big, fmt, ap);
    if (n > 0) StreamWrite(s, big, static_cast<size_t>(n) < sizeof big ? static_cast<size_t>(n) : sizeof big - 1);
    return n;
}

// ---- a model of the log writer's own locks (platform/src/os/os_alarm.cpp), in its order: port_console_drain FIRST, then
// sLogWriteLock -> sLogPendLock (swap) ; sLogFileLock (the card write). port_log appends under sLogPendLock and, when full, writes itself.
std::mutex sLogWriteLock, sLogPendLock, sLogFileLock;
size_t sLogPending = 0;
std::atomic<unsigned> sLogPasses{0};
void LogWritePending() {
    gSink->Drain();
    std::lock_guard<std::mutex> wl(sLogWriteLock);
    {
        std::lock_guard<std::mutex> lk(sLogPendLock);
        sLogPending = 0;
    }
    std::lock_guard<std::mutex> fl(sLogFileLock);
    sLogPasses.fetch_add(1, std::memory_order_relaxed);
}
void LogModel(const char* text) {
    bool full;
    {
        std::lock_guard<std::mutex> lk(sLogPendLock);
        sLogPending += strlen(text);
        full = sLogPending > 4096;
    }
    if (full) LogWritePending();
}

// ---- the lines: "w<k> <seq> <filler> <sum>\n", checked at the end
unsigned Sum(const char* p, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<unsigned char>(p[i])) * 16777619u;
    return h & 0xFFFFu;
}
int MakeLine(char* out, size_t cap, int k, unsigned seq, size_t fill) {
    int n = snprintf(out, cap, "w%d %u ", k, seq);
    for (size_t i = 0; i < fill && static_cast<size_t>(n) + 16 < cap; ++i) out[n++] = static_cast<char>('a' + (seq + i) % 26);
    n += snprintf(out + n, cap - n, " %04x\n", Sum(out, static_cast<size_t>(n)));
    return n;
}

// (no sscanf: the output is not NUL-terminated and is tens of MiB)
bool ParseHead(const char* p, size_t n, int* k, unsigned* seq) {  // "w<k> <seq> "
    size_t i = 1;
    if (i >= n || p[i] < '0' || p[i] > '9') return false;
    *k = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') *k = *k * 10 + (p[i++] - '0');
    if (i >= n || p[i++] != ' ' || i >= n || p[i] < '0' || p[i] > '9') return false;
    *seq = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') *seq = *seq * 10u + static_cast<unsigned>(p[i++] - '0');
    return i < n && p[i] == ' ';
}
bool ParseHex4(const char* p, unsigned* v) {
    *v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = p[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        *v = *v * 16u + static_cast<unsigned>(d);
    }
    return true;
}
std::atomic<bool> gStop{false};
std::atomic<unsigned> gProgress[16];
std::atomic<unsigned> gLast[16];
std::mutex gCpu;  // the guest CPU (os_thread.cpp: interrupt handlers run with it held, at a safe point)
int gMode = 0;    // 0 old, 1 safe, 2 stress
int gPauseUs = 100;  // (stress: a writer's pause after each line; a long soak passes more)
void* gStream;

void Print(int k, const char* line, int n, unsigned seq) {
    if (gMode == 0) {
        if (seq & 1) StreamPuts(gStream, line);
        else StreamWrite(gStream, line, static_cast<size_t>(n));
    } else {
        if (seq % 3 == 0) gSink->Put(1, line, static_cast<size_t>(n));
        else {
            // (a printf of the line: formatted by the Sink on the thread's stack, buffered)
            char copy[600];
            memcpy(copy, line, static_cast<size_t>(n));
            copy[n] = 0;
            struct Call {
                static int Go(port::console::Sink* s, const char* fmt, ...) {
                    va_list ap;
                    va_start(ap, fmt);
                    const int r = s->VPrint(1, fmt, ap);
                    va_end(ap);
                    return r;
                }
            };
            Call::Go(gSink, "%s", copy);
        }
    }
    (void)k;
}

void Writer(int k) {
    char line[600];
    char big[2200];
    for (unsigned seq = 1; !gStop.load(std::memory_order_relaxed); ++seq) {
        const int n = MakeLine(line, sizeof line, k, seq, 8 + (seq * 37u + k * 11u) % 180);
        const bool asAlarm = gMode == 2 && k == 0;  // (writer 0 = the game thread: it prints with the guest CPU held, as an alarm handler does)
        if (asAlarm) gCpu.lock();
        if (gMode == 2 && seq % 97 == 0) {
            // a line longer than the Sink's stack buffer (1 KiB): written at once under the stream lock, after what is buffered
            const int m = MakeLine(big, sizeof big, k, seq, 1500);
            struct Call {
                static int Go(port::console::Sink* s, const char* fmt, ...) {
                    va_list ap;
                    va_start(ap, fmt);
                    const int r = s->VPrint(1, fmt, ap);
                    va_end(ap);
                    return r;
                }
            };
            Call::Go(gSink, "%s", big);
            (void)m;
        } else {
            Print(k, line, n, seq);
        }
        if (asAlarm) gCpu.unlock();
        if (gMode == 2 && seq % 41 == 0) LogModel("a log line");
        if (gMode >= 1 && seq % 53 == 0) gSink->Flush(1);  // (the back end's fprintf + fflush(stderr) per picture)
        gLast[k].store(seq, std::memory_order_relaxed);
        gProgress[k].fetch_add(1, std::memory_order_relaxed);
        if (gOutLen.load(std::memory_order_relaxed) > kOutCap / 2) gStop = true;  // (enough text to check: the run ends early)
        if (gMode == 2) std::this_thread::sleep_for(std::chrono::microseconds(gPauseUs));  // (stress: slower lines, so a long run is not cut short by the output)
        else if (seq % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <old|safe|stress> <fbf|lbf|nbf> [seconds] [writers]\n", argv[0]); return 2; }
    gMode = !strcmp(argv[1], "old") ? 0 : !strcmp(argv[1], "safe") ? 1 : 2;
    const int bufMode = !strcmp(argv[2], "fbf") ? 0 : !strcmp(argv[2], "lbf") ? 1 : 2;
    const double seconds = argc > 3 ? atof(argv[3]) : 3.0;
    const int writers = argc > 4 ? atoi(argv[4]) : 4;
    if (argc > 5) gPauseUs = atoi(argv[5]);
    signal(SIGSEGV, OnSignal);
    signal(SIGBUS, OnSignal);
    signal(SIGILL, OnSignal);
    gOut = static_cast<char*>(malloc(kOutCap));
    port::console::Sink::LockFail() = &Fail;
    static port::console::Sink sink;
    gSink = &sink;
    gStream = MakeStream(bufMode);
    sink.Init(port::console::Ops{&StreamWrite, &StreamFlush, &StreamVPrint}, nullptr /* (stdout: unused here) */, gStream);
    if (gMode >= 1) sink.AttachWriter();
    if (gMode == 2) { gHook = true; gSlow = true; }

    std::vector<std::thread> threads;
    for (int k = 0; k < writers; ++k) threads.emplace_back(Writer, k);
    // the log writer thread (logFlusherMain: a pass every 100 ms; here every millisecond, to meet the printers far more often)
    threads.emplace_back([&] {
        while (!gStop.load(std::memory_order_relaxed)) {
            if (gMode == 0) StreamFlush(gStream);  // (logWritePending's fflush(stderr), before stdio31)
            else LogWritePending();
            gProgress[14].fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    // another thread now and then: fflush(NULL) and, in the stress run, a reopen (freopen + setvbuf of stderr, entry.cpp)
    threads.emplace_back([&] {
        for (unsigned i = 0; !gStop.load(std::memory_order_relaxed); ++i) {
            if (gMode == 0) StreamFlushAll();
            else sink.FlushAll();
            if (gMode == 2 && i % 10 == 0) sink.Exclusive([] { std::this_thread::sleep_for(std::chrono::microseconds(300)); });
            if (gMode == 2) {  // (an interrupt handler at a safe point: holds the guest CPU, prints)
                std::lock_guard<std::mutex> cpu(gCpu);
                char line[64];
                const int n = snprintf(line, sizeof line, "alarm %u\n", i);
                sink.Put(1, line, static_cast<size_t>(n));
            }
            gProgress[15].fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    // the watchdog
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<unsigned> seen(16, 0);
    auto lastMove = std::vector<std::chrono::steady_clock::time_point>(16, t0);
    while (!gStop.load() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto now = std::chrono::steady_clock::now();
        for (int k = 0; k < 16; ++k) {
            if (k >= writers && k < 14) continue;
            const unsigned p = gProgress[k].load();
            if (p != seen[k]) { seen[k] = p; lastMove[k] = now; }
            else if (std::chrono::duration<double>(now - lastMove[k]).count() > 15.0) {
                fprintf(stderr, "DEADLOCK: thread %d made no progress for 15 s\n", k);
                _exit(3);
            }
        }
    }
    gStop = true;
    for (auto& t : threads) t.join();
    if (gMode >= 1) {
        // (the last pass's own writes can run the hook, which buffers a line for the NEXT pass: drain until a pass prints nothing new)
        for (int i = 0; i < 20; ++i) {
            const unsigned before = gHookPrints.load();
            sink.Drain();
            if (gHookPrints.load() == before) break;
        }
    } else {
        StreamFlush(gStream);
    }

    // ---- the check
    char why[160] = "";
    bool stateOk = StreamStateOk(why, sizeof why);
    const size_t len = gOutLen.load();
    if (len > kOutCap) { fprintf(stderr, "output too long for the check buffer\n"); return 2; }
    std::vector<unsigned> expect(writers, 1);
    unsigned good = 0, torn = 0, lost = 0, repeated = 0, hookLines = 0, alarmLines = 0;
    size_t pos = 0;
    while (pos < len) {
        const char* e = static_cast<const char*>(memchr(gOut + pos, '\n', len - pos));
        const size_t end = e ? static_cast<size_t>(e - gOut) : len;
        const char* p = gOut + pos;
        const size_t n = end - pos;
        pos = end + 1;
        int k;
        unsigned seq, sum;
        if (n > 5 && !strncmp(p, "hook ", 5)) { ++hookLines; continue; }
        if (n > 6 && !strncmp(p, "alarm ", 6)) { ++alarmLines; continue; }
        if (n < 8 || p[0] != 'w' || !ParseHead(p, n, &k, &seq) || k < 0 || k >= writers) { ++torn; continue; }
        if (!ParseHex4(p + n - 4, &sum) || p[n - 5] != ' ' || Sum(p, n - 5) != sum) { ++torn; continue; }
        if (seq < expect[k]) { ++repeated; continue; }
        lost += seq - expect[k];
        expect[k] = seq + 1;
        ++good;
    }
    for (int k = 0; k < writers; ++k) lost += gLast[k].load() + 1 - expect[k];  // (a tail never written)
    const port::console::Stats& st = sink.stats();
    printf("%s %s: %.1f s, %u lines intact, %u torn, %u lost, %u repeated, bad write sizes %u, FILE state %s%s; writes %u, log passes %u; hook lines %u/%u, alarm lines %u; "
           "sink: buffered %u, at once %u, dropped %u, passes %u, fflush(NULL) %u\n",
           argv[1], argv[2], std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), good, torn, lost, repeated, gBadWrites.load(), stateOk ? "ok" : "BROKEN: ", why, gWrites.load(), sLogPasses.load(),
           hookLines, gHookPrints.load(), alarmLines, st.buffered.load(), st.direct.load(), st.dropped.load(), st.drains.load(), st.flushAll.load());
    const bool hookOk = gMode != 2 || hookLines + st.dropped.load() >= gHookPrints.load();
    return (torn || lost || repeated || gBadWrites.load() || !stateOk || !hookOk) ? 1 : 0;
}
