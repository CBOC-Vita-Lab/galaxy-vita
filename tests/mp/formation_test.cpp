// Host tests for the puppet formation logic (owner: formation): decomp/include/Game/MP/MpFormationLogic.hpp (header only).
// Frames on the local gravity's tangent plane (flat ground, small planets, zero gravity, a camera looking straight down), the latched
// frame, formation and photo places, the engage / release rules (demo, spawn window, grace, apart, star), ground candidates (narrow
// paths), the spring blend (nothing pops, nothing overshoots), long moves (appear), drawn rotations, ranks, hostile numbers.
//
// Build and run (from the worktree root): sh tests/mp/formation_build.sh
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "Game/MP/MpFormationLogic.hpp"

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpFormation;

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool nearV(const V3& a, const V3& b, float eps = 1e-3f) { return len(sub(a, b)) <= eps; }
static bool orthonormal(const Frame& f) {
    return near(len(f.up), 1.0f) && near(len(f.right), 1.0f) && near(len(f.back), 1.0f) && near(dot(f.up, f.right), 0.0f) &&
           near(dot(f.up, f.back), 0.0f) && near(dot(f.right, f.back), 0.0f);
}

// a camera at `eye` looking at `at` with the world's up hint
struct Cam {
    V3 right, fwd, up;
};
static Cam lookAt(const V3& eye, const V3& at, const V3& upHint) {
    Cam c;
    c.fwd = sub(at, eye);
    normalize(&c.fwd);
    c.right = cross(c.fwd, upHint);
    normalize(&c.right);
    c.up = cross(c.right, c.fwd);
    return c;
}

static TurnIn turn(const V3& anchor, const V3& gravity, const Cam& c) {
    TurnIn t;
    t.active = true;
    t.anchor = anchor;
    t.cutscene = t.spawn = t.star = false;
    makeFrame(gravity, c.right, c.fwd, c.up, &t.cam);
    return t;
}
static SlotIn slotAt(const V3& p, bool onGround = false) {
    SlotIn s;
    s.eligible = true;
    s.pos = p;
    s.onGround = onGround;
    s.inDemo = false;
    return s;
}

// one full turn for one slot (as MpFormation.cpp does it, without the game's ground checks: every candidate has ground)
static SlotOut step(Formation& f, int slot, const TurnIn& t, const SlotIn& in, int rank, int n, uint8_t ground = 0xF) {
    f.decide(slot, t, in);
    f.s[slot].ground = ground;
    const V3 tg = f.target(slot, t, in, rank, n);
    return f.finish(slot, in, tg, f.s[slot].mode == MODE_ARC ? 1.0f : 0.0f);
}

static void testFrames() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 100, 0), v3(0, 1, 0));
    Frame f;
    CHECK(makeFrame(v3(0, -1, 0), c.right, c.fwd, c.up, &f));
    CHECK(orthonormal(f));
    CHECK(nearV(f.up, v3(0, 1, 0)) && nearV(f.right, v3(1, 0, 0)) && nearV(f.back, v3(0, 0, -1)));
    CHECK(dot(f.back, c.fwd) > 0.0f);   // back = away from the camera
    // gravity not unit, sideways (a wall planet): still orthonormal, up opposite the gravity
    CHECK(makeFrame(v3(-9.8f, 0, 0), c.right, c.fwd, c.up, &f) && orthonormal(f) && nearV(f.up, v3(1, 0, 0)));
    // zero gravity: the camera's up
    CHECK(makeFrame(v3(0, 0, 0), c.right, c.fwd, c.up, &f) && orthonormal(f) && nearV(f.up, c.up));
    // a camera rolled so its right is along the up: the forward decides
    CHECK(makeFrame(v3(0, -1, 0), v3(0, 1, 0), v3(0, 0, -1), v3(-1, 0, 0), &f) && orthonormal(f) && dot(f.back, v3(0, 0, -1)) > 0.99f);
    // a camera straight above looking down (top-down): its right still gives the side
    const Cam top = lookAt(v3(0, 1000, 0), v3(0, 0, 0), v3(0, 0, -1));
    CHECK(makeFrame(v3(0, -1, 0), top.right, top.fwd, top.up, &f) && orthonormal(f));
    // nonsense in: false, no NaN out
    const float nan = std::nanf("");
    CHECK(!makeFrame(v3(nan, 0, 0), v3(nan, 0, 0), v3(nan, 0, 0), v3(nan, 0, 0), &f));
    CHECK(!makeFrame(v3(0, 0, 0), v3(0, 0, 0), v3(0, 0, 0), v3(0, 0, 0), &f));

    // small planet (radius 300 around the origin): at every point the frame lies on the tangent plane and spots stay ~level
    for (int k = 0; k < 64; k++) {
        const float a = k * 0.1f, b = k * 0.37f;
        const V3 n = v3(std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b));
        const V3 pos = mul(n, 300.0f);
        const Cam cc = lookAt(add(pos, add(mul(n, 400.0f), v3(0.3f, 0.1f, 0.2f))), pos, v3(0, 1, 0));
        Frame g;
        const bool ok = makeFrame(mul(n, -1.0f), cc.right, cc.fwd, cc.up, &g);
        CHECK(ok);
        if (!ok) continue;
        CHECK(orthonormal(g) && nearV(g.up, n));
        const V3 spot = add(pos, mul(spaceDir(g, 0, 0), kSpacing));
        CHECK(near(dot(sub(spot, pos), n), 0.0f, 1e-2f));   // on the tangent plane (the caller's ground check brings it down)
        CHECK(len(spot) - 300.0f < 62.0f);                   // radius 300: the tangent spot floats ~58 units (sqrt(300^2+190^2)-300)
    }
}

static void testLatch() {
    Latch l;
    l.reset();
    bool re = false;
    Frame cam;
    const Cam c0 = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    makeFrame(v3(0, -1, 0), c0.right, c0.fwd, c0.up, &cam);
    Frame f = l.update(cam, &re);
    CHECK(!re && l.on && nearV(f.right, cam.right) && orthonormal(f));
    // the camera turns 30 degrees around the player: the latched frame stays (no swinging)
    auto around = [](float deg) {
        const float a = deg * 0.017453293f;
        return lookAt(v3(800 * std::sin(a), 300, 800 * std::cos(a)), v3(0, 0, 0), v3(0, 1, 0));
    };
    Cam c1 = around(30.0f);
    makeFrame(v3(0, -1, 0), c1.right, c1.fwd, c1.up, &cam);
    f = l.update(cam, &re);
    CHECK(!re && nearV(f.right, v3(1, 0, 0)));
    // 180 degrees (a cut to the other side): kept (the puppet is on the other side of the screen, still apart)
    c1 = around(180.0f);
    makeFrame(v3(0, -1, 0), c1.right, c1.fwd, c1.up, &cam);
    f = l.update(cam, &re);
    CHECK(!re && nearV(f.right, v3(1, 0, 0)));
    // 90 degrees: a puppet on the latched side would stand in front of / behind the player: latched again
    c1 = around(90.0f);
    makeFrame(v3(0, -1, 0), c1.right, c1.fwd, c1.up, &cam);
    f = l.update(cam, &re);
    CHECK(re && nearV(f.right, cam.right));
    // the photo (arc): 180 degrees (the star-get camera looks at the player from the front) is latched again: "behind" is behind
    l.reset();
    makeFrame(v3(0, -1, 0), c0.right, c0.fwd, c0.up, &cam);
    l.update(cam, &re, true);
    c1 = around(180.0f);
    makeFrame(v3(0, -1, 0), c1.right, c1.fwd, c1.up, &cam);
    f = l.update(cam, &re, true);
    CHECK(re && nearV(f.right, cam.right) && nearV(f.back, cam.back));
    // the first latch after a reset is no re-latch (only a camera turn counts)
    l.reset();
    f = l.update(cam, &re);
    CHECK(!re && l.on);
    // the gravity turns (walking round a planet): the right is carried along, stays on the new tangent plane
    l.reset();
    makeFrame(v3(0, -1, 0), c0.right, c0.fwd, c0.up, &cam);
    l.update(cam, &re);
    for (int k = 1; k <= 30; k++) {
        const float a = k * 0.02f;   // up tilts toward -z, 1.1 degrees a turn
        const V3 up = v3(0, std::cos(a), -std::sin(a));
        Frame g;
        g.up = up;
        g.right = v3(1, 0, 0);   // (the camera keeps its right: no relatch expected)
        g.back = cross(up, g.right);
        f = l.update(g, &re);
        CHECK(!re && orthonormal(f) && near(dot(f.right, up), 0.0f));
    }
}

static void testPlaces() {
    Frame f;
    f.up = v3(0, 1, 0);
    f.right = v3(1, 0, 0);
    f.back = v3(0, 0, -1);
    CHECK(nearV(spaceDir(f, 0, 0), v3(1, 0, 0)) && nearV(spaceDir(f, 1, 0), v3(-1, 0, 0)) && nearV(spaceDir(f, 2, 0), v3(0, 0, -1)));
    CHECK(nearV(spaceDir(f, 0, 1), v3(-1, 0, 0)) && nearV(spaceDir(f, 0, 2), v3(0, 0, -1)) && nearV(spaceDir(f, 0, 3), v3(0, 0, 1)));
    CHECK(nearV(spaceDir(f, 9, 9), spaceDir(f, 2, 3)) && nearV(spaceDir(f, -3, -3), spaceDir(f, 0, 0)));   // clamped
    // the first choices of ranks 0..2 are three different places
    CHECK(!nearV(spaceDir(f, 0, 0), spaceDir(f, 1, 0)) && !nearV(spaceDir(f, 1, 0), spaceDir(f, 2, 0)) &&
          !nearV(spaceDir(f, 0, 0), spaceDir(f, 2, 0)));
    // the photo: every place behind the player (away from the camera), on the plane, apart from each other and from the player
    for (int n = 1; n <= 3; n++) {
        for (int r = 0; r < n; r++) {
            const V3 o = arcOffset(f, r, n, 0);
            CHECK(dot(o, f.back) > 0.0f && near(dot(o, f.up), 0.0f) && len(o) >= kArcRadius - 0.01f);
            for (int q = 0; q < r; q++) CHECK(len(sub(o, arcOffset(f, q, n, 0))) > 150.0f);
            CHECK(near(dot(arcOffset(f, r, n, 1), f.right), -dot(o, f.right)));          // mirrored
            CHECK(near(len(arcOffset(f, r, n, 2)), len(o) * 0.5f, 0.01f));               // half radius
        }
    }
    CHECK(near(dot(arcOffset(f, 0, 2, 0), f.right), -dot(arcOffset(f, 1, 2, 0), f.right)));   // two: symmetric
    CHECK(len(arcOffset(f, 2, 3, 0)) > len(arcOffset(f, 0, 3, 0)));                          // three: the middle further back
    // ranks: by slot among the others present
    CHECK(rankOf(0, 1, 0xB) == 0 && rankOf(1, 1, 0xB) == -1 && rankOf(3, 1, 0xB) == 1 && rankOf(2, 1, 0xB) == -1);
    CHECK(rankOf(1, 0, 0xF) == 0 && rankOf(2, 0, 0xF) == 1 && rankOf(3, 0, 0xF) == 2 && remoteCount(0, 0xF) == 3);
    CHECK(rankOf(0, 3, 0xF) == 0 && rankOf(2, 3, 0xF) == 2 && remoteCount(3, 0x9) == 1 && remoteCount(1, 0x2) == 0);
}

static void testEngageRelease() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    Formation f;
    f.reset();
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    // close but no reason (plain play): nothing
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(10, 0, 0))) == MODE_NONE && f.s[1].event == EV_NONE);
    // a demo: on (event once)
    t.cutscene = true;
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(10, 0, 0))) == MODE_SPACE && f.s[1].event == EV_ON && f.s[1].why == WHY_DEMO);
    CHECK(f.decide(1, t, slotAt(v3(10, 0, 0))) == MODE_SPACE && f.s[1].event == EV_NONE);
    // too far to engage
    CHECK(f.decide(2, t, slotAt(v3(130, 0, 0))) == MODE_NONE);
    // its own STATE in a demo is a reason too
    t.cutscene = false;
    SlotIn d = slotAt(v3(0, 0, 50));
    d.inDemo = true;
    CHECK(f.decide(2, t, d) == MODE_SPACE && f.s[2].why == WHY_DEMO);
    // the spawn window: why = spawn
    f.reset();
    t.spawn = true;
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(0, 0, 0))) == MODE_SPACE && f.s[1].why == WHY_SPAWN);
    // grace: after the window it lasts kGraceTurns while still close, then off
    t.spawn = false;
    int turns = 0;
    while (turns < kGraceTurns + 100 && f.decide(1, t, slotAt(v3(0, 0, 0))) == MODE_SPACE) turns++;
    CHECK(turns == kGraceTurns + 100);   // still inside the player after the grace: held (not slid back into him)
    CHECK(f.decide(1, t, slotAt(v3(20, 0, 0))) == MODE_SPACE);                       // it moved a little: still held
    CHECK(f.decide(1, t, slotAt(v3(kHoldMove + 5.0f, 0, 0))) == MODE_NONE && f.s[1].event == EV_OFF && f.s[1].why == WHY_GRACE);
    // apart: released at once beyond kRelease (hysteresis: kept between kEngage and kRelease)
    f.reset();
    t.cutscene = true;
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(100, 0, 0))) == MODE_SPACE);
    CHECK(f.decide(1, t, slotAt(v3(200, 0, 0))) == MODE_SPACE);
    CHECK(f.decide(1, t, slotAt(v3(250, 0, 0))) == MODE_NONE && f.s[1].why == WHY_APART && f.s[1].event == EV_OFF);
    // not eligible (hidden, bubble, dead): off
    CHECK(f.decide(1, t, slotAt(v3(0, 0, 0))) == MODE_SPACE);
    SlotIn h = slotAt(v3(0, 0, 0));
    h.eligible = false;
    CHECK(f.decide(1, t, h) == MODE_NONE && f.s[1].why == WHY_HIDDEN);
    // the star: everyone within kGather joins the photo, farther ones do not; it ends with the demo
    f.reset();
    t.cutscene = true;
    t.star = true;
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(2500, 0, 0))) == MODE_ARC && f.s[1].why == WHY_STAR);
    CHECK(f.decide(2, t, slotAt(v3(3500, 0, 0))) == MODE_NONE);
    CHECK(f.decide(1, t, slotAt(v3(5000, 0, 0))) == MODE_ARC);   // (once in, it stays for the demo)
    t.star = false;
    t.cutscene = false;
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(5000, 0, 0))) == MODE_NONE && f.s[1].why == WHY_STAR_END && f.s[1].event == EV_OFF);
    // inactive: everything off
    t.active = false;
    t.cutscene = true;
    f.begin(t, true);
    CHECK(f.decide(1, t, slotAt(v3(0, 0, 0))) == MODE_NONE);
}

static void testTargets() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    Formation f;
    f.reset();
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    f.begin(t, true);
    // on top of each other: rank 0 to the right, rank 1 to the left, rank 2 behind, kSpacing away
    for (int r = 0; r < 3; r++) {
        f.decide(1 + r, t, slotAt(v3(0, 0, 0)));
        const V3 o = f.target(1 + r, t, slotAt(v3(0, 0, 0)), r, 3);
        CHECK(nearV(o, mul(spaceDir(f.frame, r, 0), kSpacing)));
    }
    // already partly apart along its side: only the missing part
    V3 o = f.target(1, t, slotAt(v3(60, 0, 0)), 0, 3);
    CHECK(nearV(o, v3(kSpacing - 60.0f, 0, 0)));
    // vertical difference (a jump) is never pushed
    o = f.target(1, t, slotAt(v3(0, 80, 0)), 0, 3);
    CHECK(nearV(o, v3(kSpacing, 0, 0)));
    // falloff between kEngage and kRelease: less push, none at kRelease
    f.decide(1, t, slotAt(v3(0, 0, 200)));
    o = f.target(1, t, slotAt(v3(0, 0, 200)), 0, 3);
    CHECK(o.x > 0.0f && o.x < kSpacing);
    CHECK(nearV(f.target(1, t, slotAt(v3(0, 0, kRelease)), 0, 3), v3(0, 0, 0)));
    // on the ground, a narrow path along the camera's view (no ground right or left): behind; nowhere: no push
    SlotIn g = slotAt(v3(0, 0, 0), true);
    f.decide(1, t, g);
    f.s[1].ground = 0x4 | 0x8;   // back, front
    CHECK(nearV(f.target(1, t, g, 0, 1), mul(f.frame.back, kSpacing)));
    f.s[1].ground = 0x2;         // only the left
    CHECK(nearV(f.target(1, t, g, 0, 1), v3(-kSpacing, 0, 0)));
    f.s[1].ground = 0;
    CHECK(nearV(f.target(1, t, g, 0, 1), v3(0, 0, 0)));
    // in the air (the arrival flight) the ground is not asked
    SlotIn air = slotAt(v3(0, 0, 0), false);
    CHECK(!f.needsGround(1, air) && nearV(f.target(1, t, air, 0, 1), v3(kSpacing, 0, 0)));
    // the photo: places behind the (sideways-following, never rising) anchor; no ground there = its place in the air
    f.reset();
    t.star = true;
    f.begin(t, true);
    f.decide(1, t, slotAt(v3(500, 0, 0)));
    f.s[1].ground = 0x1;
    CHECK(nearV(f.target(1, t, slotAt(v3(500, 0, 0)), 0, 2), sub(arcOffset(f.frame, 0, 2, 0), v3(500, 0, 0))));
    f.s[1].ground = 0x2;   // its own place has no ground: mirrored
    CHECK(nearV(f.target(1, t, slotAt(v3(500, 0, 0)), 0, 2), sub(arcOffset(f.frame, 0, 2, 1), v3(500, 0, 0))));
    f.s[1].ground = 0;
    CHECK(nearV(f.target(1, t, slotAt(v3(500, 0, 0)), 0, 2), sub(arcOffset(f.frame, 0, 2, 0), v3(500, 0, 0))));
    t.anchor = v3(40, 250, 0);   // the local player jumps with the star, a little sideways
    f.begin(t, true);
    CHECK(nearV(f.photoAnchor(t), v3(40, 0, 0)));
    // the candidates the caller checks are the places the target picks from
    f.decide(1, t, slotAt(v3(500, 0, 0)));
    CHECK(nearV(f.candidate(1, t, 0, 2, 1), add(v3(40, 0, 0), arcOffset(f.frame, 0, 2, 1))));
}

// 2, 3 and 4 players: every remote player's spot (rank 0..2) is its own, kSpacing from the local player, and no two spots coincide;
// the result depends on nothing but the inputs (same inputs, same spots: every console computes the same formation).
static void testSlotsForPlayers() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    for (int players = 2; players <= 4; players++) {
        for (int local = 0; local < players; local++) {
            V3 spots[3];
            int ns = 0;
            unsigned mask = (1u << players) - 1;
            Formation f;
            f.reset();
            TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
            t.cutscene = true;
            f.nRemote = remoteCount(local, mask);
            f.begin(t, true);
            for (int s = 0; s < players; s++) {
                const int rank = rankOf(s, local, mask);
                if (s == local) { CHECK(rank == -1); continue; }
                CHECK(rank >= 0 && rank <= 2);
                f.decide(s, t, slotAt(v3(0, 0, 0)));
                const V3 o = f.target(s, t, slotAt(v3(0, 0, 0)), rank, f.nRemote);
                CHECK(near(len(o), kSpacing, 0.01f));
                CHECK(near(dot(o, f.frame.up), 0.0f, 0.01f));
                for (int k = 0; k < ns; k++) CHECK(len(sub(o, spots[k])) > kSpacing * 0.9f);   // nobody on anybody
                spots[ns++] = o;
            }
            CHECK(ns == players - 1);
            // the same again from a fresh state: identical (no clock, no randomness)
            Formation g;
            g.reset();
            g.nRemote = f.nRemote;
            g.begin(t, true);
            int k = 0;
            for (int s = 0; s < players; s++) {
                if (s == local) continue;
                g.decide(s, t, slotAt(v3(0, 0, 0)));
                CHECK(nearV(g.target(s, t, slotAt(v3(0, 0, 0)), rankOf(s, local, mask), g.nRemote), spots[k++], 0.0f));
            }
        }
    }
}

// a lone puppet that truly stands to the left keeps the left: the way out of the formation never crosses the player
static void testSideKept() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    Formation f;
    f.reset();
    f.nRemote = 1;
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    f.begin(t, true);
    SlotIn left = slotAt(v3(-60, 0, 0));
    f.decide(1, t, left);
    CHECK(f.s[1].side == -1);
    V3 o = f.target(1, t, left, 0, 1);
    CHECK(o.x < 0.0f);                                           // pushed further left, not across
    CHECK(near(left.pos.x + o.x, -kSpacing, 0.01f));
    // exactly on top: by rank (right)
    Formation g;
    g.reset();
    g.nRemote = 1;
    g.begin(t, true);
    g.decide(1, t, slotAt(v3(0, 0, 0)));
    CHECK(g.s[1].side == 0);
    CHECK(g.target(1, t, slotAt(v3(0, 0, 0)), 0, 1).x > 0.0f);
    // several remote players: ranks only (two puppets must not take the same side)
    Formation h;
    h.reset();
    h.nRemote = 2;
    h.begin(t, true);
    h.decide(1, t, slotAt(v3(-60, 0, 0)));
    CHECK(h.s[1].side == 0);
    CHECK(h.target(1, t, slotAt(v3(-60, 0, 0)), 0, 2).x > 0.0f);
}

// a place over a death area or a hole (no ground candidate) is never taken; a lone puppet on a one-sided ledge goes to the side
// that has ground; no ground anywhere: no push at all (it stays at its true place)
static void testGroundRejection() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    Formation f;
    f.reset();
    f.nRemote = 1;
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    f.begin(t, true);
    SlotIn g = slotAt(v3(0, 0, 0), true);
    f.decide(1, t, g);
    for (unsigned m = 0; m < 16; m++) {
        f.s[1].ground = (uint8_t)m;
        const V3 o = f.target(1, t, g, 0, 1);
        if (m == 0) { CHECK(nearV(o, v3(0, 0, 0))); continue; }
        int pick = -1;
        for (int i = 0; i < 4 && pick < 0; i++) if (m & (1u << i)) pick = i;
        CHECK(nearV(o, mul(spaceDir(f.frame, 0, pick), kSpacing)));   // the first candidate with ground, in the fixed order
    }
}

// review2 #3: the ground probe (4 ray casts + 4 area queries per puppet) runs when a space begins and then every 8th turn, also when
// all four candidates have ground (the normal flat-floor result 0xF, which used to read as "not probed yet")
static void testGroundProbeRate() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    for (unsigned result = 0; result < 16; result += 5) {   // 0, 5, 10, 15: 0xF is the case that failed
        Formation f;
        f.reset();
        f.nRemote = 1;
        TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
        t.cutscene = true;   // a space (grace turns)
        SlotIn g = slotAt(v3(0, 0, 0), true);
        int probes = 0;
        for (uint32_t turnNo = 100; turnNo < 160; turnNo++) {
            f.begin(t, true);
            f.decide(1, t, g);
            if (f.s[1].mode != MODE_NONE && f.needsGround(1, g) && f.wantsGroundProbe(1, turnNo)) {
                probes++;   // (the caller's 4 ray casts happen here)
                f.setGround(1, (uint8_t)(result == 10 ? 0xF : result));
            }
            f.end();
        }
        if (!(probes >= 1 && probes <= 9)) printf("probes=%d result=%u\n", probes, result);
        CHECK(probes >= 1 && probes <= 9);   // 1 at the start + one per 8 turns
    }
}

// the star-get photo takes a far puppet whose own STATE says it is in a demo (its console runs the star-get too), not a far one that is not
static void testPhotoFarInDemo() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    Formation f;
    f.reset();
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = t.star = true;
    f.begin(t, true);
    SlotIn far1 = slotAt(v3(0, 0, 5000));
    CHECK(f.decide(1, t, far1) == MODE_NONE);
    far1.inDemo = true;
    CHECK(f.decide(1, t, far1) == MODE_ARC && f.s[1].why == WHY_STAR);
    const SlotOut o = f.finish(1, far1, f.target(1, t, far1, 0, 1), 1.0f);
    CHECK(o.appear);   // (it appears at the arc, it does not fly across the level)
}

// puppets that overlap each other away from the local player: the lowest slot stays, the others take places around it by their number
static void testGroups() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    const V3 spot = v3(3000, 0, 0);   // the group's shared place, far from the local player at the origin
    for (int air = 0; air < 2; air++) {
        Formation f;
        f.reset();
        f.nRemote = 3;
        TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
        t.cutscene = true;
        f.begin(t, true);
        SlotIn in[Formation::SLOTS];
        for (int i = 0; i < Formation::SLOTS; i++) in[i] = slotAt(spot, air == 0);
        in[0].eligible = false;   // the local player's own slot
        for (int i = 0; i < Formation::SLOTS; i++) f.decide(i, t, in[i]);
        f.decideGroups(t, in);
        CHECK(f.s[1].mode == MODE_NONE);   // the lowest slot of the cluster stays where it is
        CHECK(f.s[2].mode == MODE_GROUP && f.s[2].groot == 1 && f.s[2].grank == 0 && f.s[2].event == EV_ON);
        CHECK(f.s[3].mode == MODE_GROUP && f.s[3].groot == 1 && f.s[3].grank == 1);
        for (int i = 2; i <= 3; i++) f.s[i].ground = 0xF;
        const V3 o2 = f.target(2, t, in[2], 0, 3), o3 = f.target(3, t, in[3], 1, 3);
        CHECK(near(len(o2), kSpacing, 0.01f) && near(len(o3), kSpacing, 0.01f));
        CHECK(len(sub(o2, o3)) > kSpacing);                       // right and left: not on each other
        CHECK(dot(o2, f.frame.right) > 0.0f && dot(o3, f.frame.right) < 0.0f);
        // no ground at the right: the next candidate (left is taken by the next puppet in rank, so behind comes before for rank 0)
        if (air == 0) {
            f.s[2].ground = 0x4;
            CHECK(nearV(f.target(2, t, in[2], 0, 3), mul(f.frame.back, kSpacing)));
            f.s[2].ground = 0;
            CHECK(nearV(f.target(2, t, in[2], 0, 3), v3(0, 0, 0)));
        }
        // the same cluster again from a fresh state: the same places (deterministic)
        Formation g;
        g.reset();
        g.nRemote = 3;
        g.begin(t, true);
        for (int i = 0; i < Formation::SLOTS; i++) g.decide(i, t, in[i]);
        g.decideGroups(t, in);
        g.s[2].ground = g.s[3].ground = 0xF;
        CHECK(nearV(g.target(2, t, in[2], 0, 3), o2, 0.0f) && nearV(g.target(3, t, in[3], 1, 3), o3, 0.0f));
        // they move apart (3 units a turn, slot 3 only): released beyond kRelease, no group left on it, the offset falls to zero
        for (int k = 0; k < 400; k++) {
            in[3].pos = add(in[3].pos, v3(0, 0, 3.0f));
            for (int i = 0; i < Formation::SLOTS; i++) g.decide(i, t, in[i]);
            g.decideGroups(t, in);
        }
        CHECK(g.s[3].mode == MODE_NONE && g.s[2].mode == MODE_GROUP);
        // a puppet apart from every other one is never grouped, and nothing is grouped in normal play (no reason, no previous group)
        Formation h;
        h.reset();
        h.nRemote = 3;
        TurnIn quiet = turn(v3(0, 0, 0), v3(0, -1, 0), c);
        h.begin(quiet, true);
        for (int i = 0; i < Formation::SLOTS; i++) h.decide(i, quiet, in[i]);
        h.decideGroups(quiet, in);
        for (int i = 0; i < Formation::SLOTS; i++) CHECK(h.s[i].mode == MODE_NONE);
    }
    // held after the grace (they still stand on each other): no slide back until one has really moved
    Formation f;
    f.reset();
    f.nRemote = 2;
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    f.begin(t, true);
    SlotIn in[Formation::SLOTS];
    for (int i = 0; i < Formation::SLOTS; i++) in[i] = slotAt(spot);
    in[0].eligible = false;
    in[3].eligible = false;
    auto turnAll = [&]() { for (int i = 0; i < Formation::SLOTS; i++) f.decide(i, t, in[i]); f.decideGroups(t, in); };
    turnAll();
    CHECK(f.s[2].mode == MODE_GROUP);
    t.cutscene = false;
    for (int k = 0; k < kGraceTurns + 200; k++) turnAll();
    CHECK(f.s[2].mode == MODE_GROUP);
    in[2].pos = add(spot, v3(0, 0, kHoldMove + 5.0f));
    turnAll();
    CHECK(f.s[2].mode == MODE_NONE && f.s[2].event == EV_OFF);
}

static void testBlend() {
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    // spawn: two players on the start point for 3 s, then the remote walks away (4 units a turn): the drawn position moves at
    // most ~2x walking speed from frame to frame (no pop), reaches 95 % of the place in kSettle, never overshoots it, and ends
    // exactly on the true position with nothing left on.
    Formation f;
    f.reset();
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.spawn = true;
    V3 pos = v3(0, 0, 0);
    V3 lastDrawn = pos;
    float maxStep = 0.0f, maxOver = 0.0f;
    int reached = -1;
    for (int k = 0; k < kSpawnTurns + 400; k++) {
        t.spawn = k < kSpawnTurns;
        if (k >= kSpawnTurns) pos = add(pos, v3(0, 0, 4.0f));   // walks toward the camera
        f.begin(t, true);
        const SlotOut o = step(f, 1, t, slotAt(pos), 0, 1);
        const V3 drawn = add(pos, o.offset);
        CHECK(!o.appear);
        if (k > 0) maxStep = std::fmax(maxStep, len(sub(drawn, lastDrawn)));
        if (k < kSpawnTurns) {
            maxOver = std::fmax(maxOver, o.offset.x - kSpacing);
            if (reached < 0 && o.offset.x >= 0.95f * kSpacing) reached = k + 1;
        }
        lastDrawn = drawn;
        f.end();
    }
    CHECK(maxStep < 19.0f);   // (kSpacing 190: the spring's peak speed is ~1.2 kSpacing per second per turn + the walk)
    CHECK(reached > 0 && reached <= (int)(kSettle * 60.0f + 1.0f));
    CHECK(maxOver <= 0.01f);
    CHECK(f.s[1].mode == MODE_NONE && nearV(f.s[1].ofs.x, v3(0, 0, 0), 1e-6f) && !f.latch.on);
    // the first turn moves only a little (eases in)
    f.reset();
    t.spawn = true;
    f.begin(t, true);
    SlotOut o = step(f, 1, t, slotAt(v3(0, 0, 0)), 0, 1);
    CHECK(o.offset.x > 0.0f && o.offset.x < 0.1f * kSpacing);

    // the arrival flight: both fly the same path fast; the puppet keeps its place beside the player without lag
    f.reset();
    t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    for (int k = 0; k < 120; k++) {
        const V3 p = v3(0, 30.0f * k, -60.0f * k);
        t.anchor = p;
        f.begin(t, true);
        o = step(f, 1, t, slotAt(p), 0, 1);
    }
    CHECK(nearV(o.offset, v3(kSpacing, 0, 0), 0.01f));

    // the same flight seen through the network: the puppet is drawn ~6 turns behind (300 units back along the path): still
    // "together" (matched against the local player's recent positions), pushed to the side of that earlier position
    f.reset();
    t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    for (int k = 0; k < 120; k++) {
        t.anchor = v3(0, 0, -50.0f * k);
        const V3 lagged = v3(0, 0, -50.0f * (k >= 6 ? k - 6 : 0));
        f.begin(t, true);
        o = step(f, 1, t, slotAt(lagged), 0, 1);
        CHECK(f.s[1].mode == MODE_SPACE || k == 0);
    }
    CHECK(nearV(o.offset, v3(kSpacing, 0, 0), 0.01f));
    // ... but a puppet 300 units away from a player standing still is not together
    f.reset();
    t.anchor = v3(0, 0, 0);
    for (int k = 0; k < 30; k++) {
        f.begin(t, true);
        o = step(f, 1, t, slotAt(v3(0, 0, 300)), 0, 1);
    }
    CHECK(f.s[1].mode == MODE_NONE && nearV(o.offset, v3(0, 0, 0)));
    // a warp of the local player forgets the old path
    t.anchor = v3(0, 0, 5000);
    f.begin(t, true);
    CHECK(f.histN == 1 && near(f.matched(t, v3(0, 0, 0), nullptr), 5000.0f));

    // the photo: a puppet 400 units away glides (no appear), one 2000 away appears at its place at once; at the end of the demo
    // the far one appears back home, the near one glides back
    f.reset();
    t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = t.star = true;
    f.begin(t, true);
    SlotOut a = step(f, 1, t, slotAt(v3(400, 0, 0)), 0, 2);
    SlotOut b = step(f, 2, t, slotAt(v3(-2000, 0, 0)), 1, 2);
    CHECK(!a.appear && b.appear);
    CHECK(nearV(add(v3(-2000, 0, 0), b.offset), arcOffset(f.frame, 1, 2, 0), 0.01f));
    float face = 0.0f;
    for (int k = 0; k < 60; k++) {
        f.begin(t, true);
        a = step(f, 1, t, slotAt(v3(400, 0, 0)), 0, 2);
        b = step(f, 2, t, slotAt(v3(-2000, 0, 0)), 1, 2);
        CHECK(!a.appear && !b.appear);
        CHECK(a.face >= face - 1e-6f);   // the turn to the camera only grows
        face = a.face;
    }
    CHECK(nearV(add(v3(400, 0, 0), a.offset), arcOffset(f.frame, 0, 2, 0), 0.5f) && face > 0.99f);
    // the remote teleports while in the photo (its own respawn): drawn place unchanged, no appear
    f.begin(t, true);
    a = step(f, 1, t, slotAt(v3(1400, 0, 0)), 0, 2);
    CHECK(!a.appear && nearV(add(v3(1400, 0, 0), a.offset), arcOffset(f.frame, 0, 2, 0), 1.0f));
    t.star = t.cutscene = false;
    f.begin(t, true);
    a = step(f, 1, t, slotAt(v3(400, 0, 0)), 0, 2);
    b = step(f, 2, t, slotAt(v3(-2000, 0, 0)), 1, 2);
    CHECK(!a.appear && b.appear && nearV(b.offset, v3(0, 0, 0)));
}

static void testRotation() {
    const float id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float out[3][3];
    // nothing asked: unchanged
    drawnRotation(id, v3(0, 1, 0), v3(0, 1, 0), v3(0, 0, 1), 0.0f, out);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) CHECK(near(out[i][j], id[i][j]));
    }
    // face the camera: the front (column 2) ends on the direction to the camera, the up stays
    drawnRotation(id, v3(0, 1, 0), v3(0, 1, 0), v3(1, 0, 0), 1.0f, out);
    CHECK(nearV(column(out, 2), v3(1, 0, 0)) && nearV(column(out, 1), v3(0, 1, 0)));
    // half way: 45 of the 90 degrees
    drawnRotation(id, v3(0, 1, 0), v3(0, 1, 0), v3(1, 0, 0), 0.5f, out);
    CHECK(nearV(column(out, 2), v3(0.70710678f, 0, 0.70710678f)));
    // behind it (180): turns, stays a rotation
    drawnRotation(id, v3(0, 1, 0), v3(0, 1, 0), v3(0, 0, -1), 1.0f, out);
    CHECK(nearV(column(out, 2), v3(0, 0, -1), 1e-2f) && near(len(column(out, 0)), 1.0f));
    // tilt: the gravity at the drawn place on a small planet: the up follows
    const V3 upD = v3(std::sin(0.5f), std::cos(0.5f), 0.0f);
    drawnRotation(id, v3(0, 1, 0), upD, v3(0, 0, 1), 0.0f, out);
    CHECK(nearV(column(out, 1), upD) && near(dot(column(out, 0), column(out, 1)), 0.0f));
    // tilt + face: up = drawn up, front toward the camera flattened on it
    drawnRotation(id, v3(0, 1, 0), upD, v3(0, 0, 1), 1.0f, out);
    CHECK(nearV(column(out, 1), upD) && column(out, 2).z > 0.99f);
    // more than 120 degrees between the ups: another surface: no tilt
    drawnRotation(id, v3(0, 1, 0), v3(0, -1, 0.01f), v3(0, 0, 1), 0.0f, out);
    CHECK(nearV(column(out, 1), v3(0, 1, 0)));
}

static void testHostile() {
    // NaN / huge numbers anywhere: nothing NaN comes out, the slot goes off
    const Cam c = lookAt(v3(0, 300, 800), v3(0, 0, 0), v3(0, 1, 0));
    Formation f;
    f.reset();
    TurnIn t = turn(v3(0, 0, 0), v3(0, -1, 0), c);
    t.cutscene = true;
    std::mt19937 rng(7);
    std::uniform_real_distribution< float > u(-1e7f, 1e7f);
    const float bad[] = {std::nanf(""), INFINITY, -INFINITY, 1e30f, -1e30f, 0.0f};
    for (int k = 0; k < 20000; k++) {
        V3 p = v3(u(rng) * (k % 3 == 0 ? 1e-5f : 1.0f), u(rng) * 1e-5f, u(rng) * 1e-5f);
        if (k % 7 == 0) p.x = bad[k % 6];
        if (k % 11 == 0) t.anchor = v3(bad[(k / 11) % 6], 0, 0);
        else t.anchor = v3(0, 0, 0);
        f.begin(t, finite3(t.anchor));
        SlotIn in = slotAt(p, (k & 1) != 0);
        in.inDemo = (k & 2) != 0;
        f.decide(1, t, in);
        f.s[1].ground = (uint8_t)(k & 0xF);
        V3 tg = f.target(1, t, in, k % 4, k % 5);
        const SlotOut o = f.finish(1, in, tg, (float)(k % 3) * 0.5f);
        CHECK(finite3(o.offset) && std::isfinite(o.face) && o.face >= 0.0f && o.face <= 1.0f);
        f.end();
    }
}

int main() {
    testFrames();
    testLatch();
    testPlaces();
    testEngageRelease();
    testTargets();
    testSlotsForPlayers();
    testSideKept();
    testGroundRejection();
    testGroundProbeRate();
    testPhotoFarInDemo();
    testGroups();
    testBlend();
    testRotation();
    testHostile();
    printf("formation_test: %d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
