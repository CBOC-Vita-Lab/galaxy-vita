#pragma once
#include <array>
#include <cstdint>

#define VGX_STATIC_CROWD_METADATA 1

namespace vgx::staticcrowd {
constexpr uint32_t kMaxTags = 65;
struct Tag {
    std::array<uint32_t, 8> words{};
    uint64_t destination = 0;
    bool valid() const { return words[0] && words[2] && words[3] && words[4] && words[5] && destination; }
    bool operator==(const Tag& other) const { return words == other.words && destination == other.destination; }
    bool operator!=(const Tag& other) const { return !(*this == other); }
};
} // namespace vgx::staticcrowd
