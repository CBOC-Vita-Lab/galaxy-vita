// GXV_LATENCY24 (latency24): the present-latency rule of frame_pace.h PresentSync with the display queue's backlog seen (Latency24), on a model
// of the console's pipeline (a model, not a console measurement: the real PresentSync class runs on stage costs a scene sets). Per drawn turn a
// front end, a back end and a GPU stage, one picture at a time each; the display queue's callbacks are in order, each after the GPU finished its
// picture and the previous callback returned (a picture is held in its callback until its slot: that is what backs the queue up); the back end's
// swap blocks while two pictures wait (three display buffers), as tests/present_sync_model.cpp. The scenes are the c27 / c31 console logs' (the
// 'present:' lines of 765 Good Egg, 766 Honeyhive, 800 Beach Bowl): what the log shows as "ready after" and "shown-minus-turn" is what the model
// is calibrated to.  Compared per scene: TODAY (the rule as it was) against LATENCY24: pictures a second, the mean shown-minus-turn (the latency
// in vblanks), the late pictures (shown after their slot), the late pictures that today's largest latency would have shown on time, the pictures
// whose hold differed from the game's step ("judder events", what the owner sees as a hitch), and the latency the rule held.
//   g++ -std=c++17 -O1 -I platform/include tests/latency24_model.cpp -o /tmp/latency24_model && /tmp/latency24_model
#include "port/frame_pace.h"
#include "latency24_ref_presentsync.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
constexpr double kV = 16.68335;  // ms per vblank
using Latency24 = port::pace::PresentSync::Latency24;
using port::pace::PresentSync;

struct Scene {
    const char* name;
    double rMean, rSd;              // the picture's readiness: vblanks from the start of its turn to the GPU's completion (what the log's "ready after" shows)
    int skipEvery;                  // 0: every turn is drawn; n: one turn in n is left out; -2: a steady 30 (every other turn)
    double spikePct = 0;            // pictures delayed by a stall (a shader link, a texture upload) ...
    double spikeVb = 0;             // ... by this many vblanks
    double lateStartSec = 0;        // seconds of a heavier start (the readiness is 1.4 vblanks later: a load / camera cut) that push the latency up first
    double burstEverySec = 0;       // a burst of late pictures (a stage change, a shader link run: the logs' 800 Beach Bowl run has one every ~25 s) ...
    unsigned burstLen = 0;          // ... this many pictures long ...
    double burstVb = 0;             // ... each this many vblanks later
};

struct Outcome {
    uint64_t pictures = 0, late = 0, lateTighter = 0, events = 0, censored = 0, probeFails = 0, raises = 0, lowers = 0, replaced = 0;
    double latMs = 0, latSd = 0, fps = 0, lMean = 0;
    unsigned lMin = 99, lMax = 0, lEnd = 0;
    uint64_t naturalHist[8] = {};
};

// Runs `seconds` of the scene; the first `warm` seconds are not counted.
Outcome Run(const Scene& sc, int mode, unsigned seed, double seconds, double warm, unsigned tolPerMille = 5, unsigned maxLat = 5, unsigned lowFloor = 2) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    PresentSync sync;
    sync.Configure(true, 1, maxLat, 3, 4);
    if (mode >= 1) {
        Latency24 l;
        l.on = true;
        l.catchUp = mode >= 2;
        l.tolPerMille = tolPerMille;
        l.lowFloor = lowFloor;
        if (const char* e = std::getenv("L24_WINDOW")) { l.window = static_cast<unsigned>(std::atoi(e)); l.minKnown = l.window / 2; }
        if (const char* e = std::getenv("L24_TOL")) l.tolPerMille = static_cast<unsigned>(std::atoi(e));
        sync.ConfigureLatency24(l);
    }
    double doneLast = 0, cbFree = 0;
    const unsigned turns = static_cast<unsigned>(seconds * 1000.0 / kV);
    const unsigned warmTurns = static_cast<unsigned>(warm * 1000.0 / kV);
    const unsigned heavyTurns = static_cast<unsigned>(sc.lateStartSec * 1000.0 / kV);
    Outcome o;
    port::pace::PresentSync::Stats before{};
    uint32_t prevShownVb = 0, prevStampV = 0;
    bool havePrev = false;
    uint64_t lSum = 0;
    unsigned countedTurns = 0;
    std::normal_distribution<double> rd(sc.rMean, sc.rSd);
    unsigned burstLeft = 0;
    for (unsigned t = 0; t < turns; ++t) {
        if (t == warmTurns) before = sync.GetStats();
        const double start = t * kV + 0.3;
        bool draw = true;
        if (sc.skipEvery == -2) draw = (t & 1) == 0;
        else if (sc.skipEvery > 0) draw = (t % sc.skipEvery) != 0;
        if (!draw) continue;
        double r = rd(rng);
        if (r < 1.2) r = 1.2;
        if (t < heavyTurns) r += 1.4;
        if (sc.spikePct > 0 && u01(rng) * 100.0 < sc.spikePct) r += sc.spikeVb;
        if (sc.burstEverySec > 0) {
            const unsigned every = static_cast<unsigned>(sc.burstEverySec * 1000.0 / kV);
            if (t % every == 0 && t > heavyTurns) burstLeft = sc.burstLen;
            if (burstLeft) {
                --burstLeft;
                r += sc.burstVb;
            }
        }
        double done = start + r * kV;
        if (done < doneLast + 3.0) done = doneLast + 3.0;  // (one GPU: pictures finish in order)
        doneLast = done;
        const double entry = std::fmax(done, cbFree);
        const bool censored = cbFree > 0 && entry - cbFree < 1.2;
        const uint32_t earliest = static_cast<uint32_t>(std::floor(entry / kV)) + 1;
        const uint32_t stamp = t;
        const uint32_t target = sync.Target(earliest, true, static_cast<uint16_t>(stamp));
        sync.NoteBacklog(censored);
        sync.Shown(target, target != earliest);
        cbFree = target != earliest ? (target - 1) * kV + 0.2 : entry + 0.1;
        if (t >= warmTurns) {
            ++o.pictures;
            lSum += sync.Latency();
            ++countedTurns;
            const uint32_t nat = earliest - stamp;
            ++o.naturalHist[nat < 7 ? nat : 7];
            if (havePrev) {
                const int32_t hold = static_cast<int32_t>(target - prevShownVb), step = static_cast<int32_t>(stamp - prevStampV);
                if (hold != step) ++o.events;
            }
        }
        prevShownVb = target;
        prevStampV = stamp;
        havePrev = true;
    }
    const auto& st = sync.GetStats();
    o.late = st.misses - before.misses;
    o.lateTighter = st.lateTighter - before.lateTighter;
    o.censored = st.censored - before.censored;
    o.replaced = st.replaced - before.replaced;
    o.probeFails = st.probeFails - before.probeFails;
    o.raises = st.raises - before.raises;
    o.lowers = st.lowers - before.lowers;
    const double n = static_cast<double>(st.stamped - before.stamped);
    const double lat = static_cast<double>(st.latSum - before.latSum) / n;
    o.latMs = lat * kV;
    o.latSd = std::sqrt(std::fmax(0.0, static_cast<double>(st.latSq - before.latSq) / n - lat * lat)) * kV;
    o.fps = o.pictures / ((turns - warmTurns) * kV / 1000.0);
    o.lMean = countedTurns ? static_cast<double>(lSum) / countedTurns : 0;
    o.lMin = st.latMin;
    o.lMax = st.latMax;
    o.lEnd = sync.Latency();
    return o;
}

void Print(const char* tag, const Outcome& o) {
    std::printf("    %-13s %5.1f pictures/s  latency %5.1f +- %4.1f ms (L mean %.2f, end %u)  late %4llu (%.2f %%, %llu on time at L5)  judder events %4llu (%.2f %%)  "
                "pictures lost to catch-up %llu  lowers %llu raises %llu failed %llu  natural 3/4/5/6/7+ %llu/%llu/%llu/%llu/%llu\n",
                tag, o.fps, o.latMs, o.latSd, o.lMean, o.lEnd, (unsigned long long)o.late, 100.0 * o.late / o.pictures, (unsigned long long)o.lateTighter,
                (unsigned long long)o.events, 100.0 * o.events / o.pictures, (unsigned long long)o.replaced, (unsigned long long)o.lowers,
                (unsigned long long)o.raises, (unsigned long long)o.probeFails, (unsigned long long)o.naturalHist[3], (unsigned long long)o.naturalHist[4],
                (unsigned long long)o.naturalHist[5], (unsigned long long)o.naturalHist[6], (unsigned long long)o.naturalHist[7]);
}
}  // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && !std::strcmp(argv[1], "-v");
    (void)verbose;
    // 1. Latency24 off is the old rule, decision for decision (random traces: late pictures, unstamped ones, resets, stamps out of range).
    {
        std::mt19937 rng(77);
        unsigned diffs = 0;
        uint64_t compared = 0;
        for (int round = 0; round < 40; ++round) {
            port::pace::PresentSync a;
            port::pace::PresentSyncRef b;
            const unsigned lo = 1 + rng() % 2, hi = 3 + rng() % 4, init = lo + rng() % 3, raise = 1 + rng() % 8;
            const bool align = (rng() % 4) != 0;
            a.Configure(align, lo, hi, init, raise);
            b.Configure(align, lo, hi, init, raise);
            uint32_t vb = 1000 + rng() % 5000;
            bool lastCensorFlip = false;
            for (int i = 0; i < 6000; ++i) {
                const uint32_t gap = 1 + rng() % 3;
                vb += gap;
                const uint32_t natural = 1 + rng() % 8 + (rng() % 40 == 0 ? 80 : 0);
                const bool valid = rng() % 25 != 0;
                const uint32_t earliest = vb + natural;
                const uint16_t stamp = static_cast<uint16_t>(vb);
                if (rng() % 400 == 0) { a.Reset(); b.Reset(); }
                const uint32_t ta = a.Target(earliest, valid, stamp), tb = b.Target(earliest, valid, stamp);
                lastCensorFlip = !lastCensorFlip;
                a.NoteBacklog(lastCensorFlip);  // (noted, not used: Latency24 is off)
                const bool waited = ta != earliest;
                a.Shown(ta, waited);
                b.Shown(tb, waited);
                ++compared;
                if (ta != tb || a.Latency() != b.Latency()) ++diffs;
            }
            const auto& sa = a.GetStats();
            const auto& sb = b.GetStats();
            if (sa.pictures != sb.pictures || sa.stamped != sb.stamped || sa.waits != sb.waits || sa.misses != sb.misses || sa.overload != sb.overload ||
                sa.replaced != sb.replaced || sa.raises != sb.raises || sa.lowers != sb.lowers || sa.latSum != sb.latSum || sa.latSq != sb.latSq ||
                sa.errSum != sb.errSum || sa.pairs != sb.pairs || std::memcmp(sa.hold, sb.hold, sizeof sa.hold) || std::memcmp(sa.step, sb.step, sizeof sa.step) ||
                std::memcmp(sa.err, sb.err, sizeof sa.err) || std::memcmp(sa.natural, sb.natural, sizeof sa.natural))
                ++diffs;
        }
        char line[200];
        std::snprintf(line, sizeof line, "Latency24 off: %llu pictures in 40 random runs, identical targets, latencies and statistics to f6c0c23's PresentSync (%u differences)",
                      static_cast<unsigned long long>(compared), diffs);
        Check(diffs == 0, line);
    }
    // 2. The rule's pieces.
    {
        Latency24 l;
        l.on = true;
        l.window = 20;
        l.minKnown = 10;
        l.catchUp = true;
        l.dropGap = 4;
        // pictures on consecutive vblanks, ready 3 vblanks after their turn (entry vblank stamp + 2: natural 3), held at L = 5: a backlog
        PresentSync s;
        s.Configure(true, 1, 5, 5);
        s.ConfigureLatency24(l);
        double cbFree = 0;
        uint32_t t = 100;
        unsigned lowers = 0, replaced = 0;
        auto step = [&](double readyVb) {
            const double done = t * kV + 0.3 + readyVb * kV;
            const double entry = std::fmax(done, cbFree);
            const bool cens = cbFree > 0 && entry - cbFree < 1.2;
            const uint32_t earliest = static_cast<uint32_t>(std::floor(entry / kV)) + 1;
            const uint32_t target = s.Target(earliest, true, static_cast<uint16_t>(t));
            s.NoteBacklog(cens);
            s.Shown(target, target != earliest);
            cbFree = target != earliest ? (target - 1) * kV + 0.2 : entry + 0.1;
            ++t;
        };
        for (int i = 0; i < 15; ++i) step(2.4);
        Check(s.Latency() == 5, "a backlog at L5: no evidence yet (the window is not over), L stays");
        for (int i = 0; i < 25; ++i) step(2.4);
        for (int i = 0; i < 700; ++i) step(2.4);
        const auto& st = s.GetStats();
        lowers = static_cast<unsigned>(st.lowers);
        replaced = static_cast<unsigned>(st.replaced);
        Check(s.Latency() == 3 && lowers == 2, "pictures ready after 3 vblanks held at L5 on consecutive vblanks: censored entries are evidence, L comes down 5 -> 4 -> 3 and stops (natural 3 is not below 3)");
        Check(replaced == 2, "and each lowering costs exactly one picture (the one whose slot the next takes), 2 in all");
        Check(st.raises == 0 && st.probeFails == 0, "with no raise and no failed lowering");
        PresentSync old;
        old.Configure(true, 1, 5, 5);
        double cbf = 0;
        uint32_t tt = 100;
        uint32_t lastChain = 0;
        for (int i = 0; i < 740; ++i) {
            const double done = tt * kV + 0.3 + 2.4 * kV;
            const double entry = std::fmax(done, cbf);
            const uint32_t earliest = static_cast<uint32_t>(std::floor(entry / kV)) + 1;
            const uint32_t target = old.Target(earliest, true, static_cast<uint16_t>(tt));
            old.Shown(target, target != earliest);
            cbf = target != earliest ? (target - 1) * kV + 0.2 : entry + 0.1;
            lastChain = target - tt;
            ++tt;
        }
        Check(old.Latency() == 4 && lastChain == 5, "today's rule, the same pictures: L stops at 4 for ever (the pictures that waited behind the hold read as ones that would miss 3) while every picture is shown 5 vblanks after its turn");
        // a lowering that fails is undone and held off
        PresentSync f;
        f.Configure(true, 1, 5, 4);
        Latency24 l2 = l;
        l2.catchUp = false;
        f.ConfigureLatency24(l2);
        uint32_t tf = 500;
        double cf = 0;
        auto stepf = [&](double readyVb) {
            const double done = tf * kV + 0.3 + readyVb * kV;
            const double entry = std::fmax(done, cf);
            const bool cens = cf > 0 && entry - cf < 1.2;
            const uint32_t earliest = static_cast<uint32_t>(std::floor(entry / kV)) + 1;
            const uint32_t target = f.Target(earliest, true, static_cast<uint16_t>(tf));
            f.NoteBacklog(cens);
            f.Shown(target, target != earliest);
            cf = target != earliest ? (target - 1) * kV + 0.2 : entry + 0.1;
            tf += 2;
        };
        for (int i = 0; i < 25; ++i) stepf(2.4);   // (30 a second: ready after 3 vblanks, L4: uncensored, natural 3 < 4: evidence, lowers to 3)
        Check(f.Latency() == 3, "at 30 a second ready after 3 vblanks, L4 comes down to 3 after one window");
        for (int i = 0; i < 6; ++i) stepf(3.6);    // the pictures are suddenly ready after 4: two of them late on probation
        Check(f.Latency() == 4 && f.GetStats().probeFails == 1, "pictures that need 4 after a lowering: undone at once (probation), counted as a failed lowering");
        for (int i = 0; i < 100; ++i) stepf(2.4);
        Check(f.Latency() == 4, "and the next lowering is held off (backoff, 600 pictures after one failure)");
        for (int i = 0; i < 600; ++i) stepf(2.4);
        Check(f.Latency() == 3, "then it is tried again (after 600: the backoff doubled once)");
    }

    // r: vblanks from the turn's start to the GPU's completion, as the logs' uncensored "ready after" shows it: Good Egg 60 at L3 (765: natural 3 for
    // 292 of 292 pictures, 58 fps), at L4 (237 of 3 and 60 of 4), Honeyhive 50 (766: 78/154/18 of 3/4/5, 50 fps), Honeyhive 38 (129/65 of 4/5),
    // steady 30 (800: 146 of 150 natural 3), Beach Bowl steady 30 (3: 53 %, 4: 31 %, 5: 14 %). Each scene starts with a heavier load of 3 s (a load, a cut)
    // that raises the latency first, as the logs show (every scene starts at L5 and comes down only after a long quiet).
    const Scene scenes[] = {
        {"Good Egg 60 held, clean tail (2.45 +- 0.1)", 2.45, 0.10, 0, 0, 0, 3.0},
        {"Good Egg 60 held (readiness 2.55 +- 0.2 vblanks)", 2.55, 0.20, 0, 0, 0, 3.0},
        {"Good Egg 56 (2.7 +- 0.25, one turn in 12 left out)", 2.70, 0.25, 12, 0, 0, 3.0},
        {"Honeyhive 50 (3.2 +- 0.45, one turn in 6 left out)", 3.20, 0.45, 6, 0, 0, 3.0},
        {"Honeyhive 38 (3.6 +- 0.5, one turn in 3 left out)", 3.60, 0.50, 3, 0, 0, 3.0},
        {"steady 30 with a burst of 8 late pictures (+2.5 vblanks) every 25 s", 2.50, 0.20, -2, 0, 0, 3.0, 25.0, 8, 2.5},
        {"Beach Bowl steady 30 with a burst of 8 late pictures every 25 s", 2.70, 0.40, -2, 0, 0, 3.0, 25.0, 8, 2.5},
        {"Good Egg 56 with a burst of 8 late pictures every 25 s", 2.70, 0.25, 12, 0, 0, 3.0, 25.0, 8, 2.5},
        {"steady 30 (2.5 +- 0.2)", 2.50, 0.20, -2, 0, 0, 3.0},
        {"Beach Bowl steady 30 (2.7 +- 0.55)", 2.70, 0.55, -2, 0, 0, 3.0},
        {"50 with stalls (3.2 +- 0.45, 2 % of pictures +2 vblanks)", 3.20, 0.45, 6, 2.0, 2.0, 3.0},
    };
    struct Avg { double lat = 0, ev = 0, late = 0, lt = 0, fps = 0, n = 0, l = 0, lost = 0; };
    std::vector<Avg> avgs[3];
    const char* tags[3] = {"TODAY", "LATENCY24", "+ catch-up"};
    for (const Scene& sc : scenes) {
        std::printf("%s\n", sc.name);
        Avg a[3];
        const unsigned seeds = 4;
        for (unsigned seed = 1; seed <= seeds; ++seed) {
            for (int m = 0; m < 3; ++m) {
                const Outcome o = Run(sc, m, seed, 140, 20);
                if (seed == 1) Print(tags[m], o);
                a[m].lat += o.latMs; a[m].ev += o.events; a[m].late += o.late; a[m].lt += o.lateTighter; a[m].fps += o.fps; a[m].n += o.pictures; a[m].l += o.lMean; a[m].lost += o.replaced;
            }
        }
        for (int m = 0; m < 3; ++m) {
            a[m].lat /= seeds; a[m].fps /= seeds; a[m].l /= seeds;
            avgs[m].push_back(a[m]);
        }
        for (int m = 1; m < 3; ++m)
            std::printf("    4 seeds %-11s: latency %.1f -> %.1f ms (%.1f ms lower), L mean %.2f -> %.2f, judder events %.2f %% -> %.2f %%, late %.2f %% -> %.2f %% (%.2f %% on time at L5), lost %.0f pictures in 120 s, %.1f -> %.1f pictures/s\n",
                        tags[m], a[0].lat, a[m].lat, a[0].lat - a[m].lat, a[0].l, a[m].l, 100.0 * a[0].ev / a[0].n, 100.0 * a[m].ev / a[m].n, 100.0 * a[0].late / a[0].n,
                        100.0 * a[m].late / a[m].n, 100.0 * a[m].lt / a[m].n, a[m].lost / seeds, a[0].fps, a[m].fps);
    }
    std::printf("claims\n");
    for (size_t i = 0; i < avgs[0].size(); ++i) {
        for (int m = 1; m < 3; ++m) {
            const Avg& a = avgs[0][i];
            const Avg& b = avgs[m][i];
            char line[300];
            std::snprintf(line, sizeof line, "%s, %s: latency %.1f ms against %.1f today", scenes[i].name, tags[m], b.lat, a.lat);
            Check(b.lat <= a.lat + 2.0, line);
            std::snprintf(line, sizeof line, "%s, %s: judder events %.2f %% against %.2f %% today", scenes[i].name, tags[m], 100.0 * b.ev / b.n, 100.0 * a.ev / a.n);
            Check(100.0 * b.ev / b.n <= 100.0 * a.ev / a.n + (m == 2 ? 1.0 : 0.5), line);
            std::snprintf(line, sizeof line, "%s, %s: pictures a second %.1f against %.1f", scenes[i].name, tags[m], b.fps, a.fps);
            Check(std::fabs(b.fps - a.fps) < 1.0, line);
        }
    }
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
