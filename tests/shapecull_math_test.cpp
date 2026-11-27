// cpu24: shapecull_math::FastPlane against RefPlane (the original plane loop of shapecull::Out) over random boxes, matrices and projections,
// including zero / denormal / huge / infinite / NaN entries. Same plane, every time.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include "port/shapecull_math.h"
using namespace shapecull_math;
static std::mt19937_64 rng(24);
static float U(float lo, float hi) { return lo + (hi - lo) * (float)((rng() >> 11) * (1.0 / 9007199254740992.0)); }
static float Odd() {  // an awkward value now and then
    switch (rng() % 14) {
    case 0: return 0.0f; case 1: return -0.0f; case 2: return 1e-40f; case 3: return -1e-39f; case 4: return 3e38f; case 5: return -3e38f;
    case 6: return __builtin_inff(); case 7: return -__builtin_inff(); case 8: return __builtin_nanf(""); case 9: return 1e-30f;
    default: return U(-1e4f, 1e4f);
    }
}
int main() {
    long n = 0, outs = 0, odd = 0, bad = 0;
    for (long it = 0; it < 12000000; ++it) {
        const bool weird = rng() % 8 == 0;
        float m[3][4], c[3], h[3], ch[3], v[3], pj[7];
        const float sc = (rng() % 3 == 0) ? 1000.0f : (rng() % 2 ? 10.0f : 1.0f);
        for (auto& r : m) for (float& x : r) x = weird && rng() % 6 == 0 ? Odd() : U(-sc, sc);
        for (int i = 0; i < 3; ++i) {
            c[i] = weird && rng() % 6 == 0 ? Odd() : U(-sc * 50, sc * 50);
            h[i] = weird && rng() % 6 == 0 ? Odd() : U(0.0f, sc * 20);
            ch[i] = std::fabs(c[i]) + h[i];
        }
        pj[0] = 0.0f;
        pj[1] = U(0.3f, 3.0f); pj[2] = U(-0.5f, 0.5f); pj[3] = U(0.3f, 4.0f); pj[4] = U(-0.5f, 0.5f); pj[5] = U(-2, 0); pj[6] = U(-2, 0);
        if (weird && rng() % 4 == 0) pj[1 + rng() % 4] = Odd();
        const float g = 1.0f + 0.01f * (float)(rng() % 51);
        for (int i = 0; i < 3; ++i) {
            v[i] = m[i][0] * c[0] + m[i][1] * c[1] + m[i][2] * c[2] + m[i][3];
            if ((fpb::Bits(v[i]) & 0x7FFFFFFFu) > 0x7F800000u) goto skip;  // (the caller returns: a NaN)
        }
        {
            const int a = RefPlane(m, c, h, v, pj, g), b = FastPlane(m, c, h, ch, v, pj, g);
            ++n; outs += a >= 0; odd += weird;
            if (a != b) { if (++bad < 10) { printf("MISMATCH ref %d fast %d (it %ld) weird %d\n", a, b, it, (int)weird); if (bad < 4) { for (int i=0;i<3;++i) printf(" m %g %g %g %g c %g h %g v %g\n", m[i][0],m[i][1],m[i][2],m[i][3],c[i],h[i],v[i]); printf(" pj %g %g %g %g g %g\n", pj[1],pj[2],pj[3],pj[4],g);} } }
        }
    skip:;
    }
    printf("%ld boxes (%ld outside, %ld with odd values), %ld mismatches\n", n, outs, odd, bad);
    return bad != 0;
}
