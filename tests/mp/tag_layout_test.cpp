// Host tests for the name tag overlap resolution (owner: tags): 2 and 3 overlapping tags are stacked clear, the nearest keeps its
// place, no jitter under small camera motion (no side flips, bounded frame-to-frame change), stability (a still scene converges
// and stays put; release hysteresis), inactive/hostile input, and the glide.
//   tests/mp/tag_layout_build.sh
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "Game/MP/MpTagLayoutLogic.hpp"
using namespace MpTagLayout;
static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static const float H = 28.0f, MINY = 20.0f, MAXY = 430.0f;

static Item mk(float x, float y, float d2, float w = 60.0f, bool act = true) { Item it; it.active = act; it.x = x; it.y = y; it.w = w; it.d2 = d2; return it; }
static void run(State& s, const Item* it, int n, float* y, int frames = 60) { for (int f = 0; f < frames; f++) resolve(s, it, n, H, MINY, MAXY, y); }
static bool overlap(const Item& a, float ya, const Item& b, float yb) { return std::fabs(a.x - b.x) < 0.5f * (a.w + b.w) && std::fabs(ya - yb) < H - 0.01f; }

static void testTwo() {
    State s; s.reset();
    Item it[2] = {mk(300, 200, 100.0f), mk(310, 206, 200.0f)};   // 0 is nearer
    float y[2];
    run(s, it, 2, y);
    CHECK(y[0] == 200.0f);                 // nearest stays
    CHECK(!overlap(it[0], y[0], it[1], y[1]));
    CHECK(y[1] == 200.0f + H);             // it was below: stays on that side, minimal push
    // far apart horizontally: untouched
    it[1].x = 500;
    run(s, it, 2, y);
    CHECK(y[0] == 200.0f && std::fabs(y[1] - 206.0f) < 0.01f);
}
static void testThree() {
    State s; s.reset();
    Item it[3] = {mk(300, 200, 300.0f), mk(305, 200, 100.0f), mk(295, 201, 200.0f)};
    float y[3];
    run(s, it, 3, y);
    CHECK(y[1] == 200.0f);   // nearest
    for (int a = 0; a < 3; a++) for (int b = a + 1; b < 3; b++) CHECK(!overlap(it[a], y[a], it[b], y[b]));
    // slot gap: distinct y for all
    CHECK(y[0] != y[2]);
}
static void testNearestOnTop() {
    // equal-ish distances: the unpushed one keeps the role when the order would swap
    State s; s.reset();
    Item it[2] = {mk(300, 200, 100.0f), mk(300, 200, 101.0f)};
    float y[2];
    run(s, it, 2, y);
    const float y1 = y[1];
    CHECK(y[0] == 200.0f && y1 != 200.0f);
    it[0].d2 = 103.0f; it[1].d2 = 101.0f;   // 1 is now slightly nearer, but 0 is the unpushed holder
    run(s, it, 2, y, 30);
    CHECK(y[0] == 200.0f && y[1] == y1);
    it[0].d2 = 400.0f;                      // clearly farther: roles swap
    run(s, it, 2, y, 60);
    CHECK(y[1] == 200.0f && y[0] != 200.0f);
}
static void testJitter() {
    // two tags drifting with small camera motion: no side flips, smooth frame to frame, never overlapping at rest
    State s; s.reset();
    float y[3], prev[3] = {0, 0, 0};
    float maxStep = 0;
    int flips = 0, lastSide = 0;
    for (int f = 0; f < 600; f++) {
        const float t = (float)f * 0.05f;
        const float sway = 3.0f * std::sin(t);        // camera sway of a few pixels
        Item it[3] = {mk(300 + sway, 200 + 2.0f * std::cos(t * 1.3f), 100.0f), mk(312 + sway, 200 + 5.0f * std::sin(t * 0.7f), 150.0f),
                      mk(500, 150, 90.0f)};
        resolve(s, it, 3, H, MINY, MAXY, y);
        if (f > 5) {
            for (int i = 0; i < 3; i++) { const float d = std::fabs(y[i] - prev[i]); if (d > maxStep) maxStep = d; }
            const int side = y[1] > y[0] ? 1 : -1;
            if (lastSide != 0 && side != lastSide) flips++;
            lastSide = side;
        }
        for (int i = 0; i < 3; i++) prev[i] = y[i];
        CHECK(y[2] == 150.0f);   // the far-away one never moves
    }
    CHECK(flips == 0);
    CHECK(maxStep < 8.0f);   // the input itself moves up to ~0.5 px/picture; smoothing keeps the output continuous
}
static void testRelease() {
    // hysteresis: separating by exactly the plate height does not chatter
    State s; s.reset();
    float y[2];
    int changes = 0;
    int last = -1;
    for (int f = 0; f < 400; f++) {
        const float dy = (H + 2.0f) + 3.0f * std::sin((float)f * 0.3f);   // oscillates around the overlap boundary +-3px
        Item it[2] = {mk(300, 200, 100.0f), mk(300, 200 + dy, 200.0f)};
        resolve(s, it, 2, H, MINY, MAXY, y);
        const int moved = (s.pushed[1] ? 1 : 0);
        if (last != -1 && moved != last) changes++;
        last = moved;
    }
    CHECK(changes <= 4 * 400 / 21);   // sanity bound far below once-per-frame chatter (would be ~120+)
    CHECK(changes < 60);
}
static void testStable() {
    State s; s.reset();
    Item it[3] = {mk(300, 200, 100.0f), mk(300, 200, 120.0f), mk(300, 200, 140.0f)};
    float y[3], z[3];
    run(s, it, 3, y, 80);
    for (int f = 0; f < 100; f++) { resolve(s, it, 3, H, MINY, MAXY, z); for (int i = 0; i < 3; i++) CHECK(z[i] == y[i]); }
    CHECK(y[0] == 200.0f && std::fabs(y[1] - 200.0f) >= H - 0.01f && std::fabs(y[2] - 200.0f) >= H - 0.01f && std::fabs(y[1] - y[2]) >= H - 0.01f);
    // glide: a tag that gets pushed while visible moves over several pictures, not in one
    State g; g.reset();
    Item a[2] = {mk(300, 200, 100.0f), mk(300, 300, 200.0f)};
    float q[2];
    resolve(g, a, 2, H, MINY, MAXY, q); resolve(g, a, 2, H, MINY, MAXY, q);
    a[1].y = 205.0f;
    resolve(g, a, 2, H, MINY, MAXY, q);
    CHECK(q[1] > 205.0f && q[1] < 200.0f + H - 1.0f);   // first picture: only part of the way to its place
    run(g, a, 2, q, 40);
    CHECK(std::fabs(q[1] - (200.0f + H)) < 0.01f || std::fabs(q[1] - (200.0f - H)) < 0.01f);
}
static void testBounds() {
    // near the top the stack goes down instead
    State s; s.reset();
    Item it[2] = {mk(300, 25, 100.0f), mk(300, 25, 200.0f)};
    float y[2];
    run(s, it, 2, y);
    CHECK(y[1] >= MINY && y[1] <= MAXY && y[1] == 25.0f + H);
    // inactive tags do not push, hostile values are ignored, n clamps
    State t; t.reset();
    Item b[5] = {mk(300, 200, 100.0f, 60.0f, false), mk(300, 200, 200.0f), mk(NAN, 200, 1.0f), mk(300, INFINITY, 1.0f), mk(300, 200, 5.0f)};
    float yy[5];
    resolve(t, b, 5, H, MINY, MAXY, yy);   // 5 > kMax: the fifth is untouched
    CHECK(yy[1] == 200.0f);
    resolve(t, b, 0, H, MINY, MAXY, yy);
    resolve(t, b, -3, H, MINY, MAXY, yy);
    // a fading tag keeps its place (frozen offset) and does not push others
    State u; u.reset();
    Item c[2] = {mk(300, 200, 100.0f), mk(300, 200, 200.0f)};
    float z[2];
    run(u, c, 2, z);
    const float off = z[1];
    c[0].active = false;
    resolve(u, c, 2, H, MINY, MAXY, z);
    CHECK(z[1] == off || std::fabs(z[1] - 200.0f) < H + 0.01f);
}
int main() {
    testTwo(); testThree(); testNearestOnTop(); testJitter(); testRelease(); testStable(); testBounds();
    printf("tag_layout_test: %d checks, %d failures\n", gChecks, gFails);
    return gFails != 0;
}
