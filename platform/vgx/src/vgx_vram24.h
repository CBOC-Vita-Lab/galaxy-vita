// vram24: the video-memory rules of the Honeyhive 18 s (night58 02-HHS 09:17:02-09:17:19), as plain functions the back end calls and tests/vram24_test.cpp checks.
// No state, no GL. All free figures in MiB.
#pragma once
#include <algorithm>

namespace vgx {
namespace vram24 {

// seam68's low-memory test (S68LowVram): what the stage holds back goes to vitaGL when free video memory is under the limit.
// Old rule (hyst off): under GXV_SEAM68_LOWMEM_MIB (8). That is BELOW the pool trim's urgent limit (12): between 8 and 12 the trim deleted idle pooled objects, DeleteGpuTex handed their
// textures to the late list, and the hold kept them there: nothing was freed, the trim fired again, until free memory fell under 8 and everything went in one lump.
// With GXV_VRAM24_HYST: the limit is at least the urgent limit (when the trim is on), and once low it stays low until `hystMiB` above that limit.
inline bool S68Low(bool wasLow, double freeMiB, double s68LowMiB, double urgentMiB, bool trimOn, bool hyst, double hystMiB) {
    if (!hyst) return freeMiB < s68LowMiB;
    const double enter = std::max(s68LowMiB, trimOn ? urgentMiB : 0.0);
    return freeMiB < enter + (wasLow ? hystMiB : 0.0);
}

// The pool trim's urgent state (every idle pooled object deleted, no one-a-present limit). Old rule: free < urgent, re-read every present while on. With hyst: stays on until hystMiB above.
inline bool TrimAll(bool wasAll, double freeMiB, double urgentMiB, bool hyst, double hystMiB) {
    return freeMiB < urgentMiB + (wasAll && hyst ? hystMiB : 0.0);
}

// The rung the memory emergency goes back to: never under the controller's floor (Controller::Drop clamps to minRung, so a fixed-size stage (minRung 6) got a "back to rung 2" line and no change).
inline int DropTo(int today, int minRung) { return std::max(today, minRung); }

} // namespace vram24
} // namespace vgx
