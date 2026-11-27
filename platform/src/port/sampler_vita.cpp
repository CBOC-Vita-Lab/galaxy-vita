// sampler30 (16 Nov 2026): the opt-in on-device sampling profiler (platform/include/port/sampler_vita.h has the switches).
//
// A sampler thread wakes about every GXV_SAMPLER_US, and for each target thread: reads its state (sceKernelGetThreadInfo); a thread that is not running
// is counted by its wait type; a running one is suspended (sceKernelSuspendThreadForVM), its registers are read (sceKernelGetThreadContextForVM),
// it is resumed (sceKernelResumeThreadForVM), and the pc and lr are stored in a fixed ring (port/sampler_ring.h). Between the suspend and the resume
// the sampler makes no other call: no log, no allocation, no lock.
//
// The three ForVM calls are exported to applications (SceThreadmgr 0x1FF5960D / 0x7EB55DAC, SceLibKernel 0x22C9595E), but the kernel lets only a
// process with the virtual-machine capability use them: CapUnlocker (kernel plug-in, which the owner has for the fourth core) grants it. Without it the
// calls are refused: one log line, no sampling, and the game is untouched.
// The register layout of the context buffer is undocumented, so at start the sampler finds it on a thread spinning in a known function (calibration).
//
// Credit: the idea (the ForVM calls, calibrating the layout on a spinning thread, wait types instead of a pc for a thread that is not running) is from
// NFS Underground 2 Vita, src/platform/sampler.cpp (https://github.com/danos-x5/NFSUG2_VITA, MIT License). This file is a new implementation and copies no
// code from it; the notice is kept anyway:
//   Copyright (c) the NFSUG2_VITA authors. Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify,
//   merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
//   subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
//
// Desktop: a stub that logs "sampler: Vita only".
#include "port/sampler_vita.h"
#include "port/sampler_ring.h"
#include "port/sampler_hist.h"
#include "port/diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__vita__)

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <time.h>
#include <atomic>

extern "C" {
// Not in the SDK headers. SceThreadmgr / SceLibKernel exports; refused at run time without CapUnlocker.
int sceKernelSuspendThreadForVM(SceUID thid);
int sceKernelResumeThreadForVM(SceUID thid);
int sceKernelGetThreadContextForVM(SceUID thid, void* cpuRegisters, void* vfpRegisters);
int port_thread_uids_by_name(const char* name, int* uids, int max) __attribute__((weak));   // vita/runtime/memory.cpp
}

namespace {
using namespace port::sampler;

enum State { kOff = 0, kStarting, kWaiting, kSampling, kDone, kFailed };
std::atomic<int> g_state{kOff};
std::atomic<bool> g_stop{false};

// A stage event from the game thread to the sampler thread: a serial (counts the events) and the stage name under a sequence lock.
struct Mailbox {
    std::atomic<uint32_t> seq{0}, serial{0};
    char name[64] = {};
};
Mailbox g_loading, g_playing;

void Post(Mailbox& m, const char* s) {
    const uint32_t q = m.seq.load(std::memory_order_relaxed);
    m.seq.store(q + 1, std::memory_order_release);
    snprintf(m.name, sizeof m.name, "%s", s ? s : "");
    m.seq.store(q + 2, std::memory_order_release);
    m.serial.fetch_add(1, std::memory_order_release);
}
void ReadName(Mailbox& m, char* out, size_t cap) {
    out[0] = 0;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t q1 = m.seq.load(std::memory_order_acquire);
        if (q1 & 1u) continue;
        snprintf(out, cap, "%s", m.name);
        if (m.seq.load(std::memory_order_acquire) == q1) return;
    }
}

// ---- settings (read once, at boot) ----
struct Config {
    ThreadSel sel;
    uint32_t periodUs = kDefaultPeriodUs, seconds = 0, delaySeconds = 0, ringCap = 131072, dumpSecs = 20;   // dumpSecs: GXV_SAMPLER_SECS, a sampler-<n>.bin every so often
    char stage[48] = {};
};
Config g_cfg;

// ---- the calibration ----
constexpr uint32_t kCtxWords = 128;
alignas(8) uint32_t g_cpu[kCtxWords];
alignas(8) uint32_t g_vfp[kCtxWords];
uint32_t g_cpuBytes = 0, g_vfpBytes = 0;
int g_pcWord = -1, g_lrWord = -1;

std::atomic<bool> g_calSpin{true};
std::atomic<SceUID> g_calTid{-1};
__attribute__((noinline)) void CalSpin() {
    while (g_calSpin.load(std::memory_order_relaxed)) { __asm__ volatile("" ::: "memory"); }
}
__attribute__((noinline)) int CalEntry(SceSize, void*) {
    g_calTid.store(sceKernelGetThreadId(), std::memory_order_relaxed);
    CalSpin();
    __asm__ volatile("" ::: "memory");   // (a call, not a jump: lr in CalSpin points into this function)
    return 0;
}

bool Within(uint32_t word, uint32_t base, uint32_t size) { return ((word & ~1u) - base) < size; }

int GetContext(SceUID tid) {
    g_cpu[0] = g_cpuBytes;
    if (g_vfpBytes) g_vfp[0] = g_vfpBytes;
    return sceKernelGetThreadContextForVM(tid, g_cpu, g_vfpBytes ? g_vfp : nullptr);
}

// Finds the buffer sizes the context call accepts (the first word of each buffer is its size). The last error is returned when none works.
int FindSizes(SceUID tid) {
    int r = -1;
    for (uint32_t vfp : {0u, 268u})
        for (uint32_t cpu = 4; cpu <= kCtxWords * 4; cpu += 4) {
            g_cpuBytes = cpu;
            g_vfpBytes = vfp;
            r = GetContext(tid);
            if (r >= 0) return r;
        }
    return r;
}

// true: the layout is known (g_pcWord, g_lrWord). false: it logged why not (needs CapUnlocker, or the calibration failed).
bool Calibrate() {
    SceUID tid = sceKernelCreateThread("smg_samplecal", CalEntry, 0x10000100, 16 * 1024, 0, 0x20000 /* core 1 */, nullptr);
    if (tid < 0 || sceKernelStartThread(tid, 0, nullptr) < 0) {
        port_log_summary("sampler: calibration thread could not start (0x%08x); not sampling", static_cast<unsigned>(tid));
        if (tid >= 0) sceKernelDeleteThread(tid);
        return false;
    }
    for (int i = 0; i < 50 && g_calTid.load(std::memory_order_relaxed) < 0; ++i) sceKernelDelayThread(2000);
    sceKernelDelayThread(5000);
    const SceUID spinner = g_calTid.load(std::memory_order_relaxed);
    const uint32_t spin = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&CalSpin)) & ~1u;
    const uint32_t entry = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&CalEntry)) & ~1u;
    bool ok = false;
    const int s0 = spinner >= 0 ? sceKernelSuspendThreadForVM(spinner) : -1;
    if (s0 < 0) {
        port_log_summary("sampler: needs CapUnlocker (the calls were refused: 0x%08x); not sampling", static_cast<unsigned>(s0));
    } else {
        const int r = FindSizes(spinner);
        sceKernelResumeThreadForVM(spinner);
        if (r < 0) {
            port_log_summary("sampler: calibration failed: sceKernelGetThreadContextForVM accepted no buffer size (last 0x%08x); not sampling", static_cast<unsigned>(r));
        } else {
            // Five readings of the spinning thread: a word counts as the pc only if it is in CalSpin every time (and as the lr: in CalEntry).
            uint32_t seenPc = ~0u, seenLr = ~0u;   // (bit i = word i matched every reading so far; words 1..31 checked)
            int got = 0;
            for (int k = 0; k < 5; ++k) {
                if (sceKernelSuspendThreadForVM(spinner) < 0) break;
                const int rr = GetContext(spinner);
                sceKernelResumeThreadForVM(spinner);
                if (rr < 0) break;
                uint32_t pcMask = 0, lrMask = 0;
                for (uint32_t i = 1; i < g_cpuBytes / 4 && i < 32; ++i) {
                    if (Within(g_cpu[i], spin, 64)) pcMask |= 1u << i;
                    if (Within(g_cpu[i], entry, 256)) lrMask |= 1u << i;
                }
                seenPc &= pcMask; seenLr &= lrMask;
                ++got;
                sceKernelDelayThread(1500);
            }
            if (got == 5 && seenPc) {
                // What one reading costs (get-info-free: suspend + context + resume on the spinner), for the log: the game thread is stopped about this long per sample.
                uint64_t sum = 0, worst = 0;
                int n = 0;
                for (int k = 0; k < 200; ++k) {
                    const uint64_t a = sceKernelGetProcessTimeWide();
                    if (sceKernelSuspendThreadForVM(spinner) < 0) break;
                    const int rr = GetContext(spinner);
                    sceKernelResumeThreadForVM(spinner);
                    const uint64_t d = sceKernelGetProcessTimeWide() - a;
                    if (rr < 0) break;
                    sum += d; ++n;
                    if (d > worst) worst = d;
                    sceKernelDelayThread(200);
                }
                if (n) port_log_summary("sampler overhead (calibration): suspend+context+resume of an idle spinner: avg %u us, worst %u us over %d readings: at %u us period about %.1f%% of the sampled thread", static_cast<unsigned>(sum / n), static_cast<unsigned>(worst), n, g_cfg.periodUs, 100.0 * (double(sum) / n) / g_cfg.periodUs);
                g_pcWord = __builtin_ctz(seenPc);
                seenLr &= ~(1u << g_pcWord);
                g_lrWord = seenLr ? __builtin_ctz(seenLr) : -1;
                ok = true;
                port_log_summary("sampler: calibrated (ForVM calls work, so CapUnlocker is loaded): context cpu %u bytes, vfp %u bytes, pc word %d, lr word %d%s",
                                 g_cpuBytes, g_vfpBytes, g_pcWord, g_lrWord, g_lrWord < 0 ? " (lr not found: callers will be missing)" : "");
            } else {
                port_log_summary("sampler: calibration failed: no context word held the spinning thread's pc in 5 readings (%d read, cpu %u bytes, vfp %u bytes); not sampling",
                                 got, g_cpuBytes, g_vfpBytes);
            }
        }
    }
    g_calSpin.store(false, std::memory_order_relaxed);
    SceUInt timeout = 1000000;
    sceKernelWaitThreadEnd(spinner >= 0 ? spinner : tid, nullptr, &timeout);
    sceKernelDeleteThread(tid);
    return ok;
}

// ---- targets ----
Header g_hdr;                  // slots, names, stage, layout (sampler thread only)
SceUID g_slotUid[kMaxSlots];   // (0 = unused)

int SlotFor(SceUID uid, const char* name) {
    for (int i = 0; i < kMaxSlots; ++i) if (g_hdr.slots[i].used && static_cast<SceUID>(g_hdr.slots[i].uid) == uid) return i;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (g_hdr.slots[i].used) continue;
        g_hdr.slots[i].used = true;
        g_hdr.slots[i].uid = static_cast<uint32_t>(uid);
        snprintf(g_hdr.slots[i].name, sizeof g_hdr.slots[i].name, "%s", name);
        g_slotUid[i] = uid;
        return i;
    }
    return -1;
}

void AddRegistered(const char* name, const char* label, const SceUID self) {
    if (!port_thread_uids_by_name) return;
    int ids[8];
    const int n = port_thread_uids_by_name(name, ids, 8);
    for (int i = 0; i < n; ++i) if (ids[i] != self) SlotFor(ids[i], label);
}

// Kernel-name match over the thread table (700 cheap calls: at the window's start and every few seconds, not every sample).
void AddKernelNamed(const char* name, const SceUID self) {
    SceKernelThreadInfo info;
    int misses = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && misses < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++misses; continue; }
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++misses; continue; }
        misses = 0;
        if (uid != self && !strcmp(info.name, name)) SlotFor(uid, name);
    }
}

void ResolveTargets(bool scanKernel) {
    const SceUID self = sceKernelGetThreadId();
    const ThreadSel& t = g_cfg.sel;
    if (t.game) AddRegistered("game-main", "game", self);
    if (t.guest) AddRegistered("guest", "guest", self);
    if (t.render) AddRegistered("vgx-render", "render", self);
    if (t.helper) {
        AddRegistered("vgx-player", "vgx-player", self);
        AddRegistered("vgx-finish", "vgx-finish", self);
        AddRegistered("vgx-fe-b", "vgx-fe-b", self);
        if (scanKernel) AddKernelNamed("smg_par", self);
    }
    for (int i = 0; i < t.nExtra; ++i) {
        AddRegistered(t.extra[i], t.extra[i], self);
        if (scanKernel) AddKernelNamed(t.extra[i], self);
    }
}

// ---- the sampling ----
Ring g_ring;
uint32_t g_consecutiveFails = 0;

// ---- the periodic binary dump (samp57) ----
constexpr uint32_t kHistCap = 32768;   // entries (512 KiB), a power of two
Hist g_hist;
struct SlotHold { uint32_t us = 0, stops = 0, maxUs = 0, fails = 0; };
SlotHold g_slotHold[kMaxSlots];        // this interval's stops and hold times
uint32_t g_runSamples = 0, g_waitSamples = 0;
uint32_t g_binIndex = 0, g_textBase = 0, g_textSize = 0;
uint64_t g_busyUs = 0;                 // this interval: time the sampler thread spent in its sampling passes (not asleep)

// The running module's code segment (the one holding this function), for the file header.
void FindText() {
    const uint32_t me = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&FindText));
    SceUID ids[64];
    SceSize n = 64;
    if (sceKernelGetModuleList(0xFF, ids, &n) < 0) return;
    for (SceSize i = 0; i < n; ++i) {
        SceKernelModuleInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
        for (int s = 0; s < 4; ++s) {
            const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mi.segments[s].vaddr));
            if (mi.segments[s].memsz && me - base < mi.segments[s].memsz) { g_textBase = base; g_textSize = mi.segments[s].memsz; return; }
        }
    }
}

// Writes sampler-<n>.bin from the histogram and the interval's counters, logs the summary and overhead lines, and starts the next interval.
void DumpBin(uint64_t openedUs, uint64_t intervalStartUs, bool last) {
    const uint64_t now = sceKernelGetProcessTimeWide();
    BinHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "GXVSAMP1", 8);
    h.version = 1;
    h.periodUs = g_cfg.periodUs;
    h.intervalMs = static_cast<uint32_t>((now - intervalStartUs) / 1000);
    h.windowMs = static_cast<uint32_t>((now - openedUs) / 1000);
    h.textBase = g_textBase; h.textSize = g_textSize;
    h.anchorAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&port_sampler_boot)) & ~1u;
    h.runSamples = g_runSamples; h.waitSamples = g_waitSamples;
    h.pcWord = static_cast<uint32_t>(g_pcWord); h.lrWord = static_cast<uint32_t>(g_lrWord);
    snprintf(h.stage, sizeof h.stage, "%s", g_hdr.stage);
    snprintf(h.anchorName, sizeof h.anchorName, "port_sampler_boot");
    uint32_t failed = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        BinSlot& b = h.slots[i];
        b.used = g_hdr.slots[i].used; b.uid = g_hdr.slots[i].uid;
        snprintf(b.name, sizeof b.name, "%s", g_hdr.slots[i].name);
        b.stops = g_slotHold[i].stops; b.holdUs = g_slotHold[i].us; b.holdMaxUs = g_slotHold[i].maxUs; b.fails = g_slotHold[i].fails;
        failed += g_slotHold[i].fails;
    }
    h.failed = failed;
    char path[64] = "";
    bool wrote = false;
    for (int tries = 0; tries < 1000 && !wrote; ++tries) {   // (the first free number: a new run does not overwrite the last one's files)
        snprintf(path, sizeof path, "ux0:data/smg/sampler-%u.bin", ++g_binIndex);
        if (FILE* t = fopen(path, "rb")) { fclose(t); continue; }
        h.index = g_binIndex;
        if (FILE* f = fopen(path, "wb")) { wrote = WriteBin(f, h, g_hist); wrote = (fclose(f) == 0) && wrote; }
        break;
    }
    const uint64_t spanUs = now - intervalStartUs ? now - intervalStartUs : 1;
    char line[600];
    int len = snprintf(line, sizeof line, "sampler bin #%u%s: %s, %u ms, stage %s, %u run + %u wait samples, %u distinct, %u dropped, %u failed, text base 0x%08x anchor 0x%08x",
                       h.index, last ? " (last)" : "", wrote ? path : "NOT WRITTEN", h.intervalMs, g_hdr.stage, g_runSamples, g_waitSamples, g_hist.Used(), g_hist.Dropped(), failed, h.textBase, h.anchorAddr);
    (void)len;
    port_log_summary("%s", line);
    // The overhead of the interval, per thread: the thread is stopped from before the suspend to after the resume; the sampler thread is busy its pass times.
    char ov[700];
    int o = snprintf(ov, sizeof ov, "sampler overhead: sampler thread busy %.1f%% of %u ms;", 100.0 * double(g_busyUs) / double(spanUs), h.intervalMs);
    for (int i = 0; i < kMaxSlots && o < static_cast<int>(sizeof ov) - 120; ++i) {
        if (!g_hdr.slots[i].used || !g_slotHold[i].stops) continue;
        o += snprintf(ov + o, sizeof ov - o, " %s: %u stops, avg %u us, max %u us, stopped %.2f%% of the time;", g_hdr.slots[i].name, g_slotHold[i].stops,
                      g_slotHold[i].us / g_slotHold[i].stops, g_slotHold[i].maxUs, 100.0 * double(g_slotHold[i].us) / double(spanUs));
    }
    port_log_summary("%s", ov);
    g_hist.Clear();
    for (int i = 0; i < kMaxSlots; ++i) g_slotHold[i] = SlotHold();
    g_runSamples = g_waitSamples = 0;
    g_busyUs = 0;
}

// One reading of one thread. The suspend-to-resume stretch makes no other call.
void SampleOne(int slot, SceUID uid) {
    SceKernelThreadInfo info;
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(uid, &info) < 0) { ++g_hdr.slots[slot].fails; ++g_consecutiveFails; return; }
    if (info.status & (SCE_THREAD_DORMANT | SCE_THREAD_DEAD | SCE_THREAD_DELETED | SCE_THREAD_STANDBY)) return;   // (not started yet, or gone)
    if (info.status & ~(SCE_THREAD_RUNNING | SCE_THREAD_READY)) {
        const uint32_t tag = MakeTag(static_cast<uint32_t>(slot), kWait, info.waitType);
        g_ring.Push(static_cast<uint32_t>(info.waitId), 0, tag);
        g_hist.Add(static_cast<uint32_t>(info.waitId), 0, tag);
        ++g_waitSamples;
        return;
    }
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    if (sceKernelSuspendThreadForVM(uid) < 0) { ++g_hdr.slots[slot].fails; ++g_consecutiveFails; return; }
    const int r = GetContext(uid);
    int resume = sceKernelResumeThreadForVM(uid);
    for (int tries = 0; resume < 0 && tries < 5; ++tries) resume = sceKernelResumeThreadForVM(uid);   // (a thread left suspended would stop the game)
    const uint32_t hold = static_cast<uint32_t>(sceKernelGetProcessTimeWide() - t0);   // (before the suspend to after the resume: an upper bound of the stop)
    g_slotHold[slot].us += hold; ++g_slotHold[slot].stops;
    if (hold > g_slotHold[slot].maxUs) g_slotHold[slot].maxUs = hold;
    if (r < 0) { ++g_hdr.slots[slot].fails; ++g_slotHold[slot].fails; ++g_consecutiveFails; return; }
    g_consecutiveFails = 0;
    const uint32_t pc = g_cpu[g_pcWord], lr = g_lrWord >= 0 ? g_cpu[g_lrWord] : 0, tag = MakeTag(static_cast<uint32_t>(slot), kRun, 0);
    g_ring.Push(pc, lr, tag);
    g_hist.Add(pc, lr, tag);
    ++g_runSamples;
}

void WriteOutput(const char* reason, uint64_t openedUs) {
    g_hdr.windowMs = static_cast<uint32_t>((sceKernelGetProcessTimeWide() - openedUs) / 1000);
    snprintf(g_hdr.reason, sizeof g_hdr.reason, "%s", reason);
    snprintf(g_hdr.anchorName, sizeof g_hdr.anchorName, "port_sampler_boot");
    g_hdr.anchorAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&port_sampler_boot)) & ~1u;
    g_hdr.pcWord = g_pcWord;
    g_hdr.lrWord = g_lrWord;
    g_hdr.periodUs = g_cfg.periodUs;
    char path[96];
    const time_t now = time(nullptr);
    struct tm tmv;
    if (now > 1577836800 && gmtime_r(&now, &tmv))
        snprintf(path, sizeof path, "ux0:data/smg/sampler-%04d%02d%02d-%02d%02d%02d.txt", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    else
        snprintf(path, sizeof path, "ux0:data/smg/sampler-t%06u.txt", static_cast<unsigned>(sceKernelGetProcessTimeWide() / 1000000));
    char line[900];
    FILE* f = fopen(path, "w");
    bool wrote = false;
    if (f) {
        // (the summary is made first: the report sorts the ring in place)
        FormatSummary(line, sizeof line, g_hdr, g_ring, path);
        WriteReport(f, g_hdr, g_ring);
        wrote = fclose(f) == 0;
    } else {
        FormatSummary(line, sizeof line, g_hdr, g_ring, "(not written: cannot open)");
    }
    (void)wrote;
    port_log_summary("%s", line);
}

// The window: waits for a stage to play (and match), optionally a delay, then samples until the seconds are up or the next stage starts to load.
int SamplerMain(SceSize, void*) {
    if (!Calibrate()) { g_state.store(kFailed); return 0; }
    // The ring: allocated once, here; nothing allocates while it samples.
    Rec* storage = static_cast<Rec*>(malloc(sizeof(Rec) * g_cfg.ringCap));
    if (!storage) {
        port_log_summary("sampler: no memory for the ring (%u samples); not sampling", g_cfg.ringCap);
        g_state.store(kFailed);
        return 0;
    }
    g_ring.Init(storage, g_cfg.ringCap);
    BinEntry* histStore = static_cast<BinEntry*>(malloc(sizeof(BinEntry) * kHistCap));
    if (!histStore) {
        port_log_summary("sampler: no memory for the histogram (%u KiB); not sampling", static_cast<unsigned>(sizeof(BinEntry) * kHistCap / 1024));
        free(storage);
        g_state.store(kFailed);
        return 0;
    }
    g_hist.Init(histStore, kHistCap);
    FindText();
    g_state.store(kWaiting);
    port_log_summary("sampler: armed: ring %u samples (%u KiB), period %u us, window %s%u s%s, stage filter '%s', delay %u s",
                     g_cfg.ringCap, g_cfg.ringCap * static_cast<uint32_t>(sizeof(Rec)) / 1024, g_cfg.periodUs, g_cfg.seconds ? "" : "until the stage changes, at most ",
                     g_cfg.seconds ? g_cfg.seconds : kDefaultMaxSec, g_cfg.seconds ? " (or until the stage changes)" : "", g_cfg.stage, g_cfg.delaySeconds);
    uint32_t seenPlay = g_playing.serial.load(std::memory_order_acquire), seenLoad = g_loading.serial.load(std::memory_order_acquire);
    char stage[64] = {};
    uint64_t openAt = 0;       // (a stage that matched: when its window opens; 0 = none pending)
    bool done = false;
    while (!done && !g_stop.load(std::memory_order_relaxed)) {
        sceKernelDelayThread(10000);
        const uint32_t load = g_loading.serial.load(std::memory_order_acquire);
        if (load != seenLoad) { seenLoad = load; openAt = 0; }   // (the stage that was about to open is gone)
        const uint32_t play = g_playing.serial.load(std::memory_order_acquire);
        if (play != seenPlay) {
            seenPlay = play;
            ReadName(g_playing, stage, sizeof stage);
            if (StageMatches(g_cfg.stage, stage)) openAt = sceKernelGetProcessTimeWide() + static_cast<uint64_t>(g_cfg.delaySeconds) * 1000000u;
        }
        if (!openAt || sceKernelGetProcessTimeWide() < openAt) continue;

        // ---- the window ----
        openAt = 0;
        snprintf(g_hdr.stage, sizeof g_hdr.stage, "%s", stage);
        g_ring.Clear();
        g_hist.Clear();
        for (int i = 0; i < kMaxSlots; ++i) g_slotHold[i] = SlotHold();
        g_runSamples = g_waitSamples = 0; g_busyUs = 0;
        ResolveTargets(true);
        port_log_summary("sampler: window opens in stage %s (%d thread(s))", stage[0] ? stage : "?", [] { int n = 0; for (int i = 0; i < kMaxSlots; ++i) n += g_hdr.slots[i].used; return n; }());
        g_state.store(kSampling);
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        const uint64_t maxUs = static_cast<uint64_t>(g_cfg.seconds ? g_cfg.seconds : kDefaultMaxSec) * 1000000u;
        const char* reason = "seconds";
        uint64_t next = t0, lastResolve = t0, lastDump = t0;
        g_consecutiveFails = 0;
        for (;;) {
            if (g_stop.load(std::memory_order_relaxed)) { reason = "shutdown"; break; }
            if (g_loading.serial.load(std::memory_order_acquire) != seenLoad) { seenLoad = g_loading.serial.load(std::memory_order_acquire); reason = "stage-change"; break; }
            uint64_t now = sceKernelGetProcessTimeWide();
            if (now - t0 >= maxUs) break;
            if (now - lastResolve >= 4000000u) { ResolveTargets(true); lastResolve = now; }
            else if (now - lastResolve >= 250000u) { ResolveTargets(false); lastResolve = now; }
            const uint64_t passStart = now;
            for (int i = 0; i < kMaxSlots; ++i) if (g_hdr.slots[i].used) SampleOne(i, static_cast<SceUID>(g_hdr.slots[i].uid));
            g_busyUs += sceKernelGetProcessTimeWide() - passStart;
            if (g_cfg.dumpSecs && sceKernelGetProcessTimeWide() - lastDump >= static_cast<uint64_t>(g_cfg.dumpSecs) * 1000000u) {
                DumpBin(t0, lastDump, false);
                lastDump = sceKernelGetProcessTimeWide();
                next = lastDump;   // (the dump took a while: no burst to catch up)
            }
            if (g_consecutiveFails >= 200) { reason = "failures"; break; }
            next += g_cfg.periodUs;
            now = sceKernelGetProcessTimeWide();
            uint64_t wait = next > now ? next - now : 0;
            if (wait > g_cfg.periodUs) wait = g_cfg.periodUs;
            if (next + 20 * static_cast<uint64_t>(g_cfg.periodUs) < now) next = now;   // (far behind: do not burst to catch up)
            sceKernelDelayThread(static_cast<SceUInt>(wait < 50 ? 50 : wait));
        }
        if (g_cfg.dumpSecs) DumpBin(t0, lastDump, true);   // (GXV_SAMPLER_SECS=0: no dumps, not even the last one)
        WriteOutput(reason, t0);
        done = true;   // (one window a run)
    }
    if (!done) {   // (stopped at exit while waiting for a stage: nothing to write)
        port_log_summary("sampler: stopped before a window opened");
    }
    free(storage);
    free(histStore);
    g_state.store(kDone);
    return 0;
}

void AtExit() {
    // A window in progress at exit is written out: ask the thread to stop and give it up to 3 s.
    if (g_state.load() != kSampling && g_state.load() != kWaiting) return;
    g_stop.store(true);
    for (int i = 0; i < 300 && g_state.load() != kDone; ++i) sceKernelDelayThread(10000);
}

}  // namespace

extern "C" void port_sampler_boot(void) {
    const char* on = getenv("GXV_SAMPLER");   // (the one getenv of a run without the sampler)
    if (!on || *on != '1') return;
    Config& c = g_cfg;
    ParseThreads(getenv("GXV_SAMPLER_THREADS"), c.sel);
    c.periodUs = ParsePeriod(getenv("GXV_SAMPLER_US"));
    c.seconds = ParseSeconds(getenv("GXV_SAMPLER_SEC"));
    c.dumpSecs = ParseDumpSecs(getenv("GXV_SAMPLER_SECS"));   // (a sampler-<n>.bin every N seconds, default 20; GXV_SAMPLER_SECS=0: no dumps)
    c.delaySeconds = ParseSeconds(getenv("GXV_SAMPLER_DELAY_SEC"));
    // (every scene goes through the stage hooks, the title and the menus too: the default filter "Galaxy" means the playable galaxies; "*" = any scene)
    snprintf(c.stage, sizeof c.stage, "%s", "Galaxy");
    if (const char* st = getenv("GXV_SAMPLER_STAGE")) {
        if (!strcmp(st, "*")) c.stage[0] = 0;
        else if (*st) snprintf(c.stage, sizeof c.stage, "%s", st);
    }
    if (const char* rg = getenv("GXV_SAMPLER_RING")) {
        const long v = atol(rg);
        c.ringCap = v < 4096 ? 4096u : v > 1048576 ? 1048576u : static_cast<uint32_t>(v);
    }
    g_state.store(kStarting);
    SceUID tid = sceKernelCreateThread("smg_sampler", SamplerMain, 0x60, 64 * 1024, 0, 0x60000 /* cores 1 and 2 */, nullptr);
    if (tid < 0 || sceKernelStartThread(tid, 0, nullptr) < 0) {
        port_log_summary("sampler: the sampler thread could not start (0x%08x); not sampling", static_cast<unsigned>(tid));
        g_state.store(kFailed);
        return;
    }
    atexit(AtExit);
    port_log_summary("sampler: GXV_SAMPLER=1: calibrating");
}

extern "C" void port_sampler_stage_loading(const char* stage) {
    const int st = g_state.load(std::memory_order_relaxed);
    if (st != kWaiting && st != kSampling) return;   // (off, still calibrating, failed or done: nothing)
    Post(g_loading, stage);
}

extern "C" void port_sampler_stage_playing(const char* stage) {
    const int st = g_state.load(std::memory_order_relaxed);
    if (st != kWaiting && st != kSampling) return;
    Post(g_playing, stage);
}

#else  // ---- desktop and the other hosts

extern "C" void port_sampler_boot(void) {
    const char* on = getenv("GXV_SAMPLER");
    if (on && *on == '1') port_log_summary("sampler: Vita only");
}
extern "C" void port_sampler_stage_loading(const char*) {}
extern "C" void port_sampler_stage_playing(const char*) {}

#endif
