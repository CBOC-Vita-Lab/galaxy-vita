// GXV_SKIP_MAXSTEP (frame_pace.h LegacySkip maxStep) on a model of the lit Observatory's pipeline (a model, not a console measurement).
// Per turn the game thread runs update (U), waits for the front end to finish the last picture when one is pending (GXV_DRAWDONE_DEFER=2:
// at the start of calcAnim), runs calcAnim (C) and, on a drawn turn, records the picture (D). The front end parses it from the start of
// the recording (F a picture), the back end executes it (B) once the GPU has finished the picture before, the GPU draws it (G). The
// skip rule is the real port::pace::LegacySkip of "Game speed Full" (PETARI_FRAMESKIP=5, pipe depth 2, pipe run 6); a turn waits for
// its retrace unless it skipped late (code 2) or already ran past it. Costs are normal around the means of the r47/r49 lit-hub logs
// (game ~14.7 ms a turn, draw ~10.6 per drawn turn, front end ~28 ms CPU a picture partly overlapping the recording, GPU ~29 ms).
// Prints, per cap, pictures a second, game speed, the turns each picture advanced (what the "present:" line calls game steps), and
// the gaps between back-end completions (what the basic log's "longest" measures).
//   tools/test_skip_maxstep_model.sh
#include "port/frame_pace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

namespace {
constexpr double kV = 16.68335;
struct Result { double fps, speed, p99, maxGap; unsigned steps[6]; double over60, over75; };

Result Run(int cap, double feMs, unsigned seed) {
    std::mt19937 rng(seed);
    auto cost = [&](double m, double sd) { std::normal_distribution<double> d(m, sd); const double v = d(rng); return v < m * 0.5 ? m * 0.5 : v; };
    port::pace::LegacySkip rule(5, 2, 6, nullptr, cap);
    double t = 0, feFree = 0, beFree = 0, gpuFree = 0, drawDone = -1, nextV = kV;
    std::vector<double> beDone, shown;
    std::vector<unsigned> step;
    unsigned turns = 0, lastDrawn = 0;
    bool any = false;
    const double end = 120000.0;
    while (t < end) {
        unsigned pending = 0;
        for (double x : beDone) if (x > t) ++pending;
        const int code = rule.Turn(static_cast<int64_t>(t * 1e6), true, pending).code;
        double w = t + cost(5.4, 1.0);
        if (drawDone > w) w = drawDone;
        drawDone = -1;
        w += cost(4.4, 0.8);
        if (code == 0) {
            const double d0 = w;
            w += cost(10.6, 1.5);
            const double fe0 = std::max(d0, feFree), fe1 = std::max(fe0 + cost(feMs, 3.0), w);
            const double be0 = std::max(fe1, beFree);
            const double be1 = std::max(be0 + cost(14.0, 1.5), gpuFree);
            const double g0 = std::max(be0 + 2.0, gpuFree);
            gpuFree = g0 + cost(29.0, 3.0);
            beFree = be1;
            feFree = fe1;
            drawDone = fe1;
            beDone.push_back(be1);
            if (beDone.size() > 16) beDone.erase(beDone.begin());
            shown.push_back(be1);
            step.push_back(any ? turns - lastDrawn : 2);
            lastDrawn = turns;
            any = true;
        }
        ++turns;
        while (nextV <= t) nextV += kV;
        t = code == 2 ? w : std::max(w, nextV);
    }
    std::sort(shown.begin(), shown.end());
    std::vector<double> gaps;
    unsigned o60 = 0, o75 = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        const double g = shown[i] - shown[i - 1];
        gaps.push_back(g);
        o60 += g > 60.0;
        o75 += g > 75.0;
    }
    std::sort(gaps.begin(), gaps.end());
    Result r{};
    const double secs = t / 1000.0;
    r.fps = shown.size() / secs;
    r.speed = 100.0 * turns * kV / t;
    r.p99 = gaps[gaps.size() * 99 / 100];
    r.maxGap = gaps.back();
    for (unsigned s : step) ++r.steps[std::min(s, 5u)];
    r.over60 = o60 * 5.0 / secs;
    r.over75 = o75 * 5.0 / secs;
    return r;
}
}  // namespace

int main() {
    // Unit checks of the cap on the real rule.
    {
        port::pace::LegacySkip off(5, 2, 6), three(5, 2, 6, nullptr, 3);
        const int64_t P = port::pace::LegacySkip::kPeriodNs;
        int64_t now = P;
        unsigned runOff = 0, runCap = 0, maxOff = 0, maxCap = 0, capped = 0;
        for (int i = 0; i < 200; ++i, now += P) {  // the back end always full: the pipe rule alone
            const auto a = off.Turn(now, true, 2), b = three.Turn(now, true, 2);
            runOff = a.code ? runOff + 1 : 0;
            runCap = b.code ? runCap + 1 : 0;
            maxOff = std::max(maxOff, runOff);
            maxCap = std::max(maxCap, runCap);
            capped += b.capped;
            Require(!a.capped, "no cap: never capped");
        }
        Require(maxOff == 6, "without the cap a full back end skips 6 turns in a row (PETARI_PIPE_RUN)");
        Require(maxCap == 2, "GXV_SKIP_MAXSTEP=3: at most 2 turns skipped in a row");
        Require(capped > 0, "the forced pictures are reported");
        port::pace::LegacySkip late(5, 2, 6, nullptr, 3);
        now = P;
        late.Turn(now, true, 0);
        now += 5 * P;  // a turn 4 periods late: the debt rule wants to skip
        unsigned run = 0, maxRun = 0;
        for (int i = 0; i < 8; ++i, now += P / 4) { const auto v = late.Turn(now, true, 0); run = v.code ? run + 1 : 0; maxRun = std::max(maxRun, run); }
        Require(maxRun <= 2, "the cap also bounds the late (catch-up) skips");
        port::pace::LegacySkip quiet(5, 2, 6, nullptr, 3);
        Require(quiet.Turn(P, false, 5).code == 0 && !quiet.Turn(2 * P, false, 5).capped, "a load or scene change draws, not counted as capped");
        port::pace::LegacySkip odd(5, 2, 6, nullptr, 1);
        now = P;
        unsigned skippedOdd = 0;
        for (int i = 0; i < 20; ++i, now += P) skippedOdd += odd.Turn(now, true, 2).code != 0;
        Require(skippedOdd > 0, "values under 2 are off");
    }
    std::printf("cap  fe ms   fps  speed  steps 1/2/3/4/5+          gap p99  max   >60ms  >75ms (a 5 s window)\n");
    for (double fe : {20.0, 22.0, 24.0}) {
        for (int cap : {0, 4, 3}) {
            double fps = 0, speed = 0, p99 = 0, mx = 0, o60 = 0, o75 = 0;
            unsigned st[6] = {};
            for (unsigned seed = 1; seed <= 4; ++seed) {
                const Result r = Run(cap, fe, seed);
                fps += r.fps / 4; speed += r.speed / 4; p99 += r.p99 / 4; mx = std::max(mx, r.maxGap); o60 += r.over60 / 4; o75 += r.over75 / 4;
                for (int i = 0; i < 6; ++i) st[i] += r.steps[i];
            }
            std::printf("%3d  %5.0f  %5.1f  %5.1f%%  %5u/%5u/%5u/%5u/%4u  %5.0f  %5.0f  %5.1f  %5.1f\n", cap, fe, fps, speed, st[1], st[2], st[3], st[4], st[5], p99, mx, o60, o75);
            if (cap == 3) {
                const Result base = Run(0, fe, 1), capped = Run(3, fe, 1);
                Require(capped.steps[4] == 0 && capped.steps[5] == 0, "with 3 no picture advances 4 turns or more");
                Require(capped.fps >= base.fps - 1.0, "and the picture rate is kept (within 1 a second)");
                Require(capped.speed >= 94.0, "at a game speed of 94 % or more");
                Require(capped.maxGap < base.maxGap, "the longest gap is shorter");
            }
        }
    }
    std::printf("skip_maxstep_model: ok\n");
    return 0;
}
