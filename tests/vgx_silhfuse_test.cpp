// silhfuse26: host test of GXV_SILH_FUSE's arithmetic and pairing (platform/vgx/src/vgx_silhfuse.h). Exhaustive over 8-bit inputs. No GL.
#include "../platform/vgx/src/vgx_silhfuse.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace vgx::silhfuse;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; if (g_fail < 30) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

// Independent reference: exact rational arithmetic, round-to-nearest of each term, sum, clamp.
static int Ref(int sKind, int dKind, bool rev, int s, int d, int a) {
    auto f = [&](int k) { return k == K_ZERO ? 0 : k == K_ONE ? 255 : k == K_SA ? a : k == K_ISA ? 255 - a : k == K_SC ? s : 255 - s; };
    long ts = (long)s * f(sKind), td = (long)d * f(dKind);
    long rts = (2 * ts + 255) / 510, rtd = (2 * td + 255) / 510;
    long r = rev ? rtd - rts : rtd + rts;
    return (int)(r < 0 ? 0 : r > 255 ? 255 : r);
}
// The GPU might add first and round once; for pairs we fuse that must be identical.
static int RefSumRound(int sKind, int dKind, bool rev, int s, int d, int a) {
    auto f = [&](int k) { return k == K_ZERO ? 0 : k == K_ONE ? 255 : k == K_SA ? a : k == K_ISA ? 255 - a : k == K_SC ? s : 255 - s; };
    long x = rev ? (long)d * f(dKind) - (long)s * f(sKind) : (long)d * f(dKind) + (long)s * f(sKind);
    long r = x >= 0 ? (2 * x + 255) / 510 : -((2 * -x + 255) / 510);
    return (int)(r < 0 ? 0 : r > 255 ? 255 : r);
}

int main() {
    long pairsFused = 0, pairsRefused = 0, cases = 0;
    for (int sk = 0; sk < K_COUNT; ++sk)
        for (int dk = 0; dk < K_COUNT; ++dk)
            for (int rev = 0; rev < 2; ++rev)
                for (int srcZero = 0; srcZero < 2; ++srcZero) {
                    Pair p; p.blend = 1; p.eq = (uint8_t)rev; p.colorWrite = 1; p.alphaWrite = 0; p.srcZero = srcZero != 0;
                    // front end BF numbers from kinds
                    static const uint8_t bf[K_COUNT] = {0, 1, 4, 5, 2, 3};
                    p.sf = bf[sk]; p.df = bf[dk];
                    uint8_t code, am;
                    const int why = Classify(p, code, am);
                    const int sEff = srcZero ? K_ZERO : sk;
                    const int nr = (KindExact((uint8_t)sEff) ? 0 : 1) + (KindExact((uint8_t)dk) ? 0 : 1);
                    if (nr > 1) { CHECK(why == RJ_TWO_ROUNDINGS); ++pairsRefused; continue; }
                    CHECK(why == RJ_OK); CHECK(CodeOn(code)); CHECK(CodeS(code) == sEff); CHECK(CodeD(code) == dk); CHECK(CodeRev(code) == (rev != 0));
                    ++pairsFused;
                    const int sMax = srcZero ? 1 : 256; // a zero source: s is 0 for every fragment
                    for (int s = 0; s < sMax; ++s)
                        for (int d = 0; d < 256; ++d)
                            for (int a = 0; a < 256; ++a) {
                                const int e = Ref(sEff, dk, rev != 0, s, d, a);
                                ++cases;
                                CHECK(BlendRef(code, s, d, a) == e);
                                CHECK(RefSumRound(sEff, dk, rev != 0, s, d, a) == e); // add-then-round-once gives the same: why one rounded term is safe
                                const float r = ShaderChannel(code, (float)s, (float)d, (float)a);
                                CHECK((int)r == e && r == (float)(int)r);
                                CHECK(OutputByte(r, 0) == e);
                                CHECK(OutputByte(r, 1) == e);
                            }
                }
    // The refused class really is the one where the orders differ (documents why they are refused): find one counter-example.
    {
        bool found = false;
        for (int s = 0; s < 256 && !found; ++s) for (int d = 0; d < 256 && !found; ++d) for (int a = 0; a < 256 && !found; ++a)
            if (Ref(K_SA, K_ISA, false, s, d, a) != RefSumRound(K_SA, K_ISA, false, s, d, a)) found = true;
        CHECK(found);
    }
    // Classify reasons
    {
        uint8_t c, am; Pair p; p.blend = 0; CHECK(Classify(p, c, am) == RJ_NOBLEND && c == 0);
        p = Pair(); p.blend = 1; p.colorWrite = 0; CHECK(Classify(p, c, am) == RJ_NOCOLOUR);
        p = Pair(); p.blend = 1; p.eq = 2; CHECK(Classify(p, c, am) == RJ_EQUATION);
        p = Pair(); p.blend = 1; p.sf = 4; p.df = 6; CHECK(Classify(p, c, am) == RJ_DSTFACTOR);
        p = Pair(); p.blend = 1; p.sf = 8; p.df = 0; CHECK(Classify(p, c, am) == RJ_DSTFACTOR);
        p = Pair(); p.blend = 1; p.sf = 4; p.df = 5; p.alphaWrite = 1; p.dstAlphaZero = 0; CHECK(Classify(p, c, am) == RJ_ALPHAMODE);
        p = Pair(); p.blend = 1; p.sf = 4; p.df = 5; p.alphaWrite = 1; p.dstAlphaZero = 1; p.srcZero = true; CHECK(Classify(p, c, am) == RJ_OK && am == A_ZERO && CodeS(c) == K_ZERO && CodeD(c) == K_ISA);
        p.dstAlphaZero = 2; CHECK(Classify(p, c, am) == RJ_OK && am == A_SRC);
        p.alphaWrite = 0; CHECK(Classify(p, c, am) == RJ_OK && am == A_KEEP);
        p.srcZero = false; CHECK(Classify(p, c, am) == RJ_TWO_ROUNDINGS); // SA / ISA with a nonzero source: refused
    }
    // The game's pair: SRC_ALPHA / INV_SRC_ALPHA, black source, alpha 0x80: result = round(d * 127 / 255).
    {
        Pair p; p.blend = 1; p.sf = 4; p.df = 5; p.alphaWrite = 1; p.dstAlphaZero = 1; p.srcZero = true;
        uint8_t c, am; CHECK(Classify(p, c, am) == RJ_OK);
        for (int d = 0; d < 256; ++d) CHECK(BlendRef(c, 0, d, 0x80) == (d * 127 + 127) / 255);
    }
    // Pairing logic
    {
        std::vector<uint8_t> v = {C_OTHER, C_HARMLESS, C_HARMLESS, C_DRAW, C_DRAW};
        auto cl = [&](long j) { return v[(size_t)j]; };
        CHECK(NextDraw(0, 5, cl) == 3);
        CHECK(NextDraw(3, 5, cl) == 4);
        CHECK(NextDraw(4, 5, cl) == -1);
        v = {C_DRAW, C_HARMLESS, C_OTHER, C_DRAW};
        CHECK(NextDraw(0, 4, cl) == -1); // another command intervenes
        v = {C_DRAW, C_HARMLESS, C_HARMLESS};
        CHECK(NextDraw(0, 3, cl) == -1); // nothing follows
    }
    // Census
    {
        Census c; c.Refuse(RJ_TWO_ROUNDINGS); c.Refuse(Census::W_GEOM); c.Refuse(-1); c.Refuse(999);
        CHECK(c.why[RJ_TWO_ROUNDINGS] == 1 && c.why[Census::W_GEOM] == 1);
        for (int w = 0; w < Census::W_COUNT; ++w) CHECK(Census::WhyName(w)[0] != '?');
    }
    std::printf("vgx_silhfuse_test: %ld pair kinds fused, %ld refused, %ld exhaustive cases, %d failures\n", pairsFused, pairsRefused, cases, g_fail);
    return g_fail ? 1 : 0;
}
