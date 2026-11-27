// dm65 (22 Nov 2026): GXV_SHADOW_STENCIL's stencil operations together with the player's own-pixel mark (port/shadow_stencil.h), through a model of the GL stencil unit.
// The ideal: a counter c (0..127: INCR +1, DECR -1 clamped at 0, SET = 64, ZERO = 0) and the mark m (a separate flag, set by the player's draws, never changed by the volumes).
// The stencil's low bits must be c and bit 7 must be m after every operation of every sequence the front end takes: at most 63 layers counted (INCR), at most 63 DECR draws after
// a silhouette, and the fill's test (!= 0 on the counter) is "c != 0" whatever the mark says. tools/test_shstencil.sh builds it with each mutant (SHSTENCIL_MUTANT 1: dm58's
// layout, 2: DECR without the counter test), which must FAIL.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "port/shadow_stencil.h"

using namespace port::shstencil;
namespace {
int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("CHECK FAILED %s:%d: %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

enum { ZERO = 1, INCR = 2, DECR = 3, SET = 4, TEST = 5 };
uint8_t Op(uint8_t v, int op) { return Apply(ForOp(op), v, true); }
bool Fill(uint8_t v) { bool p = false; Apply(ForOp(TEST), v, true, &p); return p; }
uint8_t Mark(uint8_t v) { Setting s; s.func = kAlways; s.ref = kOwnBit; s.funcMask = kOwnBit; s.depthPass = kReplace; s.writeMask = kOwnBit; return Apply(s, v, true); }   // the player's draws
bool IsMarked(uint8_t v) { return (v & kOwnBit) != 0; }
unsigned Counter(uint8_t v) { return v & kCountMask; }

struct Ideal { int c = 0; bool m = false; };
void Step(Ideal& i, int op) {
    if (op == ZERO) i.c = 0;
    else if (op == INCR) i.c = i.c < 127 ? i.c + 1 : 127;
    else if (op == DECR) i.c = i.c > 0 ? i.c - 1 : 0;
    else if (op == SET) i.c = 64;
}
void Agree(uint8_t v, const Ideal& i, const char* what, int step) {
    CHECK(static_cast<int>(Counter(v)) == i.c, "%s step %d: counter %u, ideal %d (stencil 0x%02x)", what, step, Counter(v), i.c, v);
    CHECK(IsMarked(v) == i.m, "%s step %d: mark %d, ideal %d", what, step, IsMarked(v) ? 1 : 0, i.m ? 1 : 0);
    CHECK(Fill(v) == (i.c != 0), "%s step %d: fill %d, ideal %d", what, step, Fill(v) ? 1 : 0, i.c != 0 ? 1 : 0);
}
}  // namespace

int main() {
    for (int marked = 0; marked < 2; ++marked) {
        const char* tag = marked ? "marked" : "plain";
        // (1) DECR at 0 changes nothing, the mark stays; the fill does not see the mark
        {
            uint8_t v = 0; Ideal i;
            if (marked) { v = Mark(v); i.m = true; }
            Agree(v, i, tag, 0);
            for (int k = 0; k < 200; ++k) { v = Op(v, DECR); Step(i, DECR); }
            Agree(v, i, tag, 200);
            CHECK(Counter(v) == 0, "%s: 200 DECR at 0 gave counter %u (stencil 0x%02x)", tag, Counter(v), v);
        }
        // (2) layers: n INCR then n DECR back to 0 exactly, for n up to the front end's bound
        for (int n = 1; n <= kMaxLayers; ++n) {
            uint8_t v = marked ? Mark(0) : 0; Ideal i; i.m = marked != 0;
            for (int k = 0; k < n; ++k) { v = Op(v, INCR); Step(i, INCR); Agree(v, i, tag, k); }
            for (int k = 0; k < n; ++k) { v = Op(v, DECR); Step(i, DECR); Agree(v, i, tag, n + k); }
            CHECK(Counter(v) == 0 && !Fill(v), "%s: %d layers up and down: 0x%02x", tag, n, v);
        }
        // (3) a window with a silhouette: SET, then up to 63 DECR draws (and layers on top): never 0 before the last, the fill sees it
        {
            uint8_t v = marked ? Mark(0) : 0; Ideal i; i.m = marked != 0;
            v = Op(v, SET); Step(i, SET); Agree(v, i, tag, 0);
            for (int k = 0; k < kMaxLayers; ++k) { v = Op(v, DECR); Step(i, DECR); Agree(v, i, tag, k); CHECK(Fill(v), "%s: silhouette gone after %d subtractions (0x%02x)", tag, k + 1, v); }
            for (int k = 0; k < kMaxLayers; ++k) { v = Op(v, INCR); Step(i, INCR); Agree(v, i, tag, k); }
        }
        // (4) ZERO / SET keep the mark; the player's mark keeps the counter
        {
            uint8_t v = marked ? Mark(0) : 0;
            v = Op(v, INCR); v = Op(v, INCR); v = Op(v, INCR);
            const uint8_t before = v;
            const uint8_t m = Mark(v);
            CHECK(Counter(m) == Counter(before), "mark changed the counter: 0x%02x -> 0x%02x", before, m);
            CHECK(IsMarked(m), "mark not set");
            CHECK(IsMarked(Op(m, ZERO)) && Counter(Op(m, ZERO)) == 0, "ZERO: 0x%02x", Op(m, ZERO));
            CHECK(IsMarked(Op(m, SET)) && Counter(Op(m, SET)) == kSilhouette, "SET: 0x%02x", Op(m, SET));
            CHECK(!Fill(Mark(0)), "the fill drew a marked pixel with nothing counted");
        }
    }
    // (5) random windows the front end takes: ideal vs stencil, with and without the mark placed anywhere in the sequence
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int run = 0; run < 20000; ++run) {
        uint8_t v = 0; Ideal i;
        int back = 0, subAfterSet = 0; bool sil = false;
        const int len = 1 + static_cast<int>(rnd() % 120), markAt = static_cast<int>(rnd() % (len + 2));
        for (int s = 0; s < len; ++s) {
            if (s == markAt) { v = Mark(v); i.m = true; }
            int op = ZERO + static_cast<int>(rnd() % 4);   // ZERO INCR DECR SET
            if (op == INCR && back >= (sil ? kMaxLayers : kCountMax)) op = DECR;
            if (op == DECR && sil && subAfterSet >= kMaxLayers) op = INCR;
            if (op == INCR && back >= (sil ? kMaxLayers : kCountMax)) op = ZERO;
            if (op == ZERO) { back = 0; sil = false; subAfterSet = 0; }
            if (op == INCR) ++back;
            if (op == SET) { sil = true; subAfterSet = 0; }
            if (op == DECR && sil) ++subAfterSet;
            v = Op(v, op); Step(i, op);
            Agree(v, i, "random", s);
            if (g_fail > 40) return std::printf("shstencil: too many failures\n"), 1;
        }
    }
    // (6) the bit layout itself: the mark is outside the user mask, the silhouette is inside the counter, and survives the bound of subtractions
    CHECK((kOwnBit & kUserMask) == 0, "the mark is in the volumes' write mask");
    CHECK((kSilhouette & kCountMask) == kSilhouette, "the silhouette is outside the counter");
    CHECK(kSilhouette > kMaxLayers, "a silhouette (%d) does not survive %d subtractions", kSilhouette, kMaxLayers);
    CHECK(static_cast<int>(kSilhouette) + kMaxLayers <= kCountMax, "silhouette + layers leave the counter");
    std::printf("shstencil: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
