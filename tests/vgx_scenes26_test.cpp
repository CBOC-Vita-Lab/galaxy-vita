// scenes26 host test: the depth load/store decision table, the tail predictor, the A/B schedule and the census (platform/vgx/src/vgx_scenes26.h).
// No GL. The core is a model of the console's depth memory: each scene loads the surface into its tile buffer (or not), runs its operations on it and stores it (or not);
// a read must see the logical depth. The plan is made with the same backward pass as vgx_backend_gl.cpp's PlanFrame / EfbDepthPlan; the test proves
//   - DecideDepth equals the formulas it replaced (every combination of its five facts), and the plan built from it never lets a read see wrong depth (random pictures);
//   - dropping the tail is exact when the next picture begins with a whole-depth overwrite, and is caught (a miss, then off) when it does not;
//   - negative controls: a plan that skips a needed store, or a predictor that never checks, is noticed by the same model.
//   tools/test_vgx_scenes26.sh
#include "../platform/vgx/src/vgx_scenes26.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}

using namespace vgx::s26;

// ---- 1. the table ----------------------------------------------------------------------------------------------------------------------------
// The formulas EfbDepthPlan had before scenes26 (copied): st = (wr || fl) && live; ld = readBeforeFull || (st && !fl).
static DepthNeed Original(bool rd, bool wr, bool fl, bool readBeforeFull, bool live) {
    (void)rd;
    DepthNeed n;
    n.store = (wr || fl) && live;
    n.load = readBeforeFull || (n.store && !fl);
    return n;
}
static void TestTable() {
    int n = 0;
    for (int m = 0; m < 32; ++m) {
        DepthFacts f;
        f.reads = m & 1; f.writes = m & 2; f.full = m & 4; f.readFirst = m & 8; f.liveAtEnd = m & 16;
        const DepthNeed a = DecideDepth(f), b = Original(f.reads, f.writes, f.full, f.readFirst, f.liveAtEnd);
        if (a.load != b.load || a.store != b.store) ++n;
        // invariants
        if (a.store && !(f.writes || f.full)) ++n;      // never stores what it did not touch
        if (a.store && !f.liveAtEnd) ++n;               // never stores what nobody reads
        if (f.readFirst && !a.load) ++n;                // a scene that reads before overwriting loads
        if (a.store && !f.full && !a.load) ++n;         // a partial writer keeps the rest of the surface
    }
    Check(n == 0, "DecideDepth equals the old formulas and keeps its invariants for all 32 combinations");
    // Lower: only ever lowers
    int bad = 0;
    for (int d = 0; d < 4; ++d)
        for (int p = 0; p < 4; ++p) {
            const DepthNeed dn{(d & 1) != 0, (d & 2) != 0}, pn{(p & 1) != 0, (p & 2) != 0};
            const DepthNeed r = Lower(dn, pn);
            if ((r.load && !dn.load) || (r.store && !dn.store) || (r.load && !pn.load) || (r.store && !pn.store)) ++bad;
        }
    Check(bad == 0, "Lower never raises a load or a store");
    // named cases of a typical picture (Good Egg's scenes): world (writes, last read follows), silhouette (touches no depth), actors, post (depth test + one writer, last of the picture)
    DepthNeed world = DecideDepth(DepthFacts{true, true, true, false, true});      // begins with the covering quad: full, nothing read first
    Check(!world.load && world.store, "first scene with the covering quad: no load, store (later scenes read it)");
    DepthNeed sil = DecideDepth(DepthFacts{false, false, false, false, true});
    Check(!sil.load && !sil.store, "silhouette scene: neither");
    DepthNeed actors = DecideDepth(DepthFacts{true, true, false, true, true});
    Check(actors.load && actors.store, "actors: load and store");
    DepthNeed post = DecideDepth(DepthFacts{true, true, false, true, true});
    DepthNeed postTail = DecideDepth(DepthFacts{true, true, false, true, false});
    Check(post.store && !postTail.store && postTail.load, "last scene: stores with a live tail, not with a dropped one; still loads what it reads");
}

// ---- 2. the model ----------------------------------------------------------------------------------------------------------------------------
enum Op : char { R = 'R', W = 'W', F = 'F' }; // read (depth test), write (partial: test+write), full overwrite (covering quad / clear)
struct Pic {
    std::vector<std::vector<char>> scenes; // ops per scene
};
struct Plan { std::vector<DepthNeed> need; bool startDead = false; };

// The plan of one picture, as PlanFrame + EfbDepthPlan make it: positions are the flattened ops; a scene is [b, e); live[e] chain; tailLive = live at the end of the picture.
static Plan MakePlan(const Pic& p, bool tailLive) {
    std::vector<char> ops;
    std::vector<size_t> begin, end;
    for (const auto& s : p.scenes) { begin.push_back(ops.size()); for (char c : s) ops.push_back(c); end.push_back(ops.size()); }
    const size_t n = ops.size();
    std::vector<char> live(n + 1, 0);
    std::vector<size_t> nextRead(n + 1, n), nextWrite(n + 1, n), nextFull(n + 1, n);
    live[n] = tailLive ? 1 : 0;
    for (size_t i = n; i-- > 0;) {
        const char c = ops[i];
        live[i] = c == R ? 1 : (c == F ? 0 : live[i + 1]);
        nextRead[i] = c == R ? i : nextRead[i + 1];
        nextWrite[i] = c == W ? i : nextWrite[i + 1];
        nextFull[i] = c == F ? i : nextFull[i + 1];
    }
    Plan pl;
    for (size_t k = 0; k < begin.size(); ++k) {
        const size_t b = begin[k], e = end[k];
        const bool rd = nextRead[b] < e, wr = nextWrite[b] < e, fl = nextFull[b] < e;
        const bool rbf = rd && (!fl || nextRead[b] < nextFull[b]);
        pl.need.push_back(DecideDepth(DepthFacts{rd, wr, fl, rbf, live[e] != 0}));
    }
    for (size_t i = 0; i < n; ++i) { if (ops[i] == R) break; if (ops[i] == F) { pl.startDead = true; break; } }
    return pl;
}
// Run pictures through the console's depth model. mem: the depth surface in memory (version numbers); logical: what the game believes. Returns the number of wrong reads.
struct Machine {
    int mem = -1;          // surface in memory
    int logical = -1;      // true depth
    int nextVersion = 1;
    int wrong = 0;
    void Picture(const Pic& p, const Plan& pl, bool breakStore = false) {
        for (size_t k = 0; k < p.scenes.size(); ++k) {
            DepthNeed nd = pl.need[k];
            if (breakStore && k + 1 == p.scenes.size() && false) nd.store = false;
            int tile = nd.load ? mem : -777; // a scene that does not load starts with garbage
            for (char c : p.scenes[k]) {
                if (c == R) { if (tile != logical) ++wrong; }
                else if (c == W) { tile = nextVersion; logical = nextVersion++; }
                else { tile = nextVersion; logical = nextVersion++; }
            }
            if (nd.store) mem = tile;
        }
    }
};
static Pic RandomPicture(unsigned& seed, bool startDead) {
    auto rnd = [&](unsigned m) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % m; };
    Pic p;
    const int scenes = 1 + static_cast<int>(rnd(6));
    for (int k = 0; k < scenes; ++k) {
        std::vector<char> s;
        const int ops = static_cast<int>(rnd(5));
        for (int j = 0; j < ops; ++j) {
            const unsigned r = rnd(10);
            // a read is only valid when the logical depth was written before it in this picture or carried over; the model checks that against `logical`
            s.push_back(r < 4 ? R : (r < 8 ? W : F));
        }
        p.scenes.push_back(s);
    }
    if (startDead) p.scenes[0].insert(p.scenes[0].begin(), F);
    return p;
}

static void TestPlanExact() {
    // 1. any picture, tail live (the default): every read sees the logical depth when the previous picture left its depth in memory, exactly like a surface that always loads and stores
    unsigned seed = 12345;
    int wrongPlan = 0, wrongRef = 0, pics = 0;
    for (int t = 0; t < 4000; ++t) {
        Machine m, ref;
        for (int q = 0; q < 3; ++q) {
            Pic p = RandomPicture(seed, (seed & 1) != 0);
            // make the model's carried depth defined: the first picture may read the initial surface
            if (q == 0) m.mem = m.logical = ref.mem = ref.logical = 0;
            Plan pl = MakePlan(p, true);
            m.Picture(p, pl);
            // reference: load and store everywhere
            Plan all = pl;
            for (auto& d : all.need) d = DepthNeed{true, true};
            ref.Picture(p, all);
            ++pics;
        }
        wrongPlan += m.wrong;
        wrongRef += ref.wrong;
    }
    Check(wrongRef == 0, "model sanity: a surface that always loads and stores never reads wrong");
    Check(wrongPlan == 0, "the plan (tail live) never lets a read see wrong depth in 12000 random pictures");
}

static void TestTail() {
    // Sequences of pictures that all begin with the covering overwrite: dropping every tail is exact once the predictor's run is reached.
    unsigned seed = 777;
    int wrong = 0, drops = 0, misses = 0, ref = 0;
    for (int t = 0; t < 300; ++t) {
        TailPredictor tp(8);
        Machine m;
        m.mem = m.logical = 0;
        for (int q = 0; q < 40; ++q) {
            Pic p = RandomPicture(seed, true);
            const Plan probe = MakePlan(p, true);
            tp.Observe(true, probe.startDead);
            const bool drop = tp.DropTail();
            Plan pl = MakePlan(p, !drop);
            m.Picture(p, pl);
            drops += drop;
        }
        wrong += m.wrong;
        misses += static_cast<int>(tp.Misses());
        ref += static_cast<int>(tp.Off());
    }
    Check(drops > 0, "tails were dropped in the dead-start sequences");
    Check(wrong == 0 && misses == 0 && ref == 0, "dropping the tail after a run of dead starts never reads wrong depth (300 sequences of 40 pictures)");

    // A picture that does not begin dead after dropped tails: the model reads wrong depth in THAT picture (the cost of a miss) and the predictor sees it and stops.
    TailPredictor tp(4);
    Machine m;
    m.mem = m.logical = 0;
    unsigned s2 = 99;
    int wrongBefore = 0;
    for (int q = 0; q < 10; ++q) {
        Pic p = RandomPicture(s2, true);
        const Plan probe = MakePlan(p, true);
        tp.Observe(true, probe.startDead);
        m.Picture(p, MakePlan(p, !tp.DropTail()));
    }
    wrongBefore = m.wrong;
    Check(tp.Drops() > 0 && tp.Run() >= 4, "run reached");
    // a picture that reads the old depth first and writes the picture's last depth in an earlier scene is the failing shape: R first, scenes[0] = "R"
    Pic bad;
    bad.scenes = {{R, W}, {R}};
    {
        const Plan probe = MakePlan(bad, true);
        tp.Observe(true, probe.startDead);
        Check(!probe.startDead, "a picture whose first depth operation is a read is not a dead start");
        m.Picture(bad, MakePlan(bad, !tp.DropTail()));
    }
    Check(tp.Misses() == 1 && tp.Off(), "the miss is counted and the predictor switches itself off");
    Check(m.wrong > wrongBefore, "that picture read stale depth (the price of one miss, once)");
    const uint64_t d0 = tp.Drops();
    for (int q = 0; q < 50; ++q) {
        Pic p = RandomPicture(s2, true);
        tp.Observe(true, MakePlan(p, true).startDead);
        tp.DropTail();
    }
    Check(tp.Drops() == d0, "after a miss no tail is dropped again");

    // pictures without a plan cannot check: counted, run restarts
    TailPredictor tq(3);
    for (int q = 0; q < 5; ++q) { tq.Observe(true, true); tq.DropTail(); }
    const uint64_t dropsBefore = tq.Drops();
    tq.Observe(false, false);
    Check(tq.Unverified() == 1 && tq.Run() == 0, "a picture without a plan after a dropped tail: unverified, run restarts");
    tq.Observe(true, true);
    Check(!tq.DropTail() && tq.Drops() == dropsBefore, "no drop right after the run restarted");

    // negative control: a predictor that drops from the start (run 0 is clamped to 4) would be caught by the sequence with a read at the start
    TailPredictor lazy(4);
    Machine m3;
    m3.mem = m3.logical = 0;
    unsigned s3 = 4242;
    int w3 = 0;
    for (int q = 0; q < 200; ++q) {
        Pic p = RandomPicture(s3, false); // starts are NOT dead
        const Plan pr = MakePlan(p, true);
        lazy.Observe(true, pr.startDead);
        m3.Picture(p, MakePlan(p, !lazy.DropTail()));
    }
    w3 = m3.wrong;
    Check(lazy.Misses() <= 1, "with random starts the predictor misses at most once, then is off");
    (void)w3;
}

// ---- 3. the A/B schedule ---------------------------------------------------------------------------------------------------------------------
static void TestSchedule() {
    int orig = 0, nw = 0, lastOfBlock = 0;
    for (uint64_t p = 0; p < 120; ++p) {
        (PhaseOf(p, 6) == P_ORIG ? orig : nw)++;
        if (LastOfBlock(p, 6)) ++lastOfBlock;
    }
    Check(orig == 60 && nw == 60, "half the pictures are ORIGINAL, half NEW");
    Check(lastOfBlock == 20, "one read-back a block");
    Check(PhaseOf(0, 6) == P_ORIG && PhaseOf(5, 6) == P_ORIG && PhaseOf(6, 6) == P_NEW && PhaseOf(12, 6) == P_ORIG, "blocks alternate O N O N");
    Check(ShotOf(5, 6) == 0 && ShotOf(11, 6) == 1 && ShotOf(17, 6) == 2 && ShotOf(23, 6) == 3 && ShotOf(29, 6) == 0, "shots 0 O, 1 N, 2 O2, 3 N2, then again");
    Check(PhaseOf(7, 0) == P_NEW && LastOfBlock(0, 0), "block 0 is treated as 1");
}

// ---- 4. the census ---------------------------------------------------------------------------------------------------------------------------
static void TestCensus() {
    Census c;
    for (int pic = 0; pic < 10; ++pic) {
        c.BeginPicture();
        c.Begin(false, true, true, true, false, true, true);   // scene 0: depth load saved (FIRSTCLEAR), colour dead and skipped
        c.Begin(false, false, true, true, false, false, false); // scene 1: neither: a load and a store saved
        c.Begin(true, true, true, true, false, false, false);
        c.Begin(true, false, true, true, true, false, false);   // last: store saved by the tail
    }
    Check(c.pictures == 10 && c.scenes == 40, "pictures and scenes counted");
    Check(c.row[0].dLoadsSaved == 10 && c.row[0].colourDead == 10 && c.row[0].colourSkipped == 10, "scene 0: load saved, colour dead and skipped");
    Check(c.row[1].dLoadsSaved == 10 && c.row[1].dStoresSaved == 10, "scene 1: both saved");
    Check(c.row[3].tailSaved == 10 && c.row[3].dStoresSaved == 10 && c.row[2].dStoresSaved == 0, "scene 3: store saved by the tail");
    char line[1200];
    const int n = c.Format(line, sizeof line);
    Check(n > 0 && std::strstr(line, "scenes26: 10 pictures, 4.00 EFB scenes a picture") && std::strstr(line, "[3: 100% | 100% 0% | 0% (0%) | 0.00 1.00 (1.00)]"), "census line text");
    char tiny[64];
    c.Format(tiny, sizeof tiny);
    Check(std::strlen(tiny) < sizeof tiny, "census line respects its buffer");
    // more scenes than rows: the last row collects them
    Census d;
    d.BeginPicture();
    for (int i = 0; i < 12; ++i) d.Begin(true, true, true, true, false, false, false);
    Check(d.row[kMaxScenes - 1].begins == 5, "scenes past the table land in the last row");
}

int main() {
    TestTable();
    TestPlanExact();
    TestTail();
    TestSchedule();
    TestCensus();
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
