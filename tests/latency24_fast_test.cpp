// thr24 (GXV_LATENCY24_FAST) on the pace33 host model's pipeline (copied from tests/pace33_test.cpp up to main). pace33 host model (14 Nov 2026): the display callback of GXV_PRESENT_SYNC=2 with the REAL PresentSync (frame_pace.h, GXV_LATENCY24 rule) and the REAL drain (port/drain33.h), fed by a pipeline
// whose pictures are finished a fixed time after their turn began (mode 5: A after `aMs`, the pair's second picture B `bAfterMs` after A), with one hitch that makes the standing queue.
// It prints what the console's "present:" / "latency:" lines would: vblanks from the turn to the flip over time, pictures given up, repeated pictures. A MODEL: the lanes are guesses from the c111
// logs; only the console measures milliseconds.   g++ -std=c++17 -O1 -Iplatform/include tests/pace33_test.cpp -o /tmp/pace33/t && nice -n 19 /tmp/pace33/t
#include "port/frame_pace.h"
#include "port/drain33.h"
#include "port/fe_lane.h"
#include "port/hrw_stage.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
using port::pace::PresentSync;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static const double P = 16683.35;   // us
[[maybe_unused]] static double rc0 = 0;

struct Scn {
    bool pairs = true;            // mode 5: A, B, A, B on consecutive turns; false: an even 30 (a picture every second turn) through the same callback
    double aMs = 45, bAfterMs = 3, jitterMs = 0;
    double seconds = 60;
    double hitchAtS = 3, hitchMs = 50;       // one first picture takes this long more
    double hitch2AtS = -1, hitch2Ms = 0;
    bool l24 = true, drain = true;
    unsigned window = 90;
    uint32_t marginUs = 4000;
    std::function<int(double)> still = [](double) { return 1; };
    std::function<bool(double)> quiet = [](double) { return false; };
    unsigned initL = 3;
    double burstEveryS = 0, burstMs = 0;   // (PATCH3) every so often six first pictures in a row are this much later (a census burst, a disc read: the c118 logs' 'pacing slips')
    unsigned regret = 0;
    unsigned calm = 0;         // (PATCH10) GXV_LATENCY24_CALM
    bool fast = false;         // (thr24) GXV_LATENCY24_FAST: hold-off cap 1200, regret x2, release after 180 clean pictures
    unsigned fastCap = 1200, fastMul = 2, fastClean = 180;
    double burstUntilS = 0;    // (PATCH10) the bursts end here (0: never): a stage's rough opening
};
struct Res {
    std::vector<double> latAt;    // per second: mean vblanks stamp -> shown of the pictures seen
    unsigned drops = 0, repeats = 0, jumps = 0, lFinal = 0, dropsMoving = 0, lowers = 0, raises = 0;
    double finalLat = 0, settleS = -1, worstLat = 0;
    double minGapDropsS = 1e9;
};
static Res Run(const Scn& s) {
    Res r;
    PresentSync sync;
    sync.Configure(true, 1, 5, s.initL, 4);
    if (s.l24) {
        PresentSync::Latency24 l; l.on = true; l.window = s.window; l.minKnown = l.window / 2; l.catchUp = s.drain; l.regretPics = s.regret; if (s.regret) l.probeMax = 14400; l.calmBase = s.calm;
        if (s.fast) { l.holdCap = s.fastCap; l.regretMul = s.fastMul; l.cleanPics = s.fastClean; }
        sync.ConfigureLatency24(l);
        sync.SetExternalDrop(s.drain);
    }
    port::drain33::Drain d; port::drain33::Config dc; dc.on = s.drain; d.Configure(dc);
    uint32_t seed = 4242;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
    const unsigned turns = static_cast<unsigned>(s.seconds * 60);
    double ret = 0, aDone = 0, lastDropT = -1e9;
    bool h1 = false, h2 = false;
    // the pictures seen: (shown vblank, stamp); a picture whose successor has the same vblank is never seen
    std::vector<std::pair<uint32_t, uint32_t>> seen;
    std::vector<double> sum(static_cast<size_t>(s.seconds) + 2, 0.0); std::vector<unsigned> cnt(sum.size(), 0);
    const uint32_t base = 1000;
    for (unsigned i = 0; i < turns; ++i) {
        const bool isA = !s.pairs || (i & 1) == 0;
        if (!s.pairs && (i & 1)) continue;
        const double t0 = i * P, sec = t0 / 1e6;
        double done;
        if (isA) {
            double a = s.aMs * 1000 + (s.jitterMs > 0 ? (2 * rnd() - 1) * s.jitterMs * 1000 : 0);
            if (!h1 && s.hitchAtS >= 0 && sec >= s.hitchAtS) { a += s.hitchMs * 1000; h1 = true; }
            if (!h2 && s.hitch2AtS >= 0 && sec >= s.hitch2AtS) { a += s.hitch2Ms * 1000; h2 = true; }
            if (s.burstEveryS > 0 && sec > 10 && (s.burstUntilS <= 0 || sec < s.burstUntilS) && std::fmod(sec, s.burstEveryS) < 0.2) a += s.burstMs * 1000;
            done = std::max(t0 + a, aDone + 2000);    // (the render thread and the GPU finish pictures in order)
            aDone = done;
        } else {
            done = aDone + s.bAfterMs * 1000;
            aDone = done;
        }
        const double enter = std::max(done, ret);
        const bool censored = i > 0 && enter - ret < 1200;
        const uint32_t count = base + static_cast<uint32_t>(std::floor(enter / P));
        const uint32_t earliest = count + 1, stamp = base + i;
        if (s.drain && s.l24) {
            const bool allow = d.Allow(static_cast<uint64_t>(enter), sync.Behind(earliest, true, static_cast<uint16_t>(stamp)), s.still(sec), s.quiet(sec));
            sync.AllowDrop(allow);
        }
        const uint32_t target = sync.Target(earliest, true, static_cast<uint16_t>(stamp));
        if (s.drain && sync.Dropped()) {
            d.Dropped(static_cast<uint64_t>(enter));
            ++r.drops;
            if (s.still(sec) != 1 && !s.quiet(sec)) ++r.dropsMoving;
            if (!s.quiet(sec)) { r.minGapDropsS = std::min(r.minGapDropsS, sec - lastDropT); lastDropT = sec; }
        }
        if (s.l24) {
            sync.NoteBacklog(censored);
            const double phase = std::fmod(enter, P);
            sync.NoteTight(s.drain && !censored && P - phase < s.marginUs);
        }
        uint32_t shown = earliest;
        bool waited = false;
        if (target != earliest) { ret = (target - 1 - base) * P; shown = target; waited = true; }   // (the wait returns when the vblank before the slot begins)
        else ret = enter + 50;
        sync.Shown(shown, waited);
        r.lowers += static_cast<unsigned>(sync.GetStats().lowers); r.raises += static_cast<unsigned>(sync.GetStats().raises); sync.ClearStats();
        if (!seen.empty() && seen.back().first == shown) seen.back() = {shown, stamp};   // (replaces the one before: never seen)
        else seen.push_back({shown, stamp});
        const size_t b = static_cast<size_t>(sec);
        sum[b] += static_cast<double>(shown - stamp); ++cnt[b];
    }
    const uint32_t step = s.pairs ? 1 : 2;
    for (size_t k = 1; k < seen.size(); ++k) {
        const double sec = (seen[k].first - base) * P / 1e6;
        if (s.hitchAtS >= 0 && sec >= s.hitchAtS - 0.2 && sec < s.hitchAtS + 0.6) continue;         // (the hitch itself is not the drain's)
        if (s.hitch2AtS >= 0 && sec >= s.hitch2AtS - 0.2 && sec < s.hitch2AtS + 0.6) continue;
        const uint32_t hold = seen[k].first - seen[k - 1].first, adv = seen[k].second - seen[k - 1].second;
        if (hold > step) ++r.repeats;      // a picture stayed a refresh longer than the rhythm's
        if (adv > step) ++r.jumps;         // the game's time stepped more than the rhythm's in one picture (a picture given up)
    }
    for (size_t b = 0; b < sum.size(); ++b) r.latAt.push_back(cnt[b] ? sum[b] / cnt[b] : 0);
    r.lFinal = sync.Latency();
    const size_t last = static_cast<size_t>(s.seconds) - 1;
    r.finalLat = r.latAt[last];
    for (size_t b = static_cast<size_t>(std::max(0.0, s.hitchAtS)) + 1; b <= last; ++b) {
        r.worstLat = std::max(r.worstLat, r.latAt[b]);
        if (r.settleS < 0 && std::fabs(r.latAt[b] - r.finalLat) < 0.05) r.settleS = static_cast<double>(b) - s.hitchAtS;
    }
    return r;
}
[[maybe_unused]] static void Row(const char* name, const Res& r, const Scn& s) {
    std::printf("  %-66s vblanks turn->flip at 2 / 5 / 10 / 20 / 40 s and at the end: %.1f %.1f %.1f %.1f %.1f %.1f (%3.0f ms)  L %u  given up %2u (camera moving %u)  repeated %2u  settled %4.1f s after the hitch\n",
                name, r.latAt[2], r.latAt[5], r.latAt[10], r.latAt[20], r.latAt[std::min<size_t>(40, r.latAt.size() - 3)], r.finalLat, r.finalLat * 16.68, r.lFinal, r.drops, r.dropsMoving, r.repeats, r.settleS);
    (void)s;
}


int main() {
    Scn base; base.aMs = 40; base.regret = 1200; base.calm = 450; base.seconds = 300;   // (what the console runs: GXV_DRAIN33's regret 1200 and GXV_LATENCY24_CALM 450)
    auto firstAtFinal = [](const Res& r) { size_t k = r.latAt.size() - 3; while (k > 20 && std::fabs(r.latAt[k - 1] - r.finalLat) < 0.05) --k; return k; };
    auto meanFrom = [](const Res& r, size_t a, size_t b) { double m = 0; for (size_t k = a; k < b; ++k) m += r.latAt[k]; return m / (b - a); };
    std::printf("thr24 GXV_LATENCY24_FAST on the pace33 pipeline: a rough opening (late bursts until `until` s), then a calm stage, 300 s\n");
    {
        // off = exactly as before
        Scn a = base; a.burstEveryS = 5; a.burstMs = 25; a.burstUntilS = 60;
        Scn b = a; b.fast = true; b.fastCap = b.fastMul = b.fastClean = 0;
        const Res ra = Run(a), rb = Run(b);
        CHECK(ra.raises == rb.raises && ra.lowers == rb.lowers && ra.drops == rb.drops && ra.repeats == rb.repeats && ra.latAt == rb.latAt);
    }
    unsigned n = 0, sooner = 0, later = 0, moreRaises = 0, moreRepeats = 0; double s0 = 0, s1 = 0, m0 = 0, m1 = 0; unsigned r0s = 0, r1s = 0, rep0 = 0, rep1 = 0;
    for (double until : {40.0, 60.0, 90.0}) for (double every : {2.0, 3.0, 5.0, 7.0, 12.0}) for (double ms : {14.0, 25.0, 40.0}) {
        Scn o = base; o.burstEveryS = every; o.burstMs = ms; o.burstUntilS = until;
        Scn f = o; f.fast = true;
        const Res r0 = Run(o), r1 = Run(f);
        const size_t a = firstAtFinal(r0), b = firstAtFinal(r1);
        ++n; s0 += a; s1 += b; m0 += meanFrom(r0, (size_t)until + 10, 298); m1 += meanFrom(r1, (size_t)until + 10, 298);
        r0s += r0.raises; r1s += r1.raises; rep0 += r0.repeats; rep1 += r1.repeats;
        if (b < a) ++sooner; else if (b > a) ++later;
        if (r1.raises > r0.raises) ++moreRaises;
        if (r1.repeats > r0.repeats) ++moreRepeats;
        CHECK(r1.finalLat <= r0.finalLat + 0.01);
    }
    std::printf("  %u openings: the final latency is reached sooner in %u, later in %u; seconds to it, summed %.0f -> %.0f; mean latency after the opening (+10 s) %.2f -> %.2f vblanks;\n"
                "  raises (failed lowerings) summed %u -> %u (more in %u openings), repeated pictures summed %u -> %u (more in %u)\n", n, sooner, later, s0, s1, m0 / n, m1 / n, r0s, r1s, moreRaises, rep0, rep1, moreRepeats);
    CHECK(later == 0 && sooner >= 5);
    {
        // the c118 case: bursts for ever; the cost of the faster release is more failed lowerings, each two late pictures
        Scn o = base; o.burstEveryS = 12; o.burstMs = 14; o.seconds = 600;
        Scn f = o; f.fast = true;
        const Res r0 = Run(o), r1 = Run(f);
        std::printf("  bursts for ever (c118, 14 ms every 12 s), 600 s: raised %u -> %u, lowered %u -> %u, repeated %u -> %u, given up %u -> %u, mean latency %.2f -> %.2f vblanks\n",
                    r0.raises, r1.raises, r0.lowers, r1.lowers, r0.repeats, r1.repeats, r0.drops, r1.drops, meanFrom(r0, 20, 598), meanFrom(r1, 20, 598));
        // a failed lowering undoes itself within the probation as before: never more than one raise per cap's worth of pictures
        CHECK(r1.raises <= 600u * 60u / 180u);
    }
    std::printf("%s\n", g_fail ? "FAILED" : "latency24 fast: off identical to before; final latency never worse, reached sooner");
    return g_fail ? 1 : 0;
}
