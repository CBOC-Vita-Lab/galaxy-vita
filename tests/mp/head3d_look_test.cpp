// heads: what a 3D head shows for a state (MpHead3DLook.hpp): free / taken / mine -> badge, colour, alpha, the "no" shake.
//   clang++ -std=c++20 -fsanitize=address,undefined -g -Idecomp/include tests/mp/head3d_look_test.cpp -o /tmp/head3d_look && /tmp/head3d_look
#include "Game/MP/MpHead3DLook.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace lk = MpHead3DLook;

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

int main() {
    // free: no badge, full alpha
    lk::State free_;
    free_.look = 3;
    CHECK(lk::tag(free_) == lk::TAG_FREE);
    CHECK(!lk::badge(free_, 100, 100, 90, 1.0f, 1.0f).shown);
    CHECK(Near(lk::alpha(free_, 1.0f), 1.0f));

    // taken by player 2 (slot 1): a badge "2P" in that character's colour, the head not faded
    lk::State taken;
    taken.look = 1, taken.owner = 1;
    CHECK(lk::tag(taken) == lk::TAG_TAKEN);
    const lk::Badge b = lk::badge(taken, 200, 150, 90, 1.0f, 1.0f);
    CHECK(b.shown);
    CHECK(strcmp(b.text, "2P") == 0);
    CHECK(b.fill.r == MpCharColor::fill(1).r && b.fill.g == MpCharColor::fill(1).g && b.fill.b == MpCharColor::fill(1).b);
    CHECK(b.edge.g == MpCharColor::edge(1).g);
    CHECK(!b.mine);
    CHECK(Near(lk::alpha(taken, 1.0f), 1.0f));   // (never washed: a taken head keeps its full colour)
    // on the head's upper right, inside a head-wide margin, a pill (radius = half the height)
    CHECK(b.x0 > 200 && b.x1 > b.x0 && b.y1 < 150 && b.y0 < b.y1);
    CHECK(Near(b.radius, (b.y1 - b.y0) * 0.5f));
    CHECK(b.x1 - b.x0 > b.y1 - b.y0);
    CHECK(b.x1 < 200 + 90.0f && b.y0 > 150 - 90.0f);

    // each owner its number
    for (int o = 0; o < lk::kMaxOwner; o++) {
        lk::State s;
        s.owner = o;
        const lk::Badge k = lk::badge(s, 0, 0, 90, 1.0f, 1.0f);
        CHECK(k.shown && k.text[0] == '1' + o && k.text[1] == 'P' && k.text[2] == 0);
    }
    // out of range owner: nothing (a hostile roster)
    lk::State bad;
    bad.owner = 7;
    CHECK(!lk::badge(bad, 0, 0, 90, 1.0f, 1.0f).shown);
    bad.owner = -5;
    CHECK(!lk::badge(bad, 0, 0, 90, 1.0f, 1.0f).shown);

    // mine: the same badge with a thicker rim
    lk::State mine;
    mine.look = 0, mine.owner = 0, mine.mine = true;
    CHECK(lk::tag(mine) == lk::TAG_MINE);
    const lk::Badge m = lk::badge(mine, 0, 0, 90, 1.0f, 1.0f);
    CHECK(m.shown && m.mine && strcmp(m.text, "1P") == 0);
    CHECK(m.ring > lk::badge(taken, 0, 0, 90, 1.0f, 1.0f).ring);

    // it grows and shrinks with the head (pop-in / pop-out, the selected head's growth); gone at scale 0
    const lk::Badge half = lk::badge(taken, 0, 0, 90, 0.5f, 1.0f);
    CHECK(Near(half.y1 - half.y0, (b.y1 - b.y0) * 0.5f, 1e-2f));
    CHECK(!lk::badge(taken, 0, 0, 90, 0.0f, 1.0f).shown);
    CHECK(!lk::badge(taken, 0, 0, 0.0f, 1.0f, 1.0f).shown);

    // dimmed: the whole head and its badge at kDimAlpha; the screen's fade multiplies
    lk::State dim = taken;
    dim.dimmed = true;
    CHECK(Near(lk::alpha(dim, 1.0f), lk::kDimAlpha));
    CHECK(Near(lk::alpha(dim, 0.5f), lk::kDimAlpha * 0.5f));
    CHECK(Near(lk::badge(dim, 0, 0, 90, 1.0f, 1.0f).alpha, lk::kDimAlpha));
    CHECK(Near(lk::alpha(free_, 7.0f), 1.0f));   // (clamped)
    CHECK(Near(lk::alpha(free_, -2.0f), 0.0f));

    // the shake: starts at rest, swings both ways, dies out, and is over after kShakeFrames
    CHECK(Near(lk::shakeYaw(0), 0.0f));
    float lo = 0, hi = 0, last = 0;
    for (int i = 0; i < lk::kShakeFrames; i++) {
        const float y = lk::shakeYaw(i);
        lo = y < lo ? y : lo, hi = y > hi ? y : hi;
        CHECK(fabsf(y) <= lk::kShakeDeg + 1e-3f);
        last = y;
    }
    CHECK(lo < -4.0f && hi > 4.0f);   // a visible "no"
    CHECK(fabsf(last) < 1.5f);        // (damped: nearly at rest at the end)
    CHECK(Near(lk::shakeYaw(lk::kShakeFrames), 0.0f));
    CHECK(Near(lk::shakeYaw(-3), 0.0f));
    CHECK(!lk::shaking(-1) && lk::shaking(0) && lk::shaking(lk::kShakeFrames - 1) && !lk::shaking(lk::kShakeFrames));

    printf("head3d_look_test: %d checks, %d failed\n", sChecks, sFail);
    return sFail ? 1 : 0;
}
