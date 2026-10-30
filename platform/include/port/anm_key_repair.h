// Repair for J3D key animations (ANK1 blocks, .bck) that were converted by the older data converter.
//
// The converter swapped each key array to little-endian by the count in the block header, and read that count as a
// signed 16-bit number. An array of 32768 keys or more (the rotation arrays of Bowser's three battle-start cutscenes,
// the translation arrays of Polta's power-up demo, and a few more) came out negative, so the whole array was left
// big-endian and the joints twisted and spun. The converter now reads the counts unsigned and writes the marker below
// into the padding of the blocks it converts that way. A block that has a count of 32768 or more and no marker came
// from the older converter: this swaps those arrays, in place, and writes the marker, so a second load is left alone.
//
// GXV_FIX_BCKCNT (default 1, read once): 0 leaves the data as it was found.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace port {
namespace anmkey {

constexpr uint32_t kCountLimit = 0x8000;
constexpr uint32_t kTag = 0x414E4B31;     // 'ANK1' (the converted block stores its tag as one little-endian word)
constexpr uint32_t kMarkerOffset = 0x24;  // padding between the header (0x24 bytes) and the joint table (0x40)
constexpr char kMarker[4] = {'U', 'C', 'N', 'T'};

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("GXV_FIX_BCKCNT");  // unset, empty or not a number: stays on
        if (!v || !*v) return true;
        char* end = nullptr;
        const long parsed = std::strtol(v, &end, 0);
        return end == v || *end ? true : parsed != 0;
    }();
    return on;
}

namespace detail {
inline uint16_t Rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline void Swap16(uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i, p += 2) {
        const uint8_t t = p[0];
        p[0] = p[1];
        p[1] = t;
    }
}
inline void Swap32(uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i, p += 4) {
        const uint8_t a = p[0], b = p[1];
        p[0] = p[3];
        p[1] = p[2];
        p[2] = b;
        p[3] = a;
    }
}
inline std::mutex& Lock() {
    static std::mutex m;
    return m;
}
}  // namespace detail

// `block` is the converted ANK1 block (its tag at +0). Returns how many arrays were swapped (0 to 3). Arrays that
// do not lie inside the block are left alone.
inline int RepairInPlace(void* block, bool enabled = Enabled()) {
    if (!enabled || !block) return 0;
    uint8_t* const b = static_cast<uint8_t*>(block);
    if (detail::Rd32(b) != kTag) return 0;
    const uint32_t size = detail::Rd32(b + 4);
    if (size < kMarkerOffset + 4 || size > 0x10000000u) return 0;
    if (detail::Rd32(b + 0x14) < kMarkerOffset + 4) return 0;  // no padding to hold the marker (the cooker does the same)
    const uint32_t counts[3] = {detail::Rd16(b + 0x0E), detail::Rd16(b + 0x10), detail::Rd16(b + 0x12)};  // scale, rotation, translation
    if (counts[0] < kCountLimit && counts[1] < kCountLimit && counts[2] < kCountLimit) return 0;
    const uint32_t offsets[3] = {detail::Rd32(b + 0x18), detail::Rd32(b + 0x1C), detail::Rd32(b + 0x20)};
    const uint32_t widths[3] = {4, 2, 4};
    std::lock_guard<std::mutex> lock(detail::Lock());
    if (std::memcmp(b + kMarkerOffset, kMarker, 4) == 0) return 0;
    int swapped = 0;
    for (int i = 0; i < 3; ++i) {
        if (counts[i] < kCountLimit) continue;
        const uint64_t bytes = static_cast<uint64_t>(counts[i]) * widths[i];
        if (offsets[i] < kMarkerOffset + 4 || offsets[i] + bytes > size) continue;
        if (widths[i] == 2) detail::Swap16(b + offsets[i], counts[i]);
        else detail::Swap32(b + offsets[i], counts[i]);
        ++swapped;
    }
    std::memcpy(b + kMarkerOffset, kMarker, 4);
    return swapped;
}

}  // namespace anmkey
}  // namespace port
