// cook24: the "load-split:" census. What a boot or a stage load spends, by cause, from counters that are always on.
//
// Every counter here is a relaxed 64-bit atomic bumped once per disc read / per scene load: no allocation, no lock, no file access.
// The lines are printed to the basic log (log.txt):
//   load-split: boot ...            once, when the title is up (platform/vgx/vita/vgx_host_galaxy_vita.cpp)
//   load-split: scene build ...     once per stage load, when the game's loading thread has built the scene (platform/src/port/perf.cpp)
//   load-split: closed screen ...   once per stage load, when the closed screen is released (platform/vgx/src/vgx_backend_gl.cpp)
// GXV_LOAD_SPLIT=0 prints nothing (the counters stay: they cost less than a log call).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The disc's folders the game reads from. Indexes into PortDiscSplit's arrays.
enum {
    PORT_DISC_OBJECT = 0,  // ObjectData: the actors' archives (the stationed ones at boot, the stage's own at each load)
    PORT_DISC_STAGE,       // StageData: zones, scenarios, collision
    PORT_DISC_LAYOUT,      // LayoutData: 2D layouts, fonts
    PORT_DISC_AUDIO,       // AudioRes: wave banks, sequences, streams (the title's music)
    PORT_DISC_OTHER,       // everything else (movies, the system files, ...)
    PORT_DISC_CLASSES
};

// Cumulative since the start of the process. Two snapshots subtracted give a window.
typedef struct PortDiscSplit {
    uint64_t reads[PORT_DISC_CLASSES];       // reads served (sync + async)
    uint64_t bytes[PORT_DISC_CLASSES];       // bytes handed to the game
    uint64_t workerNs[PORT_DISC_CLASSES];    // the drive worker's time per read: open + pread / memcpy from a read-ahead window
    uint64_t blockedNs[PORT_DISC_CLASSES];   // wall time guest threads sat in DVDReadPrio (sync reads), from the call to the return
    uint64_t probes;                         // sync reads of 32 bytes or less at offset 0: the game's "is this file compressed?" look
    uint64_t asyncReads;                     // DVDReadAsyncPrio calls (nobody waits in the call)
    uint64_t queueNs;                        // submit to worker start, summed (the worker busy with the read before)
    uint64_t opens, openNs;                  // open() calls the worker made (descriptor cache misses) and their time
    uint64_t stats, statNs;                  // first-use stat() calls (the sizes cache did not know the file) and their time
    uint64_t readAheadHits;                  // reads answered from the worker's read-ahead window (no card access)
    uint64_t decodes, decodeNs, decodeBytes; // Yaz0 / Yay0 archives expanded at run time (0 with a cooked install: the files are stored decompressed)
} PortDiscSplit;

// dvd.cpp
void port_load_split_disc(PortDiscSplit* out);
// The slowest read (worker time) since the last call of this function: name (path under the data root), ms. Returns 0 when none.
int port_load_split_slowest(char* name, size_t size, double* ms);
// Called by the Yaz0 / Yay0 decoders (decomp/src/JSystem/JKernel): one archive expanded.
void port_load_note_decomp(uint32_t bytes, int64_t ns);

// MainLoopFramework.cpp: waits the loading thread (any thread but the game thread's) made for the front end's finish (GXV_DRAWDONE_OTHER).
void port_load_split_finish_waits(uint64_t* count, uint64_t* ns);

// (the Vita renderer) shader files: the package's / user pack's RAM lookups that answered, and those that found nothing (the file on the card
// is read, or the shader compiled). Cumulative.
void port_load_split_shader_files(uint64_t* ramHits, uint64_t* userHits, uint64_t* misses);

// (disc26) the "io:" line: the disc, the host-job thread and the game thread's waits while a stage PLAYS (from the end of its scene build to the
// start of the next one), by class. One line when the play window ends and, with a long stage, one every GXV_IO_LINE_S (60) seconds. Always on
// (GXV_IO_LINE=0 mutes it; the counters are relaxed atomics). dvd.cpp.
void port_io_play_begin(const char* stage, uint32_t key);  // perf.cpp: port_scene_load_end
void port_io_play_end(void);                               // perf.cpp: port_scene_load_begin (the previous stage's window closes with its line)
void port_io_tick(void);                                   // perf.cpp: once a turn; prints the running line when the window is long
void port_io_note_wait(int64_t ns);                        // FileHolder.cpp: the calling guest thread waited ns for a requested file or archive
// (disc26, GXV_IO_PREFETCH_MB > 0) ask the drive worker to read a file into the prefetch cache while it is idle (path under the game's
// root as the game writes it, e.g. "/ObjectData/Foo.arc"). Returns 1 when queued.
int port_dvd_prefetch(const char* path);

// perf.cpp: "load-split:" lines on (GXV_LOAD_SPLIT, default 1).
int port_load_split_on(void);
// perf.cpp: milliseconds since the last scene build ended on the loading thread (-1 before the first).
double port_load_split_since_build_end_ms(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <stdio.h>
namespace port::loadsplit {
inline const char* ClassName(int c) {
    static const char* const n[PORT_DISC_CLASSES] = {"ObjectData", "StageData", "LayoutData", "AudioRes", "other"};
    return c >= 0 && c < PORT_DISC_CLASSES ? n[c] : "?";
}
// The window b - a as text: "<reads> reads (<probes> probes), <MiB>, blocked <ms>, drive <ms> [...], by folder: ...". Returns the length.
inline int DiscText(char* out, size_t size, const PortDiscSplit& a, const PortDiscSplit& b) {
    uint64_t reads = 0, bytes = 0, work = 0, blocked = 0;
    for (int c = 0; c < PORT_DISC_CLASSES; ++c) {
        reads += b.reads[c] - a.reads[c];
        bytes += b.bytes[c] - a.bytes[c];
        work += b.workerNs[c] - a.workerNs[c];
        blocked += b.blockedNs[c] - a.blockedNs[c];
    }
    int n = snprintf(out, size,
                     "disc %llu reads (%llu probes of 32 bytes, %llu async, %llu from the read-ahead window), %.1f MiB; callers blocked %.0f ms; drive worker %.0f ms "
                     "(%llu opens %.0f ms, %llu stats %.0f ms, queue wait %.0f ms); by folder (reads / MiB / drive ms / blocked ms):",
                     static_cast<unsigned long long>(reads), static_cast<unsigned long long>(b.probes - a.probes),
                     static_cast<unsigned long long>(b.asyncReads - a.asyncReads), static_cast<unsigned long long>(b.readAheadHits - a.readAheadHits),
                     static_cast<double>(bytes) / 1048576.0, static_cast<double>(blocked) / 1e6, static_cast<double>(work) / 1e6,
                     static_cast<unsigned long long>(b.opens - a.opens), static_cast<double>(b.openNs - a.openNs) / 1e6,
                     static_cast<unsigned long long>(b.stats - a.stats), static_cast<double>(b.statNs - a.statNs) / 1e6,
                     static_cast<double>(b.queueNs - a.queueNs) / 1e6);
    for (int c = 0; c < PORT_DISC_CLASSES && n > 0 && static_cast<size_t>(n) < size; ++c) {
        if (!(b.reads[c] - a.reads[c])) continue;
        n += snprintf(out + n, size - static_cast<size_t>(n), " %s %llu / %.1f / %.0f / %.0f;", ClassName(c),
                      static_cast<unsigned long long>(b.reads[c] - a.reads[c]), static_cast<double>(b.bytes[c] - a.bytes[c]) / 1048576.0,
                      static_cast<double>(b.workerNs[c] - a.workerNs[c]) / 1e6, static_cast<double>(b.blockedNs[c] - a.blockedNs[c]) / 1e6);
    }
    return n;
}
}  // namespace port::loadsplit
#endif
