// warpsteady31 host test: port/hrw_steady.h (GXV_HRW_PLANE_RAMP, GXV_HRW_HOLD30_MS, GXV_HRW_EDGE_PX). tools/test_hrw_steady.sh builds and runs it, then runs it against mutants of the header
// (each must FAIL). "A flip" below is one change of the second picture's method (plane / no plane, 60 / 30) between neighbouring pairs: what the tester sees as the picture flipping.
#include "port/hrw_steady.h"
#include <cstdio>
#include <cstdlib>
using namespace port::hrwsteady;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static port::hrw36::Plane MakeTestPlane(double camH) {
    const float P[16] = {1.2f, 0, 0, 0, 0, 1.8f, 0, 0, 0.0f, 0.0f, -1.002f, -1.0f, 0, 0, -2.0f, 0};
    const float g[3] = {0.0f, -static_cast<float>(camH), -400.0f}, n[3] = {0.0f, 1.0f, 0.0f}, t[3] = {3.0f, 0.5f, 2.0f};
    port::hrw36::Plane L;
    const int why = port::hrw36::MakePlane(P, g, n, t, 640, 360, 500.0, 40.0, 60.0, L);
    CHECK(why == port::hrw36::kPlaneOk);
    return L;
}
// a deterministic pseudo-random sequence: 1 = the pair has ground information
static unsigned Rng(unsigned& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

int main() {
    // ---- Scaled: the weighted plane gives w * (the plane's inverse depth) at every pixel, fade included
    {
        const port::hrw36::Plane L = MakeTestPlane(120.0);
        double worst = 0.0;
        const double ws[] = {0.25, 0.5, 0.75, 0.9};
        for (double w : ws)
            for (int iy = -4; iy <= 4; ++iy)
                for (int ix = -4; ix <= 4; ++ix) {
                    const double x = ix * 0.25, y = iy * 0.25;
                    const double a = port::hrw36::EvalK(Scaled(L, w), x, y), b = w * port::hrw36::EvalK(L, x, y);
                    worst = std::max(worst, std::fabs(a - b) / std::max(1e-6, std::fabs(b) + 1e-3));
                }
        CHECK(worst < 1e-3);
        CHECK(Scaled(L, 1.0).on && Scaled(L, 1.5).K[0] == L.K[0]);
        CHECK(!Scaled(L, 0.0).on && !Scaled(L, -1.0).on);
        // the near clamp is weighted too: a plane whose kMax binds at the near pixels
        port::hrw36::Plane C = L; C.kMax = static_cast<float>(port::hrw36::EvalK(L, 0.0, -0.9) * 0.5);
        CHECK(std::fabs(port::hrw36::EvalK(Scaled(C, 0.5), 0.0, -0.9) - 0.5 * port::hrw36::EvalK(C, 0.0, -0.9)) < 1e-9);
        // monotone in w at a near pixel (the plane comes in, never overshoots)
        double prev = -1.0;
        for (int i = 1; i <= 8; ++i) { const double k = port::hrw36::EvalK(Scaled(L, i / 8.0), 0.0, -0.5); CHECK(k >= prev); prev = k; }
        CHECK(std::fabs(prev - port::hrw36::EvalK(L, 0.0, -0.5)) < 1e-9);
    }
    // ---- PlaneRamp: steps, off = as before
    {
        PlaneRamp r; r.Configure(0);
        const float g[3] = {0, -100, -300}, n[3] = {0, 1, 0};
        CHECK(r.Good(g, n) == 1.0);
        CHECK(!r.CanDecay());
        r.Configure(4);
        double w[6];
        for (int i = 0; i < 6; ++i) w[i] = r.Good(g, n);
        CHECK(w[0] == 0.25 && w[1] == 0.5 && w[2] == 0.75 && w[3] == 1.0 && w[4] == 1.0);
        CHECK(r.CanDecay());
        CHECK(r.Decay() == 0.75 && r.Decay() == 0.5 && r.Decay() == 0.25 && r.Decay() == 0.0);
        CHECK(!r.CanDecay());                       // weight spent
        r.Good(g, n); r.Good(g, n);                 // 0.25, 0.5 ...
        r.Reset();                                  // ... a pair that cannot use a plane at all: gone at once
        CHECK(!r.CanDecay() && r.level == 0);
        r.Good(g, n);
        CHECK(r.Good(g, n) == 0.5);                 // restarts from the bottom
        // the last ground is used for at most `steps` pairs
        r.Configure(6);
        for (int i = 0; i < 6; ++i) r.Good(g, n);
        int used = 0; while (r.CanDecay() && used < 100) { r.Decay(); ++used; }
        CHECK(used == 6);
        // a gap that comes back: the weight resumes from where it fell, not from 1
        r.Configure(4);
        for (int i = 0; i < 4; ++i) r.Good(g, n);
        r.Decay(); r.Decay();                        // level 2
        CHECK(r.Good(g, n) == 0.75 && r.age == 0);
    }
    // ---- the plane flip on a 70 % trace: 0..1 per pair without the ramp, small steps with it
    {
        unsigned s = 12345;
        double maxStepOff = 0.0, maxStepOn = 0.0; int flipsOff = 0, flipsOn = 0; double prevOff = 0.0, prevOn = 0.0;
        PlaneRamp r; r.Configure(5);
        const float g[3] = {0, -100, -300}, n[3] = {0, 1, 0};
        for (int i = 0; i < 4000; ++i) {
            const bool ground = (Rng(s) % 100) < 70;
            const double off = ground ? 1.0 : 0.0;
            double on;
            if (ground) on = r.Good(g, n);
            else if (r.CanDecay()) on = r.Decay();
            else { r.Reset(); on = 0.0; }
            if (i) { maxStepOff = std::max(maxStepOff, std::fabs(off - prevOff)); maxStepOn = std::max(maxStepOn, std::fabs(on - prevOn)); flipsOff += off != prevOff; flipsOn += std::fabs(on - prevOn) >= 0.5; }
            prevOff = off; prevOn = on;
        }
        CHECK(maxStepOff == 1.0 && flipsOff > 1000);
        CHECK(maxStepOn <= 0.2 + 1e-9 && flipsOn == 0);
    }
    // ---- Hold30: 500 ms of 30 after a motion give-up, none for the other reasons, off = never
    {
        Hold30 h; h.Configure(0);
        h.Gave(true, 1000.0);
        CHECK(!h.Active(1001.0) && h.armed == 0);
        h.Configure(500);
        CHECK(!h.Active(0.0));
        h.Gave(false, 100.0);                        // a structural reason: no hold
        CHECK(!h.Active(101.0) && h.armed == 0);
        h.Gave(true, 1000.0);
        CHECK(h.Active(1001.0) && h.Active(1499.0) && !h.Active(1500.0) && !h.Active(2000.0));
        CHECK(h.armed == 1 && h.held == 2);
        h.Gave(true, 3000.0); h.Gave(true, 3100.0);  // another give-up during the window moves the end
        CHECK(h.Active(3599.0) && !h.Active(3600.0));
        h.Configure(999999);
        CHECK(h.holdMs == 5000.0);
    }
    // ---- the pacing: a stress trace of pairs every 33.3 ms where a camera pan makes about 1 pair in 3 give up (the edge strip near its limit)
    {
        // without the hold: the method flips on every give-up. With it: the 30 comes in runs of 500 ms (15 pairs) at least.
        unsigned s = 777;
        int flipsOff = 0, flipsOn = 0, shortRunsOn = 0, lenOn = 0, runOn = 0, shown60Off = 0, shown60On = 0;
        bool prevOff = true, prevOn = true; double t = 0.0;
        Hold30 h; h.Configure(500);
        const int N = 6000;
        for (int i = 0; i < N; ++i, t += 33.3) {
            const bool bad = (Rng(s) % 100) < 30;
            const bool okOff = !bad;
            bool okOn;
            if (h.Active(t)) okOn = false;
            else { okOn = !bad; if (!okOn) h.Gave(true, t); }
            flipsOff += okOff != prevOff; flipsOn += okOn != prevOn;
            shown60Off += okOff; shown60On += okOn;
            if (!okOn) ++runOn; else { if (runOn) { ++lenOn; if (runOn < 15) ++shortRunsOn; } runOn = 0; }
            prevOff = okOff; prevOn = okOn;
        }
        CHECK(flipsOff > 1500);                     // the alternation
        CHECK(flipsOn < flipsOff / 3);              // two flips per ~19 pairs against 0.42 per pair
        CHECK(shortRunsOn == 0 && lenOn > 10);     // every 30 run lasts at least 500 ms
        CHECK(shown60On < shown60Off);              // the price: less 60 (the console A/B decides)
    }
    // ---- the edge limit: one number
    {
        CHECK(EdgeLimit(-1, 48) == 48.0);          // the shipped default
        CHECK(EdgeLimit(24, 48) == 24.0);          // GXV_HRW_EDGE_PX wins
        CHECK(EdgeLimit(-1, 64) == 64.0);          // the old key still works (the 1115 owner trial: 64)
        CHECK(EdgeLimit(0, 48) == 0.0);            // 0 = never
        CHECK(EdgeLimit(-1, -5) == 0.0);
        bool off = false;
        CHECK(!EdgeGiveUp(48.0, 48.0, off) && !off);           // at the limit: still warped
        CHECK(EdgeGiveUp(48.0, 48.5, off) && off);             // over: given up
        CHECK(EdgeGiveUp(48.0, 40.0, off));                    // hysteresis: 40 is above 36 (75 %), stays off
        CHECK(!EdgeGiveUp(48.0, 35.9, off) && !off);           // below 75 %: resumes
        bool o2 = false;
        CHECK(!EdgeGiveUp(0.0, 1000.0, o2));                   // never
        bool o3 = false;
        CHECK(EdgeGiveUp(24.0, 30.0, o3) && !EdgeGiveUp(48.0, 30.0, o2));  // 30 px: given up at 24, shown at 48
    }
    printf(fails ? "hrw_steady: %d FAILED\n" : "hrw_steady: all passed\n", fails);
    return fails ? 1 : 0;
}
