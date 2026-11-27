// GXV_LODBIAS_SAMPLER (lodmask37): the pure decision logic of platform/vgx/src/vgx_lodsampler.h. No GL, no game data.
#include "vgx_cmd.h"
#include "vgx_lodsampler.h"
#include <cstdio>
#include <cstdlib>

namespace {
using namespace vgx;
using namespace vgx::lodsampler;
unsigned failures = 0, checks = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", name); ++failures; }
}
Unit One(unsigned m, int b0, const Cfg& c = Cfg{}) { // the draw reads texmap m by one stage with bias b0 (1/32 levels), nothing else
    int b[8] = {};
    b[m] = b0;
    return Classify(m, b, static_cast<uint8_t>(1u << m), 0, c);
}
}

int main() {
    // TexBind travels as words in the front end's split (vgx_fesplit.cpp): 12 bytes, the new fields start at 8 and are zero by default.
    static_assert(sizeof(TexBind) == 12, "TexBind is 12 bytes");
    TexBind tb;
    Check(tb.lodSteps == 0 && tb.lodWhy == 0 && tb.lodPad[0] == 0 && tb.lodPad[1] == 0, "a binding has no sampler bias by default");

    const Cfg off; // no rung bias, no player bias, no automip: the shipped defaults of the host
    // (1) 1/8: only multiples of 4 in 1/32 levels
    int steps = 0;
    Check(ToEighths(-64, steps) && steps == -16, "-2 levels is -16 eighths");
    Check(ToEighths(-32, steps) && steps == -8, "-1 level is -8 eighths");
    Check(ToEighths(-16, steps) && steps == -4, "-0.5 is -4 eighths");
    Check(ToEighths(16, steps) && steps == 4, "+0.5 is +4 eighths");
    Check(ToEighths(4, steps) && steps == 1, "1/8 level is one eighth");
    Check(!ToEighths(1, steps) && !ToEighths(-1, steps) && !ToEighths(2, steps) && !ToEighths(-2, steps) && !ToEighths(3, steps) && !ToEighths(5, steps) && !ToEighths(126, steps),
          "a 1/32 bias is not representable");
    // the whole register range: every representable value round-trips, no other value passes
    unsigned rep = 0;
    for (int b = -128; b <= 127; ++b) {
        int s = 0;
        const bool ok = ToEighths(b, s);
        Check(ok == (b % 4 == 0), "representable exactly when a multiple of 4");
        if (ok) { ++rep; Check(s * 4 == b, "eighths x 4 = the GX bias"); }
    }
    Check(rep == 64, "64 representable values in -128..127");
    // classes of single stages
    Check(One(0, -64).why == kMoved && One(0, -64).steps == -16, "-2: moved");
    Check(One(3, 16).why == kMoved && One(3, 16).steps == 4, "+0.5: moved, any texmap");
    Check(One(0, 0).why == kNone, "no bias: not a biased unit");
    Check(One(0, 1).why == kNotEighth && One(0, 1).steps == 0, "1/32: kept, not 1/8");
    Check(One(7, -3).why == kNotEighth, "-3/32: kept, not 1/8");
    Check(One(0, 124).why == kMoved && One(0, 124).steps == 31, "the largest GX bias (+3.875) is 31 eighths: moved");
    Check(One(0, -128).why == kRange, "the lowest GX bias (-4) is -32 eighths: below vitaGL's -31, kept (range)");
    Check(One(0, -124).why == kMoved && One(0, -124).steps == -31, "-31 eighths is the floor: moved");
    // (the unit is not read by any stage: nothing to decide)
    { int b[8] = {16}; Check(Classify(0, b, 0, 0, off).why == kNone, "a unit no stage reads"); }

    // (2) range: the back end's terms at their worst
    Cfg rung; rung.rungPct = 100;                   // the Vita's GXV_RUNG_LODBIAS=100
    Check(RungWorstLow(rung) == -13, "rung 100 %: the narrowest rung (0.5 of 640) is -13 eighths at worst (1.585 levels x 8)");
    Check(RangeOk(-16, off) && RangeOk(-31, off) && !RangeOk(-32, off), "no terms: -31 is the floor");
    Check(RangeOk(-16, rung) && RangeOk(-18, rung) && !RangeOk(-19, rung), "rung 100 %: -18 is the last that stays above -31 at the narrowest rung");
    Check(RangeOk(31, rung) && RangeOk(32, rung) && !RangeOk(33, rung), "the top: +32");
    Check(One(0, -64, rung).why == kMoved, "rung 100 %: -2 levels (-16): moved");
    Check(One(0, -96, rung).why == kRange, "rung 100 %: -3 levels (-24): kept, range");
    Check(One(0, -96, off).why == kMoved, "no terms: -3 levels is inside the range");
    Cfg ex = rung; ex.rungExtra = 8;
    Check(RungWorstLow(ex) == -21 && One(0, -64, ex).why == kRange, "rung extra 8: -13 - 8: -2 levels is now out of range");
    Cfg pl; pl.playerHundredths = -100;            // GXV_PLAYER_LODBIAS=-100: -8 eighths for the player's textures
    Check(WorstLow(0, pl) == -8 && WorstHigh(0, pl) == 0, "player -100: only the low bound moves");
    Cfg pp; pp.playerHundredths = 100;
    Check(WorstHigh(0, pp) == 8 && WorstLow(0, pp) == 0 && RangeOk(24, pp) && !RangeOk(25, pp), "player +100: only the high bound moves; +24 is the last");
    Cfg am; am.automipMode = 1; am.automipBias = -4;
    Check(WorstLow(0, am) == -4 && One(0, -108, am).why == kMoved, "automip -4: -4 more at the low end; -27 - 4 = -31 still moves");
    Check(One(0, -112, am).why == kRange, "automip -4: -28 - 4 = -32: kept");
    Cfg am0 = am; am0.automipMode = 0;
    Check(WorstLow(0, am0) == 0, "automip off: no term");
    // a sum is exact exactly when no clamp acts
    bool ex1 = false;
    Check(Sum(-13, -16, ex1) == -29 && ex1, "sum -29 exact");
    Check(Sum(-31, -1, ex1) == -31 && !ex1, "sum -32 clamps: not exact");
    Check(Sum(0, 32, ex1) == 32 && ex1, "sum +32 exact");
    Check(Sum(8, 32, ex1) == 32 && !ex1, "sum +40 clamps: not exact");
    // worst case really is the worst: for every representable bias that the front end moves, every rung of the ladder (width / 640 from 0.5 up to 1.5), with and
    // without the player's term, stays exact
    for (long pct : {0L, 50L, 100L, 150L, 200L})
        for (long extra : {0L, 4L, 24L})
            for (long ply : {-300L, -100L, 0L, 100L}) {
                Cfg c; c.rungPct = pct; c.rungExtra = extra; c.playerHundredths = ply; c.automipMode = 1; c.automipBias = -4;
                for (int b = -128; b <= 127; b += 4) {
                    const Unit u = One(2, b, c);
                    if (u.why != kMoved) continue;
                    for (float sx = 0.5f; sx <= 1.5f; sx += 0.0625f)
                        for (int pl2 = 0; pl2 < 2; ++pl2)
                            for (int am2 = 0; am2 < 2; ++am2) {
                                long base = lodbias::RungSteps(sx, pct, extra);
                                if (pl2) base += lodbias::PlayerSteps(ply);
                                if (am2) base += c.automipBias;
                                bool e = true;
                                Sum(static_cast<int8_t>(std::max<long>(-128, base)), u.steps, e);
                                // (the back end's own sums clamp at each term: the unclamped sum is the bound the front end tested)
                                const long raw = base + u.steps;
                                if (raw < lodbias::kMinSteps || raw > lodbias::kMaxSteps) { Check(false, "a moved bias stays inside the range at every rung"); }
                            }
                }
            }
    Check(true, "(sweep done)");

    // (3) an indirect read in the draw keeps every biased unit on the shader
    { int b[8] = {16, 0, 0, 0, 0, 0, 0, 0}; Check(Classify(0, b, 1, 2, off).why == kIndirect, "an indirect texmap in the draw: kept"); }
    // (4) one bias for every texmap the draw reads
    { int b[8] = {16, -16}; Check(Classify(0, b, 3, 0, off).why == kMixed && Classify(1, b, 3, 0, off).why == kMixed, "two texmaps, different biases: both kept (shared texture possible)"); }
    { int b[8] = {16, 16}; Check(Classify(0, b, 3, 0, off).why == kMoved && Classify(1, b, 3, 0, off).why == kMoved, "two texmaps, the same bias: both moved (one sampler value for both)"); }
    { int b[8] = {16, 0}; Check(Classify(0, b, 3, 0, off).why == kMixed && Classify(1, b, 3, 0, off).why == kNone, "a biased and an unbiased texmap: the biased one is kept"); }
    { int b[8] = {16, 0, 0, 0, 0, 0, 0, 24}; Check(Classify(0, b, 1, 0, off).why == kMoved, "a unit no stage reads does not matter"); }
    { int b[8] = {1, 16}; Check(Classify(0, b, 3, 0, off).why == kNotEighth && Classify(1, b, 3, 0, off).why == kMixed, "a 1/32 unit next to a 1/8 one: the first for its own reason, the second mixed"); }
    // the hash word: every unit's byte, so a change of any value changes it
    { int a[8] = {16, 0, 0, 0, 0, 0, 0, -16}, b[8] = {16, 0, 0, 0, 0, 0, 0, -12}, c2[8] = {16, 0, 0, 0, 0, 0, 0, -16};
      Check(PackBiases(a) != PackBiases(b) && PackBiases(a) == PackBiases(c2) && PackBiases(a) != 0, "the hash word follows every bias value"); }
    std::printf("lodsampler: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
