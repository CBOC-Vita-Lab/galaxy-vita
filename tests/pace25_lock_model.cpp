// pace25: the 48.0 fps lock at L 5 (night60 Beach Bowl 03:50:32-44, Good Egg 03:56:40-57:20), on an event model of the whole pacing loop with the REAL
// LegacySkip (the skip rule, port/perf.cpp defaults: PETARI_FRAMESKIP 3, PIPE_DEPTH 2, PIPE_RUN 6, GXV_SKIP_MAXSTEP 2, GXV_STALL_FORGIVE 80) and the REAL PresentSync
// (GXV_LATENCY24 window 45, calm 450, regret 1200, optionally GXV_LATENCY24_FAST).  Per drawn turn: front end hands the picture over, the render thread
// (cpu, then waits for its display buffer: three buffers), the GPU (one picture at a time), sceGxmDisplayQueueAddEntry (blocks while two entries are pending:
// displayQueueMaxPendingCount 2, so it returns when the callback of the entry two back has returned), the display callback (in order, after the GPU finished and the
// previous callback returned; PresentSync::Target, wait for the vblank before the slot).  `pending` = pictures handed to the back end whose Execute has not
// returned (port_vgx_backend_pending: sFramesQueued - sFramesShown, CompleteJob after Execute).
// GXV_PENDING_HOLD25 (the fix): a picture whose Execute is blocked in AddEntry while the display callback sits in its hold wait is finished work, parked by the
// latency, not by the back end: it is not counted.  A MODEL: the lanes are the logs' numbers (front end 7.6 ms, render cpu 3.1 ms, GPU 15.3 ms, present blocked 11.4 ms).
//   g++ -std=c++17 -O1 -Iplatform/include tests/pace25_lock_model.cpp -o /tmp/pace25_lock && /tmp/pace25_lock
#include "port/frame_pace.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using port::pace::PresentSync;
using port::pace::LegacySkip;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static const double P = 16683.35;   // us per vblank

struct Cfg {
    double gpuMs = 15.4, feMs = 7.6, cpuMs = 3.1, preMs = 1.5, bufLagMs = 0, phaseMs = 0;
    double seconds = 60, roughS = 0, roughGpuMs = 30;
    unsigned nbuf = 3; bool guard26 = false, allow25 = false, hold25 = false, fast = false, forceL = false; unsigned initL = 5, maxL = 5, minL = 1;
    bool lat24 = true;
};
struct Out {
    unsigned pictures = 0, skippedBusy = 0, skippedLate = 0, lowers = 0, raises = 0, finalL = 0, late = 0;
    double fps = 0, fpsLast30 = 0, meanBlockedMs = 0, meanSemaMs = 0, meanLat = 0; unsigned lateAtL = 0; unsigned held[5] = {}; unsigned natural[8] = {};
};
static Out Run(const Cfg& c) {
    Out o;
    PresentSync sync;
    sync.Configure(true, c.forceL ? c.initL : c.minL, c.maxL, c.initL, 4);
    if (c.lat24) {
        PresentSync::Latency24 l; l.on = true; l.window = 45; l.minKnown = 22; l.catchUp = false; l.regretPics = 1200; l.probeMax = 14400; l.calmBase = 450;
        if (c.fast) { l.holdCap = 1200; l.regretMul = 2; l.cleanPics = 180; }
        sync.ConfigureLatency24(l);
    }
    LegacySkip rule(3, 2, 6, nullptr, 2, 80 * 1000000ll);
    const uint32_t base = 1000;
    const unsigned turns = static_cast<unsigned>(c.seconds * 60);
    struct Job { double queued, E, cbRet, shown; };   // times in us; shown in vblanks
    std::vector<Job> jobs;
    double G = 0, lastE = 0, lastCbRet = 0, blockedSum = 0;
    std::vector<uint32_t> shownAt;   // vblank number per job
    std::vector<double> holdStart;   // when each job's callback began to hold (us), or -1
    std::vector<double> holdEnd;
    std::vector<double> addAt, parkA;   // when the job entered AddEntry; when it first waited on the display (buffer or swap)
    uint32_t lastShownV = 0; bool haveShownV = false; unsigned lastDrawn = 0; bool haveDrawn = false;
    unsigned drawnLast30 = 0, drawn = 0; double latSum = 0;
    for (unsigned i = 0; i < turns; ++i) {
        const double T = i * P + c.phaseMs * 1000;
        unsigned pending = 0;
        bool parked = false, holding = false;
        for (size_t k = jobs.size(); k-- > 0;) {
            if (jobs[k].E <= T) break;   // (Execute returns in order)
            if (jobs[k].queued > T) continue;
            ++pending;
            // the picture the render thread is executing, waiting for its display buffer or blocked in the swap: the display owns the wait
            if (parkA[k] <= T && T < jobs[k].E) parked = true;
        }
        for (size_t q = jobs.size() >= 4 ? jobs.size() - 4 : 0; q < jobs.size(); ++q) if (holdStart[q] >= 0 && holdStart[q] <= T && T < holdEnd[q]) holding = true;
        if (c.hold25 && parked && holding && pending > 0) --pending;
        if (c.allow25) { const unsigned a = sync.HoldAllowance(c.guard26); pending = pending > a ? pending - a : 0; }
        const LegacySkip::Verdict v = rule.Turn(static_cast<int64_t>(T * 1000.0), true, pending);
        if (v.code != 0) { if (v.gpuBusy) ++o.skippedBusy; else ++o.skippedLate; continue; }
        // a drawn turn
        const uint32_t stamp = base + i;
        Job j; j.queued = T + c.feMs * 1000;
        const size_t n = jobs.size();
        const double renderStart = std::max(j.queued, lastE);
        const double cpu1 = renderStart + c.preMs * 1000;
        double bufReady = 0;
        if (n + 1 >= c.nbuf) bufReady = (shownAt[n + 1 - c.nbuf] - base) * P + c.bufLagMs * 1000;
        const double sceneStart = std::max(cpu1, bufReady);
        const double renderDone = sceneStart + (c.cpuMs - c.preMs) * 1000;
        const double gpuStart = std::max(sceneStart, G);
        G = gpuStart + (T < c.roughS * 1e6 ? c.roughGpuMs : c.gpuMs) * 1000;
        double Ej = renderDone;
        if (n >= 2) Ej = std::max(renderDone, jobs[n - 2].cbRet);
        j.E = Ej; addAt.push_back(renderDone); parkA.push_back(sceneStart > cpu1 ? cpu1 : renderDone);
        blockedSum += Ej - renderDone;
        // the display callback
        const double cbStart = std::max({G, renderDone, lastCbRet});
        const uint32_t count = base + static_cast<uint32_t>(std::floor(cbStart / P));
        const uint32_t earliest = count + 1;
        const uint32_t target = sync.Target(earliest, true, static_cast<uint16_t>(stamp));
        sync.NoteBacklog(n > 0 && cbStart - lastCbRet < 1200);
        uint32_t shown = earliest; bool waited = false;
        double ret;
        if (target != earliest) { ret = (target - 1 - base) * P; shown = target; waited = true; holdStart.push_back(cbStart); holdEnd.push_back(ret); }
        else { ret = cbStart + 50; holdStart.push_back(-1); holdEnd.push_back(-1); }
        sync.Shown(shown, waited); latSum += static_cast<double>(shown - stamp);
        const unsigned nat = static_cast<uint16_t>(static_cast<uint16_t>(earliest) - static_cast<uint16_t>(stamp));
        ++o.natural[nat < 7 ? nat : 7];
        if (haveShownV) { const uint32_t h = shown - lastShownV; ++o.held[h < 4 ? h : 4]; }
        lastShownV = shown; haveShownV = true; (void)lastDrawn; (void)haveDrawn;
        j.cbRet = ret; j.shown = shown; jobs.push_back(j); shownAt.push_back(shown);
        lastE = Ej; lastCbRet = ret;
        ++drawn; if (i + 3600 >= turns) ++drawnLast30;
    }
    o.pictures = drawn; o.fps = drawn / c.seconds; o.fpsLast30 = drawnLast30 / (c.seconds >= 60 ? 60.0 : c.seconds);
    o.lowers = static_cast<unsigned>(sync.GetStats().lowers); o.raises = static_cast<unsigned>(sync.GetStats().raises);
    o.finalL = sync.Latency(); o.meanLat = drawn ? latSum / drawn : 0; o.lateAtL = static_cast<unsigned>(sync.GetStats().misses); o.late = static_cast<unsigned>(sync.GetStats().misses);
    o.meanBlockedMs = drawn ? blockedSum / drawn / 1000 : 0;
    return o;
}
struct Both { Out a, b; };
static Both Pair(const char* tag, Cfg c) {
    c.allow25 = false; const Out a = Run(c);
    c.allow25 = true;  const Out b = Run(c);
    std::printf("  %-34s no fix %5.1f fps (last 30 s %5.1f) L %u latency %.2f vb late %4u | fix %5.1f fps (last 30 s %5.1f) L %u latency %.2f vb late %4u\n", tag, a.fps, a.fpsLast30, a.finalL, a.meanLat, a.lateAtL, b.fps, b.fpsLast30, b.finalL, b.meanLat, b.lateAtL);
    return {a, b};
}
int main() {
    std::printf("L held at 5 (the 48.0 stretches), 3 buffers, GPU ms a picture:\n");
    for (double gpu : {12.0, 14.0, 15.5, 16.4}) for (double ph : {2.0, 6.0, 10.0}) {
        Cfg c; c.gpuMs = gpu; c.forceL = true; c.phaseMs = ph; c.seconds = 30;
        char t[64]; std::snprintf(t, sizeof t, "gpu %.1f phase %.0f ms", gpu, ph);
        const Both r = Pair(t, c);
        CHECK(r.a.fps > 47.5 && r.a.fps < 48.5);   // the lock, without the fix
        CHECK(r.b.fps > 59.5);                      // 15.5 ms pictures get 60 with it
        CHECK(r.b.meanLat <= r.a.meanLat + 0.01 && r.b.lateAtL == 0);   // no more latency, no late picture
    }
    std::printf("L adaptive (GXV_LATENCY24 as the console runs it), 120 s:\n");
    for (double gpu : {12.0, 15.5, 16.4, 18.0, 25.0, 33.0}) {
        Cfg c; c.gpuMs = gpu; c.phaseMs = 6; c.seconds = 120; c.initL = 3;
        char t[64]; std::snprintf(t, sizeof t, "gpu %.1f", gpu);
        const Both r = Pair(t, c);
        if (gpu >= 25.0) { CHECK(r.b.fps == r.a.fps && r.b.meanLat == r.a.meanLat); }   // a back end that is really behind: unchanged
        if (gpu <= 16.4) CHECK(r.b.fps > 59.0 && r.a.fps > 59.0);
        CHECK(r.b.fps >= r.a.fps - 0.01 && r.b.meanLat <= r.a.meanLat + 0.2);
        c.fast = true; std::snprintf(t, sizeof t, "gpu %.1f + FAST", gpu); Pair(t, c);
    }
    std::printf("a rough opening (GPU 30 ms for the first 15 s: L goes to 5), then GPU 15.5, 180 s; fps of the last 60 s:\n");
    for (double ph : {2.0, 6.0, 10.0}) {
        Cfg c; c.gpuMs = 15.5; c.phaseMs = ph; c.seconds = 180; c.initL = 3; c.roughS = 15;
        char t[64]; std::snprintf(t, sizeof t, "rough, phase %.0f ms", ph);
        Pair(t, c);
        c.fast = true; std::snprintf(t, sizeof t, "rough, phase %.0f ms + FAST", ph); Pair(t, c);
    }
    {   // the allowance itself: none at L <= 3, none after a late picture, L - 3 after 32 on-time ones
        PresentSync ps; ps.Configure(true, 1, 5, 5, 4);
        CHECK(ps.HoldAllowance() == 0);
        uint32_t vb = 100;
        for (int i = 0; i < 40; ++i) { const uint32_t e = vb + 4; const uint32_t t = ps.Target(e, true, static_cast<uint16_t>(vb - 0)); ps.Shown(t, true); vb += 1; }
        CHECK(ps.HoldAllowance() == ps.Latency() - 3);
        const uint32_t e = vb + 7; ps.Target(e, true, static_cast<uint16_t>(vb)); ps.Shown(e, false);   // 7 vblanks after its turn: late
        CHECK(ps.HoldAllowance() == 0);
        PresentSync lo; lo.Configure(true, 1, 3, 3, 4);
        for (int i = 0; i < 40; ++i) { const uint32_t t = lo.Target(100 + i + 2, true, static_cast<uint16_t>(100 + i)); lo.Shown(t, false); }
        CHECK(lo.HoldAllowance() == 0);
    }
    std::printf("pace26: GXV_PENDING_HOLD25=2 (guarded) against 1 and the control, GPU ms, L adaptive from 3 (120 s) and L held 5 (30 s):\n");
    for (double gpu : {12.0, 15.5, 16.4, 17.0, 18.0, 19.0, 20.0}) for (int held = 0; held < 2; ++held) for (double ph : {2.0, 10.0}) {
        Cfg c; c.gpuMs = gpu; c.phaseMs = ph; c.seconds = held ? 30 : 120; c.initL = held ? 5 : 3; c.forceL = held != 0;
        c.allow25 = false; const Out a = Run(c);
        c.allow25 = true; const Out b = Run(c);
        c.guard26 = true; const Out g = Run(c);
        std::printf("  gpu %4.1f %s phase %2.0f: control %5.1f fps late %3u | =1 %5.1f fps late %3u | =2 %5.1f fps late %3u latency %.2f\n", gpu, held ? "L5   " : "adapt", ph, a.fps, a.late, b.fps, b.late, g.fps, g.late, g.meanLat);
        if (gpu <= 15.5 && held) CHECK(g.fps > 58.0 || g.late > 3);
        if (gpu <= 12.0) CHECK(g.fps > 59.0 && g.late <= a.late + 1);
        if (gpu >= 17.0) CHECK(g.late <= a.late + 6 && g.fps >= a.fps - 0.01 && g.late * 4 < b.late);
    }
    std::printf("pace27: rough stretch of 30 ms pictures at 10-20 s while L is held 5 (a scene/load: gross lates with the allowance on), then GPU 15.5; fps of the last 60 s of 180 s, control / =1 / =2:\n");
    for (double ph : {2.0, 10.0}) for (double rs : {0.0, 20.0}) {
        Cfg c; c.gpuMs = 15.5; c.phaseMs = ph; c.seconds = 180; c.initL = 5; c.forceL = true; c.roughS = rs;
        c.allow25 = false; const Out a = Run(c);
        c.allow25 = true; const Out b = Run(c);
        c.guard26 = true; const Out g = Run(c);
        std::printf("  rough until %2.0f s phase %2.0f: %5.1f / %5.1f / %5.1f fps (last 60 s), late %u / %u / %u\n", rs, ph, a.fpsLast30, b.fpsLast30, g.fpsLast30, a.late, b.late, g.late);
        CHECK(g.fpsLast30 > 58.0);
    }
    std::printf("pace27: GPU 18 ms for the first 20 s (marginal lates the allowance causes: an early strike), then 15.5; the back-off ends by time, fps of the last 60 s of 180 s:\n");
    for (double ph : {2.0, 10.0}) {
        Cfg c; c.gpuMs = 15.5; c.phaseMs = ph; c.seconds = 180; c.initL = 5; c.forceL = true; c.roughS = 20; c.roughGpuMs = 18;
        c.allow25 = true; const Out b = Run(c);
        c.guard26 = true; const Out g = Run(c);
        std::printf("  phase %2.0f: =1 %5.1f / =2 %5.1f fps (last 60 s), late %u / %u\n", ph, b.fpsLast30, g.fpsLast30, b.late, g.late);
        CHECK(g.fpsLast30 > 58.0);
    }
    std::printf("L starts at 5 (as after the opening), the latency rule free to come down, GPU 15.5, 120 s:\n");
    for (double ph : {2.0, 6.0, 10.0}) {
        Cfg c; c.gpuMs = 15.5; c.phaseMs = ph; c.seconds = 120; c.initL = 5;
        char t[64]; std::snprintf(t, sizeof t, "L5 start, phase %.0f ms", ph);
        Pair(t, c);
        c.fast = true; std::snprintf(t, sizeof t, "L5 start, phase %.0f ms + FAST", ph); Pair(t, c);
    }
    std::printf("%s\n", g_fail ? "FAILED" : "pace25 lock model: 48.0 fps without GXV_PENDING_HOLD25, 60 with it, GPU-bound cases unchanged");
    return g_fail ? 1 : 0;
}
