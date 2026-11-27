// Host tests for Play Together "Bubbles" falls (owner: bubble): the BUBBLE codec (round trip, every malformed form refused,
// fuzzed), the settings byte (downed) codec, the bubble table and the host's "everyone caught" decision (debounce, scene serial,
// pose and message must agree, players still loading ignored), the float (reaches the hover point near the teammate along his
// gravity, a few hundred units away, never faster than the cap, stays finite on hostile input), the pops (touch, A near a grounded
// teammate, out of lives) and the lives rules; bubble2: the arrival flag (codec, never "caught"), the spin reach, the Star Bit
// segment test, the pop request codec (a BUMP of kind kBumpPop) and the bubbled console's acceptance rules, the arrival timer.
//
// Build and run (from the worktree root; gcc has the sanitizer runtime here):
//   tests/mp/bubble_build.sh
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "Game/MP/MpBubbleLogic.hpp"

using namespace MpBubbleLogic;

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool roundTrip(const MpBubbleMsg& m, MpBubbleMsg* out) {
    uint8_t b[16];
    MpWriter w(b, sizeof b);
    mp_write_bubble(w, m);
    if (!w.ok() || w.n != 6) return false;
    MpReader r(b, w.n);
    return mp_read_bubble(r, out);
}

static void testCodec() {
    MpBubbleMsg m{0x1234, 1, 3, MP_BUBBLE_BY_NONE, MP_BUBBLE_OUT_OF_LIVES}, o;
    CHECK(roundTrip(m, &o) && o.serial == 0x1234 && o.on == 1 && o.lives == 3 && o.by == MP_BUBBLE_BY_NONE && o.flags == 1);
    m = MpBubbleMsg{7, 0, 0, 2, 0};
    CHECK(roundTrip(m, &o) && o.on == 0 && o.by == 2);
    // malformed: bad on, lives, by, flags, flags on an "off", short, long
    CHECK(!roundTrip(MpBubbleMsg{1, 2, 0, 0xFF, 0}, &o));
    CHECK(!roundTrip(MpBubbleMsg{1, 1, 100, 0xFF, 0}, &o));
    CHECK(!roundTrip(MpBubbleMsg{1, 0, 1, 4, 0}, &o));
    CHECK(roundTrip(MpBubbleMsg{1, 1, 1, 0xFF, MP_BUBBLE_ARRIVAL}, &o) && o.flags == MP_BUBBLE_ARRIVAL);   // an arrival bubble
    CHECK(!roundTrip(MpBubbleMsg{1, 1, 1, 0xFF, MP_BUBBLE_ARRIVAL | MP_BUBBLE_OUT_OF_LIVES}, &o));      // never both
    CHECK(!roundTrip(MpBubbleMsg{1, 0, 1, 0xFF, MP_BUBBLE_ARRIVAL}, &o));                               // no flags on an "off"
    CHECK(!roundTrip(MpBubbleMsg{1, 1, 1, 0xFF, 4}, &o));
    CHECK(!roundTrip(MpBubbleMsg{1, 0, 1, 0xFF, MP_BUBBLE_OUT_OF_LIVES}, &o));
    uint8_t b[8] = {1, 0, 1, 3, 0xFF, 0, 0, 0};
    for (size_t n = 0; n <= sizeof b; n++) {
        MpReader r(b, n);
        CHECK(mp_read_bubble(r, &o) == (n == 6));
    }
    // fuzz: never accepts something out of range, never reads past the end
    std::mt19937 rng(7);
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        uint8_t d[10];
        const size_t n = rng() % sizeof d;
        for (size_t k = 0; k < n; k++) d[k] = (uint8_t)rng();
        if (n >= 3 && rng() % 2) d[2] = (uint8_t)(rng() % 2);
        if (n >= 5 && rng() % 2) d[4] = (uint8_t)(rng() % 2 ? 0xFF : rng() % 4);
        if (n >= 6 && rng() % 2) d[5] = (uint8_t)(rng() % 4);
        MpReader r(d, n);
        if (mp_read_bubble(r, &o)) {
            ++accepted;
            CHECK(n == 6 && o.on <= 1 && o.lives <= 99 && (o.by < MP_MAX_PLAYERS || o.by == MP_BUBBLE_BY_NONE) && o.flags <= 2 &&
                  (o.on || o.flags == 0));
        }
    }
    CHECK(accepted > 0);
    fprintf(stderr, "  bubble codec fuzz: %d of 200000 accepted\n", accepted);
}

static void testSettings() {
    MpSettings s;
    memset(&s, 0, sizeof s);
    s.mode = MP_MODE_TOGETHER; s.maxPlayers = 4; s.interaction = 1; s.sharedKills = 1; s.rivals = 1; s.raceWaitSec = 18;
    s.nameTags = 1; s.flags = 7;
    for (uint8_t d = 0; d < 4; d++) {
        s.downed = d;
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        mp_write_settings(w, s);
        CHECK(w.ok() && w.n == 16);   // still 16 bytes on the wire (a reserved byte became "downed")
        MpReader r(b, w.n);
        MpSettings o;
        const bool ok = mp_read_settings(r, &o);
        CHECK(ok == (d < MP_DOWNED_COUNT));
        if (ok) CHECK(o.downed == d && r.done());
    }
    // the default (zeroed reserved byte) is Bubbles
    MpSettings z;
    memset(&z, 0, sizeof z);
    CHECK(z.downed == MP_DOWNED_BUBBLES);
    CHECK(mp_msg_host_relays(MP_MSG_BUBBLE) && !mp_msg_host_only(MP_MSG_BUBBLE));
    CHECK(mp_msg_host_only(MP_MSG_RESTART) && !mp_msg_host_relays(MP_MSG_RESTART));
    CHECK(MP_POSE_BUBBLE == (1 << 12) && (MP_POSE_BUBBLE & (MP_POSE_FINISHED | MP_POSE_PAUSED)) == 0);
}

static void testTable() {
    Table t;
    t.reset(5);
    CHECK(t.note(1, MpBubbleMsg{5, 1, 2, MP_BUBBLE_BY_NONE, 0}) && t.e[1].on && t.e[1].lives == 2);
    CHECK(!t.note(1, MpBubbleMsg{4, 0, 2, MP_BUBBLE_BY_NONE, 0}) && t.e[1].on);   // an older scene's message: dropped
    CHECK(!t.note(7, MpBubbleMsg{5, 1, 2, MP_BUBBLE_BY_NONE, 0}));
    CHECK(t.note(2, MpBubbleMsg{5, 1, 0, MP_BUBBLE_BY_NONE, MP_BUBBLE_OUT_OF_LIVES}) && t.e[2].out);
    CHECK(t.note(1, MpBubbleMsg{5, 0, 2, 0, 0}) && !t.e[1].on);

    // everyone caught: present + in this scene + message and pose agree
    t.reset(9);
    SlotView v[4] = {{true, true, true}, {true, true, true}, {false, false, false}, {false, false, false}};
    CHECK(!everyoneCaught(t, v));   // no message yet
    t.note(0, MpBubbleMsg{9, 1, 3, MP_BUBBLE_BY_NONE, 0});
    t.note(1, MpBubbleMsg{9, 1, 3, MP_BUBBLE_BY_NONE, 0});
    CHECK(everyoneCaught(t, v));
    v[1].poseBubble = false;   // the pose says he is out already (a pop in flight): not everyone
    CHECK(!everyoneCaught(t, v));
    v[1].poseBubble = true;
    v[2] = SlotView{true, false, false};   // a present slot that is neither in the scene nor loading (stalled): ignored
    CHECK(everyoneCaught(t, v));
    v[2] = SlotView{true, false, false, true};   // G8: a third player still LOADING the scene counts as not caught
    CHECK(!everyoneCaught(t, v));
    {   // ...but only for kLoadingWaitTurns, then the host stops waiting for him
        LoadingWait lw;
        CHECK(lw.step(2, true, 100) && lw.step(2, true, 100 + kLoadingWaitTurns - 1));
        CHECK(!lw.step(2, true, 100 + kLoadingWaitTurns));
        CHECK(!lw.step(2, false, 5000) && lw.step(2, true, 5001));   // he arrived and left again: a new wait
    }
    v[2] = SlotView{true, true, false};    // ...now in the scene and free
    CHECK(!everyoneCaught(t, v));
    SlotView none[4] = {};
    CHECK(!everyoneCaught(t, none));       // nobody: never
    // an arrival bubble (drop-in / warp) is never "caught": it pops by itself in a moment
    v[2] = SlotView{true, true, true};
    CHECK(t.note(2, MpBubbleMsg{9, 1, 3, MP_BUBBLE_BY_NONE, MP_BUBBLE_ARRIVAL}) && t.e[2].arrival);
    CHECK(!everyoneCaught(t, v));
    CHECK(t.note(2, MpBubbleMsg{9, 1, 2, MP_BUBBLE_BY_NONE, 0}) && !t.e[2].arrival);   // (then a real fall of the same player)
    CHECK(everyoneCaught(t, v));
    // a late joiner / a new host that has only the pose (no message yet): not caught until the re-announce arrives
    Table late;
    late.reset(9);
    SlotView lv[4] = {{true, true, true}, {true, true, true}, {false, false, false}, {false, false, false}};
    late.note(0, MpBubbleMsg{9, 1, 3, MP_BUBBLE_BY_NONE, 0});
    CHECK(!everyoneCaught(late, lv));
    late.note(1, MpBubbleMsg{9, 1, 1, MP_BUBBLE_BY_NONE, 0});   // slot 1's re-announce
    CHECK(everyoneCaught(late, lv));
    late.leave(1);                                            // he leaves: the rest are caught
    lv[1] = SlotView{false, false, false};
    CHECK(everyoneCaught(late, lv));

    // the debounce: kRestartDebounce turns in a row, once
    RestartGate g;
    int fired = 0, at = -1;
    for (int i = 0; i < 200; i++) if (g.step(true)) { ++fired; at = i; }
    CHECK(fired == 1 && at == kRestartDebounce - 1);
    g.reset();
    for (int i = 0; i < kRestartDebounce - 1; i++) CHECK(!g.step(true));
    CHECK(!g.step(false));   // a gap starts it over
    for (int i = 0; i < kRestartDebounce - 1; i++) CHECK(!g.step(true));
    CHECK(g.step(true));
}

static Teammate mate(V3 feet, V3 up, bool ground = true) { return Teammate{true, feet, up, ground}; }

static void testFloat() {
    // a teammate on a small planet's side: up = +x
    Teammate t[4] = {};
    t[1] = mate(v3(1000, 0, 0), v3(1, 0, 0));
    V3 pos = v3(-3000, 2000, 500), vel = v3(0, 0, 0);
    const int n = nearest(t, pos);
    CHECK(n == 1);
    float maxStep = 0.0f;
    for (uint32_t i = 0; i < 600; i++) {
        const V3 goal = hoverPoint(t[1], pos, v3(0, 0, 1));
        const V3 before = pos;
        pos = stepFloat(pos, &vel, goal, t[1].up, i);
        const float step = len(sub(pos, before));
        if (step > maxStep) maxStep = step;
    }
    CHECK(maxStep <= kMaxSpeed + 0.01f);
    const V3 rel = sub(pos, t[1].feet);
    const float along = dot(rel, t[1].up);
    const float dist = len(rel);
    CHECK(along > kHoverUp - 40.0f && along < kHoverUp + 40.0f);   // above him along HIS up (bob included)
    CHECK(dist > 250.0f && dist < 420.0f);                          // a few hundred units away, not touching
    CHECK(touchPops(pos, kRadius, t) == -1);
    // the A pop: near a grounded teammate, not out of lives, past the grace time
    CHECK(mayPopWithA(true, false, 100, pos, t) == 1);
    CHECK(mayPopWithA(false, false, 100, pos, t) == -1);
    CHECK(mayPopWithA(true, true, 100, pos, t) == -1);    // out of lives: only a teammate's touch
    CHECK(mayPopWithA(true, false, 5, pos, t) == -1);     // still growing
    t[1].onGround = false;
    CHECK(mayPopWithA(true, false, 100, pos, t) == -1);   // he is in the air
    t[1].onGround = true;
    // a touch: the teammate walks into it
    Teammate w[4] = {};
    w[2] = mate(sub(pos, mul(v3(1, 0, 0), kBodyUp)), v3(1, 0, 0));
    CHECK(touchPops(pos, kRadius, w) == 2);
    w[2].feet = add(w[2].feet, v3(0, kRadius + kTouchSlack + 5.0f, 0));
    CHECK(touchPops(pos, kRadius, w) == -1);
    // the landing point is beside the teammate, just above his feet
    const V3 land = landingPoint(t[1], pos, v3(0, 0, 1));
    CHECK(len(sub(land, t[1].feet)) < 150.0f && dot(sub(land, t[1].feet), t[1].up) > 0.0f);
    // the stick only nudges
    const V3 d = driftOffset(1.0f, 1.0f, v3(1, 0, 0), v3(0, 0, 1), v3(0, 1, 0));
    CHECK(len(d) <= kDriftMax + 0.01f && len(d) > kDriftMax * 0.9f && fabsf(d.y) < 0.01f);
    CHECK(len(driftOffset(5.0f, -9.0f, v3(1, 0, 0), v3(0, 0, 1), v3(0, 1, 0))) <= kDriftMax + 0.01f);
    // hostile numbers never make a NaN position
    V3 p2 = v3(0, 0, 0), v2 = v3(0, 0, 0);
    p2 = stepFloat(p2, &v2, v3(NAN, 0, 0), v3(0, 1, 0), 3);
    CHECK(finite3(p2) && finite3(v2));
    p2 = stepFloat(p2, &v2, v3(1e30f, -1e30f, 0), v3(0, 1, 0), 3);
    CHECK(finite3(p2) && len(v2) <= kMaxSpeed + 0.01f);
    // nobody: -1
    Teammate none[4] = {};
    CHECK(nearest(none, pos) == -1 && touchPops(pos, kRadius, none) == -1 && mayPopWithA(true, false, 100, pos, none) == -1);
    // the up of a pose quaternion: identity = +y; 90 degrees about z = -x
    const int16_t qi[4] = {0, 0, 0, 32767};
    V3 u = upOfQuat(qi);
    CHECK(fabsf(u.y - 1.0f) < 1e-3f);
    const int16_t qz[4] = {0, 0, 23170, 23170};
    u = upOfQuat(qz);
    CHECK(fabsf(u.x + 1.0f) < 1e-3f);
}

static void testSpinShot() {
    // the spin: the popper's body centre within kSpinReach of the bubble's surface
    const V3 c = v3(0, 400, 0);
    CHECK(spinReaches(v3(0, 400 - kRadius - kSpinReach + 5, 0), c, kRadius));
    CHECK(!spinReaches(v3(0, 400 - kRadius - kSpinReach - 5, 0), c, kRadius));
    CHECK(!spinReaches(v3(NAN, 0, 0), c, kRadius));
    // a teammate standing at the hover point's teammate (the bubble ~330 away from his feet) reaches it with a spin
    Teammate m = mate(v3(0, 0, 0), v3(0, 1, 0));
    const V3 hover = hoverPoint(m, v3(1000, 0, 0), v3(1, 0, 0));
    CHECK(spinReaches(add(m.feet, mul(m.up, kBodyUp)), hover, kRadius));
    // ...but standing there is no touch (the touch needs him inside it)
    Teammate tt[4] = {};
    tt[0] = m;
    CHECK(touchPops(hover, kRadius, tt) < 0);
    // the Star Bit: its flight this turn passes through the sphere (segment, not only its end points)
    CHECK(segmentHitsSphere(v3(-1000, 400, 0), v3(1000, 400, 0), c, kRadius));
    CHECK(!segmentHitsSphere(v3(-1000, 600, 0), v3(1000, 600, 0), c, kRadius));
    CHECK(!segmentHitsSphere(v3(-1000, 400, 0), v3(-400, 400, 0), c, kRadius));   // stops short
    CHECK(segmentHitsSphere(v3(0, 400, 0), v3(0, 400, 0), c, kRadius));            // a still point inside
    CHECK(!segmentHitsSphere(v3(NAN, 0, 0), v3(0, 0, 0), c, kRadius));
    CHECK(!segmentHitsSphere(v3(0, 400, 0), v3(1, 400, 0), c, -1.0f));
    CHECK(!segmentHitsSphere(v3(0, 400, 0), v3(1, 400, 0), v3(INFINITY, 0, 0), kRadius));
}

static void testPopRequest() {
    uint8_t b[16];
    MpWriter w(b, sizeof b);
    CHECK(writePopRequest(w, 2, kPopShot) && w.n == 5);
    bool ok = false;
    uint8_t target = 9, cause = 9;
    CHECK(readPopRequest(b, w.n, &ok, &target, &cause) && ok && target == 2 && cause == kPopShot);
    // other BUMP kinds are not pop requests (they stay the puppets' stomp / spin / push)
    for (uint8_t k = 1; k <= 3; k++) {
        uint8_t d[5] = {1, k, 0, 0, 0};
        CHECK(!readPopRequest(d, 5, &ok, &target, &cause));
    }
    // malformed pop requests are consumed but refused: bad target, cause, padding, length
    const uint8_t bad[][6] = {{4, kBumpPop, 1, 0, 0}, {1, kBumpPop, 0, 0, 0}, {1, kBumpPop, 3, 0, 0}, {1, kBumpPop, 1, 1, 0}, {1, kBumpPop, 1, 0, 7}};
    for (const auto& d : bad) CHECK(readPopRequest(d, 5, &ok, &target, &cause) && !ok);
    for (size_t n = 2; n <= 6; n++) {
        uint8_t d[6] = {1, kBumpPop, kPopSpin, 0, 0, 0};
        CHECK(readPopRequest(d, n, &ok, &target, &cause) && ok == (n == 5));
    }
    CHECK(!readPopRequest(b, 1, &ok, &target, &cause) && !ok);
    std::mt19937 rng(11);
    int accepted = 0;
    for (int i = 0; i < 100000; i++) {
        uint8_t d[8];
        const size_t n = rng() % sizeof d;
        for (size_t k = 0; k < n; k++) d[k] = (uint8_t)(rng() % 3 ? rng() % 6 : rng());
        if (readPopRequest(d, n, &ok, &target, &cause) && ok) {
            ++accepted;
            CHECK(n == 5 && target < MP_MAX_PLAYERS && (cause == kPopSpin || cause == kPopShot));
        }
    }
    CHECK(accepted > 0);
    // the bubbled console's checks: grace, a playing teammate, a spin from near enough (by its own view); a shot from anywhere
    CHECK(!acceptPop(kPopSpin, kPopGraceTurns - 1, kPopGraceTurns, true, 100.0f, kRadius));
    CHECK(acceptPop(kPopSpin, kPopGraceTurns, kPopGraceTurns, true, 100.0f, kRadius));
    CHECK(!acceptPop(kPopSpin, 200, kPopGraceTurns, false, 100.0f, kRadius));
    CHECK(!acceptPop(kPopSpin, 200, kPopGraceTurns, true, -1.0f, kRadius));
    CHECK(acceptPop(kPopSpin, 200, kPopGraceTurns, true, kRadius + kSpinReach + kPopCheckSlack - 1.0f, kRadius));
    CHECK(!acceptPop(kPopSpin, 200, kPopGraceTurns, true, kRadius + kSpinReach + kPopCheckSlack + 1.0f, kRadius));
    CHECK(acceptPop(kPopShot, 200, kPopGraceTurns, true, 2000.0f, kRadius));   // gamefix 8: a shot reaches far, but not anywhere
    CHECK(!acceptPop(kPopShot, 200, kPopGraceTurns, true, 5000.0f, kRadius));
    CHECK(!acceptPop(kPopShot, 200, kPopGraceTurns, true, -1.0f, kRadius));   // unknown position: no pop
    CHECK(acceptPop(kPopShot, 200, kPopGraceTurns, true, kRadius + kShotReach + kPopCheckSlack - 1.0f, kRadius));
    CHECK(!acceptPop(kPopShot, 200, kPopGraceTurns, true, kRadius + kShotReach + kPopCheckSlack + 1.0f, kRadius));
    CHECK(!acceptPop(kPopShot, 200, kPopGraceTurns, false, 100.0f, kRadius));
    CHECK(!acceptPop(7, 200, kPopGraceTurns, true, 10.0f, kRadius));
    CHECK(acceptPop(kPopShot, kArrivalGraceTurns, kArrivalGraceTurns, true, 10.0f, kRadius));   // an arrival's shorter grace
    // the arrival pops by itself after ~2 s
    CHECK(!arrivalDone(kArrivalTurns - 1) && arrivalDone(kArrivalTurns) && kArrivalTurns >= 100 && kArrivalTurns <= 150);
}

static void testLives() {
    CHECK(!fallIsOutOfLives(1) && !fallIsOutOfLives(4) && fallIsOutOfLives(0) && fallIsOutOfLives(-1));
    CHECK(livesAfterPop(0, true) == 1 && livesAfterPop(3, false) == 3 && livesAfterPop(0, false) == 0);
}

// gamefix 9: a stalled peer's old pose is not "around"
static void testPoseFreshness() {
    PoseFreshness f;
    CHECK(!f.fresh(1, 100));                       // never noted
    f.note(1, 5000, 100);
    CHECK(f.fresh(1, 100) && f.fresh(1, 100 + kPoseStaleTurns));
    f.note(1, 5000, 200);                          // the same pose again: no news
    CHECK(!f.fresh(1, 100 + kPoseStaleTurns + 1));
    f.note(1, 5060, 300);                          // he sends again
    CHECK(f.fresh(1, 300 + kPoseStaleTurns) && !f.fresh(1, 300 + kPoseStaleTurns + 1));
    f.note(1, 10, 400);                            // (his counter restarted: a new session) still a change
    CHECK(f.fresh(1, 400));
    CHECK(!f.fresh(-1, 0) && !f.fresh(MP_MAX_PLAYERS, 0) && !f.fresh(2, 0));
    f.note(99, 1, 1);                              // out of range: ignored
    f.reset();
    CHECK(!f.fresh(1, 400));
}

static void testCameraAndStale() {
    // the owner's run: after the abyss fall the camera stayed 7623 units away
    CHECK(cameraNeedsCut(7623.0f, 100) && !cameraNeedsCut(7623.0f, 5));   // (rate limited)
    CHECK(!cameraNeedsCut(1252.0f, 100) && !cameraNeedsCut(kCameraLost, 100) && cameraNeedsCut(kCameraLost + 1.0f, kCameraResetGap));
    // the death queued in the turn of the pop fires 1 turn later (11 ms in the log); a real fall much later is a fall
    CHECK(staleKillAfterPop(true, 1) && staleKillAfterPop(true, kStaleKillTurns));
    CHECK(!staleKillAfterPop(true, kStaleKillTurns + 1) && !staleKillAfterPop(false, 1));
}

int main() {
    testCodec();
    testPoseFreshness();
    testSettings();
    testTable();
    testFloat();
    testSpinShot();
    testPopRequest();
    testLives();
    testCameraAndStale();
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
