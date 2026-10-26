#pragma once

#include "../src/vgx_common.h"

namespace vgx::bootguard {

inline bool Enabled() {
    // GXV_BOOTGUARD=1 opts into honest pre-title progress and HOLD precedence; read once, default off.
    static const bool enabled = vgx::EnvInt("GXV_BOOTGUARD", 0) != 0;
    return enabled;
}

}  // namespace vgx::bootguard
