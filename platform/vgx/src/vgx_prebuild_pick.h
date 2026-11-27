// big60 (13 Nov 2026): which rungs RungPrebuildStep makes the pieces of a size change for ahead of time (render targets and copy textures: 10-90 ms each when made
// in play). Pure logic, host-tested by tests/vgx_prebuild_pick_test.cpp.
//
// Today (startFirst false, the default): the one or two rungs below the current one that the controller may still step to (never below minRung; with the two-size
// rule only the floor itself), nearest first. That is right for a controller that steps in play, and useless for a stage whose preset fixes its size: its row is
// applied at the star select (the screen is covered), the size changes once at the stage's first picture, and the pieces made for the two rungs under the current
// size are not the start rung's, unless the start rung is one of those two (848x480 from 960x544, the only distance the shipped rows ever used).
// GXV_RUNG_PREBUILD_START=1: when the applied preset FIXES the size (floor = ceiling = start) and the start rung is below the current rung, the target list is that
// one rung, whatever the distance: the pieces of the stage's own size are made in the quiet moments before the stage opens (star select, load), so the one change
// the stage makes takes premade pieces ("premade taken 2" in the [seamless] line) like the 848x480 start does today.
#pragma once

namespace vgx::prebuild {

// cur: the current rung; minRung: the controller's floor; twoSize: only the floor is a size between; presetFixed / presetStart: the applied preset fixes the size / its rung
// (presetStart < 0: no preset); samplesOk(rung): the rung is single-sampled (the caller's RungSamples(rung) == 1). out[2] gets the rungs, nearest first. Returns how many.
template <class SamplesOk>
inline unsigned Pick(int cur, int minRung, bool twoSize, bool presetFixed, int presetStart, bool startFirst, SamplesOk samplesOk, int out[2]) {
    unsigned nt = 0;
    if (cur < 1 || !samplesOk(cur)) return 0;
    if (startFirst && presetFixed && presetStart >= 0 && presetStart < cur && samplesOk(presetStart)) {
        out[nt++] = presetStart;
        return nt;
    }
    for (int d = 1; d <= 2; ++d) {
        const int tr = cur - d;
        if (tr < 0 || tr < minRung || !samplesOk(tr)) break;
        if (twoSize && tr > minRung) continue;
        out[nt++] = tr;
    }
    return nt;
}

}  // namespace vgx::prebuild
