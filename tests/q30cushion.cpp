// q30cush31 host test: platform/include/port/q30cushion.h (GXV_Q30_CUSHION, the "q30sep:" window) and a cadence model of the fixed 30's display callback.
//   g++ -std=c++17 -O1 -DPORT_Q30CUSH_MUTANT -Iplatform/include tests/q30cushion.cpp -o /tmp/q30c && /tmp/q30c [tests/data/q30cush_hitch_trace.txt]
// (tools/test_q30cushion.sh builds it with ASan + UBSan.)
//
// THE MODEL (an ASSUMPTION set, stated: the console decides which of the two chains is real; the census "margin ms" histogram of the q30sep line tells them apart):
//   picture i's pair starts on the vblank grid, i * 2 vblanks. It is READY at  start + L + noise + spike  (L: the pair's latency to the GPU fence).
//   S1 "free": the producer keeps its own clock (a late picture does not delay the next pair): a late slot is made up by the slot moving later (margin + 1 vblank).
//   S3 "slaved": the pair starts only when the last picture has been shown (start_i = max(grid, flip_{i-1})): a later slot delays the next pair, so the margin 2 vblanks - L never
//      grows and a spike is a hold every time. (A back-end-bound chain, ready = release + c, has 3 vblanks - c of margin: at least one vblank while it keeps 30: never fragile.)
//   The on-disk Quality logs (c74: 568 50 ms holds + 155 longer, for 411 hitch frames in 1042 s) look like S3, not S1: holds do not heal. The census decides on the console.
//   The display side is the real PresentationSlots (frame_pace.h) plus a copy of PaceDisplay's drain rule (ring of 16 `early`, lo >= 2 + floor, one a second).
// Checks: the closed forms (a spike shorter than the margin is invisible, a longer one is one 3-vblank hold; S1 heals, S2 does not), the cushion's rules, the cushion
// in both chains (S1: no more holds than the baseline + the engagement's own; S2: gives up, at most its two holds), the drain floor, the window, the log-trace replay.
// Mutants: 1 engage returns no vblank, 2 release returns none, 3 no engagement in play, 4 the drain floor is not raised, 5 the verify step never gives up, 6 the margin is measured against the clamped slot (q30cush31b).
#include "port/frame_pace.h"
#include "port/q30cushion.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace port { namespace q30cush { int gMutant = 0; } }
using namespace port::q30cush;

static int fails = 0;
static bool g_verbose = true, g_quiet = false;
#define PR(...) do { if (!g_quiet) std::printf(__VA_ARGS__); } while (0)
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (g_verbose) { PR("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); PR(__VA_ARGS__); PR("\n"); } } } while (0)

static const double V = 16683.35;   // us

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761u + 88172645463325252ull) {}
    double U() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<double>(s >> 11) / 9007199254740992.0; }
    double N() { double a = 0; for (int i = 0; i < 12; ++i) a += U(); return a - 6.0; }
};

struct Result {
    unsigned pictures = 0, hold[6] = {}, drains = 0, engages = 0, releases = 0, gaveUp = 0;
    uint64_t avoided = 0, nearPics = 0;
    unsigned lvlPics = 0;
    double latencyAddedMs = 0;   // mean level * 16.7 over the pictures
};

struct Cfg {
    bool coupled = false;     // S3 (slaved start)
    bool cushion = false, drain = true;
    double Lus = 20000;       // S1: start-to-ready; S2: the back end's c (GPU + recording)
    double noiseUs = 400;
    std::vector<std::pair<unsigned, double>> spikes;   // picture index, extra us
    unsigned N = 3000;
    uint64_t seed = 1;
    int coverFirst = 12;      // the first pictures are behind a cover
    double gridPhaseUs = 0;
};

static Result Run(const Cfg& c) {
    Result r;
    Rng rng(c.seed);
    port::pace::PresentationSlots slots;
    Cushion cu;
    int32_t ring[16];
    unsigned ringN = 0, sinceDrain = 0, drainsTot = 0, missAfterDrain = 0;
    bool giveUp = false;
    double latEw = -1.0;
    uint32_t last = 0; bool haveLast = false;
    double flipPrev = 0;
    std::map<unsigned, double> spk;
    for (const auto& s : c.spikes) spk[s.first] += s.second;
    double lvlSum = 0;
    for (unsigned i = 0; i < c.N; ++i) {
        const double start = std::max(c.gridPhaseUs + static_cast<double>(i) * 2.0 * V, c.coupled ? flipPrev : 0.0);
        double sp = spk.count(i) ? spk[i] : 0.0;
        const double ready = start + c.Lus + c.noiseUs * rng.N() + sp;
        const uint32_t earliest = static_cast<uint32_t>(std::floor(ready / V)) + 1u;
        uint32_t due = slots.Next(earliest, 2);
        const int32_t early = static_cast<int32_t>(due - earliest);
        ring[i & 15u] = early; ++ringN; ++sinceDrain;
        const bool play = static_cast<int>(i) >= c.coverFirst;
        if (c.drain && !giveUp && ringN >= 16 && early >= 2 && (latEw < 0.0 || latEw > 3.0)) {
            int32_t lo = early;
            for (int k = 0; k < 16; ++k) lo = std::min(lo, ring[k]);
            const unsigned floor = c.cushion ? cu.DrainFloor() : 2u;
            if (lo >= static_cast<int32_t>(floor) && sinceDrain >= (play ? 30u : 2u)) { due -= 1; sinceDrain = 0; ringN = 0; ++r.drains; ++drainsTot; missAfterDrain = 0; }
        }
        int64_t margin = static_cast<int64_t>(std::llround(static_cast<double>(due) * V - ready));
        if (c.cushion) {
            const int d = cu.Picture(margin, play);
            if (d) { due = static_cast<uint32_t>(static_cast<int32_t>(due) + d); margin += static_cast<int64_t>(d) * kVblankUs; }
            lvlSum += cu.Level();
        }
        const uint32_t shown = static_cast<int32_t>(earliest - due) > 0 ? earliest : due;
        slots.Shown(shown);
        flipPrev = static_cast<double>(shown) * V;
        {   // PaceDisplay's turn-to-picture mean (EWMA 0.9 / 0.1, in vblanks) and its give-up: 2 pictures held 3..5 vblanks within 30 pictures of a drain end the drain for the stage
            const double lat = static_cast<double>(shown) - std::floor(start / V);
            latEw = latEw < 0.0 ? lat : latEw * 0.9 + lat * 0.1;
        }
        if (haveLast) {
            const uint32_t n = shown - last; ++r.hold[n < 5 ? n : 5]; ++r.pictures;
            if (n >= 3 && n <= 5 && sinceDrain < 30u && drainsTot > 0 && !giveUp && ++missAfterDrain >= 2) giveUp = true;
        }
        last = shown; haveLast = true;
    }
    r.engages = static_cast<unsigned>(cu.engages); r.releases = static_cast<unsigned>(cu.releases); r.gaveUp = static_cast<unsigned>(cu.gaveUpN);
    r.avoided = cu.avoided; r.nearPics = cu.nearPics;
    r.latencyAddedMs = c.N ? lvlSum / c.N * V / 1000.0 : 0;
    return r;
}
static unsigned Long(const Result& r) { return r.hold[3] + r.hold[4] + r.hold[5]; }

// ---- 1. the closed forms ------------------------------------------------------------------------------------------------------------------------------------------
static void TestClosedForm() {
    PR("closed forms (S1, no noise, no drain): a spike shorter than the margin is invisible; a longer one is one 3-vblank hold; the slot then moves a vblank later\n");
    // L chosen so the picture is ready 4 ms before its vblank: start (on the grid) + L; L = 2*V - 4000 - ... ready = k*V + (V - 4000)
    for (double spike : {2000.0, 3500.0, 5000.0, 9000.0}) {
        Cfg c; c.drain = false; c.noiseUs = 0; c.Lus = V - 4000.0; c.N = 200; c.coverFirst = 0;
        c.spikes = {{50, spike}, {100, spike}};
        const Result r = Run(c);
        const unsigned want = spike > 4000.0 ? 1u : 0u;   // the first spike is the only one that hits a thin margin; the slot then moved a vblank later (heals)
        CHECK(Long(r) == want, "spike %.1f ms: long holds %u, want %u", spike / 1000.0, Long(r), want);
        if (spike > 4000.0) CHECK(r.hold[3] == 1 && r.hold[2] >= r.pictures - 2, "holds %u/%u/%u", r.hold[2], r.hold[3], r.hold[4]);
        PR("  spike %.1f ms on a 4.0 ms margin: %u picture(s) held 3+ vblanks of %u\n", spike / 1000.0, Long(r), r.pictures);
    }
    // S3 slaved start: the next pair starts at the flip, so the margin is 2 vblanks - L whatever the slot does: both spikes hit
    {
        Cfg c; c.drain = false; c.noiseUs = 0; c.coupled = true; c.N = 200; c.coverFirst = 0;
        c.Lus = 2.0 * V - 4000.0;
        c.spikes = {{50, 5000}, {100, 5000}};
        const Result r = Run(c);
        PR("  S3 (slaved start): spike 5 ms on a 4 ms margin, twice: %u long holds\n", Long(r));
        CHECK(Long(r) == 2, "a slaved chain does not heal: %u", Long(r));
    }
}

// ---- 2. the cushion's rules ---------------------------------------------------------------------------------------------------------------------------------------
static void TestRules() {
    PR("cushion rules\n");
    {   // in play: 3 near pictures in the window engage; +1 vblank; verify passes when the margin grew
        Cushion c;
        int engaged = 0;
        int shift = 0;   // the slot moved a vblank later: the margins that follow have grown by 16.7 ms
        for (int i = 0; i < 20; ++i) { const int d = c.Picture((i % 4 == 0 ? 3000 : 12000) + shift, true); if (d) { ++engaged; shift = kVblankUs; CHECK(d == 1, "engage must return +1 (got %d)", d); } }
        CHECK(engaged == 1 && c.Level() == 1 && c.engages == 1, "engage: %d level %u", engaged, c.Level());
        CHECK(c.DrainFloor() == 3, "drain floor with the cushion: %u", c.DrainFloor());
        for (int i = 0; i < 40; ++i) c.Picture(20000 + (i % 3) * 1000, true);   // margins grew by a vblank: stays
        CHECK(c.Level() == 1 && !c.GaveUp(), "stays on: level %u gaveUp %d", c.Level(), c.GaveUp());
    }
    {   // fewer than 3 near pictures: nothing
        Cushion c;
        for (int i = 0; i < 200; ++i) c.Picture(i % 40 == 0 ? 3000 : 14000, true);
        CHECK(c.Level() == 0 && c.engages == 0, "two near pictures in a window must not engage (level %u)", c.Level());
    }
    {   // release after calmPics with the margin to spare, returns -1
        Config cf; cf.calmPics = 100;
        Cushion c(cf);
        for (int i = 0; i < 12 && c.Level() == 0; ++i) c.Picture(2000, true);
        CHECK(c.Level() == 1, "engaged");
        for (int i = 0; i < 20; ++i) c.Picture(2000 + kVblankUs, true);   // the verify window: the margin grew
        int rel = 0;
        for (int i = 0; i < 300; ++i) { const int d = c.Picture(30000, true); if (d) { ++rel; CHECK(d == -1, "release returns -1 (%d)", d); } }
        CHECK(rel == 1 && c.Level() == 0 && c.releases == 1, "release: %d level %u", rel, c.Level());
    }
    {   // no release while the margin without the cushion would be thin
        Config cf; cf.calmPics = 50;
        Cushion c(cf);
        for (int i = 0; i < 12 && c.Level() == 0; ++i) c.Picture(2000, true);
        for (int i = 0; i < 300; ++i) c.Picture(16683 + 3000, true);   // 3 ms without it: under the guard
        CHECK(c.Level() == 1, "thin without the cushion: stays");
    }
    {   // behind a cover: a thin phase engages at once, free
        Cushion c;
        int d = 0, n = 0;
        for (int i = 0; i < 8; ++i) { const int x = c.Picture(1500, false); if (x) { d = x; ++n; } }
        CHECK(n == 1 && d == 1 && c.engagesCover == 1, "cover engage: n=%d d=%d", n, d);
        Cushion c2;
        for (int i = 0; i < 8; ++i) c2.Picture(10000, false);
        CHECK(c2.Level() == 0, "a good phase behind a cover: nothing");
    }
    {   // verify: no gain -> goes back, once, and stays off for the stage
        Cushion c;
        for (int i = 0; i < 12 && c.Level() == 0; ++i) c.Picture(2000, true);
        CHECK(c.Level() == 1, "engaged");
        int rel = 0;
        for (int i = 0; i < 40; ++i) { const int d = c.Picture(2000, true); if (d == -1) ++rel; }   // margin still 2 ms: the chain is coupled
        CHECK(rel == 1 && c.Level() == 0 && c.GaveUp() && c.gaveUpN == 1, "verify gave up: rel %d level %u", rel, c.Level());
        for (int i = 0; i < 200; ++i) c.Picture(1000, true);
        CHECK(c.Level() == 0 && c.engages == 1, "stays off for the stage (engages %llu)", static_cast<unsigned long long>(c.engages));
        c.Reset();
        for (int i = 0; i < 12 && c.Level() == 0; ++i) c.Picture(2000, true);
        CHECK(c.Level() == 1, "a new stage may try again");
    }
    {   // census: avoided = margin with the cushion >= 0 but without it < 0
        Cushion c;
        for (int i = 0; i < 12 && c.Level() == 0; ++i) c.Picture(2000, true);
        const uint64_t a0 = c.avoided;
        c.Picture(V - 1000, true);       // 15.7 ms with, -1.0 ms without: a hold avoided
        c.Picture(V + 4000, true);       // fine either way
        CHECK(c.avoided == a0 + 1, "avoided %llu", static_cast<unsigned long long>(c.avoided - a0));
    }
    {   // the margin helper
        CHECK(MarginUs(10, 8, 1000000, 1000000 + 20000) == 1000000 + 2 * 16683 - 1000000 - 20000, "margin");
        CHECK(MarginUs(10, 8, 1000000, 1000000 + 40000) < 0, "late is negative");
    }
}

// ---- 3. the cushion in the chains ---------------------------------------------------------------------------------------------------------------------------------
static void TestChains() {
    PR("the cushion in the free chain (3000 pictures, spikes of 6 ms on a 3 ms margin every 150 pictures, no drain)\n");
    Cfg b; b.drain = false; b.noiseUs = 300; b.N = 3000; b.coverFirst = 0;
    for (unsigned i = 40; i < b.N; i += 150) b.spikes.push_back({i, 6000});
    {   // S1: the margin is 3 ms: the first spike is a hold, the slot heals by itself; with the cushion the margin is 3 + 16.7 from the start of play
        Cfg c = b; c.Lus = V - 3000.0;
        Result base = Run(c); c.cushion = true; Result cu = Run(c);
        PR("  S1: baseline long holds %u; cushion %u (engages %u, gave up %u, avoided %llu, mean +%.1f ms)\n", Long(base), Long(cu), cu.engages, cu.gaveUp, static_cast<unsigned long long>(cu.avoided), cu.latencyAddedMs);
        CHECK(Long(cu) <= Long(base) + cu.engages + cu.releases, "cushion in S1 must not add more holds than its own engagements");
    }
    {   // S3: a later slot buys nothing: the cushion engages once, the verify step sees no gain and gives up: at most two holds of its own, latency only for the verify window
        Cfg c = b; c.coupled = true; c.Lus = 2.0 * V - 3000.0;
        Result base = Run(c); c.cushion = true; Result cu = Run(c);
        PR("  S3: baseline long holds %u; cushion %u (engages %u, gave up %u, avoided %llu, mean +%.2f ms)\n", Long(base), Long(cu), cu.engages, cu.gaveUp, static_cast<unsigned long long>(cu.avoided), cu.latencyAddedMs);
        CHECK(cu.gaveUp == 1 && cu.engages == 1, "S3: the verify step must give up once (engaged %u, gave up %u)", cu.engages, cu.gaveUp);
        CHECK(Long(cu) <= Long(base) + 2, "S3: at most two holds of its own (%u vs %u)", Long(cu), Long(base));
        CHECK(cu.latencyAddedMs < 1.5, "S3: latency added only for the verify window (%.2f ms mean)", cu.latencyAddedMs);
    }
    {   // the drain and the cushion do not undo each other: a backlog of 3 vblanks with the cushion on drains to level+1
        Cfg c; c.Lus = 3000; c.noiseUs = 200; c.N = 1500; c.coverFirst = 0; c.cushion = true;
        Result r = Run(c);
        PR("  drain with a backlog (margin ~30 ms): drains %u, cushion engages %u\n", r.drains, r.engages);
        CHECK(r.engages == 0, "a roomy margin never engages the cushion (%u)", r.engages);
        Cushion cu; for (int i = 0; i < 12 && cu.Level() == 0; ++i) cu.Picture(1000, true);
        CHECK(cu.DrainFloor() == 3, "with the cushion on, the drain keeps one more vblank");
    }
}

// ---- 4. replay of the spikes in the logs --------------------------------------------------------------------------------------------------------------------------
static void TestTrace(const char* path) {
    std::vector<std::pair<std::string, std::pair<double, int>>> rows;
    if (FILE* f = std::fopen(path, "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            char tag[16]; double t; int ms;
            if (std::sscanf(line, "%15s %lf %d", tag, &t, &ms) == 3) rows.push_back({tag, {t, ms}});
        }
        std::fclose(f);
    }
    PR("trace replay: %zu logged hitch frames (36..100 ms) from the console logs; each books (interval - 33.3) ms onto the picture at that second x 30\n", rows.size());
    if (rows.empty()) { PR("  (trace file %s not found: skipped)\n", path); return; }
    for (const char* tag : {"c54", "c74"}) {
        std::vector<std::pair<unsigned, double>> sp;
        double tmax = 0;
        for (const auto& r : rows) if (r.first == tag) { sp.push_back({static_cast<unsigned>(r.second.first * 30.0), (r.second.second - 33.3) * 1000.0}); tmax = std::max(tmax, r.second.first); }
        if (sp.empty()) continue;
        const unsigned N = static_cast<unsigned>(tmax * 30.0) + 60;
        PR("  %s: %zu spikes in %.0f s (%.2f a second)\n", tag, sp.size(), tmax, sp.size() / std::max(1.0, tmax));
        PR("    margin ms | S1 base | S1 no drain | S1 cushion (engage/avoided/+ms) | S3 base | S3 cushion (engages/gave up)\n");
        unsigned tb1 = 0, tn1 = 0, tc1 = 0, tb3 = 0, tc3 = 0, n = 0;
        for (int mUs = 1000; mUs <= 16000; mUs += 3000) {
            Cfg c; c.N = N; c.spikes = sp; c.noiseUs = 300; c.coverFirst = 0; c.Lus = V - mUs;
            Result b1 = Run(c); Cfg nd = c; nd.drain = false; Result n1 = Run(nd); c.cushion = true; Result c1 = Run(c);
            Cfg d = c; d.cushion = false; d.coupled = true; d.Lus = 2.0 * V - mUs;
            Result b3 = Run(d); d.cushion = true; Result c3 = Run(d);
            PR("    %5.1f     | %5u   | %5u       | %5u (%u/%llu/%.1f)            | %5u   | %5u (%u/%u)\n", mUs / 1000.0, Long(b1), Long(n1), Long(c1), c1.engages, static_cast<unsigned long long>(c1.avoided), c1.latencyAddedMs, Long(b3), Long(c3), c3.engages, c3.gaveUp);
            tb1 += Long(b1); tn1 += Long(n1); tc1 += Long(c1); tb3 += Long(b3); tc3 += Long(c3); ++n;
            CHECK(Long(c1) <= Long(b1) + c1.engages + c1.releases + 1, "S1 cushion must not add holds beyond its own");
            CHECK(Long(c3) <= Long(b3) + 2 * (c3.engages + 1), "S3 cushion adds at most its own holds (%u vs %u)", Long(c3), Long(b3));
        }
        PR("    mean over the 6 phases (long holds, of %u pictures): S1 base %.1f, no drain %.1f, cushion %.1f; S3 base %.1f, cushion %.1f\n", N, tb1 / double(n), tn1 / double(n), tc1 / double(n), tb3 / double(n), tc3 / double(n));
    }
}

// ---- 5. the window ------------------------------------------------------------------------------------------------------------------------------------------------
static void TestWindow() {
    PR("q30sep window\n");
    SepWindow w;
    w.Picture(0, 20000, 6000);
    w.Picture(2, 20000, 6000);
    w.Picture(2, 4000, 6000);      // near
    w.Picture(3, -2000, 6000);     // late, a long hold
    w.DrainNext();
    w.Picture(1, 18000, 6000);     // a drain hold
    w.Picture(3, 9000, 6000);      // a long hold on time: not the margin's
    w.Picture(2, -500, 6000);      // late within 30 pictures of the drain
    CHECK(w.pictures == 7 && w.late == 2 && w.nearPics == 1 && w.drainHold == 1 && w.drainLate == 1 && w.lateHoldOther == 1 && w.drains == 1,
          "pictures %u late %u near %u drainHold %u drainLate %u other %u drains %u", w.pictures, w.late, w.nearPics, w.drainHold, w.drainLate, w.lateHoldOther, w.drains);
    CHECK(w.marginMin == -2000, "min %lld", static_cast<long long>(w.marginMin));
    CHECK(w.hold[1] == 1 && w.hold[2] == 3 && w.hold[3] == 2 && w.hold[0] == 1, "holds");
    char buf[512];
    w.Format(buf, sizeof buf, 5.0, 1, " (avoided 3)");
    CHECK(std::strstr(buf, "(a) late pictures 2") && std::strstr(buf, "1 drain holds") && std::strstr(buf, "cushion level 1 (avoided 3)"), "%s", buf);
    PR("  %s\n", buf);
    w.Clear();
    CHECK(w.pictures == 0 && w.late == 0, "clear");
    w.Picture(2, 5000, 6000);
    CHECK(w.drainLate == 0 && w.nearPics == 1, "after clear");
}


// ---- 6. replay of the console run QS2 (18 Nov, job 8202, GXV_Q30_CUSHION=1: the cushion never engaged) --------------------------------------------------------------
// Root cause: PaceDisplay measured the margin against the slot PresentationSlots::Next() RETURNS, which that call pulls forward to the next vblank for a late picture: a late picture
// had a margin of 0..16.7 ms and looked on time (QS2: 12 long holds, 0 late pictures, 5 near pictures in 86 windows, never 3 in 64). The fixed measure uses Nominal() (the slot the rhythm
// asked for). Here each logged picture goes through the REAL PresentationSlots (Next, Nominal) and MarginUsNominal, from its margin against the nominal slot.
// The fixture holds the logged histograms (tests/data/q30cush_qs2_windows.txt). A picture held 3 (4+) vblanks had, in the log, a margin of 0..16.7 ms (the clamped slot's): its true
// margin is that minus 1 (2) vblanks. Which pictures those were is not in the log: the smallest logged margins under 16.7 ms are taken (a long hold is some picture in 0..16.7 ms; any
// choice gives a negative true margin). The order within a window is not in the log either: SPREAD puts the thin pictures evenly across the window (the cushion's worst case), CLUSTERED
// puts them one after another in the middle (its best case).
struct ReplayRow { std::vector<int64_t> us; unsigned h3 = 0, h4 = 0, unk = 0; };
static bool LoadReplay(const char* path, std::vector<ReplayRow>& rows) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char tm[16]; unsigned n, a, b; int off = 0;
        if (std::sscanf(line, "%15s %u %u %u %n", tm, &n, &a, &b, &off) < 4 || !off) continue;
        ReplayRow r; r.h3 = a; r.h4 = b;
        for (char* tok = std::strtok(line + off, " \n"); tok; tok = std::strtok(nullptr, " \n")) {
            if (!std::strncmp(tok, "unk:", 4)) { r.unk = static_cast<unsigned>(std::atoi(tok + 4)); continue; }
            static const struct { const char* name; int64_t mid; } bin[] = {{"0-2", 1000}, {"2-4", 3000}, {"4-6", 5000}, {"6-8", 7000}, {"8-12", 10000}, {"12-16", 14000}, {"16-20", 18000},
                                                                              {"20-25", 22500}, {"25-30", 27500}, {"30-40", 35000}, {"40+", 45000}};
            char* colon = std::strchr(tok, ':');
            if (!colon) continue;
            *colon = 0;
            for (const auto& bb : bin) if (!std::strcmp(tok, bb.name)) for (int i = std::atoi(colon + 1); i > 0; --i) r.us.push_back(bb.mid);
        }
        rows.push_back(r);
    }
    std::fclose(f);
    return true;
}
struct ReplayOut { uint64_t engages = 0, lateSeen = 0, nearSeen = 0, pics = 0; };
// measureFixed: the cushion sees true margins (through Nominal); otherwise it sees what the log's measure saw (the clamped slot's margin, in 0..16.7 ms for a late picture)
static ReplayOut Replay(const std::vector<ReplayRow>& rows, bool clustered) {
    Cushion c;
    ReplayOut out;
    port::pace::PresentationSlots slots;
    const uint32_t V0 = 1000000;
    const uint64_t T0 = 4000000000ull;
    for (const ReplayRow& r : rows) {
        if (r.unk) continue;
        std::vector<int64_t> m = r.us;                 // margins as logged (ascending, bins in order)
        std::vector<int64_t> truth = m;
        unsigned need3 = r.h3, need4 = r.h4;
        for (size_t i = 0; i < truth.size() && (need3 || need4); ++i) {
            if (truth[i] >= 16683) break;
            if (need3) { truth[i] -= kVblankUs; --need3; }
            else { truth[i] -= 2 * kVblankUs; --need4; }
        }
        // order: thin ones (truth < 6 ms) placed by the mode, the rest after the window's order of the log (ascending: irrelevant to the rule)
        std::vector<int64_t> thin, rest;
        for (int64_t t : truth) (t < 6000 ? thin : rest).push_back(t);
        std::vector<int64_t> seq(truth.size());
        std::vector<bool> used(seq.size(), false);
        const size_t n = seq.size(), k = thin.size();
        for (size_t i = 0; i < k; ++i) {
            size_t pos = clustered ? n / 2 + i : static_cast<size_t>((i + 0.5) * n / k);
            while (pos < n && used[pos]) ++pos;
            if (pos >= n) { pos = 0; while (used[pos]) ++pos; }
            seq[pos] = thin[i]; used[pos] = true;
        }
        size_t ri = 0;
        for (size_t i = 0; i < n; ++i) if (!used[i]) seq[i] = rest[ri++];
        for (int64_t mt : seq) {
            // the picture's real path: ready `mt` before the start of the nominal slot (due = V0 + 4, the grid anchor at T0 = vblank V0)
            const double ready = static_cast<double>(T0) + 4.0 * V - static_cast<double>(mt);
            const uint32_t count = V0 + static_cast<uint32_t>(std::floor((ready - static_cast<double>(T0)) / V));
            slots.Shown(V0 + 2);
            const uint32_t due = slots.Next(count + 1, 2);
            const uint32_t clampAdv = due - slots.Nominal();
            const int64_t margin = MarginUsNominal(due, clampAdv, V0, T0, static_cast<uint64_t>(ready));
            ++out.pics;
            if (margin < 0) ++out.lateSeen; else if (margin < 6000) ++out.nearSeen;
            const int d = c.Picture(margin, true);
            (void)d;
        }
    }
    out.engages = c.engages;
    return out;
}
static const char* g_replayPath = "tests/data/q30cush_qs2_windows.txt";
static void TestConsoleReplay() {
    std::vector<ReplayRow> rows;
    if (!LoadReplay(g_replayPath, rows)) { CHECK(false, "console fixture %s not found", g_replayPath); return; }
    PR("console replay (QS2, 18 Nov): %zu windows from the log, every picture through the real PresentationSlots and MarginUsNominal\n", rows.size());
    const ReplayOut spread = Replay(rows, false), clustered = Replay(rows, true);
    PR("  spread:    pictures %llu, late %llu, near %llu, cushion engaged %llu\n", (unsigned long long)spread.pics, (unsigned long long)spread.lateSeen, (unsigned long long)spread.nearSeen, (unsigned long long)spread.engages);
    PR("  clustered: pictures %llu, late %llu, near %llu, cushion engaged %llu\n", (unsigned long long)clustered.pics, (unsigned long long)clustered.lateSeen, (unsigned long long)clustered.nearSeen, (unsigned long long)clustered.engages);
    CHECK(clustered.lateSeen >= 8, "the long holds must be seen as late pictures: %llu", (unsigned long long)clustered.lateSeen);
    CHECK(clustered.engages >= 1, "the cushion must engage on the logged margins (clustered): %llu", (unsigned long long)clustered.engages);
    // what the old measure saw (no late picture, 0..16.7 ms for the held ones): never engages, as on the console
    {
        std::vector<ReplayRow> old = rows;   // the log's own margins, no correction: every long hold is a picture of 0..16.7 ms
        for (ReplayRow& r : old) { r.h3 = 0; r.h4 = 0; }
        const ReplayOut a = Replay(old, false), b = Replay(old, true);
        PR("  as the console measured it (the log's own margins): engaged %llu (spread), %llu (clustered)\n", (unsigned long long)a.engages, (unsigned long long)b.engages);
        CHECK(a.engages == 0 && b.engages == 0, "the logged (clamped) margins never reach 3 near in 64: engaged %llu / %llu", (unsigned long long)a.engages, (unsigned long long)b.engages);
    }
}

static void All(const char* trace) {
    TestClosedForm();
    TestRules();
    TestChains();
    TestWindow();
    TestTrace(trace);
    TestConsoleReplay();
}

int main(int argc, char** argv) {
    const char* trace = argc > 1 ? argv[1] : "tests/data/q30cush_hitch_trace.txt";
    if (argc > 2) g_replayPath = argv[2];
    gMutant = 0;
    All(trace);
    if (fails) { PR("q30cushion: FAIL (%d) before the mutants\n", fails); return 1; }
    PR("mutants (each must be caught):\n");
    const char* what[] = {"", "engage returns no vblank", "release returns no vblank", "no engagement in play", "drain floor not raised", "verify never gives up", "margin against the clamped slot"};
    int caught = 0;
    g_verbose = false;
    for (int m = 1; m <= 6; ++m) {
        gMutant = m; fails = 0;
        g_quiet = true;
        All("/nonexistent");
        g_quiet = false;
        const int f = fails;
        gMutant = 0;
        PR("  mutant %d (%s): %s (%d checks failed)\n", m, what[m], f ? "caught" : "NOT CAUGHT", f);
        if (f) ++caught;
    }
    if (caught != 6) { PR("q30cushion: FAIL (%d of 6 mutants caught)\n", caught); return 1; }
    PR("q30cushion: PASS\n");
    return 0;
}
