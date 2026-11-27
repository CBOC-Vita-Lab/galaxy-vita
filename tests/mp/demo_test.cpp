// Host tests of the puppet demo: the switch's parse, the mock roster, the script (MpDemoLogic.hpp) and the canned poses.
// Build and run: tests/mp/demo_build.sh (ASan + UBSan; clang++ -Werror as a second compiler).
#include "Game/MP/MpDemoLogic.hpp"
#include "port/mp_demo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(c)                                                                                                                       \
    do {                                                                                                                               \
        gChecks++;                                                                                                                     \
        if (!(c)) {                                                                                                                    \
            gFail++;                                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                                        \
        }                                                                                                                              \
    } while (0)

using namespace MpDemoLogic;

static void testParse() {
    int m = -1, s = -1;
    mp_demo_parse(nullptr, nullptr, &m, &s);
    CHECK(m == MP_DEMO_OFF && s == 60);
    mp_demo_parse("", "", &m, &s);
    CHECK(m == MP_DEMO_OFF && s == 60);
    mp_demo_parse("0", nullptr, &m, &s);
    CHECK(m == MP_DEMO_OFF);
    mp_demo_parse("yes", nullptr, &m, &s);
    CHECK(m == MP_DEMO_OFF);   // only the documented values switch it on
    mp_demo_parse("1", nullptr, &m, &s);
    CHECK(m == MP_DEMO_PUPPETS && s == 60);
    mp_demo_parse("puppets", "90", &m, &s);
    CHECK(m == MP_DEMO_PUPPETS && s == 90);
    mp_demo_parse("menus", "5", &m, &s);
    CHECK(m == MP_DEMO_MENUS && s == 20);
    mp_demo_parse("1", "99999", &m, &s);
    CHECK(s == 600);
    mp_demo_parse("1", "-3", &m, &s);
    CHECK(s == 60);
    mp_demo_parse("1", "abc", &m, &s);
    CHECK(s == 60);
    // without a read of the environment the process-wide mode is off and the roster does not answer
    CHECK(!g_mpDemoRoster);
}

static void testRoster() {
    mp_demo_roster_enable(true);
    CHECK(g_mpDemoRoster);
    mp_demo_roster_local(0);
    mp_demo_roster_set(1, true, MP_LOOK_LUIGI, "Luigi", 40, false);
    mp_demo_roster_set(2, true, MP_LOOK_TOAD, "A very long name that does not fit in a name", 300, true);
    mp_demo_roster_set(9, true, 1, "x", 1, false);   // out of range: ignored
    CHECK(mp_demo_roster_present(1) && !mp_demo_roster_present(0) && !mp_demo_roster_present(9));
    CHECK(mp_demo_roster_look(1) == MP_LOOK_LUIGI && mp_demo_roster_look(2) == MP_LOOK_TOAD);
    CHECK(strcmp(mp_demo_roster_name(1), "Luigi") == 0);
    CHECK(strlen(mp_demo_roster_name(2)) <= 16);
    CHECK(mp_demo_roster_ping(1) == 40 && mp_demo_roster_ping(3) == -1);
    CHECK(mp_demo_roster_quiet(2) && !mp_demo_roster_quiet(1));
    CHECK(mp_demo_roster_local_slot() == 0 && !mp_demo_roster_is_host());
    mp_demo_roster_host(true);
    CHECK(mp_demo_roster_is_host());
    mp_demo_roster_enable(false);
    CHECK(!g_mpDemoRoster && !mp_demo_roster_present(1) && mp_demo_roster_local_slot() == -1 && !mp_demo_roster_is_host());
}

static void testScriptCoverage(bool water) {
    bool look[MP_LOOK_COUNT] = {};
    bool form[MP_FORM_COUNT] = {};
    bool act[ACT_COUNT] = {};
    bool bubble = false, pointer = false, weak = false, lost = false, anyPresent = false;
    int maxPresent = 0;
    for (int i = 0; i <= 1000; i++) {
        const float u = (float)i / 1000.0f;
        View v;
        viewAt(u, 60.0f, water, &v);
        int n = 0;
        for (int s = 0; s < kSlots; s++) {
            if (!v.present[s]) continue;
            n++;
            look[v.look[s]] = true;
            form[v.form[s]] = true;
            act[v.act[s]] = true;
            // every present puppet has a distinct character
            for (int o = s + 1; o < kSlots; o++) CHECK(!(v.present[o] && v.look[o] == v.look[s]));
            CHECK(v.look[s] < MP_LOOK_COUNT && v.form[s] < MP_FORM_COUNT && v.act[s] < ACT_COUNT);
            CHECK(v.actAge[s] >= 0.0f && v.actAge[s] <= 0.05f * 60.0f + 0.01f);
            if (v.ping[s] >= 250) weak = true;
            if (v.quiet[s]) lost = true;
        }
        anyPresent = anyPresent || n > 0;
        if (n > maxPresent) maxPresent = n;
        bubble = bubble || v.bubble;
        pointer = pointer || v.pointer;
        if (v.bubble) CHECK(v.present[1]);     // the bubble's puppet is there
        if (v.pointer) CHECK(v.present[2]);    // so is the pointer's owner
        CHECK(v.done == (u >= 1.0f));
        if (u < 0.02f || u >= 0.99f) CHECK(n == 0);   // nobody at the very start (the settle) and at the end
    }
    for (int c = 0; c < MP_LOOK_COUNT; c++) CHECK(look[c]);
    for (int f = 0; f < MP_FORM_COUNT; f++) CHECK(form[f]);   // every form id is on screen at some point (the ones that change the model colour)
    CHECK(act[ACT_SWIM] == water);
    if (!water) {
        CHECK(act[ACT_WALK] && act[ACT_JUMP] && act[ACT_SPIN] && act[ACT_CROUCH]);
    }
    CHECK(bubble && pointer && weak && lost && anyPresent && maxPresent == kSlots);
}

static Ref refFlat() {
    Ref r;
    memset(&r, 0, sizeof r);
    r.pos[0] = 1000.0f; r.pos[1] = 200.0f; r.pos[2] = -500.0f;
    r.up[1] = 1.0f;
    r.front[2] = 1.0f;
    return r;
}

static float dist(const float a[3], const float b[3]) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return MpPoseMath::len3(d);
}

static void testPoses() {
    Ref ref[3];
    ref[0] = refFlat();
    // a player standing on the far side of a planet: up = -x, front = +z
    memset(&ref[1], 0, sizeof ref[1]);
    ref[1].pos[0] = -3000.0f; ref[1].up[0] = -1.0f; ref[1].front[2] = 1.0f;
    // front nearly parallel to up (looking straight up): the frame still comes out
    memset(&ref[2], 0, sizeof ref[2]);
    ref[2].up[1] = 1.0f; ref[2].front[1] = 1.0f;
    for (const Ref& r : ref) {
        for (int a = 0; a < ACT_COUNT; a++) {
            for (int k = 0; k < 6; k++) {
                MpPoseF p;
                makePose(r, 0.5f * (float)k, (uint8_t)a, 0.13f * (float)k, MP_FORM_NORMAL, 100u + (uint32_t)k, &p);
                float qn = p.quat[0] * p.quat[0] + p.quat[1] * p.quat[1] + p.quat[2] * p.quat[2] + p.quat[3] * p.quat[3];
                CHECK(fabsf(qn - 1.0f) < 1e-3f);
                float up[3];
                MpPoseMath::quatUp(p.quat, up);
                float ru[3] = {r.up[0], r.up[1], r.up[2]};
                MpPoseMath::len3(ru);
                CHECK(MpPoseMath::dot3(up, ru) > 0.999f);   // standing on the player's ground
                // near the player: on the circle (plus the jump's height)
                float flat[3] = {p.pos[0] - r.pos[0], p.pos[1] - r.pos[1], p.pos[2] - r.pos[2]};
                const float h = MpPoseMath::dot3(flat, ru);
                for (int i = 0; i < 3; i++) flat[i] -= ru[i] * h;
                CHECK(fabsf(MpPoseMath::len3(flat) - kRadius) < 1.0f);
                CHECK(h >= -0.01f && h <= kJumpHeight + 0.5f);
                CHECK(p.sceneSerial == 1 && (p.flags & MP_POSE_VISIBLE) != 0 && (p.flags & MP_POSE_SIMPLE_BCK) != 0);
                CHECK(p.anim == MpPoseMath::animHash(actBck((uint8_t)a)));
                CHECK(p.frame >= 0.0f);
                // the wire round trip the puppets read
                uint8_t buf[MP_POSE_WIRE_BYTES + MP_POSE_TAIL_BYTES];
                const size_t n = MpPoseMath::writeState(buf, sizeof buf, p);
                CHECK(n == MP_POSE_WIRE_BYTES);
                MpPoseF back;
                CHECK(MpPoseMath::readState(buf, n, &back));
                CHECK(back.sceneSerial == 1 && back.anim == p.anim && back.form == p.form);
                CHECK(dist(back.pos, p.pos) < 0.5f);
            }
        }
    }
}

// What each activity becomes on Toad (the engine-drawn puppet maps Mario's state onto his clips).
static void testToadMapping() {
    const Ref r = refFlat();
    MpPoseF p;
    makePose(r, 0.0f, ACT_WALK, 1.0f, MP_FORM_NORMAL, 1, &p);
    CHECK(mp_toad_map(p, actBck(ACT_WALK), MP_TOAD_WALK).anim == MP_TOAD_WALK);
    makePose(r, 0.0f, ACT_SPIN, 1.0f, MP_FORM_NORMAL, 1, &p);
    CHECK(mp_toad_map(p, actBck(ACT_SPIN), MP_TOAD_WAIT).anim == MP_TOAD_SPIN);
    makePose(r, 0.0f, ACT_SWIM, 1.0f, MP_FORM_NORMAL, 1, &p);
    CHECK(mp_toad_map(p, actBck(ACT_SWIM), MP_TOAD_WAIT).anim == MP_TOAD_SWIM);
    makePose(r, 0.0f, ACT_CROUCH, 1.0f, MP_FORM_NORMAL, 1, &p);
    CHECK(mp_toad_map(p, actBck(ACT_CROUCH), MP_TOAD_WAIT).anim == MP_TOAD_WAIT);
    makePose(r, 0.0f, ACT_JUMP, 0.1f, MP_FORM_NORMAL, 1, &p);   // rising
    CHECK(mp_toad_map(p, actBck(ACT_JUMP), MP_TOAD_WAIT).anim == MP_TOAD_AIR_UP);
    makePose(r, 0.0f, ACT_JUMP, 0.8f, MP_FORM_NORMAL, 1, &p);   // falling
    CHECK(mp_toad_map(p, actBck(ACT_JUMP), MP_TOAD_WAIT).anim == MP_TOAD_AIR_DOWN);
}

static void testPointer() {
    const Ref r = refFlat();
    for (int k = 0; k < 20; k++) {
        float p[3];
        pointerPoint(r, 0.4f * (float)k, p);
        CHECK(dist(p, r.pos) < 500.0f && dist(p, r.pos) > 100.0f);
        CHECK(MpPoseMath::finitef(p[0]) && MpPoseMath::finitef(p[1]) && MpPoseMath::finitef(p[2]));
    }
    // the ping message the demo sends is a valid POINTER
    MpPointerMsg m;
    memset(&m, 0, sizeof m);
    m.serial = 1; m.on = 1;
    pointerPoint(r, 1.0f, m.pos);
    uint8_t buf[32];
    MpWriter w(buf, sizeof buf);
    mp_write_pointer(w, m);
    MpReader rd(buf, w.n);
    MpPointerMsg back;
    CHECK(w.ok() && mp_read_pointer(rd, &back) && back.on == 1);
}

int main() {
    testParse();
    testRoster();
    testScriptCoverage(false);
    testScriptCoverage(true);
    testPoses();
    testToadMapping();
    testPointer();
    printf("demo_test: %d checks, %d failed\n", gChecks, gFail);
    return gFail == 0 ? 0 : 1;
}
