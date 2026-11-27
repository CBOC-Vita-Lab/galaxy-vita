// GXV_HRW_CAPFIX (vgx_hrw_capfix.h): the half-rate reuse rule with and without 60 Assist pairs. Models the front end's INDCAP half-rate decision exactly as
// MarkTransientCopies writes it (made-frame bookkeeping, `made + 1 == frame`, camera-cut refusal) and the game side's "previous drawn turn" rules (bloom / shadow / clip),
// and shows: (1) HRW off or GXV_HRW_CAPFIX=0: the decisions are bit-for-bit the old ones, over 200000 pictures incl. cuts; (2) HRW pairing: no capture is left out, the age of
// the capture a reader of the picture's own capture sees is 0 shown pictures (old: alternately 0 and 2 shown pictures = 33.3 / 67 ms stepping), a reader of the
// previous capture sees a constant 2 (old: 2 and 4); (3) the pairing ends 1 picture after the last pair; (4) Quality's even 30 (no pair marker) is unchanged.
#include "../platform/vgx/src/vgx_hrw_capfix.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++failures; } else std::printf("PASS: %s\n", msg); } while (0)

struct Pic { bool opens; bool cut; int shownPics; };  // shownPics: how many shown pictures this A is followed by before the next A (1 plain 60, 2 a pair or an even 30)

// the old rule, verbatim: skip when the previous picture made it and nothing refuses
struct Half {
    uint64_t made = 0; bool have = false;
    vgx::hrwcapfix::Tracker tr; bool on;
    uint64_t skipped = 0, fresh = 0;
    // returns true when the capture is left out
    bool Picture(uint64_t frame, bool opens, bool cut, bool useFix) {
        const bool pairing = useFix ? tr.Pairing(frame, opens, on) : false;
        const bool old = have && made + 1 == frame && !cut;
        const bool skip = useFix ? vgx::hrwcapfix::Skip(old, pairing) : old;
        if (old && !skip) ++fresh;
        if (skip) { ++skipped; return true; }
        made = frame; have = true;
        return false;
    }
};

int main() {
    // (1) identity when no pair is ever opened (plain 60 / Quality even 30), and with the fix switched off while pairs are
    {
        Half oldR, fixR; oldR.on = false; fixR.on = true;
        uint32_t s = 12345; unsigned differ = 0;
        for (uint64_t f = 1; f <= 200000; ++f) {
            s = s * 1664525u + 1013904223u;
            const bool cut = (s >> 24) % 50 == 0;
            const bool a = oldR.Picture(f, false, cut, false), b = fixR.Picture(f, false, cut, true);
            differ += a != b;
        }
        CHECK(differ == 0 && fixR.fresh == 0 && oldR.skipped == fixR.skipped && oldR.skipped > 50000, "HRW off (no pair marker): 200000 pictures with camera cuts, every decision identical to the old rule, 0 made fresh");
        Half o2, f2; o2.on = false; f2.on = false; unsigned d2 = 0;
        for (uint64_t f = 1; f <= 100000; ++f) { const bool opens = (f % 3) != 0; d2 += o2.Picture(f, opens, false, false) != f2.Picture(f, opens, false, true); }
        CHECK(d2 == 0 && f2.fresh == 0, "GXV_HRW_CAPFIX=0 with pairs open: every decision identical to the old rule");
    }
    // (2) under pairs: the age (in shown pictures) of the capture the readers see
    {
        auto run = [](bool fix, int& minSame, int& maxSame, int& minPrev, int& maxPrev, uint64_t& skipped, uint64_t& fresh) {
            Half h; h.on = true;
            int shown = 0, madeAt = -100, prevMadeAt = -100;  // shown-picture clock; A_n is shown at 2n
            minSame = minPrev = 1000; maxSame = maxPrev = -1;
            for (uint64_t f = 1; f <= 2000; ++f, shown += 2) {
                const bool skip = h.Picture(f, true, false, fix);
                const int before = madeAt;  // a reader BEFORE the picture's own capture sees the capture left by the earlier picture
                if (!skip) { prevMadeAt = madeAt; madeAt = shown; }
                if (f > 10) {
                    const int same = shown - madeAt, prev = shown - before;
                    minSame = same < minSame ? same : minSame; maxSame = same > maxSame ? same : maxSame;
                    minPrev = prev < minPrev ? prev : minPrev; maxPrev = prev > maxPrev ? prev : maxPrev;
                }
            }
            (void)prevMadeAt; skipped = h.skipped; fresh = h.fresh;
        };
        int a0, a1, b0, b1; uint64_t sk, fr;
        run(false, a0, a1, b0, b1, sk, fr);
        std::printf("old: reader of the picture's own capture sees %d..%d shown pictures old (%.1f..%.1f ms), reader of the previous capture %d..%d; %llu left out\n", a0, a1, a0 * 16.7, a1 * 16.7, b0, b1, (unsigned long long)sk);
        CHECK(a0 == 0 && a1 == 2 && b0 == 2 && b1 == 4 && sk == 1000, "old rule under pairs: the capture alternates 0 / 2 shown pictures old (own) and 2 / 4 (previous): the 15 Hz bug");
        run(true, a0, a1, b0, b1, sk, fr);
        std::printf("new: own %d..%d shown pictures old, previous %d..%d; %llu left out, %llu made fresh\n", a0, a1, b0, b1, (unsigned long long)sk, (unsigned long long)fr);
        CHECK(a0 == 0 && a1 == 0 && sk == 0, "GXV_HRW_CAPFIX: under pairs no capture is left out, a reader of the picture's own capture always sees 0 shown pictures old");
        CHECK(b0 == 2 && b1 == 2, "GXV_HRW_CAPFIX: a reader of the previous capture sees a constant 2 shown pictures (one pair), no longer alternating 2 / 4");
        CHECK(fr == 1999, "the log counter: every A after the first that the old rule would have left out or not (it alternates) is counted when overruled: 1999 of 2000");
    }
    // (3) the pairing ends one picture after the last pair opened, then the half rate is back
    {
        Half h; h.on = true; std::vector<int> skips;
        uint64_t f = 1;
        for (; f <= 20; ++f) h.Picture(f, true, false, true);          // pairs
        for (; f <= 30; ++f) skips.push_back(h.Picture(f, false, false, true));  // pairs stop (a camera cut, a stage without them)
        // picture 21 is still pairing (the pair of 20 is not over): made; 22 follows a made picture without pairs: left out; 23 made ...
        CHECK(!skips[0] && skips[1] && !skips[2] && skips[3], "pairing ends one picture after the last pair: 21 made fresh, then the half rate alternates again (22 out, 23 made, 24 out)");
    }
    // (4) the game side: one counter of drawn turns, the same tracker
    {
        vgx::hrwcapfix::Tracker t; unsigned reuse = 0, ref = 0; uint64_t chain = 0;
        for (uint64_t pic = 1; pic <= 1000; ++pic) {
            const bool pairing = t.Pairing(pic, true, true);
            const bool old = chain + 1 == pic;      // sBloomChainPic + 1 == sBloomPic
            if (old && pairing) ++ref; else if (old) ++reuse;
            if (!(old && !pairing)) chain = pic;
        }
        CHECK(reuse == 0 && ref >= 998, "bloom / shadow / clip rule with pairs: every reuse refused");
        vgx::hrwcapfix::Tracker t2; chain = 0; reuse = 0;
        for (uint64_t pic = 1; pic <= 1000; ++pic) { const bool pairing = t2.Pairing(pic, false, true); const bool old = chain + 1 == pic; if (old && !pairing) ++reuse; if (!(old && !pairing)) chain = pic; }
        CHECK(reuse >= 499, "same rule without pairs: the reuses as before (every second picture)");
    }
    // (5) gov49: the governor's changeover keeps the water exact until plain 60 is proven
    {
        vgx::hrwcapfix::SolidPlain sp; const int need = 5; const double minFps = 57;
        CHECK(!sp.Exact(false, false, need), "governor off: never forced exact by this rule");
        CHECK(sp.Exact(true, false, need), "a stage load: exact until plain 60 has held");
        for (int s = 0; s < 4; ++s) sp.Second(true, false, true, 60, minFps);
        CHECK(sp.Exact(true, false, need), "4 solid seconds: still exact");
        sp.Second(true, false, true, 60, minFps);
        CHECK(!sp.Exact(true, false, need), "5 solid seconds: the half rate may run");
        sp.Second(true, false, true, 54, minFps);
        CHECK(sp.Exact(true, false, need), "one second under 57 fps (a borderline spot): exact again at once");
        for (int s = 0; s < 10; ++s) sp.Second(true, false, true, 60, minFps);
        CHECK(!sp.Exact(true, false, need), "recovered");
        CHECK(sp.Exact(true, true, need), "pairs engaged: exact whatever the count");
        sp.Second(true, true, true, 30, minFps);
        CHECK(sp.secs == 0 && sp.Exact(true, false, need), "pairs stopped: the count starts from 0, so the first plain seconds are exact (no half-rate phase at the changeover)");
        for (int s = 0; s < 3; ++s) sp.Second(true, false, true, 60, minFps);
        sp.Second(true, false, false, 60, minFps);   // pause / loading second
        CHECK(sp.secs == 0, "a not-play second resets");
        // a borderline spot: 60 for 6 s, 50 for 1 s, repeated: never solid for 5 s after a dip, exact at every dip
        unsigned halfSecs = 0, secsAll = 0;
        for (int rep = 0; rep < 20; ++rep) for (int s = 0; s < 7; ++s) { sp.Second(true, false, true, s < 6 ? 60 : 50, minFps); ++secsAll; if (!sp.Exact(true, false, need)) ++halfSecs; }
        CHECK(halfSecs < secsAll && halfSecs > 0, "a borderline stage still gets the half rate in its long solid stretches only");
        for (int rep = 0; rep < 20; ++rep) for (int s = 0; s < 4; ++s) { sp.Second(true, false, true, s < 3 ? 60 : 50, minFps); CHECK(sp.Exact(true, false, need), "dips every 4 s: never half rate"); }
    }
    // (6) dd24 GXV_HRW_GOV5_HITCH: one hitch does not take the proof back; a second within 30 s does; changeovers and pauses always do
    {
        vgx::hrwcapfix::SolidPlain sp; const int need = 5; const double minFps = 57;
        for (int s = 0; s < 5; ++s) sp.SecondHold(true, false, true, 60, minFps, need);
        CHECK(!sp.Exact(true, false, need), "proven");
        CHECK(sp.SecondHold(true, false, true, 41, minFps, need), "a single hitch is ridden out");
        CHECK(!sp.Exact(true, false, need), "half rate stays through the hitch");
        for (int s = 0; s < 10; ++s) sp.SecondHold(true, false, true, 60, minFps, need);
        CHECK(!sp.SecondHold(true, false, true, 50, minFps, need) && sp.Exact(true, false, need), "a second dip 10 s later: the proof goes (borderline spot)");
        for (int s = 0; s < 6; ++s) sp.SecondHold(true, false, true, 60, minFps, need);
        CHECK(!sp.Exact(true, false, need), "proven again after 5 s");
        for (int s = 0; s < 40; ++s) sp.SecondHold(true, false, true, 60, minFps, need);
        CHECK(sp.SecondHold(true, false, true, 30, minFps, need), "a hitch 40 s after the last is ridden out again");
        sp.SecondHold(true, true, true, 30, minFps, need);
        CHECK(sp.secs == 0 && sp.Exact(true, true, need), "pairs: proof gone");
        for (int s = 0; s < 3; ++s) sp.SecondHold(true, false, true, 60, minFps, need);
        CHECK(sp.Exact(true, false, need), "after pairs stop: exact until 5 s again (no half-rate phase at the changeover)");
        for (int s = 0; s < 5; ++s) sp.SecondHold(true, false, true, 60, minFps, need);
        sp.SecondHold(true, false, false, 60, minFps, need);
        CHECK(sp.secs == 0, "a pause / load second takes the proof");
        // never proven: a dip resets as before
        vgx::hrwcapfix::SolidPlain q;
        for (int s = 0; s < 3; ++s) q.SecondHold(true, false, true, 60, minFps, need);
        CHECK(!q.SecondHold(true, false, true, 50, minFps, need) && q.secs == 0, "not yet proven: a dip resets the count");
    }
    return failures ? 1 : 0;
}
