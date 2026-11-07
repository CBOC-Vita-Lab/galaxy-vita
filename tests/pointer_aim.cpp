// Host test of the mouse-style pointer arithmetic (platform/include/port/pointer_aim.h).
#include "port/pointer_aim.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace port::aim;
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static const float kWide = 960.0f / 544.0f;

// Reads a stick held at (sx, sy) for `seconds` at `rate` reads a second; returns the state.
static State run(State s, float sx, float sy, float seconds, int rate, float aspect = kWide) {
    const int reads = int(std::lround(seconds * rate));
    for (int i = 0; i < reads; ++i) Step(s, sx, sy, 1.0f / rate, aspect);
    return s;
}

int main() {
    // Dead zone: nothing moves inside it, even for many reads, and the hold timer stays at zero.
    {
        State s;
        for (float r : {0.0f, 0.05f, 0.10f, 0.139f}) {
            assert(!Step(s, r, 0, 1.0f / 60, kWide) && !Step(s, 0, -r, 1.0f / 60, kWide));
            assert(!Step(s, r * 0.7f, r * 0.7f, 1.0f / 60, kWide));
        }
        assert(s.x == 0 && s.y == 0 && s.held == 0);
        // Just outside it the movement starts from nothing (no jump at the edge of the zone) and grows with the push.
        State a, b, c;
        assert(Step(a, 0.145f, 0, 1.0f / 60, kWide) && Step(b, 0.5f, 0, 1.0f / 60, kWide) && Step(c, 1.0f, 0, 1.0f / 60, kWide));
        assert(a.x > 0 && a.x < 1e-4f && a.x < b.x && b.x < c.x);
        float dx, dy;
        assert(Deflection(0.14f, 0, dx, dy) == 0 && near(Deflection(1.0f, 0, dx, dy), 1.0f) && dx == 1 && dy == 0);
        assert(near(Deflection(2.0f, 2.0f, dx, dy), 1.0f) && near(dx, 0.70710678f) && near(dy, 0.70710678f));  // the corner is capped at a full push
        assert(near(Deflection(0.57f, 0, dx, dy), 0.5f));  // halfway between the edge of the zone and full
    }
    // The speed curve: slow and exact for a small push, a bit faster than the 1.0 path (2 units a second) for a full push.
    {
        assert(Speed(0) == 0 && near(Speed(1), kSlow + kFast) && near(Speed(1), 2.25f));
        assert(Speed(0.5f) < 0.5f * Speed(1));            // below the straight line: fine control
        assert(near(Speed(0.5f), 0.35f * 0.5f + 1.9f * 0.125f));
        float last = 0;
        for (int i = 1; i <= 100; ++i) { const float v = Speed(i / 100.0f); assert(v > last); last = v; }  // strictly increasing
    }
    // Directions: right is +x; stick up is -y (the pointer's y grows downwards); equal pixels a second on both axes.
    {
        State r = run({}, 1, 0, 0.1f, 60), u = run({}, 0, 1, 0.1f, 60), d = run({}, 0, -1, 0.1f, 60), l = run({}, -1, 0, 0.1f, 60);
        assert(r.x > 0 && r.y == 0 && l.x < 0 && u.y < 0 && u.x == 0 && d.y > 0);
        assert(near(r.x, -l.x) && near(u.y, -d.y));
        const float pxX = r.x * 480.0f, pxY = -u.y * 272.0f;  // 2 units across 960 and 544 px
        assert(near(pxX, pxY, 0.01f));
        State sq = run({}, 0, 1, 0.1f, 60, 1.0f);  // aspect 1: the same units both ways
        assert(near(-sq.y, r.x));
        // The 4:3 picture (725 px wide) has its own aspect.
        State n = run({}, 0, 1, 0.1f, 60, 544.0f * 4 / 3 / 544.0f);
        assert(near(-n.y / r.x, 4.0f / 3.0f, 1e-3f));
        // A diagonal moves along the stick's direction.
        State dg = run({}, 0.6f, 0.6f, 0.1f, 60, 1.0f);
        assert(near(dg.x, -dg.y) && dg.x > 0);
    }
    // Acceleration while held: the gain climbs from 1 to 1 + kRamp over kRampSeconds, then stays; integrated exactly.
    {
        assert(near(GainIntegral(0, 0), 0) && near(GainIntegral(0, 0.3f), 0.3f + 0.6f * 0.09f / 1.2f));
        assert(near(GainIntegral(0, 1.0f), 1.0f + 0.6f * (0.3f + 0.4f)));
        assert(near(GainIntegral(2.0f, 1.0f), 1.6f));                   // fully ramped
        assert(near(GainIntegral(0.2f, 1.0f), GainIntegral(0, 1.2f) - GainIntegral(0, 0.2f)));
        const State fresh = run({}, 0.5f, 0, 0.5f, 60), later = run(State{0, 0, 3.0f}, 0.5f, 0, 0.5f, 60);
        assert(later.x > fresh.x * 1.2f && near(later.x / fresh.x, 0.8f / 0.625f, 1e-3f));                                // the same push goes further once held
        State s = run({}, 0.5f, 0, 0.5f, 60);
        assert(near(s.held, 0.5f, 1e-3f));
        Step(s, 0, 0, 1.0f / 60, kWide);                                 // letting go ends the acceleration
        assert(s.held == 0);
        const float before = s.x;
        Step(s, 0.5f, 0, 1.0f / 60, kWide);
        State again = {};
        Step(again, 0.5f, 0, 1.0f / 60, kWide);
        assert(near(s.x - before, again.x));                             // a new push starts slow again
    }
    // Screen clamp: the pointer stops at the edge of the picture on each axis and never leaves it.
    {
        State s = run({}, 1, 0, 5.0f, 60);
        assert(s.x == 1.0f && s.y == 0);
        s = run(s, 1, 1, 5.0f, 60);
        assert(s.x == 1.0f && s.y == -1.0f);
        s = run(s, -1, -1, 10.0f, 30);
        assert(s.x == -1.0f && s.y == 1.0f);
        State e = run({}, 0.3f, -0.9f, 20.0f, 30);
        assert(e.x >= -1 && e.x <= 1 && e.y >= -1 && e.y <= 1 && e.y == 1.0f);
        // Pushing against an edge keeps the other axis sliding.
        State edge{1.0f, 0, 0};
        edge = run(edge, 1, -0.5f, 0.2f, 60);
        assert(edge.x == 1.0f && edge.y > 0);
    }
    // Frame-rate independence: the same stick for the same time lands in the same place at 30 and 60 reads a second.
    {
        for (float sx : {0.25f, 0.3f, -0.35f}) for (float sy : {0.0f, 0.1f, -0.15f}) {
            const State a = run({}, sx, sy, 2.0f, 30), b = run({}, sx, sy, 2.0f, 60), c = run({}, sx, sy, 2.0f, 120);
            assert(std::fabs(a.x) < 1 && std::fabs(a.y) < 1);  // short of the clamp, so this tests the arithmetic
            assert(near(a.x, b.x, 2e-5f) && near(a.y, b.y, 2e-5f) && near(c.x, b.x, 2e-5f) && near(c.y, b.y, 2e-5f));
            assert(near(a.held, 2.0f, 1e-3f) && near(b.held, 2.0f, 1e-3f));
        }
        // A push, a pause and a second push (the boundaries fall on read boundaries of both rates).
        auto script = [](int rate) {
            State s;
            s = run(s, 0.45f, 0.1f, 0.5f, rate);
            s = run(s, 0.0f, 0.0f, 0.2f, rate);
            s = run(s, -0.3f, -0.4f, 0.8f, rate);
            s = run(s, 0.25f, 0.0f, 0.5f, rate);
            return s;
        };
        const State a = script(30), b = script(60);
        assert(std::fabs(b.x) < 1 && std::fabs(b.y) < 1);
        assert(near(a.x, b.x, 5e-5f) && near(a.y, b.y, 5e-5f));
        // Into the clamp: both rates end on the same edge.
        const State c = run({}, 1, -1, 3.0f, 30), d = run({}, 1, -1, 3.0f, 60);
        assert(c.x == d.x && c.y == d.y && c.x == 1.0f && c.y == 1.0f);
        // A read longer than the 1.0 path's cap counts as the cap, not as the whole gap (a hitch does not fling the pointer).
        State hitch, capped;
        Step(hitch, 0.5f, 0, 0.5f, kWide);
        Step(capped, 0.5f, 0, kMaxStep, kWide);
        assert(hitch.x == capped.x && near(hitch.held, kMaxStep));
    }
    // Bad numbers never move the pointer into NaN.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        State s{0.25f, -0.25f, 0};
        assert(!Step(s, nan, 0, 1.0f / 60, kWide) && !Step(s, 0, inf, 1.0f / 60, kWide));
        Step(s, 0.8f, 0, nan, kWide);       // an unreadable interval moves nothing
        Step(s, 0.8f, 0, -1.0f, kWide);
        Step(s, 0.8f, 0, 1.0f / 60, nan);   // an unusable aspect falls back to 1
        assert(std::isfinite(s.x) && std::isfinite(s.y) && s.x > 0.25f && s.y == -0.25f);
    }
    std::puts("PASS: mouse-style pointer arithmetic (dead zone, curve, hold acceleration, clamp, 30/60 reads a second)");
}
