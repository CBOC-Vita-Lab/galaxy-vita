// pace33 host model (14 Nov 2026): the display callback of GXV_PRESENT_SYNC=2 with the REAL PresentSync (frame_pace.h, GXV_LATENCY24 rule) and the REAL drain (port/drain33.h), fed by a pipeline
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
static double rc0 = 0;

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
static void Row(const char* name, const Res& r, const Scn& s) {
    std::printf("  %-66s vblanks turn->flip at 2 / 5 / 10 / 20 / 40 s and at the end: %.1f %.1f %.1f %.1f %.1f %.1f (%3.0f ms)  L %u  given up %2u (camera moving %u)  repeated %2u  settled %4.1f s after the hitch\n",
                name, r.latAt[2], r.latAt[5], r.latAt[10], r.latAt[20], r.latAt[std::min<size_t>(40, r.latAt.size() - 3)], r.finalLat, r.finalLat * 16.68, r.lFinal, r.drops, r.dropsMoving, r.repeats, r.settleS);
    (void)s;
}

int main() {
    std::printf("pace33 host model: the standing queue of a fixed cadence and its drain. Lanes: Honeyhive mode 5 A finished ~45 ms after its turn began [inferred from c111: front end 20 ms, render 12, GPU], B 3 ms after A.\n");
    std::printf("A hitch of 50 ms at second 3 makes a queue of 3 refreshes (the console: 2-3 after the stage's first seconds, 5-6 at worst).\n\n== mode 5 (pictures on consecutive refreshes)\n");
    Scn base;
    {
        Scn a = base; a.l24 = false; a.drain = false; a.initL = 5;
        const Res r = Run(a); Row("as c111 ran (GXV_LATENCY24=0, no drain)", r, a);
        CHECK(r.drops == 0 && r.finalLat >= 5.0);                    // the queue stays for good
        Scn b = base; b.drain = false;
        const Res rb = Run(b); Row("GXV_LATENCY24=1 alone", rb, b);
        CHECK(rb.finalLat > 3.5);                                     // L comes down, the chain of slots does not
        const Res rc = Run(base); Row("GXV_LATENCY24=1 + GXV_DRAIN33=1, camera still", rc, base);
        CHECK(rc.finalLat <= 3.05 && rc.repeats == 0 && rc.drops >= 2 && rc.drops <= 4);
        CHECK(rc.minGapDropsS >= 0.99);                               // never twice within a second
        CHECK(rc.settleS <= 25);
        rc0 = rc.settleS;
    }
    {
        Scn m = base; m.still = [](double t) { return t < 30 ? 0 : 1; };
        const Res r = Run(m); Row("... the camera moving for the first 30 s", r, m);
        CHECK(r.finalLat <= 3.05 && r.minGapDropsS >= 2.9);           // a picture every 3 s at most while it moves
        Scn u = base; u.still = [](double) { return -1; };
        Row("... stillness never known (no pair information)", Run(u), u);
        Scn q = base; q.quiet = [](double t) { return t >= 3.2 && t < 4.5; };
        const Res rq = Run(q); Row("... a covered screen from 3.2 to 4.5 s (a wipe)", rq, q);
        CHECK(rq.finalLat <= 3.05);
        Scn w = base; w.window = 45;
        const Res rw = Run(w); Row("... GXV_LATENCY24_WINDOW=45 (L comes down every 3 s, not 6)", rw, w);
        CHECK(rw.finalLat <= 3.05 && rw.repeats == 0 && rw.settleS <= rc0);
        Scn big = base; big.hitchMs = 150;
        const Res rbig = Run(big); Row("... a hitch of 150 ms (a queue of 9 refreshes)", rbig, big);
        CHECK(rbig.finalLat <= 3.05 && rbig.drops <= 10 && rbig.minGapDropsS >= 0.99);
    }
    {
        std::printf(" lanes and jitter (the latency settles where the pictures have the margin, GXV_DRAIN33_MARGIN_US 4000):\n");
        for (double a : {28.0, 40.0, 45.0, 47.0, 49.0, 56.0, 62.0})
            for (double j : {0.0, 2.0}) {
                Scn s = base; s.aMs = a; s.jitterMs = j; s.seconds = 120;
                const Res r = Run(s);
                char nm[96]; std::snprintf(nm, sizeof nm, "A finished %.0f ms after its turn, jitter +-%.0f ms", a, j);
                Row(nm, r, s);
                CHECK(r.repeats <= 4);                                // the drain does not buy latency with repeated pictures
                CHECK(r.drops <= 10);
                CHECK(r.finalLat <= std::ceil((a + j + 4.0) / 16.683) + 1.01);
            }
        Scn nm = base; nm.aMs = 47; nm.jitterMs = 2; nm.marginUs = 0; nm.seconds = 120;
        const Res r0 = Run(nm); Row("A 47 ms +-2 with the margin test off (GXV_DRAIN33_MARGIN_US=0)", r0, nm);
    }
    {
        Scn h = base; h.hitch2AtS = 30; h.hitch2Ms = 80; h.seconds = 70;
        const Res r = Run(h); Row("a second hitch (80 ms) at 30 s", r, h);
        CHECK(r.finalLat <= 3.05);
    }
    std::printf("\n== PATCH3: late pictures in bursts (a dozen first pictures 14 ms late every 12 s), 600 s: does L go down and up for ever?\n");
    {
        Scn b = base; b.aMs = 40; b.burstEveryS = 12; b.burstMs = 14; b.seconds = 600;
        Scn c = b; c.regret = 1200;
        const Res r0 = Run(b), r1 = Run(c);
        std::printf("  as c118 (no regret rule):          L lowered %u raised %u, pictures given up %u, repeated %u, at the end %.1f vblanks\n", r0.lowers, r0.raises, r0.drops, r0.repeats, r0.finalLat);
        std::printf("  GXV_DRAIN33_REGRET=1200 (PATCH3):  L lowered %u raised %u, pictures given up %u, repeated %u, at the end %.1f vblanks\n", r1.lowers, r1.raises, r1.drops, r1.repeats, r1.finalLat);
        CHECK(r1.raises <= r0.raises && r1.drops <= r0.drops && r1.repeats <= r0.repeats);
        CHECK(r1.raises * 2 <= r0.raises && r1.repeats * 2 <= r0.repeats);
    }
    std::printf("\n== PATCH10: a rough opening (late bursts until 60 s), then a calm stage, 300 s: how long does L stay up? (GXV_LATENCY24_CALM)\n");
    {
        auto meanFrom = [](const Res& r, size_t a, size_t b) { double m = 0; for (size_t k = a; k < b; ++k) m += r.latAt[k]; return m / (b - a); };
        unsigned sooner = 0, later = 0, n = 0; double s0 = 0, s1 = 0;
        auto firstAtFinal = [](const Res& r) { size_t k = r.latAt.size() - 3; while (k > 20 && std::fabs(r.latAt[k - 1] - r.finalLat) < 0.05) --k; return k; };
        for (double until : {40.0, 60.0, 90.0}) for (double every : {2.0, 3.0, 5.0, 7.0, 12.0}) for (double ms : {14.0, 25.0, 40.0}) {
            Scn o = base; o.aMs = 40; o.burstEveryS = every; o.burstMs = ms; o.burstUntilS = until; o.seconds = 300; o.regret = 1200;
            Scn c = o; c.calm = 450;
            const Res r0 = Run(o), r1 = Run(c);
            const size_t f0 = firstAtFinal(r0), f1 = firstAtFinal(r1);
            ++n; s0 += f0; s1 += f1;
            if (f1 < f0) ++sooner; else if (f1 > f0) ++later;
            if (f1 + 20 < f0)
                std::printf("  opening rough until %2.0f s (a burst of %2.0f ms every %2.0f s): at the final latency from %3zu s (PATCH3) -> %3zu s; mean over the calm part %.2f -> %.2f vblanks; raised %u -> %u, repeated %u -> %u\n",
                            until, ms, every, f0, f1, meanFrom(r0, (size_t)until, 298), meanFrom(r1, (size_t)until, 298), r0.raises, r1.raises, r0.repeats, r1.repeats);
            CHECK(r1.finalLat <= r0.finalLat + 0.01);
            CHECK(f1 <= f0);
            CHECK(r1.raises <= r0.raises && r1.repeats <= r0.repeats);
        }
        std::printf("  %u openings: the final latency is reached sooner in %u, later in %u, the same in the rest; seconds to it, summed: %.0f -> %.0f\n", n, sooner, later, s0, s1);
        CHECK(sooner >= 5 && later == 0);
        // the c118 case (bursts for ever): the release must not bring the down-and-up back
        Scn b = base; b.aMs = 40; b.burstEveryS = 12; b.burstMs = 14; b.seconds = 600; b.regret = 1200;
        Scn c = b; c.calm = 450;
        const Res r0 = Run(b), r1 = Run(c);
        std::printf("  bursts for ever (c118), 600 s: PATCH3 raised %u repeated %u given up %u | with the release raised %u repeated %u given up %u\n", r0.raises, r0.repeats, r0.drops, r1.raises, r1.repeats, r1.drops);
        CHECK(r1.raises <= r0.raises && r1.repeats <= r0.repeats);
        // off = PATCH3 exactly
        Scn z = b; z.calm = 0; const Res rz = Run(z);
        CHECK(rz.raises == r0.raises && rz.lowers == r0.lowers && rz.drops == r0.drops);
    }
    std::printf("\n== PATCH4: the fe-lane census books each late picture to the part that made it late (port/fe_lane.h)\n");
    {
        port::felane::Census c;
        uint32_t rs = 9;
        auto jit = [&rs](uint32_t amp) { rs = rs * 1664525u + 1013904223u; return (rs >> 8) % (amp + 1); };
        for (int i = 0; i < 150; ++i) {
            port::felane::Pic q;
            q.us[port::felane::kGame] = 11000 + jit(600); q.us[port::felane::kTail] = 9000 + jit(800); q.us[port::felane::kJoin] = 300 + jit(200);
            q.us[port::felane::kPass] = 900 + jit(100); q.us[port::felane::kQueue] = 0; q.turnGapUs = 33367; q.lostUs = 200;
            if (i % 30 == 7) { q.us[port::felane::kGame] += 7000; q.turnGapUs += 6000; }    // five late by the game thread
            if (i % 50 == 11) q.us[port::felane::kTail] += 9000;                              // three by the front end's own work
            if (i == 99) q.us[port::felane::kJoin] += 6000;                                   // one by the join
            c.Add(q);
        }
        const port::felane::Census::Out o = c.Summarise();
        char line[1024];
        port::felane::Format(line, sizeof line, o, 5.0);
        std::printf("  %s\n", line);
        CHECK(o.late == 9 && o.lateBy[port::felane::kGame] == 5 && o.lateBy[port::felane::kTail] == 3 && o.lateBy[port::felane::kJoin] == 1 && o.lateTurn == 5);
        CHECK(o.totMed > 21000 && o.totMed < 23000 && o.totMax > 30000);
    }
    std::printf("\n== PATCH6: the per-stage choice (port/hrw_stage.h) changes only behind a covered screen\n");
    {
        port::hrwstage::Choice c;
        CHECK(!c.PairsOff() && c.Current() == 0);
        CHECK(!c.Update(1, false) && !c.PairsOff() && c.DeferredNow() && c.Pending() == 1);   // a new value in the open: it waits
        CHECK(!c.Update(1, false) && !c.DeferredNow());                                         // ... and is reported once
        CHECK(c.Update(1, true) && c.PairsOff() && c.Changes() == 1);                           // the cover comes: taken
        unsigned mid = 0;
        for (int turn = 0; turn < 3600; ++turn) {                                               // a minute of open play with the value flapping: nothing changes
            if (c.Update(turn % 7 == 0 ? 2 : 1, false)) ++mid;
        }
        CHECK(mid == 0 && c.PairsOff());
        CHECK(c.Update(2, true) && !c.PairsOff() && c.Current() == 2);                          // the next stage's load: the re-projected 60
        CHECK(!c.Update(2, true) && c.Changes() == 2);
        CHECK(c.Update(3, true) && c.PairsOff() && c.Current() == 3);                           // a locked 30 with the Quality picture keys: pairs off as 1
        CHECK(c.Update(9, true) && c.Current() == 0);                                           // out of range = no word
        std::printf("  changes in the open: %u (must be 0); changes behind a cover: %u; deferrals reported: %u\n", mid, c.Changes(), c.Deferrals());
    }
    std::printf("\n== an even 30 through the same callback (Performance's steady 30)\n");
    {
        Scn e = base; e.pairs = false; e.aMs = 45; e.l24 = false; e.drain = false; e.initL = 5;
        Row("GXV_LATENCY24=0 (L stays where the hitch left it)", Run(e), e);
        Scn f = base; f.pairs = false; f.aMs = 45;
        const Res r = Run(f); Row("GXV_LATENCY24=1 + GXV_DRAIN33=1", r, f);
        CHECK(r.finalLat <= 3.05 && r.repeats == 0);
    }
    std::printf("\n== off: GXV_DRAIN33=0 leaves the decisions of GXV_LATENCY24 as they were\n");
    {
        Scn a = base; a.drain = false;
        Scn b = a;
        const Res ra = Run(a), rb = Run(b);
        CHECK(ra.drops == 0 && ra.latAt == rb.latAt);
        port::drain33::Drain off; port::drain33::Config oc; off.Configure(oc);
        CHECK(!off.Allow(1, true, 1, true));
        std::printf("  drain off: no picture given up (%u), Allow() false\n", ra.drops);
    }
    std::printf(g_fail ? "\npace33_test: FAILED (%d)\n" : "\npace33_test: ALL PASSED\n", g_fail);
    return g_fail != 0;
}
