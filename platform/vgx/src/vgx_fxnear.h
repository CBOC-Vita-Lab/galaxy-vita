// compfix26: the compositor's "near" test as a pure function (tests/vgx_compfix26_test.cpp). A blended draw is protected from the reduced-size path when it is near the camera
// (a small crisp sprite, an effect on the lens). The old rule used the draw's NEAREST VERTEX against GXV_FXLOWRES_NEAR (2500 units). The play camera is about 1300 units from Mario
// (Mario 160 units tall = ~82 px on 544 lines at a 45 degree field of view), so every glow sheet around him had a vertex under 2500 and was protected: the compositor measured
// them, called them "near", and routed nothing. A draw that covers a good part of the screen is a layer, not a sprite: with GXV_FXLOWRES_AUTO it counts as far unless its nearest
// vertex is under `minFrac` of the limit (really on the lens).
#pragma once

namespace vgx::fxnear {

struct Rule {
    float nearUnits = 2500.0f;  // GXV_FXLOWRES_NEAR
    float bigScreens = 0.15f;   // GXV_FXLOWRES_NEAR_BIG (thousandths): fill of one draw from which it is a layer; 0 = the old rule
    float minFrac = 0.25f;      // GXV_FXLOWRES_NEAR_MIN (percent): a layer under this part of the limit is still near
};
// fillScreens: the draw's own fill; nearW: its nearest vertex's distance from the camera plane (game units).
inline bool BigLayer(const Rule& r, float fillScreens, float nearW) {
    return r.bigScreens > 0.0f && fillScreens >= r.bigScreens && nearW >= r.nearUnits * r.minFrac;
}
inline bool IsNear(const Rule& r, float fillScreens, float nearW, bool autoOn) {
    return nearW < r.nearUnits && !(autoOn && BigLayer(r, fillScreens, nearW));
}

} // namespace vgx::fxnear
