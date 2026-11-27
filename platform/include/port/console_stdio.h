// stdio31 (18 Nov 2026): stdout and stderr shared by many threads, made safe on the Vita.
//
// The Vita's C library (VitaSDK newlib) is built without _RETARGETABLE_LOCKING: flockfile is empty and no stdio call locks its FILE. Two
// threads writing the same buffered stream at once lose updates to its _p / _w fields; __sfvwrite_r then sees -1 bytes of room and memmoves
// (size_t)-1 bytes over the heap (the Strikers port found it on 18 Nov: a console crash in pte_osTlsGetValue, the TLS list holding ASCII; their
// host test with the SDK's own objects under qemu-arm failed 11 runs of 11). In Galaxy the diagnostic build (tools/vgx_build/build.py --logging)
// writes stderr from the game thread, the render / back-end threads, the shader worker, the net threads and the audio threads, and the log
// writer thread flushes it every 100 ms. (The player build discards console output before it is formatted, vita/vgx-game/silent_stdio.cpp.)
//
// The fix keeps the streams' buffering exactly as it was (the Strikers port's first fix, unbuffered streams, stalled their console).
// What a thread prints to stdout or stderr is formatted on its own stack and appended to a per-stream memory buffer under a short lock
// (`pend_`: a memcpy, nothing else). Only one thread at a time touches the FILE itself, under the stream lock (`file_`): the log writer
// thread every 100 ms (platform/src/os/os_alarm.cpp, logWritePending -> port_console_drain), a caller of fflush(stdout / stderr), a line too
// long for the stack buffer, a buffer that is full, and every write before the writer thread exists (start-up), which then writes at once.
//
// Lock order (the proof that this cannot deadlock):
//   * `pend_` is a leaf: it is held only around LogSwap::Append / Take (memcpy and pointer swaps); nothing is called under it.
//   * `file_` is taken only by the functions of this class. Under it run: `pend_` (Take), and the real stdio calls on stdout / stderr (newlib's
//     __sfvwrite_r / __sflush_r -> _write_r -> sceIoWrite). Newlib's stdio calls none of the port's code (the only link wraps on that path are
//     the cold-trace build's sceIoWrite observer, and a print from such a hook is caught by the re-entry guard below). So `file_` is never
//     wanted while `pend_` is held, and no other lock of the port is held under `file_` except through such a hook.
//   * Re-entry: a thread that already holds `file_` (a hook printing from inside the write) never waits for it again: its text is appended
//     if it fits, else counted as dropped. The lock is never taken twice by one thread.
//   * Timer and interrupt contexts: the port has none that preempt a thread. The os-alarm timer thread (os_alarm.cpp, timerThread) prints
//     nothing; it only raises an interrupt bit. Alarm and other interrupt handlers run on a guest thread at a safe point (os_thread.cpp,
//     runIrqHandlers), never in the middle of this code (there is no safe point in it), so a handler that prints takes these locks as any
//     caller does. Neither lock is held while waiting for the guest CPU or for any port lock.
//   * The log writer (os_alarm.cpp) calls Drain() BEFORE it takes its own locks (sLogWriteLock, sLogPendLock, sLogFileLock), and no code holds
//     one of those while printing to the console (they guard memcpy and the log FILE only). The only order that exists is file_ -> log locks
//     (a hook that logs from inside the write), never the reverse.
// tests/stdio31/ has the host proofs: the SDK's own newlib objects under qemu-arm (the unlocked pattern corrupts; this one does not) and a
// stress test with a lock-order check (PORT_CONSOLE_LOCK_CHECK), an alarm-like thread printing while it holds a "CPU" lock, a write hook that
// prints, fflush(NULL), long lines and a reopen.
//
// GXV_STDIO_SAFE=0 (env.txt; default 1) is the kill switch: every call goes straight to the C library as before this fix (unlocked).
// No heap use: the console is written from everywhere, including while the game's allocator is busy (the buffers are fixed).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <atomic>
#include <mutex>
#include "port/log_sink.h"

#ifndef PORT_CONSOLE_CAPACITY
#define PORT_CONSOLE_CAPACITY (16u << 10)
#endif

namespace port {
namespace console {

// The real stdio calls (the C library's, behind the link wraps), on an opaque stream.
struct Ops {
    size_t (*write)(void* stream, const void* data, size_t n);
    int (*flush)(void* stream);
    int (*vprint)(void* stream, const char* fmt, va_list ap);
};

struct Stats {
    std::atomic<unsigned> buffered{0};  // writes appended to a stream's buffer
    std::atomic<unsigned> direct{0};    // writes made at once under the stream lock (no writer thread yet, a full buffer, a long line)
    std::atomic<unsigned> dropped{0};   // writes lost: a full buffer met by a thread that already holds the stream lock (a hook in the write)
    std::atomic<unsigned> drains{0};    // passes that wrote buffered text
    std::atomic<unsigned> flushAll{0};  // fflush(NULL) calls (made a flush of stdout and stderr only: the FILE list is never walked)
};

class Sink {
public:
    static constexpr size_t kCap = PORT_CONSOLE_CAPACITY;  // per stream, two buffers each
    static constexpr size_t kLine = 1024;                  // a formatted print up to this size is buffered; longer ones are written at once

    Sink() : swap_{LogSwap(a_[0], b_[0], kCap), LogSwap(a_[1], b_[1], kCap)} {}

    void Init(const Ops& ops, void* out, void* err) {
        ops_ = ops;
        stream_[0] = out;
        stream_[1] = err;
    }
    // 0 = stdout, 1 = stderr, -1 = another stream (not handled here).
    int Index(const void* stream) const { return stream == stream_[1] ? 1 : stream == stream_[0] ? 0 : -1; }
    bool Safe() const { return safe_.load(std::memory_order_relaxed); }
    // The kill switch (GXV_STDIO_SAFE=0): what is buffered is written first, then every call goes straight to the C library.
    void SetSafe(bool on) {
        if (on == Safe()) return;
        if (!on) Exclusive([] {});
        safe_.store(on, std::memory_order_relaxed);
    }
    // The writer thread exists from now on (it calls Drain every 100 ms): prints are buffered. Before, every print is written at once.
    void AttachWriter() { writer_.store(true, std::memory_order_release); }
    bool WriterAttached() const { return writer_.load(std::memory_order_acquire); }

    // fwrite / fputs / fputc / putc / puts on stream s.
    void Put(int s, const char* p, size_t n) {
        if (!Safe()) { ops_.write(stream_[s], p, n); return; }
        if (n == 0) return;
        if (WriterAttached()) {
            bool ok;
            {
                PendHold hold(*this);
                ok = swap_[s].Append(p, n);
            }
            if (ok) { stats_.buffered.fetch_add(1, std::memory_order_relaxed); return; }
        }
        if (InFile()) {  // (a print from inside our own write: never wait for ourselves)
            bool ok;
            {
                PendHold hold(*this);
                ok = swap_[s].Append(p, n);
            }
            (ok ? stats_.buffered : stats_.dropped).fetch_add(1, std::memory_order_relaxed);
            return;
        }
        FileHold hold(*this);
        DrainLocked();  // (what this thread and the others buffered before goes first)
        ops_.write(stream_[s], p, n);
        dirty_[s] = true;
        stats_.direct.fetch_add(1, std::memory_order_relaxed);
    }
    // vfprintf / fprintf / printf / vprintf on stream s. Returns what the C library's call returns (the length, or < 0).
    int VPrint(int s, const char* fmt, va_list ap) {
        if (!Safe()) return ops_.vprint(stream_[s], fmt, ap);
        char line[kLine];
        va_list again;
        va_copy(again, ap);
        const int n = vsnprintf(line, sizeof line, fmt, ap);
        if (n < 0) { va_end(again); return n; }
        if (static_cast<size_t>(n) < sizeof line) {
            va_end(again);
            Put(s, line, static_cast<size_t>(n));
            return n;
        }
        if (InFile()) {  // (a long print from inside our own write: its first kLine - 1 bytes, rather than waiting for ourselves)
            va_end(again);
            Put(s, line, sizeof line - 1);
            return n;
        }
        int r;
        {
            FileHold hold(*this);
            DrainLocked();
            r = ops_.vprint(stream_[s], fmt, again);
            dirty_[s] = true;
            stats_.direct.fetch_add(1, std::memory_order_relaxed);
        }
        va_end(again);
        return r;
    }
    // fflush(stdout) / fflush(stderr): what is buffered for the stream, and the FILE's own buffer, reach the system now (as before).
    int Flush(int s) {
        if (!Safe()) return ops_.flush(stream_[s]);
        if (InFile()) return 0;
        FileHold hold(*this);
        DrainLocked();
        dirty_[s] = false;
        return ops_.flush(stream_[s]);
    }
    // fflush(NULL): stdout and stderr only. The C library's walk of every FILE would also flush streams other threads are reading or writing
    // without a lock (a read stream's buffer is dropped and its position moved under its reader); no code of the Vita build calls it.
    int FlushAll() {
        stats_.flushAll.fetch_add(1, std::memory_order_relaxed);
        const int a = Flush(0);
        const int b = Flush(1);
        return a ? a : b;
    }
    // The writer thread's pass (every 100 ms, and port_log_flush): everything buffered, then each stream that got text is flushed.
    void Drain() {
        if (!Safe()) {  // (as before this fix: logWritePending flushed stderr; and text a thread buffered just as the switch went off)
            if (!InFile()) {
                FileHold hold(*this);
                DrainLocked();
            }
            ops_.flush(stream_[1]);
            return;
        }
        if (InFile()) return;
        FileHold hold(*this);
        DrainLocked();
        for (int s = 0; s < 2; ++s)
            if (dirty_[s]) { dirty_[s] = false; ops_.flush(stream_[s]); }
    }
    // freopen / setvbuf on stdout or stderr: done with the stream lock held, after what is buffered has been written.
    template <class F> void Exclusive(F&& f) {
        if (InFile()) { f(); return; }
        FileHold hold(*this);
        DrainLocked();
        for (int s = 0; s < 2; ++s)
            if (dirty_[s]) { dirty_[s] = false; ops_.flush(stream_[s]); }
        f();
    }
    const Stats& stats() const { return stats_; }

#if defined(PORT_CONSOLE_LOCK_CHECK)
    // (tests) the lock-order check: calls `fail` when file_ is wanted while pend_ is held, or either lock is wanted twice by one thread.
    static void (*&LockFail())(const char*) {
        static void (*f)(const char*) = nullptr;
        return f;
    }
#endif

private:
    static bool& InFile() {
        static thread_local bool v = false;
        return v;
    }
    static bool& InPend() {
        static thread_local bool v = false;
        return v;
    }
    static void Check(bool bad, const char* what) {
#if defined(PORT_CONSOLE_LOCK_CHECK)
        if (bad && LockFail()) LockFail()(what);
#else
        (void)bad;
        (void)what;
#endif
    }
    struct PendHold {
        Sink& k;
        explicit PendHold(Sink& s) : k(s) {
            Check(InPend(), "pend_ taken twice by one thread");
            k.pend_.lock();
            InPend() = true;
        }
        ~PendHold() {
            InPend() = false;
            k.pend_.unlock();
        }
    };
    struct FileHold {
        Sink& k;
        explicit FileHold(Sink& s) : k(s) {
            Check(InPend(), "file_ wanted while pend_ is held (lock order)");
            Check(InFile(), "file_ taken twice by one thread");
            k.file_.lock();
            InFile() = true;
        }
        ~FileHold() {
            InFile() = false;
            k.file_.unlock();
        }
    };
    // file_ held. LogSwap's rule: one taker at a time (file_), and the taken buffer is written before the next Take swaps it back in.
    void DrainLocked() {
        for (int s = 0; s < 2; ++s) {
            const char* data = nullptr;
            size_t n;
            {
                PendHold hold(*this);
                n = swap_[s].Take(&data);
            }
            if (n) {
                ops_.write(stream_[s], data, n);
                dirty_[s] = true;
                stats_.drains.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::mutex pend_;  // leaf: memcpy / swap only
    std::mutex file_;  // the only lock under which stdout / stderr are touched
    Ops ops_{};
    void* stream_[2] = {nullptr, nullptr};
    std::atomic<bool> safe_{true};
    std::atomic<bool> writer_{false};
    bool dirty_[2] = {false, false};  // (file_) written since the last flush
    Stats stats_;
    LogSwap swap_[2];
    char a_[2][kCap];
    char b_[2][kCap];
};

}  // namespace console
}  // namespace port
