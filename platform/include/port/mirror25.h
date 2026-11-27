// reflfur25 (12 Nov): Mario's reflection on glass / ice (and on a water surface), the cuts and the census. Header only, so a host test can link the
// pure parts (tests/mirror25.cpp, tools/test_mirror25.sh). Everything defaults to 0 = the game's own draw, bit-identical.
//
// How the game makes the reflection (decomp: MarioActorShadow.cpp calcViewReflectionModel, MarioActorDraw.cpp drawReflectModel): there is NO mirror
// camera and no texture. The ONE Mario model is drawn again, in the Indirect draw pass, with its base matrix reflected (the model's Y column negated,
// origin 10 units under the floor point, mode 2 = glass / ice floor codes 14 and 33) and view slot 3 of its matrix buffer (viewCalc3: only the joint ->
// view concatenation is redone, the pose of calcAnim is shared, the GPU skins). Mode 2 draws it twice: a depth-only pass (display list 5: ZMode ALWAYS +
// write, no colour, ZScaleOffset 1e-5: it PUNCHES the mirrored model's depth through the opaque floor it lies behind) and an additive pass (display
// list 2: ZMode LEQUAL + write, blend SRCALPHA / ONE, alpha = _A09, 0..64, ramped). Modes 3 / 6 / 7 (swimming) draw one pass.
//
//   GXV_MIRROR25=0|1|2   (stage-settable, key "mirror")
//     1: the reflection of a picture whose mirrored model is entirely outside the picture's frustum is not calculated (viewCalc3) or drawn (exact: the
//        sphere is the mirrored model's own generous bound; the near-camera case is never skipped).
//     2: 1, and the mode-2 reflection is drawn in ONE pass: the additive pass with ZMode ALWAYS + write replaces the depth-only pass (it punches the floor
//        itself). NOT exact: where the mirrored model overlaps itself and the original's depth-only pass (the LAST drawn surface's depth, +1e-5)
//        rejected the farther fragment, that fragment now adds too (up to one more layer of an at most 25 % additive tint on a figure ~80 px tall).
//   GXV_MIRROR25_CENSUS=1  count (and with level 0 only count) what level 1 would skip: "mirror25:" every 300 pictures.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "port/hot_knob.h"
#include "port/stage_knob.h"

namespace port {
namespace mirror25 {

inline int EnvI(const char* n, int d) {
    float ov;
    if (port::stageknob::Lookup(n, &ov)) return static_cast<int>(ov);
    const char* e = std::getenv(n);
    return e && *e ? static_cast<int>(std::strtol(e, nullptr, 0)) : d;
}
inline int Level() { return PORT_STAGE_KNOB(int, [] { const int v = EnvI("GXV_MIRROR25", 0); return v < 0 ? 0 : (v > 2 ? 2 : v); }()); }
inline int CensusOn() { return PORT_STAGE_KNOB(int, EnvI("GXV_MIRROR25_CENSUS", 0) != 0 ? 1 : 0); }

enum Reason { kDrawn = 0, kHidden, kFading, kOffscreen, kReasons };
// The mirrored model's bound: the figure (160 units tall, arms out) around the reflection's origin, the root joint's offset, and what he carries.
inline float Radius(bool carrying) { return carrying ? 900.0f : 600.0f; }
// The camera closer than this (+ the radius) to the origin is never judged: the clipping frustum's own near plane is 500 units out.
inline float NearGuard() { return 600.0f; }

struct Input {
    bool hidden;      // the game's own gate: isAllHidden()
    bool fading;      // the game's own gate: the swimming ramps not yet visible, or not a reflecting mode
    bool outside;     // the clipping judge says the bounding sphere is outside the picture's frustum
    bool cameraNear;  // the camera is within NearGuard() + radius of the origin
};
inline Reason Decide(const Input& in) {
    if (in.hidden) return kHidden;
    if (in.fading) return kFading;
    if (in.outside && !in.cameraNear) return kOffscreen;
    return kDrawn;
}
inline bool Skips(Reason r) { return r == kOffscreen; }  // (kHidden and kFading are the game's own returns, only counted)

struct Census {
    uint32_t pictures = 0;
    uint32_t reason[kReasons] = {0, 0, 0, 0};
    uint32_t mode[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // reflecting pictures by the game's mode (_A08)
    uint32_t single = 0;                          // level 2: pictures drawn in one pass
};
inline bool Tick(Census& c, uint32_t every = 300) { return ++c.pictures >= every; }
inline int Format(char* buf, size_t n, const Census& c, int level) {
    return snprintf(buf, n,
                         "mirror25: level %d, %u pictures: reflections drawn %u, skipped off screen %u (the game's own: hidden %u, not reflecting yet %u); by mode: glass %u, water surface %u, under water %u; one-pass %u",
                         level, static_cast<unsigned>(c.pictures), static_cast<unsigned>(c.reason[kDrawn]), static_cast<unsigned>(c.reason[kOffscreen]),
                         static_cast<unsigned>(c.reason[kHidden]), static_cast<unsigned>(c.reason[kFading]), static_cast<unsigned>(c.mode[2]),
                         static_cast<unsigned>(c.mode[3]), static_cast<unsigned>(c.mode[6] + c.mode[7]), static_cast<unsigned>(c.single));
}

}  // namespace mirror25
}  // namespace port
