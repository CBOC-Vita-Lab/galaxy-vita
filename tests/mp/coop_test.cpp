// Galaxy 3.0 multiplayer: host tests of the coop extras' pure logic (decomp/include/Game/MP/MpCoopLogic.hpp) and their payload
// codecs (platform/include/port/mp_proto.h): greeting emote rate limits, the idle rule, the gesture stop rule, the name tag
// flash, the voices, the pointer ping's sender pacing (<= 15 Hz, hold threshold, one OFF at release) and receiver (timeout,
// smoothing), the super jump arming rule, and hostile payloads.
// Build and run (from the worktree root): tests/mp/coop_build.sh
#include "Game/MP/MpCoopLogic.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

using namespace MpCoopLogic;

static void TestRateGates() {
    RateGate g;
    g.reset();
    CHECK(g.allow(5000, kEmoteMinMs));
    CHECK(!g.allow(5001, kEmoteMinMs));
    CHECK(!g.allow(5999, kEmoteMinMs));
    CHECK(g.allow(6000, kEmoteMinMs));
    // a 10 Hz button masher gets exactly one greeting per second
    g.reset();
    int sent = 0;
    for (int64_t t = 0; t < 10000; t += 100) sent += g.allow(t, kEmoteMinMs) ? 1 : 0;
    CHECK(sent == 10);
    // a clock that went backwards re-arms (never a stuck gate)
    CHECK(g.allow(100, kEmoteMinMs));
    // receiver: 800 ms
    RateGate r;
    r.reset();
    CHECK(r.allow(0, kEmoteRecvMinMs) && !r.allow(700, kEmoteRecvMinMs) && r.allow(800, kEmoteRecvMinMs));
    printf("rate gates: ok\n");
}

static IdleIn Idle() {
    IdleIn in;
    in.onGround = true; in.jumping = false; in.defaultAnim = true; in.stickOn = false; in.walkSpeed = 0.0f;
    in.plainStatus = true; in.busy = false; in.formOk = true;
    return in;
}

static void TestIdle() {
    CHECK(idleForGesture(Idle()));
    IdleIn in = Idle(); in.onGround = false; CHECK(!idleForGesture(in));
    in = Idle(); in.jumping = true; CHECK(!idleForGesture(in));
    in = Idle(); in.defaultAnim = false; CHECK(!idleForGesture(in));
    in = Idle(); in.stickOn = true; CHECK(!idleForGesture(in));
    in = Idle(); in.walkSpeed = 0.5f; CHECK(!idleForGesture(in));
    in = Idle(); in.walkSpeed = -0.5f; CHECK(!idleForGesture(in));
    in = Idle(); in.walkSpeed = 0.01f; CHECK(idleForGesture(in));
    in = Idle(); in.plainStatus = false; CHECK(!idleForGesture(in));
    in = Idle(); in.busy = true; CHECK(!idleForGesture(in));
    in = Idle(); in.formOk = false; CHECK(!idleForGesture(in));
    // the gesture stops at the first move input, a jump, crouch, leaving the ground, or after 2.5 s
    CHECK(!gestureShouldStop(false, false, false, true, 10));
    CHECK(gestureShouldStop(true, false, false, true, 10));
    CHECK(gestureShouldStop(false, true, false, true, 10));
    CHECK(gestureShouldStop(false, false, true, true, 10));
    CHECK(gestureShouldStop(false, false, false, false, 10));
    CHECK(gestureShouldStop(false, false, false, true, kEmoteGestureTurns));
    printf("idle rule: ok\n");
}

static void TestFlashAndVoice() {
    CHECK(!flashOn(-1));
    CHECK(flashOn(0) && flashOn(7) && !flashOn(8) && !flashOn(11) && flashOn(12));
    CHECK(!flashOn(kEmoteFlashTurns) && !flashOn(100000));
    int on = 0;
    for (int t = 0; t < kEmoteFlashTurns; t++) on += flashOn(t) ? 1 : 0;
    CHECK(on == 64);   // 8 of every 12 turns, 1.6 s
    CHECK(!strcmp(voiceFor(MP_LOOK_MARIO).se, "SE_PV_JUMP_JOY") && voiceFor(MP_LOOK_MARIO).port12 == 0);
    CHECK(voiceFor(MP_LOOK_COSMIC).port12 == 0);
    CHECK(!strcmp(voiceFor(MP_LOOK_LUIGI).se, "SE_PV_JUMP_JOY") && voiceFor(MP_LOOK_LUIGI).port12 == 20);
    CHECK(voiceFor(MP_LOOK_COSMIC_LUIGI).port12 == 20);
    CHECK(!strcmp(voiceFor(MP_LOOK_TOAD).se, "SE_SV_KINOPIO_TALK_HEY") && voiceFor(MP_LOOK_TOAD).port12 < 0);
    CHECK(!strcmp(voiceFor(MP_LOOK_YELLOW_TOAD).se, "SE_SV_KINOPIO_TALK_HEY"));
    CHECK(voiceFor(200).se != nullptr);   // out of range: Mario's
    printf("flash + voice: ok\n");
}

static void TestPointerSender() {
    PointerSender s;
    s.reset();
    const int64_t turnNs = 16666667;
    int64_t t = 1000000000;
    // a quick Star Bit shot (B for 5 turns): nothing at all
    for (int i = 0; i < 5; i++, t += turnNs) CHECK(s.step(true, t) == POINTER_NONE);
    CHECK(s.step(false, t) == POINTER_NONE);
    // held for 3 s at 60 turns/s: first send after the hold threshold, then <= 15 Hz; one OFF at release
    int on = 0, first = -1;
    for (int i = 0; i < 180; i++, t += turnNs) {
        PointerSend r = s.step(true, t);
        CHECK(r != POINTER_OFF);
        if (r == POINTER_ON) { on++; if (first < 0) first = i; }
    }
    CHECK(first == kPointerHoldTurns - 1);
    CHECK(on >= 40 && on <= 46);   // 3 s - threshold at 15 Hz
    CHECK(s.step(false, t) == POINTER_OFF);
    CHECK(s.step(false, t + turnNs) == POINTER_NONE);
    // turns faster than real time (a skipped cutscene): still <= 15 Hz of wall clock
    s.reset();
    on = 0;
    for (int i = 0; i < 600; i++, t += 2000000) on += s.step(true, t) == POINTER_ON ? 1 : 0;   // 600 turns in 1.2 s
    CHECK(on <= 19);
    // jittery turns (10..25 ms) for 60 s: never more than 15 a second on average, and the rate holds up (>= 14/s)
    s.reset();
    on = 0;
    uint32_t x = 99;
    const int64_t t0 = t;
    while (t - t0 < 60000000000LL) {
        x = x * 1103515245u + 12345u;
        t += 10000000 + (int64_t)((x >> 16) % 15000000);
        on += s.step(true, t) == POINTER_ON ? 1 : 0;
    }
    CHECK(on <= 60 * 15 + 1 && on >= 60 * 14);
    printf("pointer sender: ok (jitter: %d in 60 s)\n", on);
}

static void TestPointerView() {
    PointerView v;
    v.reset();
    CHECK(!v.step(0));
    MpPointerMsg m = {1, 1, {100.0f, 0.0f, 0.0f}};
    v.receive(m, 1000);
    CHECK(v.step(1000) && v.pos[0] == 100.0f);   // the first point is shown as is
    m.pos[0] = 200.0f;
    v.receive(m, 1066);
    CHECK(v.step(1070) && v.pos[0] > 100.0f && v.pos[0] < 200.0f);   // then smoothed towards the newest
    for (int i = 0; i < 30; i++) v.step(1080);
    CHECK(fabsf(v.pos[0] - 200.0f) < 0.5f);
    // silence: hidden after kPointerStaleMs
    CHECK(v.step(1066 + kPointerStaleMs));
    CHECK(!v.step(1066 + kPointerStaleMs + 1));
    // a new ping starts fresh (no slide from the old point)
    m.pos[0] = -500.0f;
    v.receive(m, 3000);
    CHECK(v.step(3000) && v.pos[0] == -500.0f);
    // OFF hides at once
    m.on = 0;
    v.receive(m, 3010);
    CHECK(!v.step(3010));
    // clock backwards: hidden, never stuck on
    m.on = 1;
    v.receive(m, 5000);
    CHECK(!v.step(4000));
    printf("pointer view: ok\n");
}

static void TestSuperJump() {
    CHECK(superJumpArmed(true, true, false, false));
    CHECK(!superJumpArmed(false, true, false, false));   // interaction off: plain push, never a bounce
    CHECK(!superJumpArmed(true, false, false, false));   // A not held: the plain stomp
    // mppause: no body push while a demo runs (the fly-in), for 3 s after it, and for the first 4 s of a scene; after that the push is as designed
    CHECK(pushHeld(1000, 1000, 0, false, false, false));          // the scene's first turn
    CHECK(pushHeld(1239, 1000, 0, false, false, false) && !pushHeld(1240, 1000, 0, false, false, false));
    CHECK(pushHeld(5000, 1000, 0, false, true, false) && pushHeld(5000, 1000, 0, false, false, true));   // a demo, ours or theirs
    CHECK(pushHeld(5179, 1000, 5000, true, false, false) && !pushHeld(5180, 1000, 5000, true, false, false));   // 3 s after the last demo turn
    CHECK(!pushHeld(5000, 1000, 0, false, false, false));         // normal play: bumping on
    CHECK(!superJumpArmed(true, true, true, false));     // on the ground (walking into a friend with A held does nothing)
    CHECK(!superJumpArmed(true, true, false, true));
    printf("super jump rule: ok\n");
}

static void TestCodecs() {
    uint8_t b[32];
    // emote: exact 2 bytes, id < 4, only the gesture flag
    for (int id = 0; id < 256; id++) {
        for (int fl = 0; fl < 4; fl++) {
            b[0] = (uint8_t)id; b[1] = (uint8_t)fl;
            MpReader r(b, 2);
            MpEmoteMsg e;
            const bool ok = mp_read_emote(r, &e);
            CHECK(ok == (id < MP_EMOTE_COUNT && fl <= 1));
        }
    }
    // pointer: round trip; every truncation and every extension fails; non-finite or huge coordinates fail
    MpPointerMsg m = {0xBEEF, 1, {-1234.5f, 99999.0f, 0.25f}};
    MpWriter w(b, sizeof b);
    mp_write_pointer(w, m);
    CHECK(w.ok() && w.n == 15);
    for (size_t n = 0; n <= 20; n++) {
        MpReader r(b, n);
        MpPointerMsg q;
        CHECK(mp_read_pointer(r, &q) == (n == 15));
        if (n == 15) CHECK(q.serial == 0xBEEF && q.on == 1 && q.pos[0] == -1234.5f && q.pos[1] == 99999.0f && q.pos[2] == 0.25f);
    }
    const float bad[] = {INFINITY, -INFINITY, NAN, 2.0e6f, -2.0e6f};
    for (float f : bad) {
        for (int k = 0; k < 3; k++) {
            MpPointerMsg q = m;
            q.pos[k] = f;
            MpWriter w2(b, sizeof b);
            mp_write_pointer(w2, q);
            MpReader r(b, w2.n);
            MpPointerMsg o;
            CHECK(!mp_read_pointer(r, &o));
        }
    }
    // random garbage never crashes and never yields an out-of-range result
    uint32_t x = 12345;
    for (int it = 0; it < 200000; it++) {
        const size_t n = (size_t)(x % 20);
        for (size_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; b[i] = (uint8_t)(x >> 16); }
        x = x * 1103515245u + 12345u;
        MpReader r1(b, n), r2(b, n);
        MpEmoteMsg e;
        MpPointerMsg p;
        if (mp_read_emote(r1, &e)) CHECK(e.emote < MP_EMOTE_COUNT);
        if (mp_read_pointer(r2, &p)) CHECK(p.on <= 1 && fabsf(p.pos[0]) < 1.0e6f && fabsf(p.pos[1]) < 1.0e6f && fabsf(p.pos[2]) < 1.0e6f);
    }
    // the session table: the new id is relayed by the host and is not host-only; it sits in the puppet range
    CHECK(mp_msg_host_relays(MP_MSG_POINTER) && !mp_msg_host_only(MP_MSG_POINTER));
    CHECK(MP_MSG_POINTER >= MP_MSG_GAME_FIRST && MP_MSG_POINTER <= 0x2F);
    printf("codecs: ok\n");
}

int main() {
    TestRateGates();
    TestIdle();
    TestFlashAndVoice();
    TestPointerSender();
    TestPointerView();
    TestSuperJump();
    TestCodecs();
    if (g_fail) {
        printf("FAILED: %d checks\n", g_fail);
        return 1;
    }
    printf("all coop tests passed\n");
    return 0;
}
