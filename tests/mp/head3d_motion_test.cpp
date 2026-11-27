// heads3d: the character select's 3D heads move with the file select's own timing (MpHead3DMotion.hpp, from FileSelectItem.cpp).
//   clang++ -std=c++20 -fsanitize=address,undefined -g -Idecomp/include tests/mp/head3d_motion_test.cpp -o /tmp/head3d_motion && /tmp/head3d_motion
#include "Game/MP/MpHead3DMotion.hpp"

#include <math.h>
#include <stdio.h>

namespace mo = MpHead3DMotion;

static int sFail = 0, sChecks = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        ++sChecks;                                                       \
        if (!(c)) {                                                      \
            ++sFail;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)
static bool Near(float a, float b, float e = 1e-3f) { return fabsf(a - b) <= e; }

static void IdleSpin() {
    // at rest a head turns 0.5 degrees a frame, forever (FileSelectItem::updateRotate's floor): a full turn in 720 frames
    mo::Motion m;
    mo::reset(m, false);
    for (int i = 0; i < 720; i++) mo::step(m);
    CHECK(Near(m.spin, 0.5f));
    CHECK(Near(m.rotY, 0.0f, 0.05f) || Near(m.rotY, 360.0f, 0.05f));
    for (int i = 0; i < 90; i++) mo::step(m);
    CHECK(Near(m.rotY, 45.0f, 0.05f));
}

static void FlickDecays() {
    // a flick of 400 screen pixels: 12 degrees a frame, slowing by 0.98 a frame down to the 0.5 floor, keeping its direction
    mo::Motion m;
    mo::reset(m, false);
    mo::flick(m, 400.0f);
    CHECK(Near(m.spin, 12.0f));
    mo::step(m);
    CHECK(Near(m.spin, 12.0f * 0.98f));
    for (int i = 0; i < 400; i++) mo::step(m);
    CHECK(Near(m.spin, 0.5f));
    mo::flick(m, -2000.0f);   // (at most 25 a frame)
    CHECK(Near(m.spin, -25.0f));
    for (int i = 0; i < 400; i++) mo::step(m);
    CHECK(Near(m.spin, -0.5f));   // a head flicked left keeps turning left
    mo::flick(m, NAN);            // (garbage from the pointer is ignored)
    CHECK(Near(m.spin, -0.5f));
}

static void GrowAndTurn() {
    // pointed: 1.0 -> 1.2 within 30 frames (ScaleController::exeToBig), and facing the front after 40 (turnToFront(40))
    mo::Motion m;
    mo::reset(m, false);
    for (int i = 0; i < 100; i++) mo::step(m);   // (turned 50 degrees)
    CHECK(Near(m.rotY, 50.0f, 0.1f));
    mo::point(m, true);
    float last = m.grow;
    bool monotonic = true;
    for (int i = 0; i < 31; i++) {
        mo::step(m);
        monotonic = monotonic && m.grow >= last - 1e-6f;
        last = m.grow;
    }
    CHECK(monotonic);
    CHECK(Near(m.grow, 1.2f, 0.01f));
    for (int i = 0; i < 9; i++) mo::step(m);   // (40 frames of the turn)
    CHECK(Near(m.rotY, 0.0f, 0.01f));
    CHECK(m.turnDur == 0);
    mo::step(m);   // then it turns slowly again
    CHECK(Near(m.rotY, 0.5f, 0.01f));
    // a head at 300 degrees turns the short way (through 360), not back through 180
    mo::Motion n;
    mo::reset(n, false);
    n.rotY = 300.0f;
    mo::point(n, true);
    mo::step(n);
    CHECK(n.rotY < 0.0f && n.rotY > -60.0f);
    // left: back to 1.0
    mo::point(m, false);
    for (int i = 0; i < 31; i++) mo::step(m);
    CHECK(Near(m.grow, 1.0f, 0.01f));
    // pointing twice does not restart the grow
    mo::point(m, true);
    for (int i = 0; i < 10; i++) mo::step(m);
    const float g = m.grow;
    mo::point(m, true);
    mo::step(m);
    CHECK(m.grow >= g);
}

static void Blinks() {
    mo::Blink b;
    mo::blinkEnter(b, mo::BLINK_OPEN, 200);
    int blinks = 0, normals = 0;
    for (int f = 0; f < 199; f++) blinks += mo::blinkStep(b, 0.5f, false, false, 200) == mo::ACT_BLINK;
    CHECK(blinks == 0);
    CHECK(mo::blinkStep(b, 0.5f, false, false, 200) == mo::ACT_BLINK);   // frame 200: the eyes shut
    for (int f = 0; f < 10; f++) normals += mo::blinkStep(b, 0.5f, false, false, 250) == mo::ACT_NORMAL;
    CHECK(normals == 1 && b.state == mo::BLINK_OPEN && b.limit == 250);   // 10 frames later they open
    // spun hard for long: the eyes close, then open once it turns slowly for 60 frames
    mo::Blink c;
    mo::blinkEnter(c, mo::BLINK_OPEN, 300);
    mo::BlinkAct act = mo::ACT_NONE;
    int f = 0;
    for (; f < 200 && act != mo::ACT_CLOSE; f++) act = mo::blinkStep(c, 12.0f, false, false, 300);
    CHECK(act == mo::ACT_CLOSE && f == 91);   // (2 counts a frame past 10 degrees: over 180 on the 91st frame)
    for (f = 0; f < 61; f++) act = mo::blinkStep(c, 1.0f, false, false, 300);
    CHECK(act == mo::ACT_OPEN);
    act = mo::blinkStep(c, 1.0f, true, false, 300);
    CHECK(act == mo::ACT_NORMAL && c.state == mo::BLINK_OPEN);
    // turning to the front opens the eyes at once
    mo::Blink d;
    mo::blinkEnter(d, mo::BLINK_SLEEP, 300);
    CHECK(mo::blinkStep(d, 0.0f, false, true, 300) == mo::ACT_NORMAL && d.state == mo::BLINK_OPEN);
    CHECK(mo::blinkStep(d, 0.0f, false, true, 300) == mo::ACT_NONE);
}

static void Sizes() {
    // a 105-unit head box at 0.84, 2.24 world units a square unit, a 60-unit model head: scale 3.29; grown 1.2; popping
    CHECK(Near(mo::modelScale(105.0f, 0.84f, 2.24f, 60.0f, 1.0f, 1.0f), 105.0f * 0.84f * 2.24f / 60.0f));
    CHECK(Near(mo::modelScale(105.0f, 0.84f, 2.24f, 60.0f, 1.2f, 1.0f), 1.2f * 105.0f * 0.84f * 2.24f / 60.0f));
    CHECK(mo::modelScale(105.0f, 0.84f, 2.24f, 0.0f, 1.0f, 1.0f) == 0.0f);   // (a model that measured nothing is not drawn huge)
    CHECK(mo::modelScale(105.0f, 0.84f, 2.24f, NAN, 1.0f, 1.0f) == 0.0f);
    CHECK(mo::pop(0.0f, true) == 0.0f && mo::pop(1.0f, true) == 1.0f && mo::pop(NAN, true) == 0.0f);
    float peak = 0.0f;
    for (int i = 1; i < 100; i++) peak = fmaxf(peak, mo::pop(i / 100.0f, true));
    CHECK(peak > 1.05f && peak < 1.15f);   // (the pop overshoots a little)
    for (int i = 1; i < 100; i++) CHECK(mo::pop(i / 100.0f, false) <= 1.0f);
    CHECK(Near(mo::wrap(-190.0f, -180.0f, 360.0f), 170.0f) && Near(mo::wrap(725.0f, 0.0f, 360.0f), 5.0f));
}

static void FrontMode() {
    // character select: a flicked head spins, slows, then eases back to face the camera and stays there
    mo::Motion m;
    m.front = true;
    mo::reset(m, false);
    CHECK(m.front);
    mo::flick(m, 400.0f);
    for (int i = 0; i < 30; i++) mo::step(m);
    CHECK(fabsf(m.spin) > 1.0f);
    for (int i = 0; i < 600; i++) mo::step(m);
    CHECK(m.rotY == 0.0f && m.spin == 0.0f);
    for (int i = 0; i < 100; i++) mo::step(m);
    CHECK(m.rotY == 0.0f);   // (it does not drift)
}

int main() {
    FrontMode();
    IdleSpin();
    FlickDecays();
    GrowAndTurn();
    Blinks();
    Sizes();
    printf("head3d motion: %d checks, %d failed\n", sChecks, sFail);
    return sFail ? 1 : 0;
}
