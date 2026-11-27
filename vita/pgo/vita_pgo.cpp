// Profile recording on the console (instrumented builds only: VITA_PGO_GENERATE=1, compiled with GXV_PGO_GENERATE=1; tools/pgo/README.md,
// "Record on the console").  The game units and the renderer's are compiled with -fprofile-generate; this file writes their counters to the
// memory card as a raw clang profile (the vendored compiler-rt writer, vita_pgo_core.c), while the game keeps running:
//   * a helper thread at the lowest user priority, every GXV_PGO_DUMP_S seconds (env.txt; default 120; 0 = no timer), and at once when the
//     marker file ux0:data/smg/pgo.dump exists (made by the relay's `put`; the thread removes it);
//   * at exit (port_pgo_at_exit, from entry.cpp's exit, abort and sceKernelExitProcess paths), on the calling thread.
// Nothing here runs on the game or renderer threads except that exit call.  A dump is a snapshot: the counters are read while they count
// (a counter being incremented is read as before or after it; the increments of two threads at once may be lost, as in the harness).
// Files (a file is written under a temporary name and renamed, so the relay never sees half of one):
//   ux0:data/smg/pgo/default-<n>.profraw   the snapshots of this run (the last three are kept)
//   ux0:data/smg/pgo.profraw               the newest snapshot, flat (the relay reaches ux0:data/smg/<name> only)
//   ux0:data/smg/pgo/last.txt              the log of the newest dump (functions, counters, bytes, milliseconds)
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "port/diagnostics.h"
#include "port/port.h"
#include "vita_pgo_core.h"

#define PGO_DIR "ux0:data/smg/pgo"
#define PGO_FLAT "ux0:data/smg/pgo.profraw"
#define PGO_MARKER "ux0:data/smg/pgo.dump"

namespace {
std::mutex g_dumpMutex;
std::atomic<int> g_started{0};
std::atomic<int> g_seq{0};
std::atomic<int> g_exitDone{0};

struct Sink {
    SceUID fd;
};

int FileSink(void* ctx, const void* data, size_t size) {
    const SceUID fd = static_cast<Sink*>(ctx)->fd;
    const char* p = static_cast<const char*>(data);
    while (size) {
        const size_t n = size > (256u << 10) ? (256u << 10) : size;  // (256 KiB pieces: the card's own IO interleaves with the game's)
        const int w = sceIoWrite(fd, p, n);
        if (w <= 0) return 1;
        p += w;
        size -= static_cast<size_t>(w);
    }
    return 0;
}

// rename over an existing file: the system call refuses an existing target, so the old one goes first (a moment in which only the
// numbered file exists; the relay polls the flat one rarely and gets the previous or the new one, never half of one)
bool ReplaceFile(const char* from, const char* to) {
    sceIoRemove(to);
    return sceIoRename(from, to) >= 0;
}

bool CopyFile(const char* from, const char* to) {
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    const SceUID out = sceIoOpen(to, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return false; }
    static char buf[128 << 10];  // (only the dump thread or the exit path copies, one at a time under g_dumpMutex)
    bool ok = true;
    for (;;) {
        const int n = sceIoRead(in, buf, sizeof buf);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (sceIoWrite(out, buf, static_cast<SceSize>(n)) != n) { ok = false; break; }
    }
    sceIoClose(in);
    sceIoClose(out);
    return ok;
}

// Remove the files of an earlier run (default-*.profraw*, the flat file): a snapshot is always from this run.
void CleanOld() {
    const SceUID d = sceIoDopen(PGO_DIR);
    if (d >= 0) {
        SceIoDirent e;
        for (;;) {
            std::memset(&e, 0, sizeof e);
            if (sceIoDread(d, &e) <= 0) break;
            if (!std::strncmp(e.d_name, "default-", 8)) {
                char path[300];
                std::snprintf(path, sizeof path, PGO_DIR "/%s", e.d_name);
                sceIoRemove(path);
            }
        }
        sceIoDclose(d);
    }
    sceIoRemove(PGO_FLAT);
}

void Note(const char* text) {
    const SceUID fd = sceIoOpen(PGO_DIR "/last.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, text, static_cast<SceSize>(std::strlen(text)));
    sceIoClose(fd);
}

// One snapshot.  `why` is for the log ("timer", "marker", "exit").  Returns true when the files are in place.
bool Dump(const char* why) {
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    const int n = ++g_seq;
    char tmp[96], path[96], line[256];
    std::snprintf(tmp, sizeof tmp, PGO_DIR "/default-%d.profraw.tmp", n);
    std::snprintf(path, sizeof path, PGO_DIR "/default-%d.profraw", n);
    const SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        port_log_summary("pgo: cannot create %s (0x%08X)", tmp, static_cast<unsigned>(fd));
        return false;
    }
    Sink s{fd};
    const int rc = vita_pgo_write(FileSink, &s);
    sceIoClose(fd);
    if (rc) {
        sceIoRemove(tmp);
        port_log_summary("pgo: writing %s failed (card full?)", tmp);
        return false;
    }
    bool ok = ReplaceFile(tmp, path);
    // the flat copy for the relay, made from the finished file (the same bytes), then the older snapshots go
    char flatTmp[] = PGO_FLAT ".tmp";
    ok = ok && CopyFile(path, flatTmp) && ReplaceFile(flatTmp, PGO_FLAT);
    if (n > 3) {
        char old[96];
        std::snprintf(old, sizeof old, PGO_DIR "/default-%d.profraw", n - 3);
        sceIoRemove(old);
    }
    const unsigned ms = static_cast<unsigned>((sceKernelGetProcessTimeWide() - t0) / 1000);
    std::snprintf(line, sizeof line, "pgo: dump %d (%s): %u functions, %u counters, %u bytes, %u ms, process time %u s%s\n", n, why,
                  static_cast<unsigned>(vita_pgo_num_functions()), static_cast<unsigned>(vita_pgo_num_counters()),
                  static_cast<unsigned>(vita_pgo_size()), ms, static_cast<unsigned>(sceKernelGetProcessTimeWide() / 1000000), ok ? "" : " (FAILED to place)");
    port_log_summary("%s", line);
    Note(line);
    return ok;
}

int DumpThread(SceSize, void*) {
    const char* e = std::getenv("GXV_PGO_DUMP_S");
    const long period = e && *e ? std::atol(e) : 120;
    uint64_t next = sceKernelGetProcessTimeWide() + static_cast<uint64_t>(period > 0 ? period : 0) * 1000000ull;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);  // (one wake-up a second: a stat and a clock read)
        if (g_exitDone.load(std::memory_order_relaxed)) break;
        SceIoStat st;
        const bool marker = sceIoGetstat(PGO_MARKER, &st) >= 0;
        if (marker) sceIoRemove(PGO_MARKER);
        const bool due = period > 0 && sceKernelGetProcessTimeWide() >= next;
        if (marker || due) {
            Dump(marker ? "marker" : "timer");
            next = sceKernelGetProcessTimeWide() + static_cast<uint64_t>(period > 0 ? period : 0) * 1000000ull;
        }
    }
    return sceKernelExitDeleteThread(0);
}
}  // namespace

// Called once from port_vita_game_entry after env.txt is read.
extern "C" void port_pgo_start(void) {
    if (g_started.exchange(1)) return;
    sceIoMkdir(PGO_DIR, 0777);
    CleanOld();
    const char* e = std::getenv("GXV_PGO_DUMP_S");
    port_log_summary("pgo: INSTRUMENTED build (-fprofile-generate): %u functions, %u counters, a raw profile of %u bytes; dump every %s s, marker " PGO_MARKER
                     ", at exit; files " PGO_DIR "/default-<n>.profraw and " PGO_FLAT,
                     static_cast<unsigned>(vita_pgo_num_functions()), static_cast<unsigned>(vita_pgo_num_counters()), static_cast<unsigned>(vita_pgo_size()),
                     e && *e ? e : "120");
    // 191 = the lowest user priority; no core is asked for (it runs where the game leaves room, mostly core 3 or the audio core)
    const SceUID t = sceKernelCreateThread("smg_pgodump", DumpThread, 191, 0x4000, 0, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
    else port_log_summary("pgo: cannot start the dump thread (0x%08X): only the exit dump will be written", static_cast<unsigned>(t));
}

// The last snapshot, on the thread that ends the process (once).
extern "C" void port_pgo_at_exit(const char* why) {
    if (!g_started.load() || g_exitDone.exchange(1)) return;
    Dump(why);
    port_log_flush();
}
