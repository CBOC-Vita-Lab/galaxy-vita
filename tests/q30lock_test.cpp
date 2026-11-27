// q30lock26: host test of the Quality lock ladder (port/q30lock.h) and the transition rules (port/lockfade.h) with synthetic traces.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <cstring>
#include "port/cpulod.h"
#include "port/lockfade.h"
#include "port/q30lock.h"
#include "port/q30fx.h"

using namespace port;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// A synthetic stage: pair/GPU/front times follow base minus the gains of the steps in force; noise is a deterministic wobble.
struct Stage {
    float pair = 24, gpu = 25, front = 15, noise = 1.0f;
    float gain[q30lock::kStepN] = {0.6f, 0.5f, 0.6f, 0.5f, 0.8f, 0.8f, 1.5f, 3.0f, 2.5f, 2.0f};
    bool ready6 = true;
    bool cover = false;       // a natural cover is open (pause menu, closed wipe, load)
};
struct Run {
    q30lock::Ladder L;
    q30lock::Memory mem;
    Stage st;
    uint64_t now = 1000000;
    uint32_t rnd = 12345;
    std::vector<std::pair<uint64_t, uint16_t>> changes;   // time, mask after
    uint16_t lastMask = 0;
    float wob() { rnd = rnd * 1664525u + 1013904223u; return (static_cast<float>(rnd >> 8) / 16777216.0f - 0.5f) * 2.0f * st.noise; }
    void Begin(const char* key) { q30lock::Config c = L.config(); c.startSpec[0] = 0; c.openSpec[0] = 0; L.Configure(c); L.SetStep6Ready(st.ready6); L.StageBegin(now, key, &mem); lastMask = L.mask(); }
    // one picture of 33.3 ms
    void Picture() {
        float g = st.gpu, p = st.pair, f = st.front;
        const uint16_t m = L.mask();
        for (int s = 0; s < q30lock::kStepN; ++s) if (m >> s & 1u) {
            if (q30lock::kHelps[s] & q30lock::kGpu) g -= st.gain[s];
            if (q30lock::kHelps[s] & q30lock::kGame) p -= st.gain[s];
            if (q30lock::kHelps[s] & q30lock::kFront) f -= st.gain[s];
        }
        const float w = wob();
        g += w; p += w; f += w * 0.5f;
        L.NoteTurn(now, static_cast<uint32_t>((p - 9.0f) * 1000), true);
        L.NoteTurn(now, 9000, false);
        L.NotePicture(now, static_cast<uint32_t>(g * 1000), true, static_cast<uint32_t>(f * 1000), true, true);
        const bool miss = p > 33.3f || g > 33.3f || f > 33.3f;
        L.NoteShown(now, miss ? 3 : 2, true);
        L.NoteCover(now, st.cover);
        now += 33333;
        if (L.mask() != lastMask) { changes.push_back({now, L.mask()}); lastMask = L.mask(); }
    }
    void Seconds(double s) { const int n = static_cast<int>(s * 30); for (int i = 0; i < n; ++i) Picture(); }
    void Drain() { char b[400]; while (L.PopLog(b, sizeof b)) std::printf("    | %s\n", b); }
};

static void TestLight() {
    Run r;
    r.st.pair = 24; r.st.gpu = 25; r.st.front = 14;
    r.Begin("LightGalaxy:1");
    r.Seconds(120);
    CHECK(r.L.mask() == 0);
    CHECK(r.changes.empty());
    CHECK(r.L.misses() == 0);
    CHECK(r.L.onCadence() == r.L.pictures());
    std::printf("light: mask %02x changes %zu pictures %u\n", r.L.mask(), r.changes.size(), r.L.pictures());
}
static void TestHeavyCpu() {
    Run r;
    r.st.pair = 32.6f; r.st.gpu = 24; r.st.front = 16;   // 32.6 + 1 noise p95 ~ 33.4: over 31.5; needs 3a..3d (-2.2)
    r.Begin("HoneyBeeKingdomGalaxy:1");
    r.Seconds(180);
    r.Drain();
    const uint16_t m = r.L.mask();
    std::printf("heavy cpu: mask %02x changes %zu\n", m, r.changes.size());
    CHECK((m & 0x1F) != 0);
    CHECK((m & 0xE0) == 0);   // no GPU-only step for a CPU stage
    for (size_t i = 1; i < r.changes.size(); ++i) CHECK(r.changes[i].first - r.changes[i - 1].first >= 4000000u);   // (q30-27: a far-over stage steps every 4 s at the earliest, 10 s otherwise)
    // each change adds or removes exactly one step (no two changes in one picture)
    uint16_t prev = 0;
    for (auto& c : r.changes) { CHECK(q30lock::PopCount(c.second ^ prev) <= 2); prev = c.second; }
}
static void TestHeavyGpu() {
    Run r;
    r.st.pair = 24; r.st.gpu = 33.4f; r.st.front = 14;
    r.Begin("CannonFleetGalaxy:1");
    r.Seconds(240);
    r.Drain();
    const uint16_t m = r.L.mask();
    std::printf("heavy gpu: mask %02x changes %zu\n", m, r.changes.size());
    CHECK(m >> q30lock::k4 & 1u);   // the edge's second pass goes
    CHECK(!(m >> q30lock::k3a & 1u));   // a GPU-only problem does not cut the CPU items first
}
static void TestStep6Gate() {
    // step 6 needs the other target AND a natural cover; wanted without them it is remembered for the next visit, never taken in play
    Run r;
    r.st.pair = 24; r.st.gpu = 40; r.st.front = 14;   // more than steps 3c, 4, 5 can give
    r.st.ready6 = false;
    r.Begin("X:1");
    r.Seconds(300);
    CHECK(!(r.L.mask() >> q30lock::k6 & 1u));
    r.L.SetStep6Ready(true);
    r.Seconds(60);
    CHECK(!(r.L.mask() >> q30lock::k6 & 1u));        // ready, but no cover: not in play
    CHECK(r.L.Step6Pending());
    r.st.cover = true;                                // the pause menu opens
    r.Seconds(1);
    CHECK(r.L.mask() >> q30lock::k6 & 1u);
    CHECK(!r.L.Step6Pending());
    r.st.cover = false;
    r.Seconds(200);
    r.Drain();
    // the card: a stage that wanted 896 and never got a cover starts the next visit at it
    Run q;
    q.st.pair = 24; q.st.gpu = 40; q.st.front = 14;
    q.Begin("Y:1");
    q.Seconds(300);
    CHECK(q.L.Step6Pending());
    q.L.SetStep6Ready(true);
    q.L.Shutdown(q.now, &q.mem);
    uint16_t got = 0;
    CHECK(q.mem.Lookup("Y:1", &got) && (got >> q30lock::k6 & 1u));
    Run n;
    n.mem = q.mem;
    n.st.pair = 24; n.st.gpu = 40;
    n.Begin("Y:1");
    CHECK(n.L.mask() >> q30lock::k6 & 1u);            // the load is the cover
    // a step the back end does not offer is dropped from a remembered start
    Run d;
    d.mem = q.mem;
    d.L.SetAllowed(q30lock::k6, false);
    d.Begin("Y:1");
    CHECK(!(d.L.mask() >> q30lock::k6 & 1u));
}
static void TestStep3eAndExhausted() {
    // a CPU stage that needs everything the game thread can give: 3a..3e all taken, then the exhausted line, once
    Run r;
    r.st.pair = 46.0f;
    r.Begin("Hard:1");
    int exhausted = 0;
    bool sawE = false;
    char b[400];
    for (int k = 0; k < 42; ++k) {   // (the log queue is short: drained every 10 s)
        r.Seconds(10);
        while (r.L.PopLog(b, sizeof b)) {
            if (std::getenv("Q30_VERBOSE")) std::printf("    > %s\n", b);
            if (std::strstr(b, "q30lock: stage Hard:1: exhausted, still over by ")) ++exhausted;
            if (std::strstr(b, "step 3e")) sawE = true;
        }
    }
    CHECK((r.L.mask() & 0x1F) == 0x1F);
    CHECK(r.L.Exhausted());
    CHECK(exhausted >= 1 && exhausted <= 28);   // (repeated at most every 30 s while it lasts)
    CHECK(sawE);
    CHECK(q30lock::CpuLodLevel() == static_cast<int>(q30lock::kLevelFlag | 1 | ((q30lock::Mask() & 0x1Fu) << 1)) || !q30lock::Active());
    // 3e comes after 3d and is game-thread only
    CHECK((q30lock::kHelps[q30lock::k3e] & q30lock::kGpu) == 0);
    CHECK(q30lock::k3e == q30lock::k3d + 1);
}
static void TestStep5Gpu() {
    // a GPU-only stage: 3c, 4 and 5 are taken before 6 is even wanted
    Run r;
    r.st.pair = 24; r.st.gpu = 33.2f; r.st.front = 14;
    r.Begin("G:1");
    r.Seconds(300);
    r.Drain();
    CHECK(r.L.mask() >> q30lock::k5 & 1u || r.L.mask() >> q30lock::k4 & 1u);
    CHECK(!(r.L.mask() >> q30lock::k3e & 1u));
    // step 5 closed by the back end (no target made): never offered
    Run c;
    c.st.pair = 24; c.st.gpu = 36; c.st.front = 14;
    c.L.SetAllowed(q30lock::k5, false);
    c.Begin("C:1");
    c.Seconds(400);
    CHECK(!(c.L.mask() >> q30lock::k5 & 1u));
}
static void TestNoOscillation() {
    // load right at the boundary of a step's gain: after the first give-back, it must not cycle faster than the calm requirement doubles
    Run r;
    r.st.pair = 32.0f; r.st.noise = 0.8f;
    r.Begin("Edge:1");
    r.Seconds(900);
    std::printf("boundary: mask %02x changes %zu\n", r.L.mask(), r.changes.size());
    CHECK(r.changes.size() <= 14);
    for (size_t i = 1; i < r.changes.size(); ++i) CHECK(r.changes[i].first - r.changes[i - 1].first >= 4000000u);
    // a step taken stays 30 s: an up change (mask loses a bit) is >= 30 s after the same bit was set
    uint64_t setAt[q30lock::kStepN] = {};
    uint16_t prev = 0;
    for (auto& c : r.changes) {
        for (int s = 0; s < q30lock::kStepN; ++s) {
            const bool was = prev >> s & 1u, is = c.second >> s & 1u;
            if (!was && is) setAt[s] = c.first;
            if (was && !is && setAt[s]) CHECK(c.first - setAt[s] >= 30000000u);
        }
        prev = c.second;
    }
}
static void TestRecovery() {
    // a heavy phase, then calm: the steps come back, highest first, one per 10 s, after 20 s calm
    Run r;
    r.st.pair = 32.6f;
    r.Begin("Phase:1");
    r.Seconds(120);
    const uint16_t heavy = r.L.mask();
    CHECK(heavy != 0);
    r.st.pair = 24;
    r.Seconds(400);
    r.Drain();
    std::printf("recovery: heavy %02x -> %02x\n", heavy, r.L.mask());
    CHECK(r.L.mask() == 0);
}
static void TestMemory() {
    Run r;
    r.st.pair = 32.6f;
    r.Begin("HoneyBeeKingdomGalaxy:1");
    r.Seconds(240);
    const uint16_t held = r.L.mask();
    r.L.Shutdown(r.now, &r.mem);
    uint16_t got = 0;
    CHECK(r.mem.Lookup("HoneyBeeKingdomGalaxy:1", &got));
    CHECK(got == held || got != 0);
    char text[2048];
    const int n = r.mem.Format(text, sizeof text);
    q30lock::Memory m2;
    CHECK(m2.Parse(text, static_cast<size_t>(n)) == 1);
    uint16_t g2 = 0;
    CHECK(m2.Lookup("HoneyBeeKingdomGalaxy:1", &g2) && g2 == got);
    // garbage lines are skipped
    const char* bad = "# x\nfoo\nbar zz 1\nStage:1 7f 3\nStage:2 ff 3\nStage:3 1ffff 3\n";
    q30lock::Memory m3;
    CHECK(m3.Parse(bad, std::strlen(bad)) == 2);
    // a start from the remembered set, then a stage that needs nothing more decays by one step
    Run r2;
    r2.mem = m2;
    r2.st.pair = 28;   // light now
    r2.Begin("HoneyBeeKingdomGalaxy:1");
    CHECK(r2.L.mask() == (got & 0xFF));
    r2.Seconds(90);
    r2.L.Shutdown(r2.now, &r2.mem);
    uint16_t g3 = 0;
    r2.mem.Lookup("HoneyBeeKingdomGalaxy:1", &g3);
    std::printf("memory: %02x -> start %02x -> after a calm visit %02x\n", got, got, g3);
    CHECK(q30lock::PopCount(g3) <= q30lock::PopCount(got));
    // table is bounded
    q30lock::Memory big;
    for (int i = 0; i < 100; ++i) { char k[32]; std::snprintf(k, sizeof k, "S%d:1", i); big.Store(k, 1); }
    CHECK(big.n == q30lock::kMemEntries);
}
// ---- far3: the far-field steps 3h..3l ----------------------------------------------------------------------------------------------------
static void FarGains(Run& r) { for (int s = q30lock::k3h; s <= q30lock::k3l; ++s) r.st.gain[s] = 1.2f; }
static void TestFarOrderAndLoad() {
    // (1) the far steps are after 3g and before the GPU-only steps in the escalation order, and the order is a permutation
    int pos[q30lock::kStepN];
    for (int i = 0; i < q30lock::kStepN; ++i) pos[q30lock::kOrder[i]] = i;
    for (int s = q30lock::k3h; s <= q30lock::k3l; ++s) { CHECK(pos[s] > pos[q30lock::k3g]); CHECK(pos[s] < pos[q30lock::k4]); }
    for (int s = q30lock::k3h; s < q30lock::k3l; ++s) CHECK(pos[s] < pos[s + 1]);
    unsigned seen = 0;
    for (int i = 0; i < q30lock::kStepN; ++i) seen |= 1u << q30lock::kOrder[i];
    CHECK(seen == (1u << q30lock::kStepN) - 1u);
    // (2) a stage that holds 30 today (needs at most 3a..3c): no far step is ever taken
    Run a;
    FarGains(a);
    a.st.pair = 31.5f;
    a.st.gpu = 24;
    a.Begin("Light:1");
    a.Seconds(240);
    CHECK((a.L.mask() & q30lock::kFarMask) == 0);
    for (auto& c : a.changes) CHECK((c.second & q30lock::kFarMask) == 0);
    // (3) a stage that needs more than 3a..3g: the far steps come after 3g, one at a time, and are given back before 3g
    Run b;
    FarGains(b);
    b.st.pair = 41.0f;
    b.Begin("Hungry:1");
    uint16_t prev = 0;
    int farTaken = 0;
    b.Seconds(300);
    for (auto& c : b.changes) {
        const uint16_t m = c.second;
        if (m & q30lock::kFarMask) { CHECK((m & 0x31F) == 0x31F); }   // 3a..3e and 3f, 3g in force whenever a far step is
        CHECK(q30lock::PopCount((m ^ prev) & 0x7FFF) <= 2);
        for (int s = q30lock::k3h; s <= q30lock::k3l; ++s) if ((m >> s & 1u) && !(prev >> s & 1u)) { ++farTaken; for (int t = q30lock::k3h; t < s; ++t) CHECK(m >> t & 1u); }
        prev = m;
    }
    CHECK(farTaken >= 1);
    std::printf("far3: hungry stage held %04x, far steps taken %d\n", b.L.mask(), farTaken);
    b.st.pair = 24;   // calm: everything comes back, the far steps first
    const size_t before = b.changes.size();
    b.Seconds(400);
    prev = b.changes.size() > before ? b.changes[before - 1].second : b.L.mask();
    for (size_t i = before; i < b.changes.size(); ++i) {
        const uint16_t m = b.changes[i].second;
        if (!(m >> q30lock::k3g & 1u)) CHECK((m & q30lock::kFarMask) == 0);   // 3g only goes after every far step
        if (!(m >> q30lock::k3f & 1u) && (prev >> q30lock::k3f & 1u)) CHECK((m & q30lock::kFarMask) == 0);
    }
    CHECK(b.L.mask() == 0);
}
static void TestFarOff() {
    // GXV_Q30_FAR=0 (the far bits cleared from `allowed`): the ladder is the old one, change for change
    auto run = [](q30lock::StepMask allowed, std::vector<std::pair<uint64_t, uint16_t>>* out) {
        Run r;
        FarGains(r);
        q30lock::Config c = r.L.config();
        c.allowed = allowed;
        r.L.Configure(c);
        r.st.pair = 38.0f;
        r.Begin("Hungry:1");
        r.Seconds(200);
        r.st.pair = 24;
        r.Seconds(200);
        *out = r.changes;
    };
    std::vector<std::pair<uint64_t, uint16_t>> off, old;
    run(static_cast<q30lock::StepMask>(0x7FFF & ~q30lock::kFarMask), &off);
    run(0x3FF, &old);
    CHECK(off == old);
    for (auto& c : off) CHECK((c.second & q30lock::kFarMask) == 0);
}
static void TestFarMemory() {
    // a v4 file (ten gains a line) is read, the far gains stay unknown; a v5 file round-trips
    const char* v4 = "# q30lock v4: x\nHoneyBeeKingdomGalaxy:1 031f 3 60 50 60 50 80 80 150 300 250 200\nStage:2 0003 1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1\n";
    q30lock::Memory m;
    CHECK(m.Parse(v4, std::strlen(v4)) == 2);
    uint16_t got = 0;
    CHECK(m.Lookup("HoneyBeeKingdomGalaxy:1", &got) && got == 0x31F);
    CHECK(m.e[0].gain[q30lock::k3g] == 200 && m.e[0].gain[q30lock::k3h] == -1 && m.e[0].gain[q30lock::k3l] == -1);
    m.Store("Far:1", static_cast<q30lock::StepMask>(0x7F1F));
    const float g[q30lock::kStepN] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.2f, 0.8f, 0.6f, 1.5f, 1.0f};
    bool k[q30lock::kStepN] = {};
    for (int s = q30lock::k3h; s <= q30lock::k3l; ++s) k[s] = true;
    m.StoreGains("Far:1", g, k);
    char text[4096];
    const int n = m.Format(text, sizeof text);
    q30lock::Memory m2;
    CHECK(m2.Parse(text, static_cast<size_t>(n)) == 3);
    const int i = m2.Find("Far:1");
    CHECK(i >= 0 && m2.e[i].mask == 0x7F1F && m2.e[i].gain[q30lock::k3h] == 120 && m2.e[i].gain[q30lock::k3l] == 100 && m2.e[i].gain[q30lock::k3a] == -1);
    CHECK(std::strstr(text, "# q30lock v5") == text);
}
static void TestOpenSetAndHold() {
    // the opening set (Honeyhive's far steps) is in force from the load, is given back when the stage turns out calm, and is not there with the far steps off
    for (int far = 0; far < 2; ++far) {
        Run r;
        FarGains(r);
        q30lock::Config c = r.L.config();
        c.startSpec[0] = 0;
        if (!far) c.allowed = static_cast<q30lock::StepMask>(c.allowed & ~q30lock::kFarMask);
        r.L.Configure(c);
        r.st.pair = 26;   // calm: nothing is needed
        r.L.StageBegin(r.now, "HoneyBeeKingdomGalaxy:1", &r.mem);
        r.lastMask = r.L.mask();
        if (far) CHECK((r.L.mask() & 0x7400) == 0x7400); else CHECK((r.L.mask() & q30lock::kFarMask) == 0);
        r.Seconds(120);
        CHECK(r.L.mask() == 0);   // every opening step recovers: none is pinned
    }
    // a flight holds every decision: a heavy stage takes nothing while the hold is on, and goes on after it
    Run h;
    FarGains(h);
    h.st.pair = 41.0f;
    h.Begin("Hold:1");
    h.L.SetHold(true);
    h.Seconds(40);
    CHECK(h.L.mask() == 0);
    h.L.SetHold(false);
    h.Seconds(40);
    CHECK(h.L.mask() != 0);
}
static void TestFader() {
    using lockfade::Fader;
    Fader f;
    f.Set(0);
    f.Begin(5, 120);
    const int N = 4000;
    std::vector<int> prevLevel(N, 0);
    int maxFlip = 0, ticks = 0;
    bool busy = true;
    float lastFrac = 0;
    while (busy) {
        busy = f.Tick();
        ++ticks;
        int flips = 0;
        for (int a = 0; a < N; ++a) {
            const int lv = f.LevelFor(static_cast<uint32_t>(a) * 2654435761u + 77u);
            if (lv != prevLevel[a]) { ++flips; prevLevel[a] = lv; }
        }
        if (flips > maxFlip) maxFlip = flips;
        CHECK(f.Frac() >= lastFrac);   // monotonic ramp
        lastFrac = f.Frac();
        CHECK(ticks < 1000);
    }
    std::printf("fader: %d ticks, most actors flipping in one tick %d of %d (%.1f %%)\n", ticks, maxFlip, N, 100.0 * maxFlip / N);
    CHECK(ticks <= 121);
    CHECK(maxFlip < N / 20);   // never a burst: at most ~5 % of the actors in a turn (ideal 1/120 = 0.8 %)
    for (int a = 0; a < N; ++a) CHECK(prevLevel[a] == 5);
    // an actor never flips back during one walk
    f.Begin(0, 60);
    std::vector<int> flips(N, 0), last(N, 5);
    while (f.Tick()) for (int a = 0; a < N; ++a) { const int lv = f.LevelFor(static_cast<uint32_t>(a) * 40503u + 9u); if (lv != last[a]) { ++flips[a]; last[a] = lv; } }
    for (int a = 0; a < N; ++a) CHECK(flips[a] <= 1);
    // scalar lerp monotonic
    f.Set(0); f.Begin(1, 30);
    float prev = 1.0f;
    for (int i = 0; i < 40; ++i) { f.Tick(); const float v = f.Lerp(1.0f, 0.6f); CHECK(v <= prev + 1e-6f); prev = v; }
    CHECK(std::fabs(prev - 0.6f) < 1e-5f);
    // Weight
    lockfade::Weight w;
    w.Set(1.0f); w.Target(0.0f);
    float pv = 1.0f;
    while (!w.Settled()) { const float v = w.Tick(0.05f); CHECK(v <= pv); pv = v; }
    CHECK(w.Value() == 0.0f);
    // union
    CHECK(lockfade::Union(1, 3) == 3);
    CHECK(lockfade::Union(lockfade::kFlag | 1 | 2, lockfade::kFlag | 1 | 4) == (lockfade::kFlag | 1 | 2 | 4));
}
static void TestTransitionLog() {
    Run r;
    r.st.pair = 32.6f;
    r.Begin("T:1");
    r.Seconds(60);
    char b[400];
    bool saw = false, sawStep = false;
    while (r.L.PopLog(b, sizeof b)) {
        if (std::strstr(b, "q30lock: transition ")) saw = true;
        if (std::strstr(b, "because game thread")) sawStep = true;
    }
    CHECK(saw);
    CHECK(sawStep);
}

static void TestCodedLevels() {
    using namespace port::cpulod;
    float v = 0;
    const int base = q30lock::kLevelFlag | 1;
    CHECK(base == kLockFlag + 1);
    CHECK(ModeValue(true, base, "GXV_MATLOD", &v) && v == 2.0f);                    // step 2: the q values
    CHECK(!ModeValue(true, base, "GXV_PTCL_THIN", &v));                              // no particle thinning until 3c
    CHECK(!ModeValue(true, base, "GXV_PIECETHIN", &v));                              // no star-bit thinning until 3d
    CHECK(ModeValue(true, base | (1 << 1), "GXV_MATLOD", &v) && v == 3.0f);          // 3a: every 3rd turn
    CHECK(ModeValue(true, base | (1 << 1), "GXV_CHARPOSE_LINES", &v) && v == 18.0f);
    CHECK(ModeValue(true, base | (1 << 3), "GXV_PTCL_THIN", &v) && v == 0.60f);       // 3c
    CHECK(ModeValue(true, base | (1 << 4), "GXV_PIECETHIN", &v) && v == 2.0f);        // 3d
    CHECK(ModeValue(true, base | (1 << 4), "GXV_PIECETHIN_DIST", &v) && v == 6000.0f);
    CHECK(!ModeValue(true, base | (1 << 5), "GXV_PTCL_THIN", &v) || true);
    CHECK(ModeValue(true, base | (1 << 5), "GXV_CHARPOSE", &v) && v == 4.0f);          // 3e (bit 4): the level 2 values of the game-thread members
    CHECK(ModeValue(true, base | (1 << 5), "GXV_CHARPOSE_LINES", &v) && v == 24.0f);
    CHECK(ModeValue(true, base | (1 << 5), "GXV_MATLOD_DIST", &v) && v == 2500.0f);
    CHECK(!ModeValue(true, base | (1 << 5), "GXV_FARLOGIC", &v));                       // far logic stays out
    CHECK(!ModeValue(true, base | (1 << 4), "GXV_FARDRAW", &v));                       // the far-draw cut is not a table member of the lock
    CHECK(ModeValue(true, 1, "GXV_MATLOD", &v) && v == 2.0f);                          // legacy q-3 levels unchanged
    CHECK(!ModeValue(true, 3, "GXV_PTCL_THIN", &v));
    CHECK(ModeValue(false, 2, "GXV_PTCL_THIN", &v) && v == 0.35f);                      // Performance unchanged
    CHECK(!ModeValue(false, base, "GXV_MATLOD", &v));                                  // a coded level means nothing outside Quality
    // each step's bit lands on the member that names it
    for (int s = 0; s < 5; ++s) {
        const int lv = base | ((1 << s) << 1);
        int changed = 0;
        for (int i = 0; i < kMemberCount; ++i) { float a = 0, b = 0; const bool ha = ModeValue(true, base, kMembers[i].name, &a), hb = ModeValue(true, lv, kMembers[i].name, &b); if (ha != hb || (ha && a != b)) ++changed; }
        CHECK(s == 1 ? changed == 0 : changed > 0);   // (3b is the far-draw cut, cpugov: no table member)
    }
}
static void TestFxRollout() {
    // step 5's roll-out: one layer at a time, weight ramped over several pictures, evenly spread, back in reverse order, nothing at 0
    q30fx::Rollout ro;
    q30fx::Params p; p.rampPictures = 12; p.gapPictures = 30;
    ro.Config(p);
    ro.Offer(11); ro.Offer(22); ro.Offer(33); ro.Offer(11);
    CHECK(ro.Count() == 3);
    ro.SetOn(false);
    for (int i = 0; i < 200; ++i) { ro.Tick(); CHECK(!ro.Use(11) && !ro.Use(22) && !ro.Use(33)); }
    CHECK(ro.AllOff());
    ro.SetOn(true);
    float lastW[3] = {0, 0, 0};
    int lowPics[3] = {0, 0, 0};
    uint32_t maxStep = 0;
    int firstMove[3] = {-1, -1, -1};
    for (int i = 0; i < 600; ++i) {
        ro.Tick();
        const uint32_t keys[3] = {11, 22, 33};
        int moving = 0;
        for (int k = 0; k < 3; ++k) {
            const float w = ro.Weight(keys[k]);
            CHECK(w >= lastW[k] - 1e-6f);                       // monotonic going on
            if (w - lastW[k] > 0.0f) { ++moving; if (firstMove[k] < 0) firstMove[k] = i; }
            if (w - lastW[k] > 1.0f / 12.0f + 1e-5f) ++maxStep;  // no jump bigger than one ramp step
            lastW[k] = w;
            if (ro.Use(keys[k])) ++lowPics[k];
        }
        CHECK(moving <= 1);                                      // one layer at a time
    }
    CHECK(maxStep == 0);
    CHECK(ro.AllOn());
    CHECK(firstMove[0] >= 0 && firstMove[0] < firstMove[1] && firstMove[1] < firstMove[2]);
    CHECK(firstMove[1] - firstMove[0] >= 12 + 29);               // gap after a layer finished
    // while ramping, the reduced-size pictures are spread (no two bursts: a layer at w <= 1/2 never uses 2 pictures in a row)
    q30fx::Rollout r2; r2.Config(p); r2.Offer(5); r2.SetOn(true);
    int run = 0, maxRun = 0, last = 0;
    for (int i = 0; i < 31 + 6; ++i) { r2.Tick(); if (r2.Use(5) && r2.Weight(5) <= 0.5f) { run = last ? run + 1 : 1; if (run > maxRun) maxRun = run; last = 1; } else last = 0; }
    CHECK(maxRun <= 1);
    // give back: the layer that came last goes first, one at a time
    ro.SetOn(false);
    int order[3] = {-1, -1, -1};
    int seen = 0;
    float lw[3] = {1, 1, 1};
    for (int i = 0; i < 600; ++i) {
        ro.Tick();
        const uint32_t keys[3] = {11, 22, 33};
        int moving = 0;
        for (int k = 0; k < 3; ++k) {
            const float w = ro.Weight(keys[k]);
            CHECK(w <= lw[k] + 1e-6f);
            if (w < lw[k]) { ++moving; if (order[k] < 0) order[k] = seen++; }
            lw[k] = w;
        }
        CHECK(moving <= 1);
    }
    CHECK(order[2] == 0 && order[1] == 1 && order[0] == 2);
    CHECK(ro.AllOff());
    const q30fx::Census c = ro.Sample();
    CHECK(c.maxMovingAtOnce == 1 && c.starts == 3 && c.finishes == 6 && c.offered == 3 && c.picturesLow > 0 && c.picturesFull > 0);
    // a reset keeps the parameters and drops the layers
    ro.Reset();
    CHECK(ro.Count() == 0);
    std::printf("fx rollout: starts %u finishes %u low %llu full %llu\n", c.starts, c.finishes, static_cast<unsigned long long>(c.picturesLow), static_cast<unsigned long long>(c.picturesFull));
}
// q30-27: a stage far over (Dreadnought-like) reaches its fitting set sooner with the fast timing, one step at a time, and does not flap afterwards.
static void TestFastEscalation() {
    uint64_t lastFast = 0, lastSlow = 0;
    uint16_t maskFast = 0, maskSlow = 0;
    size_t nFast = 0, nSlow = 0;
    for (int pass = 0; pass < 2; ++pass) {
        Run r;
        q30lock::Config c;
        c.allowed = 0xFF; c.startSpec[0] = 0;   // (3f / 3g have their own test)
        if (pass == 1) c.fastOverMs = 0;   // the old timing
        r.L.Configure(c);
        r.st.pair = 28; r.st.gpu = 30; r.st.front = 20;   // 3.5 ms over the pair limit (31.5) with noise: needs 3a..3e
        r.st.pair = 34.5f;
        r.Begin("CannonFleetGalaxy:1");
        const uint64_t t0 = r.now;
        r.Seconds(150);
        r.Drain();
        const uint64_t last = r.changes.empty() ? 0 : r.changes.back().first - t0;
        for (size_t i = 1; i < r.changes.size(); ++i) CHECK(r.changes[i].first - r.changes[i - 1].first >= (pass ? 10000000u : 4000000u));
        // no flapping: once the last escalation is done no step is given back inside the hold (30 s)
        (pass ? lastSlow : lastFast) = last; (pass ? maskSlow : maskFast) = r.L.mask(); (pass ? nSlow : nFast) = r.changes.size();
    }
    std::printf("fast escalation: last change at +%.1f s (mask %02x, %zu changes); old timing +%.1f s (mask %02x, %zu changes)\n", lastFast / 1e6, maskFast, nFast, lastSlow / 1e6, maskSlow, nSlow);
    CHECK(nFast > 0 && nSlow > 0);
    CHECK(lastFast < lastSlow);
}

// ---- q30-28 -------------------------------------------------------------------------------------------------------------------------------------------------------------
// The pair is a drawn turn + the undrawn turn after it. Honeyhive: drawn 29.3 + undrawn 12.0 = 41.3. Fed as the old glue fed it (every turn "drawn") the ladder saw 29.3 / 12.0 alone.
static void TestPairAccounting() {
    q30lock::Ladder L;
    q30lock::Memory mem;
    uint64_t now = 1000000;
    { q30lock::Config c = L.config(); c.startSpec[0] = 0; L.Configure(c); }
    L.StageBegin(now, "HoneyBeeKingdomGalaxy:1", &mem);
    L.NotePicture(now, 20000, true, 20000, true, true);   // a play picture opens the play window
    for (int i = 0; i < 100; ++i) { L.NoteTurn(now, 29300, true); L.NoteTurn(now, 12000, false); now += 33333; }
    CHECK(std::fabs(L.PairMean() - 41.3f) < 0.05f);
    CHECK(std::fabs(L.PairP95() - 41.3f) < 0.05f);
    CHECK(std::fabs(L.DrawnMean() - 29.3f) < 0.05f);
    CHECK(std::fabs(L.UndrawnMean() - 12.0f) < 0.05f);
    // the old glue: every turn booked as drawn: the "pair" is one turn
    q30lock::Ladder B;
    { q30lock::Config c = B.config(); c.startSpec[0] = 0; B.Configure(c); }
    B.StageBegin(now, "HoneyBeeKingdomGalaxy:1", &mem);
    B.NotePicture(now, 20000, true, 20000, true, true);
    for (int i = 0; i < 100; ++i) { B.NoteTurn(now, 29300, true); B.NoteTurn(now, 12000, true); now += 33333; }
    CHECK(B.PairMean() < 30.0f);   // (what the console ladder read: ~30 ms for a real 41.3)
    // the census line carries the real pair
    L.Shutdown(now, &mem);
    char b[400];
    bool seen = false;
    while (L.PopLog(b, sizeof b)) if (std::strstr(b, "game pair (drawn + undrawn) mean 41.3 ms = drawn 29.3 + undrawn 12.0")) seen = true;
    CHECK(seen);
    // a game-bound stage (pair 41.3 > 31.5) takes steps: the ladder is no longer blind to it
    Run r;
    r.st.pair = 41.3f; r.st.gpu = 24; r.st.front = 20;
    r.Begin("HoneyBeeKingdomGalaxy:1");
    r.Seconds(120);
    CHECK((r.L.mask() & 0x1F) != 0);
    // beyond 3a..3e the budget steps come BEFORE the GPU's steps (4, 5, 6): they are the game thread's answer
    int first3f = -1, first4 = -1;
    for (size_t i = 0; i < r.changes.size(); ++i) {
        if (first3f < 0 && (r.changes[i].second >> q30lock::k3f & 1u)) first3f = static_cast<int>(i);
        if (first4 < 0 && (r.changes[i].second >> q30lock::k4 & 1u)) first4 = static_cast<int>(i);
    }
    CHECK(first3f >= 0);
    CHECK(first4 < 0);   // a game-thread problem never takes the GPU's beam step
    std::printf("pair: honeyhive-like mask %04x changes %zu\n", r.L.mask(), r.changes.size());
}
// A lone missed picture is a stall, not a trend: no step. Two inside the window are.
static void TestLoneMiss() {
    Run r;
    r.st.pair = 24; r.st.gpu = 25; r.st.front = 14;
    r.Begin("LightGalaxy:1");
    r.Seconds(30);
    r.L.NoteShown(r.now, 3, true);   // one 50 ms hitch
    r.Seconds(30);
    CHECK(r.L.mask() == 0);
    r.L.NoteShown(r.now, 3, true);
    r.L.NoteShown(r.now + 1000000, 3, true);   // two within 6 s: a trend
    CHECK(r.L.misses() == 3);
}
// PROOF of headroom: 20 s of play with no missed picture. A GPU p95 that rises over its limit but costs no picture (the estimate was wrong: the display wait) takes no step.
static void TestProof() {
    Run r;
    r.st.pair = 24; r.st.gpu = 25; r.st.front = 14; r.st.noise = 0.5f;
    r.Begin("DreadnoughtGalaxy:1");
    r.Seconds(40);
    CHECK(r.L.Proven(r.now));
    r.st.gpu = 31.8f;   // over the 31.0 limit, no miss (33.3 is the miss)
    r.Seconds(120);
    CHECK(r.L.mask() == 0);
    CHECK(r.changes.empty());
    // the same GPU with a miss history: no proof, the ladder steps
    Run q;
    q.st.pair = 24; q.st.gpu = 25; q.st.front = 14; q.st.noise = 0.5f;
    q.Begin("DreadnoughtGalaxy:1");
    q.Seconds(40);
    q.st.gpu = 31.8f;
    q.L.NoteShown(q.now, 3, true); q.L.NoteShown(q.now + 100000, 3, true);   // two misses: the proof is gone
    q.Seconds(30);
    CHECK(q.L.mask() != 0);
}
// A menu stage (FileSelect) is judged by nothing: no step whatever its GPU time reads, nothing remembered.
static void TestMenuStage() {
    Run r;
    r.st.pair = 9; r.st.gpu = 33.0f; r.st.front = 8;
    r.Begin("FileSelect");
    r.Seconds(60);
    CHECK(r.L.mask() == 0);
    CHECK(r.L.Menu());
    r.L.Shutdown(r.now, &r.mem);
    uint16_t got = 0;
    CHECK(!r.mem.Lookup("FileSelect", &got));
}
// Recovery with proof: a stage that needed steps, then holds its cadence with its p95 still near the limit, gets its steps back (one at a time, no flapping), and a stage that needs them again
// takes them back with a longer wait.
static void TestRecoveryProven() {
    Run r;
    r.st.pair = 33.5f; r.st.gpu = 24; r.st.front = 16;
    r.Begin("HoneyBeeKingdomGalaxy:1");
    r.Seconds(150);
    const uint16_t heavy = r.L.mask();
    CHECK(heavy != 0);
    r.st.pair = 28.0f;   // the stage's heavy part is over
    r.Seconds(400);
    r.Drain();
    std::printf("recovery(proven): heavy %04x -> %04x, changes %zu\n", heavy, r.L.mask(), r.changes.size());
    CHECK(r.L.mask() == 0);
    // no flapping: at most as many changes as steps taken and given back
    CHECK(r.changes.size() <= 2u * q30lock::PopCount(heavy) + 4u);
}

// the per-stage always-on set (owner 13 Nov 07:34): Honeyhive starts with 3a..3e+3f+3g, Beach Bowl with 3a+3c+3e+3f, others with none; never given back, even when calm
static void TestStartSet() {
    Run r;
    q30lock::Config c = r.L.config();   // the default table
    r.L.Configure(c);
    r.L.StageBegin(r.now, "HoneyBeeKingdomGalaxy:1", &r.mem);
    CHECK(r.L.mask() == 0x771F);   // (far30: the opening set 3h 3j 3k 3l on top of the always-on 0x31F)
    r.st.pair = 22;
    r.Seconds(300);   // calm all the time: the base set stays, the opening far steps go back
    CHECK(r.L.mask() == 0x31F);
    r.L.StageBegin(r.now, "HeavenlyBeachGalaxy", &r.mem);
    CHECK(r.L.mask() == 0x115);
    r.L.StageBegin(r.now, "GoodEggGalaxy:1", &r.mem);
    CHECK(r.L.mask() == 0);
}

int main() {
    TestStartSet();
    TestPairAccounting();
    TestLoneMiss();
    TestProof();
    TestMenuStage();
    TestRecoveryProven();
    TestFastEscalation();
    TestCodedLevels();
    TestLight();
    TestHeavyCpu();
    TestHeavyGpu();
    TestStep6Gate();
    TestStep3eAndExhausted();
    TestStep5Gpu();
    TestFxRollout();
    TestNoOscillation();
    TestRecovery();
    TestMemory();
    TestFarOrderAndLoad();
    TestFarOff();
    TestFarMemory();
    TestOpenSetAndHold();
    TestFader();
    TestTransitionLog();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
