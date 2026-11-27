// stdio31: stdout and stderr written by many threads, without the C library's missing FILE locks (platform/include/port/console_stdio.h has
// the why, the design and the lock-order proof). The link wraps every console stdio call of the game's ONE link (the runtime archive, the game
// archive, vitaGL, the net stack and libstdc++ are inputs of the same ld command; vita/vgx-game/CMakeLists.txt, PORT_SILENT_STDIO=OFF, the
// diagnostic build). A call on any other FILE goes to the C library unchanged (each of those has one owner; the report lists them).
// The player build (PORT_SILENT_STDIO=ON) discards console output instead (silent_stdio.cpp) and never uses this file.
#include "port/console_stdio.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <atomic>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

extern "C" {
int __real_vfprintf(FILE*, const char*, va_list);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_putc(int, FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __real_fflush(FILE*);
void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
}

namespace {
size_t RealWrite(void* stream, const void* data, size_t n) { return __real_fwrite(data, 1, n, static_cast<FILE*>(stream)); }
int RealFlush(void* stream) { return __real_fflush(static_cast<FILE*>(stream)); }
int RealVPrint(void* stream, const char* fmt, va_list ap) { return __real_vfprintf(static_cast<FILE*>(stream), fmt, ap); }

port::console::Sink& Console() {
    static port::console::Sink* sink = [] {
        // (constructed in place in static storage on first use: a print can come before the C++ constructors of other units have run)
        alignas(port::console::Sink) static unsigned char room[sizeof(port::console::Sink)];
        auto* s = new (room) port::console::Sink();
        s->Init(port::console::Ops{&RealWrite, &RealFlush, &RealVPrint}, stdout, stderr);
        return s;
    }();
    return *sink;
}
// -1: another stream. (stdout / stderr are the same two FILE objects for every thread: this newlib keeps them global, __sf[3].)
int Index(FILE* stream) { return stream == stdout ? 0 : stream == stderr ? 1 : -1; }
}  // namespace

extern "C" {
// The log writer thread exists (platform/src/os/os_alarm.cpp, logFlusherMain): from now on prints are buffered and written by it.
void port_console_attach_writer(void) { Console().AttachWriter(); }
// The log writer's pass (os_alarm.cpp, logWritePending, before it takes any of its own locks).
void port_console_drain(void) { Console().Drain(); }
// freopen / setvbuf of stdout or stderr (vita/vgx-game/entry.cpp, GXV_STDERR_FILE=1): run with the stream lock held.
void port_console_exclusive(void (*fn)(void*), void* arg) { Console().Exclusive([&] { fn(arg); }); }
void port_console_configure(void);
}

// GXV_STDIO_SELFTEST=1 (default 0; the diagnostic build only): the console step of the report (reports/stdio31.md). Three helper threads print
// to the console the whole session, about 60 lines a second each, the way the per-picture diagnostics of several threads do: fprintf(stderr),
// fputs / fwrite(stderr) and printf(stdout), with fflush(stderr), fflush(NULL) and a line longer than the stack buffer now and then. Every line
// carries its thread, a sequence number and a check sum; tools/stdio31_check_console.py reads stderr.txt (GXV_STDERR_FILE=1) and counts lost,
// torn and repeated lines. With GXV_STDIO_SAFE=0 it is the control arm (the old unlocked writes: expect corruption; emulator only).
// One "stdio-selftest:" line in the log every 60 s.
namespace {
std::atomic<unsigned> sSelfLines[3];
unsigned SelfSum(const char* p, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<unsigned char>(p[i])) * 16777619u;
    return h & 0xFFFFu;
}
void* SelfTestMain(void* arg) {
    const int k = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    static const char kText[] = "the quick brown fox jumps over the lazy dog 0123456789";
    char body[200];
    char longBody[1400];
    struct timespec start = {2, 0};  // (after the start-up's reopen of stderr on stderr.txt, GXV_STDERR_FILE=1)
    nanosleep(&start, nullptr);
    for (unsigned seq = 1;; ++seq) {
        const int n = std::snprintf(body, sizeof body, "stdio-selftest t%d #%u %.*s", k, seq, static_cast<int>(10 + seq % 40), kText);
        const unsigned sum = SelfSum(body, static_cast<size_t>(n));
        if (k == 0) {
            std::fprintf(stderr, "%s sum=%04x\n", body, sum);
        } else if (k == 1) {
            char line[240];
            const int m = std::snprintf(line, sizeof line, "%s sum=%04x\n", body, sum);
            if (seq & 1) std::fputs(line, stderr);
            else std::fwrite(line, 1, static_cast<size_t>(m), stderr);
        } else {
            std::printf("%s sum=%04x\n", body, sum);
            if (seq % 30 == 0) std::fflush(stderr);
            if (seq % 600 == 0) std::fflush(nullptr);
        }
        if (seq % 100 == 0) {  // (longer than the 1 KiB stack buffer: written at once under the stream lock)
            const int m = std::snprintf(longBody, sizeof longBody, "stdio-selftest t%d #%u.long ", k, seq);
            for (int i = m; i < 1300; ++i) longBody[i] = static_cast<char>('a' + i % 26);
            longBody[1300] = 0;
            std::fprintf(stderr, "%s sum=%04x\n", longBody, SelfSum(longBody, 1300));
        }
        sSelfLines[k].fetch_add(1, std::memory_order_relaxed);
        struct timespec ts = {0, 16 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}
void* SelfTestCensus(void*) {
    for (unsigned minute = 1;; ++minute) {
        struct timespec ts = {60, 0};
        nanosleep(&ts, nullptr);
        const port::console::Stats& st = Console().stats();
        if (port_log_summary)
            port_log_summary("stdio-selftest: %u min, lines t0 %u t1 %u t2 %u; console: buffered %u, at once %u, dropped %u, passes %u, fflush(NULL) %u (GXV_STDIO_SAFE=%d)",
                             minute, sSelfLines[0].load(), sSelfLines[1].load(), sSelfLines[2].load(), st.buffered.load(), st.direct.load(), st.dropped.load(),
                             st.drains.load(), st.flushAll.load(), Console().Safe() ? 1 : 0);
    }
    return nullptr;
}
void StartSelfTest() {
    const char* e = std::getenv("GXV_STDIO_SELFTEST");
    if (!e || e[0] != '1') return;
    for (int k = 0; k < 4; ++k) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u << 10);
        pthread_t th;
        if (pthread_create(&th, &attr, k < 3 ? &SelfTestMain : &SelfTestCensus, reinterpret_cast<void*>(static_cast<intptr_t>(k))) == 0) pthread_detach(th);
        pthread_attr_destroy(&attr);
    }
    if (port_log_summary) port_log_summary("stdio-selftest: three threads print to the console about 60 lines a second each (GXV_STDIO_SELFTEST=1)");
}
}  // namespace

extern "C" {
// GXV_STDIO_SAFE (default 1; 0 = the old unlocked writes), read once env.txt is in the environment (entry.cpp). One line in the log.
void port_console_configure(void) {
    const char* e = std::getenv("GXV_STDIO_SAFE");
    const bool on = !(e && e[0] == '0');
    Console().SetSafe(on);
    if (port_log_summary)
        port_log_summary("stdio: console streams %s (GXV_STDIO_SAFE=%d)", on ? "locked: one writer at a time, buffered prints written by the log thread"
                                                                             : "UNLOCKED (the C library has no FILE locks: two threads printing at once can corrupt the heap)", on ? 1 : 0);
    StartSelfTest();
}

int __wrap_vfprintf(FILE* stream, const char* format, va_list args) {
    const int s = Index(stream);
    return s < 0 ? __real_vfprintf(stream, format, args) : Console().VPrint(s, format, args);
}
__attribute__((format(printf, 2, 3))) int __wrap_fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int r = __wrap_vfprintf(stream, format, args);
    va_end(args);
    return r;
}
int __wrap_vprintf(const char* format, va_list args) { return Console().VPrint(0, format, args); }
__attribute__((format(printf, 1, 2))) int __wrap_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int r = Console().VPrint(0, format, args);
    va_end(args);
    return r;
}
int __wrap_fputs(const char* text, FILE* stream) {
    const int s = Index(stream);
    if (s < 0) return __real_fputs(text, stream);
    Console().Put(s, text, std::strlen(text));
    return 0;
}
int __wrap_puts(const char* text) {
    // (one unit with its newline, so another thread's text cannot land between them)
    const size_t n = std::strlen(text);
    char line[port::console::Sink::kLine];
    if (n + 1 <= sizeof line) {
        std::memcpy(line, text, n);
        line[n] = '\n';
        Console().Put(0, line, n + 1);
    } else {
        Console().Put(0, text, n);
        Console().Put(0, "\n", 1);
    }
    return 0;
}
int __wrap_fputc(int value, FILE* stream) {
    const int s = Index(stream);
    if (s < 0) return __real_fputc(value, stream);
    const char c = static_cast<char>(value);
    Console().Put(s, &c, 1);
    return static_cast<unsigned char>(value);
}
int __wrap_putc(int value, FILE* stream) {
    const int s = Index(stream);
    if (s < 0) return __real_putc(value, stream);
    const char c = static_cast<char>(value);
    Console().Put(s, &c, 1);
    return static_cast<unsigned char>(value);
}
int __wrap_putchar(int value) {
    const char c = static_cast<char>(value);
    Console().Put(0, &c, 1);
    return static_cast<unsigned char>(value);
}
size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* stream) {
    const int s = Index(stream);
    if (s < 0) return __real_fwrite(data, size, count, stream);
    if (!size || !count) return 0;
    Console().Put(s, static_cast<const char*>(data), size * count);
    return count;
}
int __wrap_fflush(FILE* stream) {
    if (!stream) return Console().Safe() ? Console().FlushAll() : __real_fflush(nullptr);  // (never the C library's walk of every FILE: console_stdio.h, FlushAll)
    const int s = Index(stream);
    return s < 0 ? __real_fflush(stream) : Console().Flush(s);
}
}
