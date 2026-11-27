// smooth25 host model: the hitch classifier's game-thread fields, the present-cadence metric (PaceMetric), the stage tally cache, and the fixed 30 pacer with
// GXV_PACE_PASSOFFER against traces shaped like c54's Honeyhive windows.  bash tools/test_smooth25.sh
#include <algorithm>
#include <cassert>
#include <cstring>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <vector>

#include "port/frame_pace.h"
#include "port/hitch24.h"
#include "../platform/vgx/src/vgx_stage_tally.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---- 1. the classifier: the 624 ms picture ------------------------------------------------------------------------------------------------
static void TestHitch() {
    using namespace port::hitch24;
    Picture p;  // frame 2595: 624 ms, back end 123 (uploads 44 ms, a target made, other 79)
    p.intervalMs = 624; p.beMs = 123; p.uploadMs = 44; p.otherMs = 79; p.targetsMade = 1;
    Verdict v = Classify(p);
    CHECK(v.cause == kUnknown, "without the game thread's numbers it stays unexplained (as in the log): %s %.0f", CauseName(v.cause), v.ms);
    p.turnGapMs = 540; p.turns = 3;  // the game thread was stuck in one turn for 540 ms
    v = Classify(p);
    CHECK(v.cause == kGame && v.ms >= 500, "game thread: %s %.0f", CauseName(v.cause), v.ms);
    p.turnGapMs = 540; p.gateMs = 520;  // ... waiting for the back end's queue
    v = Classify(p);
    CHECK(v.cause == kGate, "gate: %s", CauseName(v.cause));
    p.turnGapMs = 34; p.gateMs = 0; p.turns = 36;  // turns kept coming, no picture arrived
    v = Classify(p);
    CHECK(v.cause == kUpstream && v.ms > 490, "upstream: %s %.0f", CauseName(v.cause), v.ms);
    // a back end bound picture stays the back end's
    Picture q;
    q.intervalMs = 120; q.beMs = 118; q.otherMs = 64; q.targetsMade = 2; q.turnGapMs = 17; q.turns = 7;
    v = Classify(q);
    CHECK(v.cause == kTarget && v.ms == 64, "targets: %s %.0f", CauseName(v.cause), v.ms);
    // a 45 ms frame whose back end took 13 and whose game turns were 17 ms apart: pacing / front end, not the back end
    Picture r;
    r.intervalMs = 45; r.beMs = 13; r.otherMs = 12; r.turnGapMs = 17; r.turns = 3;
    v = Classify(r);
    CHECK(v.cause == kUpstream, "mixed: %s", CauseName(v.cause));
    for (unsigned c = 0; c < kCauseN; ++c) CHECK(CauseName(static_cast<Cause>(c))[0] != '?', "name %u", c);
}

// ---- 2. PaceMetric ---------------------------------------------------------------------------------------------------------------------------
static void TestMetric() {
    using port::pace::PaceMetric;
    PaceMetric m;
    // title / Observatory window: 150 pictures, every hold 2
    for (int i = 0; i < 150; ++i) m.Picture(2, 2, true);
    PaceMetric::Window w = m.Take();
    CHECK(w.pictures == 150 && w.hold[2] == 150 && w.longest == 2 && w.late == 0 && w.playLate == 0, "clean window");
    CHECK(m.RunP99() == 2, "p99 %u", m.RunP99());
    // c54 window "130; 0/0/96/28/6", all in play
    for (int i = 0; i < 96; ++i) m.Picture(2, 2, true);
    for (int i = 0; i < 28; ++i) m.Picture(3, 2, true);
    for (int i = 0; i < 6; ++i) m.Picture(4, 2, true);
    w = m.Take();
    CHECK(w.pictures == 130 && w.late == 34 && w.late2 == 6 && w.longest == 4 && w.playLate == 34 && w.playLate2 == 6, "late counts %u %u", w.late, w.late2);
    CHECK(m.RunP99() == 4 || m.RunP99() == 3, "p99 after the bad window %u", m.RunP99());
    // a loading window: the long holds are quiet, not in play: the gate set stays clean
    PaceMetric l;
    for (int i = 0; i < 122; ++i) l.Picture(2, 2, true);
    for (int i = 0; i < 6; ++i) l.Picture(4, 2, false);
    w = l.Take();
    CHECK(w.late == 6 && w.playLate == 0 && w.playPictures == 122 && l.RunP99() == 2, "loading window in play late %u", w.playLate);
    // the 60 fps mode: target 1
    PaceMetric f;
    for (int i = 0; i < 280; ++i) f.Picture(1, 1, true);
    for (int i = 0; i < 20; ++i) f.Picture(2, 1, true);
    w = f.Take();
    CHECK(w.target == 1 && w.late == 20 && w.late2 == 0 && w.longest == 2, "60 mode");
    char buf[512];
    f.Format(buf, sizeof buf, w);
    CHECK(std::strstr(buf, "run p99 1 vblanks") || std::strstr(buf, "run p99 2 vblanks"), "format: %s", buf);
    // unknown and gaps are not pictures
    PaceMetric g;
    g.Picture(0, 2, true); g.Picture(9, 2, true); g.Picture(40, 2, true);
    CHECK(g.Take().pictures == 0, "gaps");
    // p99 boundary: 100 in play, 1 late -> still 2 (99 %); 2 late -> 3
    PaceMetric b;
    for (int i = 0; i < 99; ++i) b.Picture(2, 2, true);
    b.Picture(3, 2, true);
    CHECK(b.RunP99() == 2, "p99 at 1/100: %u", b.RunP99());
    b.Picture(3, 2, true);
    CHECK(b.RunP99() == 3, "p99 at 2/101: %u", b.RunP99());
}

// ---- 3. the stage tally cache against the plain loop --------------------------------------------------------------------------------------------
struct Rec { uint64_t vs, fs; uint32_t lb; };
struct Set { std::vector<Rec> recs; };
using Sets = std::unordered_map<uint32_t, Set>;
using Ids = std::unordered_map<uint64_t, uint16_t>;

static bool Same(const vgx::stagetally::CntMap& a, const vgx::stagetally::Order& oa, const vgx::stagetally::CntMap& b, const vgx::stagetally::Order& ob) {
    if (oa != ob) return false;
    for (size_t i = 0; i < oa.size(); ++i) {
        const auto& x = a.at(oa[i].second);
        const auto& y = b.at(ob[i].second);
        if (x.n != y.n || x.lb != y.lb || x.vs != y.vs || x.fs != y.fs) return false;
    }
    return true;
}
static void TestTally() {
    using namespace vgx::stagetally;
    std::mt19937 rng(12345);
    const uint32_t kPlayer = 0x504C5952u, kMenu = 0x4D454E55u;
    unsigned checked = 0, nonEmpty = 0;
    for (int round = 0; round < 60; ++round) {
        Ids vs, fs;
        const unsigned nv = 20 + rng() % 40, nf = 30 + rng() % 80;
        for (unsigned i = 0; i < nv; ++i) vs[0x1000 + i] = static_cast<uint16_t>(i);
        for (unsigned i = 0; i < nf; ++i) fs[0x2000 + i] = static_cast<uint16_t>(i);
        Sets sets;
        const unsigned nstages = 3 + rng() % 12;
        std::vector<uint32_t> keys;
        for (unsigned s = 0; s < nstages; ++s) {
            const uint32_t key = s == 0 ? kMenu : s == 1 ? kPlayer : 0x10000000u + rng() % 100000;
            Set& set = sets[key];
            const unsigned nr = rng() % 200;
            for (unsigned r = 0; r < nr; ++r)
                set.recs.push_back(Rec{0x1000 + (rng() % (nv + 3)), 0x2000 + (rng() % (nf + 3)), static_cast<uint32_t>(rng() % 50000)});  // (a few unknown hashes too)
            keys.push_back(key);
        }
        Cache cache;
        size_t recs = 0;
        for (const auto& kv : sets) recs += kv.second.recs.size();
        const unsigned minN = 1 + rng() % 4;
        std::unordered_set<uint32_t> held;
        for (int q = 0; q < 12; ++q) held.insert((static_cast<uint32_t>(rng() % nv) << 16) | (rng() % nf));
        const auto skip = [&](uint32_t k) { return held.count(k) != 0; };
        for (int pass = 0; pass < 3; ++pass) {
            for (const uint32_t target : keys) {
                if (target == kPlayer || target == kMenu) continue;
                CntMap ref, got;
                Order refOrder, gotOrder;
                PlainTally(sets, target, kPlayer, vs, fs, ref);
                PlainOrder(ref, minN, skip, refOrder);
                cache.Select(sets, recs, 7u, target, kPlayer, vs, fs, minN, skip, got, gotOrder);
                CHECK(Same(ref, refOrder, got, gotOrder), "round %d pass %d target %08x", round, pass, target);
                ++checked;
                nonEmpty += !refOrder.empty();
            }
            // a target that has no list at all (a new stage)
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, 0xDEAD0001u, kPlayer, vs, fs, ref);
            PlainOrder(ref, minN, skip, refOrder);
            cache.Select(sets, recs, 7u, 0xDEAD0001u, kPlayer, vs, fs, minN, skip, got, gotOrder);
            CHECK(Same(ref, refOrder, got, gotOrder), "round %d unlisted target", round);
            // records are added to one stage (the state key changes): the cache is made again and still equals the loop
            const uint32_t k = keys[rng() % keys.size()];
            for (int a = 0; a < 5; ++a) { sets[k].recs.push_back(Rec{0x1000 + (rng() % nv), 0x2000 + (rng() % nf), static_cast<uint32_t>(rng() % 50000)}); ++recs; }
        }
        // new shaders registered: the id maps change (regs changes)
        vs[0x1000 + nv] = static_cast<uint16_t>(nv);
        for (const uint32_t target : keys) {
            if (target == kPlayer || target == kMenu) continue;
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, target, kPlayer, vs, fs, ref);
            PlainOrder(ref, minN, skip, refOrder);
            bool rebuilt = false;
            cache.Select(sets, recs, 8u, target, kPlayer, vs, fs, minN, skip, got, gotOrder, &rebuilt);
            CHECK(Same(ref, refOrder, got, gotOrder), "after a registration, round %d", round);
            break;
        }
    }
    CHECK(checked > 200 && nonEmpty > 100, "the random sets were exercised: %u checked, %u non-empty", checked, nonEmpty);
    std::printf("tally: %u target builds compared (%u with a non-empty shared list)\n", checked, nonEmpty);
}

// ---- 4. the fixed 30 pacer on a CPU-bound trace -----------------------------------------------------------------------------------------------------
// A model of the game thread: a turn starts at max(the end of the turn before, its retrace), costs D when it draws and N when it does not; a picture is
// shown on the first even vblank at least 2 after the previous one and after its turn's end + `pipe` ms (the front end, the back end, the GPU).
struct SimResult { unsigned pictures = 0, hold[6] = {}, late = 0; double seconds = 0; unsigned skipsDue = 0; };
static SimResult Simulate(bool pass, double D, double N, double pipeMs, double seconds, double burstFrom = -1, double burstTo = -1, double burstD = 0) {
    port::pace::Controller c(false, port::pace::Policy::Fixed30);
    c.SetPassOffer(pass);
    const double P = 16683.35;  // us
    double end = 0;
    std::vector<double> shown;  // vblank index of each picture
    int lastShownV = -100;
    SimResult r;
    const int turns = static_cast<int>(seconds * 60);
    double nextReady = 0;
    unsigned pending = 0;
    std::vector<double> inflight;  // times at which the pictures in the pipe finish
    for (int i = 0; i < turns; ++i) {
        const double start = std::max(end, i * P);
        const double nowMs = start / 1000.0;
        while (!inflight.empty() && inflight.front() <= start) inflight.erase(inflight.begin());
        pending = static_cast<unsigned>(inflight.size());
        const int decision = c.Turn(static_cast<uint64_t>(start) + 1000000, true, false, pending, 0, 0);
        const bool drew = decision == 0;
        const bool inBurst = burstFrom >= 0 && nowMs / 1000.0 >= burstFrom && nowMs / 1000.0 < burstTo;
        const double cost = drew ? (inBurst ? burstD : D) * 1000.0 : N * 1000.0;
        end = start + cost;
        if (decision != 2) end = std::max(end, (i + 1) * P) - 0.0;  // (the retrace wait; a late turn has none)
        if (decision == 2) end = start + cost;
        if (drew) {
            const double ready = std::max(start + cost + pipeMs * 1000.0, nextReady);
            // the display shows it at the first vblank after `ready`, no sooner than 2 vblanks after the picture before (PresentationSlots: due = last + 2)
            int v = static_cast<int>(ready / P) + 1;
            if (lastShownV > -100 && v < lastShownV + 2) v = lastShownV + 2;
            if (lastShownV > -100) {
                const int h = v - lastShownV;
                ++r.hold[h < 5 ? h : 5];
                if (h > 2) ++r.late;
            }
            lastShownV = v;
            ++r.pictures;
            inflight.push_back(ready);
        }
    }
    r.seconds = seconds;
    return r;
}
static void TestPacer() {
    // pair cost under two retraces: nothing to skip, both policies give 30 pictures a second, every hold 2
    SimResult a = Simulate(false, 20, 7, 8, 10), b = Simulate(true, 20, 7, 8, 10);
    CHECK(a.hold[2] >= a.pictures - 3 && b.hold[2] >= b.pictures - 3 && a.pictures > 290, "easy trace: %u %u", a.pictures, b.pictures);
    CHECK(b.pictures >= a.pictures, "pass-on never fewer pictures");
    std::printf("easy   (D20 N7 pipe 8):      old %u pictures in 10 s, holds 2/3/4+ %u/%u/%u; pass-on %u, %u/%u/%u\n", a.pictures, a.hold[2], a.hold[3], a.hold[4] + a.hold[5], b.pictures, b.hold[2],
                b.hold[3], b.hold[4] + b.hold[5]);
    // CPU-bound: a pair of 44 ms (D 30 + N 14): the c54 window's 21 pictures a second
    for (double D : {28.0, 30.0, 34.0}) {
        a = Simulate(false, D, 12, 10, 20);
        b = Simulate(true, D, 12, 10, 20);
        std::printf("bound  (D%.0f N12 pipe 10):   old %.1f pictures/s, holds 1/2/3/4+ %u/%u/%u/%u; pass-on %.1f, %u/%u/%u/%u\n", D, a.pictures / a.seconds, a.hold[1], a.hold[2], a.hold[3],
                    a.hold[4] + a.hold[5], b.pictures / b.seconds, b.hold[1], b.hold[2], b.hold[3], b.hold[4] + b.hold[5]);
        CHECK(b.pictures + 2 >= a.pictures, "pass-on gives at least as many pictures (%u vs %u)", b.pictures, a.pictures);
        CHECK(b.hold[1] == 0 && a.hold[1] == 0, "no two pictures on adjacent vblanks");
    }
    // bursts (0.7 s of heavy turns every few seconds, like the 07.2 / 11.2 / 16.3 clusters): the pictures a burst costs
    a = Simulate(false, 22, 8, 8, 20, 5.0, 5.7, 40);
    b = Simulate(true, 22, 8, 8, 20, 5.0, 5.7, 40);
    std::printf("burst  (D22 N8, 0.7 s of D40): old %u pictures, holds 2/3/4+ %u/%u/%u; pass-on %u, %u/%u/%u\n", a.pictures, a.hold[2], a.hold[3], a.hold[4] + a.hold[5], b.pictures, b.hold[2], b.hold[3],
                b.hold[4] + b.hold[5]);
    CHECK(b.hold[4] + b.hold[5] <= a.hold[4] + a.hold[5], "pass-on has no more 4+ holds in a burst");
    // the Controller itself: with pass-on a picture is never offered on two consecutive turns, and an allowed due turn that is left out is offered again at once
    port::pace::Controller c(false, port::pace::Policy::Fixed30);
    c.SetPassOffer(true);
    uint64_t t = 1000000;
    int prev = 1;
    for (int i = 0; i < 400; ++i) {
        t += 16683;
        const unsigned pending = (i % 7 == 3) ? 2u : 0u;  // the back end holds two pictures now and then
        const int d = c.Turn(t, true, false, pending, 0, 0);
        CHECK(!(d == 0 && prev == 0), "two pictures on consecutive turns at %d", i);
        prev = d;
    }
}

// ---- 5. PresentationSlots after one late picture (frame_pace.h): the grid follows the picture, no double advance ------------------------------------
struct MockDisplay {
    uint32_t vcount = 0;
    uint32_t Count() const { return vcount; }
    int WaitVblank() { ++vcount; return 0; }
};
static void TestSlots() {
    port::pace::PresentationSlots slots;
    MockDisplay d;
    // pictures ready at vblank 10, 12, 14 (+1 late: 17), 18, 20, 22 ... (the callback sees them in order; a picture that is ready later than the display is free waits)
    const uint32_t ready[] = {10, 12, 14, 17, 18, 20, 22, 24, 26, 28, 30, 33, 34, 36};
    std::vector<uint32_t> shown;
    for (uint32_t r : ready) {
        if (d.vcount < r) d.vcount = r;
        const uint32_t earliest = d.Count() + 1;
        const uint32_t due = slots.Next(earliest, 2);
        const auto res = port::pace::WaitForSlot(d, due, true);
        slots.Shown(res.shown);
        shown.push_back(res.shown);
    }
    std::printf("slots: ready  ");
    for (uint32_t r : ready) std::printf("%u ", r);
    std::printf("\nslots: shown  ");
    for (uint32_t v : shown) std::printf("%u ", v);
    std::printf("\nslots: holds  ");
    unsigned min = 99, over = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        const unsigned h = shown[i] - shown[i - 1];
        std::printf("%u ", h);
        min = std::min(min, h);
        over += h > 2;
    }
    std::printf("\n");
    CHECK(min >= 2, "never two pictures on adjacent vblanks (min hold %u)", min);
    // the first late picture is shown 1 vblank late (hold 3) and the grid stays on its new phase (odd vblanks before, even after): later pictures are held 2 again, the
    // +1 latency stays, and the second late picture (ready 33, due 32 on the new grid) lands on its slot: it is absorbed (one long hold in all, no double advance)
    CHECK(over == 1 && (shown[0] & 1) != (shown.back() & 1), "one long hold for the first late picture, none for the second (%u)", over);
}

int main() {
    TestHitch();
    TestMetric();
    TestTally();
    TestPacer();
    TestSlots();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("smooth25 model: all passed\n");
    return 0;
}
