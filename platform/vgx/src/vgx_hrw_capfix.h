// GXV_HRW_CAPFIX (default 1; 0 = the old behaviour): the pure rule shared by every "reuse what an earlier picture made" half-rate path (host-tested: tests/hrw_capfix_test.cpp,
// tools/test_hrw_capfix.sh). Those paths count PICTURES THE FRONT END MADE / DRAWN TURNS (the A pictures); 60 Assist's second picture of a pair (a warped B) takes no number there,
// so "the previous picture" is a pair back (33.3 ms at 60) and a rule "reuse when the last one made it" alternates 33 ms / 67 ms old. While a pair was opened by this picture or
// the one before, nothing made earlier is reused. HRW off: a pair is never opened, Pairing() is false for ever and the old rules run unchanged.
#pragma once
#include <cstdint>

namespace vgx::hrwcapfix {
struct Tracker {
    uint64_t pairAt = 0;  // the picture number (front end: g_frame; game side: the drawn turn) that last opened a pair; 0 = never
    // `no`: this picture's number (>= 1); `opens`: this picture opened a pair; `on`: GXV_HRW_CAPFIX.
    bool Pairing(uint64_t no, bool opens, bool on) {
        if (opens) pairAt = no;
        return on && pairAt != 0 && no <= pairAt + 1;
    }
};
// The half-rate skip: `wanted` is the old rule's verdict (leave the capture / work out); it is overruled while pairing.
inline bool Skip(bool wanted, bool pairing) { return wanted && !pairing; }

// GXV_HRW_GOV5_EXACT (gov49): the water's capture is left out at half rate only on a stage whose plain 60 is PROVEN solid. The 60 Assist governor (GXV_HRW_GOV5) switches the pairs on and off
// on a borderline stage; pairs always capture fresh (above), plain 60 at half rate shows the water one picture late on every second picture (a 30 Hz misregistration while the camera moves,
// a flicker on the bridge of Beach Bowl). So the capture stays exact while the pairs run and until plain 60 has held `needSecs` play seconds in a row after the pairs stopped (or after a stage
// load / pause); a changeover therefore shows the same water on both sides and the half rate comes back only where it cannot be seen. Once a second, game thread; Exact() any thread.
struct SolidPlain {
    int secs = 0;
    // `govOn`: the governor drives mode 5 (not forced); `engaged`: the pairs run; `play`: a settled play second; `fps`: the plain rate of the second; `minFps`: what counts as solid.
    void Second(bool govOn, bool engaged, bool play, double fps, double minFps) {
        if (govOn && !engaged && play && fps >= minFps) { if (secs < 1000000) ++secs; } else secs = 0;
    }
    // dd24, GXV_HRW_GOV5_HITCH=1 (default 0 = Second): once plain 60 is PROVEN (secs >= needSecs) ONE second under minFps while the pairs are off does not take the proof back (a hitch: a
    // load, a cut, a one-off effect). A second dip within gapSecs plain seconds of the one ridden out is a borderline spot (the 30 Hz water the proof exists for): the proof goes as before.
    // The proof also ends at a changeover (Reset), a pause / stage load (play false) and with the governor off. Returns true when a dip was ridden out.
    int sinceDip = 1000000;
    bool SecondHold(bool govOn, bool engaged, bool play, double fps, double minFps, int needSecs, int gapSecs = 30) {
        if (!govOn || engaged || !play) { secs = 0; sinceDip = 1000000; return false; }
        if (sinceDip < 1000000) ++sinceDip;
        if (fps >= minFps) { if (secs < 1000000) ++secs; return false; }
        if (secs >= needSecs && needSecs > 0 && sinceDip >= gapSecs) { sinceDip = 0; return true; }
        secs = 0;
        return false;
    }
    void Reset() { secs = 0; sinceDip = 1000000; }
    bool Exact(bool govOn, bool engaged, int needSecs) const { return govOn && (engaged || secs < needSecs); }
};
}  // namespace vgx::hrwcapfix
