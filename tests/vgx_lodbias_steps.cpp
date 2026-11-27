// Host test of vgx_lodbias.h: every sum that reaches glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, n) stays in vitaGL's -31 .. +32, -32 included.
// vitaGL's two lines (textures.c glTexParameteri, utils/gxm_utils.h vglSetTexLodBias), modelled below, are the reason:
//   tex->lod_bias = (uint32_t)(n + GL_MAX_TEXTURE_LOD_BIAS /* 31 */);   control_words[0] = control_words[0] & 0xF81FFFFF | (bias << 21);
// tools/test_vgx_lodbias_steps.sh builds and runs it (no GL, no EGL, no game data).
#include "vgx_lodbias.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace {
int g_failed = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAILED: %s\n", what); }
}
constexpr uint32_t kFieldMask = 0x07E00000u; // bits 21..26: the 6-bit bias field; 0xF81FFFFF keeps the rest of the word

// vitaGL's setter on a control word (the library's text, as built in the reader archive vp30).
uint32_t SetLodBias(uint32_t word, long n) {
    const uint32_t bias = static_cast<uint32_t>(static_cast<int32_t>(n) + 31); // (glTexParameteri, GL_MAX_TEXTURE_LOD_BIAS = 31)
    return (word & 0xF81FFFFFu) | (bias << 21);
}
// the bias the hardware reads back from the word, in eighths of a level, and whether anything outside the field was touched
long FieldSteps(uint32_t word) { return static_cast<long>((word & kFieldMask) >> 21) - 31; }
bool OutsideTouched(uint32_t before, uint32_t after) { return ((before ^ after) & ~kFieldMask) != 0; }

// the old sum (r110 .. 9 Nov: clamp to -32) and the unclamped one, for what the clamp is against
long Unclamped(float sx, long pct, long extra) {
    const float levels = sx > 0.0f ? std::log2(1.5f / sx) : 0.0f;
    return -std::lround(levels * 8.0f * static_cast<float>(pct) / 100.0f) - extra;
}
} // namespace

int main() {
    using namespace vgx::lodbias;
    // ---- the library's range, from its own two lines
    const uint32_t word = 0xA5A5A5A5u;
    for (long n = -31; n <= 32; ++n) {
        const uint32_t w = SetLodBias(word, n);
        Check(FieldSteps(w) == n && !OutsideTouched(word, w), "every n in -31 .. +32 lands in the 6-bit field and touches nothing else");
    }
    Check(FieldSteps(SetLodBias(word, -32)) == 32 && OutsideTouched(word, SetLodBias(word, -32)),
          "-32 reads +32 steps (the opposite sign) and sets bits 27-31 of the word: the wrap this header exists for");
    Check(OutsideTouched(word, SetLodBias(word, 33)), "+33 spills into bit 27 too");
    Check(OutsideTouched(word, SetLodBias(word, -40)), "-40 spills");
    // ---- ClampSteps
    Check(ClampSteps(-32) == -31 && ClampSteps(-31) == -31 && ClampSteps(-30) == -30 && ClampSteps(0) == 0 && ClampSteps(32) == 32 && ClampSteps(33) == 32,
          "ClampSteps keeps -31 .. +32");
    Check(ClampSteps(-1000000) == -31 && ClampSteps(1000000) == 32, "ClampSteps, far out");
    // ---- RungSteps: the console's numbers (tools/test_vgx_sharp24_gl.sh asserts the same on the real back end)
    const float sx960 = 960.0f / 640.0f, sx720 = 720.0f / 640.0f, sx608 = 608.0f / 640.0f, sx480 = 480.0f / 640.0f, sx536 = 536.0f / 640.0f;
    Check(RungSteps(sx960, 100, 0) == 0 && RungSteps(sx960, 0, 0) == 0, "top rung: no share of the rung's, no bias");
    Check(RungSteps(sx720, 60, 0) == -2 && RungSteps(sx608, 60, 0) == -3, "GXV_RUNG_LODBIAS=60: 720x404 -2, 608x348 -3");
    Check(RungSteps(sx720, 100, 0) == -3 && RungSteps(sx608, 100, 0) == -5, "GXV_RUNG_LODBIAS=100 (the shipped default): 720x404 -3, 608x348 -5");
    Check(RungSteps(sx720, 100, 2) == -5 && RungSteps(sx960, 100, 2) == -2, "GXV_RUNG_LODBIAS_EXTRA=2 on top: 720 -5, 960 -2 (the top rung gets it too)");
    Check(RungSteps(sx960, 0, 4) == -4, "EXTRA alone at 960x544: -4");
    Check(RungSteps(sx608, 200, 24) == -31, "RUNG_LODBIAS=200 + EXTRA=24 at 608x348: -31 (the sum is -35)");
    Check(Unclamped(sx608, 200, 24) == -35, "...which the unclamped sum would have given");
    // the first combination that reached -32 before the clamp was -31: the old clamp to -32 let this through
    Check(Unclamped(sx480, 100, 24) == -32 && RungSteps(sx480, 100, 24) == -31, "480x340 (the emergency rung without wide rungs), RUNG_LODBIAS=100 + EXTRA=24: sum -32 -> -31");
    Check(Unclamped(sx536, 120, 24) == -32 && RungSteps(sx536, 120, 24) == -31, "536x304 (the wide ladder's emergency rung), RUNG_LODBIAS=120 + EXTRA=24: sum -32 -> -31");
    // odd inputs: no UB, no wrap
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    Check(RungSteps(0.0f, 200, 24) == -24 && RungSteps(-1.0f, 200, 24) == -24, "no rung width: no share, EXTRA only");
    Check(RungSteps(nan, 100, 8) == -8, "a rung width that is not a number: EXTRA only");
    Check(RungSteps(inf, 200, 0) >= kMinSteps && RungSteps(inf, 200, 0) <= 0, "an infinite rung width stays in range");
    Check(RungSteps(1e-30f, 200, 24) == -31, "a vanishing rung width: -31, not an out-of-range lround");
    Check(RungSteps(100.0f, 200, 0) == 0, "a rung wider than the top one: 0 (the rung never blurs)");
    Check(RungSteps(1.6f, 100, 8) == -7, "a rung a little wider than the top one (1024 wide) gives back a step of EXTRA: -8 + 1, as before the header existed");
    // ---- every setting the environment can make: GXV_RUNG_LODBIAS 0..200, _EXTRA / _EXTRA_AF 0..24, every width a rung has (and some it has not)
    long lo = 0, loUnclamped = 0;
    bool allIn = true;
    for (int w = 320; w <= 1200; w += 4)
        for (long pct = 0; pct <= 200; pct += 5)
            for (long extra = 0; extra <= 24; ++extra) {
                const long r = RungSteps(static_cast<float>(w) / 640.0f, pct, extra);
                if (r < kMinSteps || r > 0) allIn = false;
                lo = std::min(lo, r);
                loUnclamped = std::min(loUnclamped, Unclamped(static_cast<float>(w) / 640.0f, pct, extra));
            }
    Check(allIn && lo == -31, "every rung width x RUNG_LODBIAS 0..200 x EXTRA 0..24: -31 .. 0, and -31 is reached");
    Check(loUnclamped <= -32, "(the unclamped sum does go below -31 in that grid: the clamp is not idle)");
    // ---- against the expression the back end had before the header (RungLodBiasSteps of 10 Nov: clamp to -31 .. 0 of the same float sum): identical where that was defined
    bool same = true;
    for (int w = 320; w <= 1400; w += 2)
        for (long pct = 0; pct <= 200; pct += 3)
            for (long extra = 0; extra <= 24; extra += 2) {
                const float sx = static_cast<float>(w) / 640.0f;
                const long old = std::clamp(Unclamped(sx, pct, extra), -31L, 0L);
                if (old != RungSteps(sx, pct, extra)) same = false;
            }
    Check(same, "RungSteps equals the 10 Nov inline expression over widths 320 .. 1400, RUNG_LODBIAS 0 .. 200, EXTRA 0 .. 24");
    // ---- the player's share
    Check(PlayerSteps(0) == 0 && PlayerSteps(-100) == -8 && PlayerSteps(-50) == -4 && PlayerSteps(100) == 8 && PlayerSteps(-300) == -24, "GXV_PLAYER_LODBIAS: -100 is one level sharper (-8 steps)");
    Check(PlayerSteps(-100000) == -24 && PlayerSteps(100000) == 8, "GXV_PLAYER_LODBIAS beyond -300 .. +100 is cut to it");
    Check(PlayerSum(-3, -8) == -11 && PlayerSum(0, -8) == -8 && PlayerSum(-5, 8) == 3, "player sum: plain addition inside the range");
    Check(PlayerSum(-31, -24) == -31 && PlayerSum(-31, -1) == -31 && PlayerSum(-30, -8) == -31, "player sum: never below -31 (-32 and below wrap)");
    Check(PlayerSum(0, 40) == 32 && PlayerSum(0, 33) == 32, "player sum: never above +32");
    bool sumIn = true;
    long sumLo = 0, sumHi = 0;
    for (long rung = -31; rung <= 0; ++rung)
        for (long hundredths = -400; hundredths <= 200; hundredths += 5) {
            const long v = PlayerSum(static_cast<int8_t>(rung), PlayerSteps(hundredths));
            sumLo = std::min(sumLo, v);
            sumHi = std::max(sumHi, v);
            if (v < kMinSteps || v > kMaxSteps) sumIn = false;
        }
    Check(sumIn && sumLo == -31 && sumHi == 8, "rung -31 .. 0 + player -400 .. +200 hundredths: -31 .. +8, never -32");
    // ---- and the word the library makes from the extremes is clean
    Check(!OutsideTouched(word, SetLodBias(word, PlayerSum(-31, -24))) && !OutsideTouched(word, SetLodBias(word, RungSteps(sx608, 200, 24))),
          "the extremes of both sums leave every other field of the control word alone");
    std::printf(g_failed ? "%d of %d checks FAILED\n" : "all %d checks passed\n", g_failed ? g_failed : g_checks, g_checks);
    return g_failed ? 1 : 0;
}
