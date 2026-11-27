// Host tests for decomp/include/Game/MP/MpLodLogic.hpp (lod2: puppet level of detail by distance).
//   g++ -std=c++20 -fsanitize=address,undefined -g -Wall -Wextra -I decomp/include tests/mp/lod_test.cpp -o /tmp/lod_test && /tmp/lod_test
#include "Game/MP/MpLodLogic.hpp"

#include <cstdio>
#include <vector>

using namespace MpLod;

static int sFail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
            sFail++;                                                     \
        }                                                                \
    } while (0)

static float sq(float d) { return d * d; }

static void testBands() {
    const Config c;
    // climbing: each band is entered beyond its rateIn, not before
    int b = 0;
    b = nextBand(b, sq(3400), c); CHECK(b == 0);
    b = nextBand(b, sq(3501), c); CHECK(b == 1);
    b = nextBand(b, sq(5999), c); CHECK(b == 1);
    b = nextBand(b, sq(6001), c); CHECK(b == 2);
    b = nextBand(b, sq(9001), c); CHECK(b == 3);
    b = nextBand(b, sq(1.0e6f), c); CHECK(b == 3);   // (nothing above)
    // coming back: only inside rateOut
    b = nextBand(b, sq(8500), c); CHECK(b == 3);
    b = nextBand(b, sq(7999), c); CHECK(b == 2);
    b = nextBand(b, sq(5300), c); CHECK(b == 2);
    b = nextBand(b, sq(5199), c); CHECK(b == 1);
    b = nextBand(b, sq(3100), c); CHECK(b == 1);
    b = nextBand(b, sq(2999), c); CHECK(b == 0);
    // a jump skips bands in one call (a warp): near to far and back
    b = nextBand(0, sq(20000), c); CHECK(b == 3);
    b = nextBand(3, sq(100), c); CHECK(b == 0);
    // jitter across a threshold never changes the band: it stays inside the dead zone
    int changes = 0;
    b = 0;
    for (int i = 0; i < 2000; i++) {
        const float d = 3250.0f + 240.0f * std::sin(i * 0.7f);   // 3010..3490: inside the dead zone (3000..3500)
        const int nb = nextBand(b, sq(d), c);
        changes += nb != b;
        b = nb;
    }
    CHECK(changes == 0);
    // a slow drift up and down passes each threshold exactly once each way
    changes = 0;
    b = 0;
    for (float d = 0; d < 12000; d += 7) { const int nb = nextBand(b, sq(d), c); changes += nb != b; b = nb; }
    for (float d = 12000; d > 0; d -= 7) { const int nb = nextBand(b, sq(d), c); changes += nb != b; b = nb; }
    CHECK(changes == 6);
    CHECK(b == 0);
    // out-of-range bands are clamped
    CHECK(nextBand(-5, 0.0f, c) == 0);
    CHECK(nextBand(99, sq(100), c) == 0);
    CHECK(bandPeriod(0) == 1 && bandPeriod(1) == 2 && bandPeriod(2) == 3 && bandPeriod(3) == 4 && bandPeriod(9) == 4 && bandPeriod(-1) == 1);
}

static void testSkipAndStagger() {
    // one puppet: computed exactly once in `period` pictures
    for (int period = 1; period <= 4; period++) {
        for (int phase = 0; phase < 4; phase++) {
            int computed = 0;
            for (uint32_t pic = 0; pic < 120; pic++) computed += skipPicture(pic, phase, period) ? 0 : 1;
            CHECK(computed == 120 / period);
        }
    }
    CHECK(!skipPicture(7, 3, 0));
    CHECK(!skipPicture(7, 3, 1));
    // three puppets, all period 4: phases 0,1,2 and at most one computes in any picture
    const int period[SLOTS] = {0, 4, 4, 4};
    uint8_t phase[SLOTS];
    assignPhases(period, SLOTS, phase);
    CHECK(phase[1] == 0 && phase[2] == 1 && phase[3] == 2);
    int worst = 0;
    for (uint32_t pic = 0; pic < 400; pic++) {
        int n = 0;
        for (int s = 1; s < SLOTS; s++) n += skipPicture(pic, phase[s], period[s]) ? 0 : 1;
        worst = n > worst ? n : worst;
    }
    CHECK(worst == 1);
    // three puppets, period 3: exactly one per picture
    const int p3[SLOTS] = {0, 3, 3, 3};
    assignPhases(p3, SLOTS, phase);
    for (uint32_t pic = 0; pic < 60; pic++) {
        int n = 0;
        for (int s = 1; s < SLOTS; s++) n += skipPicture(pic, phase[s], p3[s]) ? 0 : 1;
        CHECK(n == 1);
    }
    // mixed: period 2 (slots 0,2), period 1 (slot 1), period 4 (slot 3): the two halves alternate
    const int mixed[SLOTS] = {2, 1, 2, 4};
    assignPhases(mixed, SLOTS, phase);
    CHECK(phase[0] == 0 && phase[2] == 1 && phase[1] == 0 && phase[3] == 0);
    for (uint32_t pic = 0; pic < 8; pic++) {
        const int n = (skipPicture(pic, phase[0], 2) ? 0 : 1) + (skipPicture(pic, phase[2], 2) ? 0 : 1);
        CHECK(n == 1);
    }
    // phases are always below the period (and a counter wrap is harmless)
    const int w[SLOTS] = {4, 4, 4, 4};
    assignPhases(w, SLOTS, phase);
    for (int s = 0; s < SLOTS; s++) CHECK(phase[s] < 4);
    CHECK(skipPicture(0xFFFFFFFFu, 1, 4) == (((0xFFFFFFFFu + 1u) % 4u) != 0));
}

static void testShadow() {
    const Config c;
    // level 0, ordinary puppet: volume close, circle beyond, hysteresis in between
    uint8_t m = SH_BLOB;
    m = nextShadow(m, sq(1000), 0, false, false, c); CHECK(m == SH_VOLUME);
    m = nextShadow(m, sq(2100), 0, false, false, c); CHECK(m == SH_VOLUME);   // (kept up to volOut)
    m = nextShadow(m, sq(2300), 0, false, false, c); CHECK(m == SH_BLOB);
    m = nextShadow(m, sq(2000), 0, false, false, c); CHECK(m == SH_BLOB);     // (not taken back before volIn)
    m = nextShadow(m, sq(1700), 0, false, false, c); CHECK(m == SH_VOLUME);
    int flips = 0;
    m = SH_BLOB;
    for (int i = 0; i < 2000; i++) {
        const float d = 2000.0f + 190.0f * std::sin(i * 0.9f);   // 1810..2190: the dead zone
        const uint8_t n = nextShadow(m, sq(d), 0, false, false, c);
        flips += n != m;
        m = n;
    }
    CHECK(flips == 0);   // (it started as the circle and never left the dead zone)
    // the nearest / watched / formation puppet keeps the volume much farther
    m = SH_VOLUME;
    m = nextShadow(m, sq(4400), 0, true, false, c); CHECK(m == SH_VOLUME);
    m = nextShadow(m, sq(4600), 0, true, false, c); CHECK(m == SH_BLOB);
    m = nextShadow(m, sq(4100), 0, true, false, c); CHECK(m == SH_BLOB);
    m = nextShadow(m, sq(3900), 0, true, false, c); CHECK(m == SH_VOLUME);
    // governor level 1 and up: the circle everywhere (nobody pays for the volume), the near ones included
    for (int level = 1; level <= 4; level++) {
        CHECK(nextShadow(SH_VOLUME, sq(100), level, false, false, c) == SH_BLOB);
        CHECK(nextShadow(SH_VOLUME, sq(100), level, true, false, c) == SH_BLOB);
        CHECK(nextShadow(SH_BLOB, sq(5000), level, false, false, c) == SH_BLOB);
    }
    // far: none at any level
    for (int level = 0; level <= 4; level++) CHECK(nextShadow(SH_VOLUME, sq(6000), level, true, true, c) == SH_OFF);
}

static void testTiny() {
    const Config c;
    // a 190-unit body on a 480 px screen with a 45 degree field of view: ~ 110 px at 1000 units, 6 px at ~ 18000
    CHECK(bodyPixels(190, 1000, 45, 480) > 100.0f && bodyPixels(190, 1000, 45, 480) < 120.0f);
    CHECK(bodyPixels(190, 18000, 45, 480) < 6.2f && bodyPixels(190, 18000, 45, 480) > 5.8f);
    CHECK(bodyPixels(190, 0.0f, 45, 480) > 1.0e6f);    // at the camera: never tiny
    CHECK(bodyPixels(190, 500, 0.0f, 480) > 1.0e6f);   // (a broken field of view: not tiny)
    CHECK(bodyPixels(190, 500, 45, 480) > bodyPixels(190, 900, 45, 480));
    CHECK(bodyPixels(190, 900, 60, 480) < bodyPixels(190, 900, 45, 480));   // (a wider view: smaller)
    CHECK(bodyPixels(190, 900, 45, 912) > bodyPixels(190, 900, 45, 480));   // (more pixels: bigger)
    bool tiny = false;
    tiny = nextTiny(tiny, 7.0f, c); CHECK(!tiny);
    tiny = nextTiny(tiny, 5.9f, c); CHECK(tiny);
    tiny = nextTiny(tiny, 7.9f, c); CHECK(tiny);    // (not back before tinyOut)
    tiny = nextTiny(tiny, 8.1f, c); CHECK(!tiny);
    int flips = 0;
    tiny = false;
    for (int i = 0; i < 2000; i++) {
        const bool n = nextTiny(tiny, 7.0f + 0.9f * std::sin(i * 0.8f), c);
        flips += n != tiny;
        tiny = n;
    }
    CHECK(flips == 0);   // 6.1 .. 7.9: inside the dead zone and it started not tiny
}

static void testNearest() {
    const Config c;
    float d2[SLOTS] = {-1, sq(1000), sq(1000), sq(3000)};
    int h = pickNearest(-1, d2, SLOTS, c);
    CHECK(h == 1);   // a tie goes to the lower slot
    // two equally near puppets with jitter: the holder keeps the title
    int changes = 0;
    for (int i = 0; i < 2000; i++) {
        d2[1] = sq(1000.0f + 40.0f * std::sin(i * 0.37f));
        d2[2] = sq(1000.0f + 40.0f * std::cos(i * 0.51f));
        const int n = pickNearest(h, d2, SLOTS, c);
        changes += n != h;
        h = n;
    }
    CHECK(changes == 0);
    // another puppet clearly closer (squared distance under bias * holder's): takes over
    d2[1] = sq(1000); d2[2] = sq(700);
    h = pickNearest(h, d2, SLOTS, c);
    CHECK(h == 2);   // 490000 < 0.7 * 1000000
    // not yet when only a little closer
    d2[1] = sq(750); d2[2] = sq(700);
    CHECK(pickNearest(2, d2, SLOTS, c) == 2);
    d2[1] = sq(500);
    CHECK(pickNearest(2, d2, SLOTS, c) == 1);   // 250000 < 0.7 * 490000
    // the holder leaves (candidate -1): the best one
    d2[2] = -1.0f; d2[3] = sq(2000); d2[1] = sq(4000);
    CHECK(pickNearest(2, d2, SLOTS, c) == 3);
    // nobody
    for (int s = 0; s < SLOTS; s++) d2[s] = -1.0f;
    CHECK(pickNearest(2, d2, SLOTS, c) == -1);
    CHECK(pickNearest(-1, d2, SLOTS, c) == -1);
    CHECK(pickNearest(77, d2, SLOTS, c) == -1);
}

static void testSpecial() {
    const Config c;
    // the watched puppet is nearest whatever its distance: band 0 and the near shadow rule
    CHECK(isSpecial(2, -1, 2, 0));
    CHECK(!isSpecial(1, -1, 2, 0));
    CHECK(isSpecial(1, 1, -1, 0));
    CHECK(isSpecial(3, -1, -1, 1u << 3));
    CHECK(!isSpecial(3, -1, -1, 1u << 2));
    CHECK(!isSpecial(-1, -1, -1, 0xFu));   // (no slot: nothing special)
    CHECK(!isSpecial(7, 7, 7, 0xFFu));
    CHECK(bandFor(3, sq(20000), true, c) == 0);
    CHECK(bandFor(0, sq(20000), false, c) == 3);
    // a watched puppet 12000 units away (a planet hop): every picture, volume shadow up to the larger distance
    CHECK(bandPeriod(bandFor(0, sq(12000), isSpecial(2, 0, 2, 0), c)) == 1);
    CHECK(nextShadow(SH_BLOB, sq(3000), 0, isSpecial(2, 0, 2, 0), false, c) == SH_VOLUME);
    CHECK(nextShadow(SH_BLOB, sq(3000), 0, isSpecial(2, 0, -1, 0), false, c) == SH_BLOB);
}

static void testMargin() {
    const Config c;
    CHECK(cullRadius(260.0f, 1.0f, c) > 260.0f);
    CHECK(cullRadius(260.0f, 2.0f, c) == 2.0f * cullRadius(260.0f, 1.0f, c));
    CHECK(cullRadius(260.0f, 0.0f, c) == 0.0f);
}

static void testOnePass() {
    const Config c;
    CHECK(onePassOk(false, 0.0f, c));              // no blend: always (at any distance, even at the camera)
    CHECK(onePassOk(false, sq(100000), c));
    CHECK(!onePassOk(true, sq(100), c));           // blending close up: both passes
    CHECK(!onePassOk(true, sq(2400), c));
    CHECK(onePassOk(true, sq(2600), c));           // blending far away: one
}

// optfix item 1: at the reference camera (45 degrees, 544 px, body 190 units at scale 1) the pixel tiers choose exactly what the distance
// tiers chose, away from the boundaries; elsewhere they follow the on-screen size.
static void testShadowPx() {
    const Config c;
    const float fovy = 45.0f, screenH = 544.0f;
    auto px = [&](float dist, float scale = 1.0f) { return bodyPixels(190.0f * scale, dist, fovy, screenH); };
    // 1. equivalence along a walk away and back, both tiers (hysteresis state carried), levels 0..4
    for (int special = 0; special < 2; special++) {
        for (int level = 0; level <= 4; level++) {
            uint8_t a = SH_BLOB, b = SH_BLOB;
            int diffs = 0, nearBoundary = 0;
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i <= 800; i++) {
                    const float d = pass == 0 ? 100.0f + i * 10.0f : 8100.0f - i * 10.0f;
                    a = nextShadow(a, sq(d), level, special != 0, false, c);
                    b = nextShadowPx(b, px(d), level, special != 0, false, c);
                    if (a != b) {
                        diffs++;
                        const float lo = special ? 3900.0f : 1750.0f, hi = special ? 4600.0f : 2300.0f;   // (the hysteresis band, +-3 %)
                        if (d > lo && d < hi) nearBoundary++;
                    }
                }
            }
            CHECK(diffs == nearBoundary);   // every difference sits inside the boundary band (rounding of the pixel constants)
            CHECK(diffs <= 40);
        }
    }
    // 2. far always off, the governor's levels 1+ the circle, whatever the size
    for (int level = 0; level <= 4; level++) CHECK(nextShadowPx(SH_VOLUME, 500.0f, level, true, true, c) == SH_OFF);
    for (int level = 1; level <= 4; level++) CHECK(nextShadowPx(SH_VOLUME, 500.0f, level, false, false, c) == SH_BLOB);
    // 3. it follows the screen: a giant (scale 2) keeps the volume twice as far; a small one (scale 0.4) loses it sooner; narrow lens farther
    uint8_t m = SH_BLOB;
    m = nextShadowPx(m, px(3000, 2.0f), 0, false, false, c); CHECK(m == SH_VOLUME);      // (a distance rule said circle beyond 2200)
    m = nextShadowPx(SH_BLOB, px(1000, 0.4f), 0, false, false, c); CHECK(m == SH_BLOB);   // (a distance rule said volume inside 1800)
    CHECK(nextShadowPx(SH_BLOB, bodyPixels(190, 3000, 25.0f, 544), 0, false, false, c) == SH_VOLUME);   // zoomed in
    CHECK(nextShadowPx(SH_BLOB, bodyPixels(190, 1500, 80.0f, 544), 0, false, false, c) == SH_BLOB);     // very wide
    // 4. hysteresis: no flip inside the dead zone
    m = SH_BLOB;
    int flips = 0;
    for (int i = 0; i < 1000; i++) {
        const float p = 63.0f + (i % 2 ? 2.0f : -2.0f);   // between Stay (57) and Enter (69)
        const uint8_t n = nextShadowPx(m, p, 0, false, false, c);
        if (n != m) flips++;
        m = n;
    }
    CHECK(flips == 0);
}

// fix2: the game passes MR::getScreenHeight() = the EFB height (456 on the Vita), not 544. The reach of the volume (the farthest distance at which a
// puppet that had no volume gets one, and the farthest at which it keeps it) must never be below the OLD distance tiers (1800 / 2200 ordinary,
// 4000 / 4500 nearest) at any field of view, and equal them (1 %) at the reference 45 degrees. Before the fix: 84 % of them.
static void testShadowReachAt456() {
    const Config c;
    const float H = 456.0f;
    for (float fov : {45.0f, 60.0f}) {
        for (int special = 0; special < 2; special++) {
            auto vol = [&](uint8_t prev, float d) {
                const float px = refPixels(bodyPixels(190.0f, d, fov, H), H, c);
                return nextShadowPx(prev, px, 0, special != 0, false, c, d * d) == SH_VOLUME;
            };
            float enter = 0, stay = 0;
            for (float d = 100.0f; d < 9000.0f; d += 1.0f) { if (vol(SH_BLOB, d)) enter = d; if (vol(SH_VOLUME, d)) stay = d; }
            const float oldIn = special ? 4000.0f : 1800.0f, oldOut = special ? 4500.0f : 2200.0f;
            CHECK(enter >= oldIn * 0.99f);
            CHECK(stay >= oldOut * 0.99f);
            if (fov == 45.0f) { CHECK(enter <= oldIn * 1.01f); CHECK(stay <= oldOut * 1.01f); }
        }
    }
}

// optfix item 2: the trim
static void testTrim() {
    const Config c;
    CHECK(c.trimPxIn == 40.0f && c.trimPxOut > c.trimPxIn);
    bool t = false;
    t = nextTrim(t, 41.0f, c); CHECK(!t);
    t = nextTrim(t, 39.0f, c); CHECK(t);
    t = nextTrim(t, 44.0f, c); CHECK(t);    // (back only at trimPxOut)
    t = nextTrim(t, 47.0f, c); CHECK(!t);
    int flips = 0;
    for (int i = 0; i < 1000; i++) {
        const bool n = nextTrim(t, 43.0f + (i % 2 ? 2.0f : -2.0f), c);
        if (n != t) flips++;
        t = n;
    }
    CHECK(flips <= 1);
    // 40 px of body at the reference camera is about 3100 units away: nearer puppets keep their face and hands
    CHECK(!nextTrim(false, bodyPixels(190, 2500, 45, 544), c));
    CHECK(nextTrim(false, bodyPixels(190, 3300, 45, 544), c));
}

int main() {
    testShadowPx();
    testShadowReachAt456();
    testTrim();
    testBands();
    testSkipAndStagger();
    testShadow();
    testTiny();
    testNearest();
    testSpecial();
    testMargin();
    testOnePass();
    if (sFail == 0) std::printf("lod_test: all passed\n");
    return sFail == 0 ? 0 : 1;
}
