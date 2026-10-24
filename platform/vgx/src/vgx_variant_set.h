#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace vgx {
struct VariantKeyHash {
    std::size_t operator()(uint64_t key) const noexcept {
        // Fold the shader IDs into the low word before ILP32 narrows size_t.
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 33;
        return static_cast<std::size_t>(key);
    }
};
using VariantSet = std::unordered_set<uint64_t, VariantKeyHash>;
} // namespace vgx
