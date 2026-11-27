// Stage-settable choice of draws for the reduced-size effect path (GXV_FXLOWRES=1; freeze25). A stage preset lists up to four fragment shaders by
// their stable content key: the scene dump's "fh" (GXV_DROP_FS=h:<key>) cut to its low 24 bits, because a stage value is a float and carries 24 bits
// exactly (fxlow_k1..fxlow_k4, written in decimal; 0 = the slot is unused). A listed draw is moved to the half-size target whatever its distance
// (the near limit does not protect it: the preset's author chose it) and its run is taken whatever its fill (the gate GXV_FXLOWRES_MIN does not apply).
#pragma once
#include <cstdint>
namespace vgx::fxkeys {
constexpr uint32_t kMask = 0xFFFFFFu;
constexpr uint32_t Short(uint32_t fh) { return fh & kMask; }
constexpr uint32_t Clean(long v) { return (v > 0 && v <= static_cast<long>(kMask)) ? static_cast<uint32_t>(v) : 0u; }
constexpr bool Listed(const uint32_t keys[4], uint32_t fh) {
    const uint32_t s = Short(fh);
    if (s == 0) return false;
    for (int i = 0; i < 4; ++i)
        if (keys[i] == s) return true;
    return false;
}
constexpr bool Any(const uint32_t keys[4]) { return (keys[0] | keys[1] | keys[2] | keys[3]) != 0; }
} // namespace vgx::fxkeys
