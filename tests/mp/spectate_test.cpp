// Host tests for decomp/include/Game/MP/MpSpectateLogic.hpp (spectate: who a waiting player watches, and the follow camera's maths).
//   tests/mp/spectate_build.sh        (g++/clang++ -std=c++20 -fsanitize=address,undefined)
#include "Game/MP/MpSpectateLogic.hpp"

#include <cstdio>
#include <vector>

using namespace MpSpectate;

static int sFail = 0, sChecks = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        sChecks++;                                                       \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
            sFail++;                                                     \
        }                                                                \
    } while (0)

namespace {

SelIn mk(int local, uint8_t reason, std::initializer_list<int> watchable, bool startOk = true, bool allowOwn = true) {
    SelIn in;
    in.local = local;
    in.reason = reason;
    in.startOk = startOk;
    in.allowOwn = allowOwn;
    for (int s : watchable) in.canWatch[s] = true;
    return in;
}

SelOut run(Selector& sel, const SelIn& in, int turns) {
    SelOut o;
    for (int i = 0; i < turns; i++) o = sel.step(in);
    return o;
}

// ---- selection ------------------------------------------------------------------------------------------------------------
void testStartRules() {
    Selector sel;
    SelIn in = mk(1, REASON_BUBBLE, {0, 2});
    SelOut o = run(sel, in, kStartDelayTurns - 1);
    CHECK(o.target == -1 && o.ev == EV_NONE);                 // not yet: a bubble that pops at once never starts
    o = sel.step(in);
    CHECK(o.target == 2 && o.ev == EV_START);                 // the ring from our own slot (1): the next one up
}

void testStartOrderAndPrefer() {
    Selector sel;
    SelIn in = mk(1, REASON_BUBBLE, {0, 2, 3});
    SelOut o = run(sel, in, kStartDelayTurns);
    CHECK(o.ev == EV_START);
    CHECK(o.target == 2);   // the ring from our own slot: the next slot up
    Selector sel2;
    in.prefer = 3;
    o = run(sel2, in, kStartDelayTurns);
    CHECK(o.target == 3);   // the teammate the bubble floats to
    Selector sel3;
    in.prefer = 1;          // ourselves: ignored
    o = run(sel3, in, kStartDelayTurns);
    CHECK(o.target == 2);
    Selector sel4;
    in.prefer = 0;
    in.canWatch[0] = false; // cannot be watched: ignored
    o = run(sel4, in, kStartDelayTurns);
    CHECK(o.target == 2);
}

void testNoAutoStartWhenNotWorthIt() {
    Selector sel;
    SelIn in = mk(0, REASON_BUBBLE, {1}, /*startOk*/ false);
    SelOut o = run(sel, in, 600);
    CHECK(o.target == -1 && o.ev == EV_NONE);                 // a teammate is near: the player pops the bubble himself
    in.step = +1;                                             // ...but R starts the view at once
    o = sel.step(in);
    CHECK(o.target == 1 && o.ev == EV_START);
}

void testNobodyToWatch() {
    Selector sel;
    SelIn in = mk(0, REASON_OUT, {});                         // everyone waiting at once: nobody can be watched
    SelOut o = run(sel, in, 1000);
    CHECK(o.target == -1 && o.ev == EV_NONE);
    in.step = +1; o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_NONE);
    in.step = -1; o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_NONE);
    in.step = 0; in.pick = 1; o = sel.step(in);
    CHECK(o.target == -1);
    // one comes back: the view starts after the delay
    in.pick = -1; in.canWatch[2] = true;
    o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 2 && o.ev == EV_START);
}

void testRingOrder() {
    Selector sel;
    SelIn in = mk(2, REASON_OUT, {0, 1, 3}, true, /*allowOwn*/ true);
    SelOut o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 3);
    in.step = +1; o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_OWN);                  // after the last: our own view
    CHECK(sel.optedOut());
    in.step = 0; o = run(sel, in, 400);
    CHECK(o.target == -1);                                    // chosen: no auto start while this wait lasts
    in.step = +1; o = sel.step(in);
    CHECK(o.target == 0 && o.ev == EV_START);                 // R from our own view: the first
    in.step = +1; o = sel.step(in);
    CHECK(o.target == 1 && o.ev == EV_SWITCH);
    in.step = +1; o = sel.step(in);
    CHECK(o.target == 3);
    in.step = -1; o = sel.step(in);
    CHECK(o.target == 1);
    in.step = -1; o = sel.step(in);
    CHECK(o.target == 0);
    in.step = -1; o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_OWN);                  // L before the first: our own view
    in.step = -1; o = sel.step(in);
    CHECK(o.target == 3 && o.ev == EV_START);                 // L from our own view: the last
}

void testRingWithoutOwnStop() {
    Selector sel;
    SelIn in = mk(0, REASON_RACE, {1, 2}, true, /*allowOwn*/ false);
    SelOut o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 1);
    in.step = +1; o = sel.step(in);
    CHECK(o.target == 2);
    in.step = +1; o = sel.step(in);
    CHECK(o.target == 1 && o.ev == EV_SWITCH);                // a race has no own-view stop: it wraps
    in.step = -1; o = sel.step(in);
    CHECK(o.target == 2);
    // a single teammate: pressing changes nothing
    SelIn one = mk(0, REASON_RACE, {3}, true, false);
    Selector s1;
    o = run(s1, one, kStartDelayTurns);
    CHECK(o.target == 3);
    one.step = +1; o = s1.step(one);
    CHECK(o.target == 3 && o.ev == EV_NONE);
}

void testTargetLeaves() {
    Selector sel;
    SelIn in = mk(0, REASON_RACE, {1, 2, 3}, true, false);
    SelOut o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 1);
    in.canWatch[1] = false;                                   // finished / left / fell
    o = sel.step(in);
    CHECK(o.target == 2 && o.ev == EV_SWITCH && o.forced);
    in.canWatch[2] = false;
    o = sel.step(in);
    CHECK(o.target == 3 && o.ev == EV_SWITCH && o.forced);
    in.canWatch[3] = false;                                   // the last one: nobody left
    o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_LOST && o.forced);
    o = run(sel, in, 500);
    CHECK(o.target == -1);
    in.canWatch[2] = true;                                    // someone watchable again: restarts after the delay, not at once
    o = run(sel, in, kStartDelayTurns - 1);
    CHECK(o.target == -1);
    o = sel.step(in);
    CHECK(o.target == 2 && o.ev == EV_START);
}

void testTargetLeavesWraps() {
    Selector sel;
    SelIn in = mk(0, REASON_OUT, {1, 3}, true, true);
    in.pick = 3; SelOut o = sel.step(in);
    CHECK(o.target == 3 && o.ev == EV_START);                 // a touch on the strip's head starts at once
    in.pick = -1;
    in.canWatch[3] = false;                                   // the highest one leaves: the next one up wraps to the first
    o = sel.step(in);
    CHECK(o.target == 1 && o.ev == EV_SWITCH && o.forced);
}

void testReturn() {
    Selector sel;
    SelIn in = mk(1, REASON_BUBBLE, {0}, true, true);
    SelOut o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 0);
    in.reason = REASON_NONE;                                  // the bubble popped / the next race / a scene change
    o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_END);
    o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_NONE);                 // once
    // chosen own view, then back in play, then bubbled again: auto start is allowed again
    in.reason = REASON_BUBBLE;
    o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 0);
    in.step = +1; o = sel.step(in);
    CHECK(o.ev == EV_OWN && sel.optedOut());
    in.step = 0; in.reason = REASON_NONE; sel.step(in);
    CHECK(!sel.optedOut());
    in.reason = REASON_BUBBLE;
    o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 0 && o.ev == EV_START);
}

void testReturnFromOwnAndNeverWatchSelf() {
    Selector sel;
    SelIn in = mk(2, REASON_OUT, {0, 2}, true, true);         // (a bad input: our own slot marked watchable)
    SelOut o = run(sel, in, kStartDelayTurns);
    CHECK(o.target == 0);
    in.step = +1; o = sel.step(in);
    CHECK(o.target == -1 && o.ev == EV_OWN);                  // slot 2 is us: never a stop
    in.step = +1; o = sel.step(in);
    CHECK(o.target == 0);
}

void testRobustness() {
    // every combination of masks and presses keeps the target legal
    Selector sel;
    unsigned seed = 7;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        SelIn in;
        in.local = (seed >> 3) & 3;
        in.reason = (uint8_t)((seed >> 5) % 4);
        in.startOk = (seed >> 7) & 1;
        in.allowOwn = (seed >> 8) & 1;
        for (int s = 0; s < kSlots; s++) in.canWatch[s] = (seed >> (10 + s)) & 1;
        in.step = (int)((seed >> 16) % 3) - 1;
        in.pick = (int)((seed >> 20) % 6) - 1;
        const SelOut o = sel.step(in);
        CHECK(o.target >= -1 && o.target < kSlots);
        CHECK(o.target != in.local);
        if (in.reason == REASON_NONE) CHECK(o.target == -1);
        if (o.target >= 0) CHECK(in.canWatch[o.target]);
    }
}

// ---- the follow camera ----------------------------------------------------------------------------------------------------
FollowIn at(V3 feet, V3 up, V3 front, V3 vel = {0, 0, 0}) { FollowIn f; f.feet = feet; f.up = up; f.front = front; f.vel = vel; return f; }

float angleBetween(V3 a, V3 b) {
    float c = dot(normOr(a, v3(0, 1, 0)), normOr(b, v3(0, 1, 0)));
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return acosf(c);
}

void testFollowReset() {
    FollowParams p;
    FollowState s;
    FollowIn in = at(v3(100, 50, -300), v3(0, 1, 0), v3(0, 0, 1));
    FollowOut o = followStep(&s, in, p);
    // behind (opposite the facing) and above, at the set distance, looking at the character's chest
    CHECK(fabsf(len(sub(o.pos, o.watch)) - p.dist) < 1.0f);
    CHECK(o.pos.z < in.feet.z);                               // behind a character facing +z
    CHECK(o.pos.y > o.watch.y);
    CHECK(fabsf(o.watch.y - (50 + p.height)) < 1e-3f && fabsf(o.watch.x - 100) < 1e-3f);
    CHECK(angleBetween(o.up, in.up) < 1e-4f);
    // standing still: nothing moves, however long
    FollowOut o2 = o;
    for (int i = 0; i < 600; i++) o2 = followStep(&s, in, p);
    CHECK(len(sub(o2.pos, o.pos)) < 1e-2f);
}

void testFollowStepNoOvershoot() {
    FollowParams p;
    FollowState s;
    FollowIn in = at(v3(0, 0, 0), v3(0, 1, 0), v3(0, 0, 1));
    followStep(&s, in, p);
    in.feet = v3(400, 0, 300);                                // a teleport (a warp, a new target)
    const V3 want = idealFocus(in, p, v3(0, 1, 0));
    float prev = 1e9f;
    for (int i = 0; i < 200; i++) {
        FollowOut o = followStep(&s, in, p);
        const float d = len(sub(want, o.watch));
        CHECK(d <= prev + 1e-3f);                             // closer every turn: never past it
        // along the jump direction the focus never goes beyond the goal
        CHECK(dot(sub(o.watch, want), v3(400, 0, 300)) <= 1e-2f);
        prev = d;
    }
    CHECK(prev < 1.0f);
}

void testFollowRunningAndLaunch() {
    FollowParams p;
    FollowState s;
    V3 feet = v3(0, 0, 0);
    FollowIn in = at(feet, v3(0, 1, 0), v3(0, 0, 1), v3(0, 0, 12));
    followStep(&s, in, p);
    float worst = 0;
    for (int i = 0; i < 400; i++) {                           // a run, then a launch star's fast flight, then a stop
        const float speed = i < 200 ? 12.0f : (i < 300 ? 110.0f : 0.0f);
        in.vel = v3(0, 0, speed);
        in.feet = add(in.feet, in.vel);
        FollowOut o = followStep(&s, in, p);
        const float lag = len(sub(idealFocus(in, p, v3(0, 1, 0)), o.watch));
        if (i > 20 && i < 300 && lag > worst) worst = lag;
        CHECK(lag < p.maxLag + 1.0f);
        CHECK(fabsf(len(sub(o.pos, o.watch)) - p.dist) < 1.0f);   // the orbit radius never changes
    }
    CHECK(worst < 140.0f);                                    // the character stays near the centre even at launch speed
}

void testFollowHeadingFollowsOnlyWhileMoving() {
    FollowParams p;
    FollowState s;
    FollowIn in = at(v3(0, 0, 0), v3(0, 1, 0), v3(0, 0, 1));
    followStep(&s, in, p);
    const V3 h0 = s.head;
    in.front = v3(1, 0, 0);                                   // turns to face +x but stands still: the view stays
    for (int i = 0; i < 200; i++) followStep(&s, in, p);
    CHECK(len(sub(s.head, h0)) < 1e-4f);
    in.vel = v3(12, 0, 0);                                    // runs +x: the view swings round behind it
    for (int i = 0; i < 400; i++) { in.feet = add(in.feet, in.vel); followStep(&s, in, p); }
    CHECK(dot(s.head, v3(-1, 0, 0)) > 0.99f);
}

void testFollowGravityFlip() {
    FollowParams p;
    FollowState s;
    FollowIn in = at(v3(0, 0, 0), v3(0, 1, 0), v3(0, 0, 1));
    followStep(&s, in, p);
    in.up = v3(0, -1, 0);                                     // an exact flip (a gravity switch)
    in.front = v3(0, 0, 1);
    V3 lastPos = followPose(s, p).pos;
    float minDist = 1e9f, maxStep = 0, prevAng = 0;
    for (int i = 0; i < 120; i++) {
        FollowOut o = followStep(&s, in, p);
        const float d = len(sub(o.pos, in.feet));
        if (d < minDist) minDist = d;
        const float st = len(sub(o.pos, lastPos));
        if (st > maxStep) maxStep = st;
        lastPos = o.pos;
        CHECK(fabsf(len(o.up) - 1.0f) < 1e-3f);
        CHECK(fabsf(dot(o.up, normOr(sub(o.pos, o.watch), v3(1, 0, 0))) - sinf(p.pitch)) < 5e-2f);   // always above, in its own frame
        const float ang = angleBetween(o.up, in.up);
        CHECK(ang <= (i == 0 ? 3.15f : prevAng + 1e-3f));     // the remaining turn only shrinks: no overshoot, no wobble
        prevAng = ang;
        CHECK(fabsf(len(sub(o.pos, o.watch)) - p.dist) < 1.0f);
    }
    CHECK(minDist > p.dist * 0.5f);                           // the view never swings through the character
    CHECK(maxStep < p.dist * 0.06f);                          // and never jumps: a smooth orbit (about 3 degrees a turn)
    CHECK(angleBetween(s.up, in.up) < 1e-3f);                 // done within two seconds (120 turns)
    const FollowOut f = followPose(s, p);
    CHECK(dot(sub(f.pos, f.watch), in.up) > 0.0f);            // above the character in the NEW frame
}

void testFollowPlanetLoop() {
    // the character runs once round a small planet (gravity up turns steadily): the view stays upright and level
    FollowParams p;
    FollowState s;
    const float R = 600.0f;
    float worstUp = 0, worstPitch = 0;
    for (int i = 0; i < 1200; i++) {
        const float a = i * 0.0125f;                          // one lap in ~500 turns
        FollowIn in;
        in.up = v3(0, cosf(a), sinf(a));
        in.feet = mul(in.up, R);
        in.front = v3(0, -sinf(a), cosf(a));
        in.vel = mul(in.front, R * 0.0125f);
        FollowOut o = followStep(&s, in, p);
        if (i > 30) {
            const float ang = angleBetween(o.up, in.up);
            if (ang > worstUp) worstUp = ang;
            const V3 off = sub(o.pos, o.watch);
            const float pitch = asinf(dot(normOr(off, o.up), o.up));
            if (fabsf(pitch - p.pitch) > worstPitch) worstPitch = fabsf(pitch - p.pitch);
            CHECK(dot(o.up, in.up) > 0.95f);
            // the character is near the picture's centre: the viewing direction points at it
            CHECK(len(sub(o.watch, add(in.feet, mul(in.up, p.height)))) < 140.0f);
        }
    }
    CHECK(worstUp < 0.3f);
    CHECK(worstPitch < 0.02f);
}

void testFollowBadInput() {
    FollowParams p;
    FollowState s;
    FollowIn in = at(v3(10, 20, 30), v3(0, 1, 0), v3(0, 0, 1));
    FollowOut a = followStep(&s, in, p);
    FollowIn bad = in;
    bad.feet.x = NAN;
    FollowOut b = followStep(&s, bad, p);
    CHECK(len(sub(a.pos, b.pos)) < 1e-4f);
    bad = in; bad.up = v3(0, 0, 0);                           // a zero up: keeps the old frame
    b = followStep(&s, bad, p);
    CHECK(finite3(b.pos) && angleBetween(b.up, a.up) < 1e-3f);
    bad = in; bad.front = v3(0, 1, 0);                        // facing straight up the frame: no heading change, no NaN
    bad.vel = v3(0, 0, 20);
    for (int i = 0; i < 50; i++) b = followStep(&s, bad, p);
    CHECK(finite3(b.pos) && finite3(b.up));
    bad = in; bad.feet = v3(1e12f, 0, 0);
    b = followStep(&s, bad, p);
    CHECK(finite3(b.pos));
}

}  // namespace

// review2 #7: no start while a game event camera / demo runs: a start that is due is deferred (blocked), then happens when it ends
void testEventBlocked() {
    Selector sel;
    SelIn in = mk(1, REASON_BUBBLE, {0, 2});
    in.eventBlocked = true;
    SelOut o = run(sel, in, kStartDelayTurns + 40);
    CHECK(o.target == -1 && o.ev == EV_NONE && o.blocked);    // the automatic start is due but deferred, for as long as the event lasts
    in.eventBlocked = false;
    o = sel.step(in);
    CHECK(o.ev == EV_START && o.target == 2 && !o.blocked);   // the wait is kept: it starts at once when the event is over
    // a press or a touch during the event: nothing starts (and a press is not remembered)
    Selector s2;
    in = mk(1, REASON_BUBBLE, {0, 2});
    in.eventBlocked = true;
    in.step = 1;
    o = s2.step(in);
    CHECK(o.ev == EV_NONE && o.target == -1 && o.blocked);
    in.step = 0;
    in.pick = 0;
    o = s2.step(in);
    CHECK(o.ev == EV_NONE && o.target == -1 && o.blocked);
    // not yet due: not blocked (nothing was deferred); the wait keeps counting during the event
    Selector s3;
    in = mk(1, REASON_BUBBLE, {0, 2});
    in.eventBlocked = true;
    o = run(s3, in, 10);
    CHECK(o.ev == EV_NONE && !o.blocked);
    in.eventBlocked = false;
    o = run(s3, in, kStartDelayTurns - 10 - 1);
    CHECK(o.ev == EV_NONE);
    o = s3.step(in);
    CHECK(o.ev == EV_START);
    // already watching: an event camera does not stop the selector (the camera side restarts ours), switching still works
    Selector s4;
    in = mk(1, REASON_BUBBLE, {0, 2, 3});
    o = run(s4, in, kStartDelayTurns);
    CHECK(o.ev == EV_START);
    in.eventBlocked = true;
    in.step = 1;
    o = s4.step(in);
    CHECK(o.ev == EV_SWITCH && o.target == 3);
    // back in play during an event: the end is never blocked
    in = mk(1, REASON_NONE, {0, 2, 3});
    in.eventBlocked = true;
    o = s4.step(in);
    CHECK(o.ev == EV_END);
}

int main() {
    testStartRules();
    testEventBlocked();
    testStartOrderAndPrefer();
    testNoAutoStartWhenNotWorthIt();
    testNobodyToWatch();
    testRingOrder();
    testRingWithoutOwnStop();
    testTargetLeaves();
    testTargetLeavesWraps();
    testReturn();
    testReturnFromOwnAndNeverWatchSelf();
    testRobustness();
    testFollowReset();
    testFollowStepNoOvershoot();
    testFollowRunningAndLaunch();
    testFollowHeadingFollowsOnlyWhileMoving();
    testFollowGravityFlip();
    testFollowPlanetLoop();
    testFollowBadInput();
    std::printf("spectate_test: %d checks, %d failed\n", sChecks, sFail);
    return sFail ? 1 : 0;
}
