// recover30: model of the Quality ladder's recovery (port/q30lock.h) against today's flat waits, with deliberately wrong cost guesses, a load exactly at the limit and loads that
// oscillate with a 3..10 s period. The model does NOT trust the ladder's cost guesses: the stage's TRUE step costs are scaled by 0.5 / 1 / 2 against kDefaultGainMs, and the
// ladder must learn (or be backed off) without toggling. Prints the tables used in new/recover30/REPORT.txt.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <vector>
#include <cstring>
#include "port/cpulod.h"
#include "port/lockfade.h"
#include "port/q30lock.h"

using namespace port;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Load { float pair, gpu, front; };
struct Res {
    double tFull = -1;        // seconds from the load drop to the step set being empty (-1 never)
    double tHalf = -1;        // seconds to half of the steps given back
    int changes = 0;          // mask changes in the whole run
    double perMin = 0;
    int retakes10 = 0;        // a step taken again within 10 s of its give-back
    int missPics = 0;         // pictures whose pair / gpu / front is over 33.3 ms (a missed picture)
    int pics = 0;
    double slowSec = 0;       // seconds the game ran under 98 % speed
    double meanSteps = 0;     // mean number of steps in force over the run
};
struct Sim {
    q30lock::Ladder L;
    q30lock::Memory mem;
    float scale = 1.0f;       // the TRUE step costs against kDefaultGainMs
    std::function<Load(double)> load;
    double dropAt = -1;       // the time the load drops (for tFull)
    uint64_t now = 1000000;
    uint32_t rnd = 777;
    float noise = 0.8f;
    bool hideMiss = false;    // the pacer slips the game clock instead of showing a 3-vblank picture (GXV_SLIP): the slip is the only sign
    float wob() { rnd = rnd * 1664525u + 1013904223u; return (static_cast<float>(rnd >> 8) / 16777216.0f - 0.5f) * 2.0f * noise; }
    Res Run(double seconds, const q30lock::Config& cfg, const char* key = "Model:1") {
        Res r;
        L.Configure(cfg);
        L.SetStep6Ready(false);
        L.StageBegin(now, key, &mem);
        const uint64_t t0 = now;
        uint16_t last = L.mask();
        uint64_t releasedAt[q30lock::kStepN] = {};
        const int n = static_cast<int>(seconds * 30);
        int initial = q30lock::PopCount(L.mask());
        double stepSum = 0;
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(now - t0) / 1e6;
            Load b = load(t);
            const uint16_t m = L.mask();
            float g = b.gpu, p = b.pair, f = b.front;
            for (int s = 0; s < q30lock::kStepN; ++s) if (m >> s & 1u) {
                const float gain = q30lock::kDefaultGainMs[s] * scale;
                if (q30lock::kHelps[s] & q30lock::kGpu) g -= gain;
                if (q30lock::kHelps[s] & q30lock::kGame) p -= gain;
                if (q30lock::kHelps[s] & q30lock::kFront) f -= gain;
            }
            const float w = wob();
            g += w; p += w; f += w * 0.5f;
            L.NoteTurn(now, static_cast<uint32_t>((p - 9.0f) * 1000), true);
            L.NoteTurn(now, 9000, false);
            const bool miss = !hideMiss && (p > 33.3f || g > 33.3f || f > 33.3f);
            // the game's speed: a pair over two refreshes slips (the pacer asks for up to 20 %); the turn gaps are the pair's halves
            const float pairMs = std::max(33.366f, std::min(p, 33.366f * 1.25f));
            const uint32_t gap = static_cast<uint32_t>(pairMs * 500.0f);
            L.NoteSpeed(now, gap, true);
            L.NoteSpeed(now, gap, true);
            if (pairMs > 33.366f / 0.98f) r.slowSec += 1.0 / 30.0;
            L.NotePicture(now, static_cast<uint32_t>(g * 1000), true, static_cast<uint32_t>(f * 1000), true, true);
            L.NoteShown(now, miss ? 3 : 2, true);
            L.NoteCover(now, false);
            ++r.pics; if (miss) ++r.missPics;
            now += 33333;
            const uint16_t nm = L.mask();
            stepSum += q30lock::PopCount(nm);
            if (nm != last) {
                ++r.changes;
                if (getenv("DBG")) std::printf("   t=%6.1f mask %03x -> %03x  p95pair %.1f\n", t, last, nm, static_cast<double>(L.PairP95()));
                for (int s = 0; s < q30lock::kStepN; ++s) {
                    const bool was = last >> s & 1u, is = nm >> s & 1u;
                    if (was && !is) releasedAt[s] = now;
                    if (!was && is && releasedAt[s] && now - releasedAt[s] < 10000000u) ++r.retakes10;
                }
                last = nm;
            }
            if (dropAt >= 0 && t >= dropAt) {
                if (r.tFull < 0 && L.mask() == 0) r.tFull = t - dropAt;
                if (r.tHalf < 0 && q30lock::PopCount(L.mask()) * 2 <= std::max(1, initial)) r.tHalf = t - dropAt;
            }
        }
        r.perMin = r.changes / (seconds / 60.0);
        r.meanSteps = stepSum / n;
        char b[400]; while (L.PopLog(b, sizeof b)) { if (getenv("DBG")) std::printf("%s\n", b); }
        return r;
    }
};
static q30lock::Config Today() { q30lock::Config c; c.recover = false; c.multi = false; c.slipPct = 0; return c; }
static q30lock::Config Mine() { q30lock::Config c; return c; }
static void Row(const char* name, const Res& a, const Res& b) {
    std::printf("  %-34s | full %6.1f -> %6.1f s | half %6.1f -> %6.1f | changes/min %5.2f -> %5.2f | retakes<10s %2d -> %2d | missed pics %4d -> %4d | slow s %5.1f -> %5.1f | mean steps %.2f -> %.2f\n",
                name, a.tFull, b.tFull, a.tHalf, b.tHalf, a.perMin, b.perMin, a.retakes10, b.retakes10, a.missPics, b.missPics, a.slowSec, b.slowSec, a.meanSteps, b.meanSteps);
}
// heavy for 120 s, then the load drops to `after` (pair ms with no steps)
static void StepDrop(Sim& s, float heavy, float after, float scale, double at = 120) {
    s.scale = scale;
    s.dropAt = at;
    s.load = [=](double t) { return Load{t < at ? heavy : after, 24.0f, 16.0f}; };
}
int main() {
    std::printf("recover30 model: today (flat waits) -> recover30\n");
    const float scales[3] = {0.5f, 1.0f, 2.0f};
    for (float sc : scales) {
        char name[80];
        std::printf(" true step costs x%.1f of the ladder's guesses\n", static_cast<double>(sc));
        { Sim a, b; StepDrop(a, 36.0f, 24.0f, sc); StepDrop(b, 36.0f, 24.0f, sc); std::snprintf(name, sizeof name, "load drops 36 -> 24 (big room)"); Row(name, a.Run(420, Today()), b.Run(420, Mine())); }
        { Sim a, b; StepDrop(a, 36.0f, 29.0f, sc); StepDrop(b, 36.0f, 29.0f, sc); std::snprintf(name, sizeof name, "load drops 36 -> 29 (2.5 ms room)"); Row(name, a.Run(420, Today()), b.Run(420, Mine())); }
        { Sim a, b; StepDrop(a, 36.0f, 31.0f, sc); StepDrop(b, 36.0f, 31.0f, sc); std::snprintf(name, sizeof name, "load drops 36 -> 31 (0.5 ms room)"); Row(name, a.Run(420, Today()), b.Run(420, Mine())); }
        { Sim a, b; StepDrop(a, 34.5f, 33.0f, sc, 0); StepDrop(b, 34.5f, 33.0f, sc, 0); std::snprintf(name, sizeof name, "load exactly at limit (33.0, p95 31.5+)"); Row(name, a.Run(600, Today()), b.Run(600, Mine())); }
        for (int per : {3, 5, 7, 10}) {
            Sim a, b;
            a.scale = b.scale = sc;
            auto ld = [per](double t) { return Load{(static_cast<int>(t / (per * 0.5)) & 1) ? 34.0f : 24.0f, 24.0f, 16.0f}; };
            a.load = b.load = ld;
            std::snprintf(name, sizeof name, "load oscillates 24/34, period %d s", per);
            const Res ra = a.Run(600, Today()), rb = b.Run(600, Mine());
            Row(name, ra, rb);
            // never worse than a handful of changes a minute, and the step set does not toggle at the load's period
            CHECK(rb.perMin <= std::max(4.0, ra.perMin * 1.5 + 1.0));
            CHECK(rb.retakes10 <= std::max(3, ra.retakes10 + 2));
        }
        // the room case: the new rule is not slower than today's
        Sim a, b; StepDrop(a, 36.0f, 24.0f, sc); StepDrop(b, 36.0f, 24.0f, sc);
        const Res ra = a.Run(420, Today()), rb = b.Run(420, Mine());
        CHECK(rb.tFull >= 0 && (ra.tFull < 0 || rb.tFull <= ra.tFull));
        CHECK(rb.perMin <= 6.0);
        CHECK(rb.retakes10 <= 3);
        Sim c, d; StepDrop(c, 36.0f, 31.0f, sc); StepDrop(d, 36.0f, 31.0f, sc);
        const Res rc = c.Run(420, Today()), rd = d.Run(420, Mine());
        CHECK(rd.perMin <= std::max(4.0, rc.perMin * 1.5 + 1.0));
        CHECK(rd.retakes10 <= std::max(3, rc.retakes10 + 2));
    }
    {   // the slip: a pair of 36 ms with every picture held 2 vblanks (the game runs at ~93 %): today nothing sees it (no miss, 20 s of proof)
        std::printf(" slip hidden by the pacer\n");
        for (float pair : {35.0f, 38.0f, 40.0f}) {
            Sim a, b;
            a.hideMiss = b.hideMiss = true;
            a.load = b.load = [pair](double) { return Load{pair, 24.0f, 16.0f}; };
            char name[80];
            std::snprintf(name, sizeof name, "pair %.0f ms, no missed picture", static_cast<double>(pair));
            const Res ra = a.Run(300, Today()), rb = b.Run(300, Mine());
            Row(name, ra, rb);
            CHECK(rb.slowSec <= ra.slowSec + 1.0);   // never slower to react than today; with a pair the steps cannot cure, the slip stays (then the ladder is exhausted)
        }
    }
    {   // memory v4: the learned gains survive a visit; the stage opens with the remembered set and gives it back at once
        Sim a;
        a.load = [](double t) { return Load{t < 100 ? 35.5f : 25.0f, 24.0f, 16.0f}; };
        a.Run(160, Mine(), "Mem:1");
        a.L.Shutdown(a.now, &a.mem);
        q30lock::Memory m2;
        char buf[4096];
        const int n = a.mem.Format(buf, sizeof buf);
        CHECK(m2.Parse(buf, static_cast<size_t>(n)) == 1);
        const int gi = m2.Find("Mem:1");
        CHECK(gi >= 0 && m2.e[gi].gain[0] >= 0);   // a measured gain came back through the file
        std::printf(" memory line: %s", std::strstr(buf, "Mem:1"));
        CHECK(a.mem.Find("Mem:1") >= 0);
        // a second visit: starts with the remembered set (if any) and, with the load calm, is back at full quality sooner than today's flat 30 s hold
        Sim b, c;
        b.mem = a.mem; c.mem = a.mem;
        b.load = c.load = [](double) { return Load{25.0f, 24.0f, 16.0f}; };
        b.dropAt = c.dropAt = 0;
        const Res rc = c.Run(120, Today(), "Mem:1"), rb = b.Run(120, Mine(), "Mem:1");
        std::printf(" second visit, calm: full quality after %.1f s (today %.1f s)\n", rb.tFull, rc.tFull);
        CHECK(rb.tFull < 0 ? a.mem.Find("Mem:1") >= 0 && a.mem.e[a.mem.Find("Mem:1")].mask == 0 : (rc.tFull < 0 || rb.tFull <= rc.tFull));
    }
    {   // a stage that opens with a remembered set (3a+3b+3c) and a calm load: how soon is it back at full quality?
        Sim a, b;
        a.mem.Store("Open:1", 0x07); b.mem.Store("Open:1", 0x07);
        a.load = b.load = [](double) { return Load{26.0f, 24.0f, 16.0f}; };
        a.dropAt = b.dropAt = 0;
        const Res ra = a.Run(120, Today(), "Open:1"), rb = b.Run(120, Mine(), "Open:1");
        std::printf(" opening with a remembered set 3a+3b+3c, calm: full quality after %.1f s (today %.1f s)\n", rb.tFull, ra.tFull);
        CHECK(rb.tFull >= 0 && rb.tFull <= 20.0 && (ra.tFull < 0 || rb.tFull < ra.tFull));
    }
    {   // several steps in ONE change when the room covers their learned gains
        Sim a;
        a.load = [](double t) { return Load{t < 80 ? 36.0f : 24.0f, 24.0f, 16.0f}; };
        a.dropAt = 80;
        const Res r = a.Run(200, Mine());
        std::printf(" multi: %d changes in the whole run, set at the end %03x\n", r.changes, a.L.mask());
        CHECK(a.L.mask() == 0);
        char line[160][160];
        (void)line;
        char cl[10][160];
        const int n = a.L.Census(a.now, cl, 10);
        CHECK(n >= 1);
        for (int i = 0; i < n; ++i) std::printf("   %s\n", cl[i]);
        CHECK(std::strstr(cl[0], "recover: q30lock step") && std::strstr(cl[0], "given back"));
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
