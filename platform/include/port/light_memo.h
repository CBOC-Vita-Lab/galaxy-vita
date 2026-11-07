// GXV_LIGHT_DEDUP (decomp/src/Game/Map/LightFunction.cpp, decomp/src/RVL_SDK/gx/GXLight.c): the memo of the last light set-up a
// draw-buffer pass loaded. Header-only, so that tests/light_dedup.cpp runs the same code.
//
// A set-up (LightFunction::loadActorLightInfo) sends light objects that are a function of its inputs only (`key`: the light
// info's bytes and the camera's view matrix). `gen` is the GX light generation (__PortGXLightGen), which moves at every light
// object sent and every drop of the light shadow. When a set-up has the key of the last one and the generation has not moved since
// that one finished, light memory holds exactly what it would send: it sends nothing (but what it sets outside light memory).
#pragma once

#include <stdint.h>
#include <string.h>

namespace port {

struct LightMemo {
    enum { kKeyMax = 112 };
    uint32_t gen;    // the generation when the last set-up finished; 0: none (a scope opened or closed since)
    uint32_t bytes;  // the key's size
    uint8_t key[kKeyMax];
};

// true: the set-up with this key may be left out (see above). false: make it, then call LightMemoDone with the generation then.
// The key (two parts, a + b, at most kKeyMax bytes in all) is kept for the next call.
inline bool LightMemoSame(LightMemo& m, uint32_t gen, const void* a, uint32_t na, const void* b, uint32_t nb) {
    if (na + nb > LightMemo::kKeyMax) {
        m.gen = 0;
        return false;
    }
    const bool same = m.gen != 0 && m.gen == gen && m.bytes == na + nb && memcmp(m.key, a, na) == 0 && memcmp(m.key + na, b, nb) == 0;
    if (!same) {
        m.gen = 0;  // (until LightMemoDone: a set-up cut short leaves no memo)
        m.bytes = na + nb;
        memcpy(m.key, a, na);
        memcpy(m.key + na, b, nb);
    }
    return same;
}
inline void LightMemoDone(LightMemo& m, uint32_t gen) {
    m.gen = gen;
}
inline void LightMemoDrop(LightMemo& m) {
    m.gen = 0;
}

}  // namespace port
