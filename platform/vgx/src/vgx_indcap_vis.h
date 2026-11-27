// GXV_INDCAP_VIS (default 0; 2 count only, 1 apply): the "Indirect" capture left out when EVERY reader of it in its picture is invisible. The existing GXV_INDCAP_SKIP
// rule leaves a capture out when no later draw samples it; this extends "no reader" to "no VISIBLE reader". Pure decision, host-tested (tests/indcap_vis_test.cpp,
// tools/test_indcap_vis.sh); MarkTransientCopies (vgx_draw.cpp) feeds it the readers it has already found and the facts it already has.
// Exactness: a draw whose scissor is empty (w <= 0 or h <= 0) writes no pixel of the EFB, so what it samples cannot show in this picture or any later one; the same
// holds for a draw whose known extent (DrawRec::ex..) is empty. A draw lying wholly outside the EFB is the same unless the back end's overscan margin is on (the margin
// is drawn and can be sampled later): then it is "visible". Anything that is not a plain draw with known boxes is a visible reader: an atlas / EFB copy that samples the
// texture, a draw with a side effect other than its pixels (the occlusion-query flag), and every doubt. Whatever the existing rule keeps the capture for (a reader of the
// previous picture's capture, warm-up, a stage with such readers) it keeps here too; HRW pairs, camera cuts, PREVCAP, a clear, a scenes25 class capture refuse.
#pragma once
#include <cstdint>

namespace vgx::indcapvis {
enum Verdict : uint8_t { V_DRY = 0, V_VISIBLE, V_ATLAS, V_OCC, V_HELD, V_PAIR, V_CUT, V_SHAPE, V_PREVCAP, V_COUNT };
inline const char* Name(int v) {
    static const char* const n[V_COUNT] = {"dry", "visible reader", "atlas / copy reader", "occlusion-query reader", "held (previous-picture readers / stage / warm-up)",
                                           "HRW pairing", "camera cut", "capture shape (clear / class capture)", "GXV_PREVCAP"};
    return v >= 0 && v < V_COUNT ? n[v] : "?";
}
struct Reader {
    bool atlas = false;      // an EFB / atlas copy that samples the texture
    bool occ = false;        // DrawRec::occ (an occlusion query counts its pixels)
    int exX = 0, exY = 0, exW = 0, exH = 0;   // DrawRec::ex..
    int scX = 0, scY = 0, scW = 0, scH = 0;   // DrawRec::rs.sc..
};
struct Facts {
    int efbW = 0, efbH = 0;
    bool overscan = false;   // GXV_OVERSCAN is on: a box outside the EFB may land in the margin
    bool held = false;       // the old rule keeps the capture (IcHeld / IcEarlyStage / not warm)
    bool pairing = false;    // HRW pair (hrwPair or the picture has a pair id)
    bool cut = false;        // IcCamCut()
    bool shape = false;      // the capture has a clear, or is a scenes25 class capture
    bool prevcap = false;    // GXV_PREVCAP on
};
// true: the draw puts no pixel anywhere a later read could see.
inline bool Invisible(const Reader& r, const Facts& f) {
    if (r.atlas || r.occ) return false;
    const auto box = [&](int x, int y, int w, int h) { // 1: nothing, 0: unknown / something
        if (w <= 0 || h <= 0) return true;
        if (f.overscan || f.efbW <= 0 || f.efbH <= 0) return false;
        return x >= f.efbW || y >= f.efbH || x + w <= 0 || y + h <= 0;
    };
    return box(r.exX, r.exY, r.exW, r.exH) || box(r.scX, r.scY, r.scW, r.scH);
}
// `n` readers (n >= 1: the capture HAS readers; no reader at all is the old rule's). V_DRY: leave the capture out.
inline Verdict Decide(const Reader* rd, int n, const Facts& f) {
    if (n < 1) return V_VISIBLE;
    if (f.prevcap) return V_PREVCAP;
    if (f.shape) return V_SHAPE;
    if (f.pairing) return V_PAIR;
    if (f.cut) return V_CUT;
    Verdict why = V_DRY;
    for (int i = 0; i < n; ++i) {
        if (Invisible(rd[i], f)) continue;
        why = rd[i].atlas ? V_ATLAS : rd[i].occ ? V_OCC : V_VISIBLE;
        break;
    }
    if (why != V_DRY) return why;
    if (f.held) return V_HELD;
    return V_DRY;
}
}  // namespace vgx::indcapvis
