#pragma once
// The shader-key memo of the resolution cache (GXV_RSCACHE, vgx_draw.cpp): State::keyHash + the vertex-matrix mask and the
// short-palette flag -> the fragment and vertex shader ids (and the register-folded fragment id). A set-associative table (the
// first version was direct-mapped with 1,024 entries and missed 7.5% of its lookups in the Comet Observatory, nearly all of
// them conflicts between a few hundred live keys). The table only decides whether a lookup finds an entry: an entry holds what
// the miss path computed for exactly that (hash, mask, flag), so a hit and a miss give the same shader ids; a bigger or
// differently shaped table can change how often the miss path runs, never what a draw resolves to. Header-only, so that
// tests/vgx_keymemo_test.cpp can drive the real code.
#include <cstdint>

namespace vgx {

struct KeyMemo {
    uint64_t h = 0;          // State::keyHash
    uint32_t vcdMtxMask = 0;
    uint16_t fsId = 0, vsId = 0, fsIdFold = 0;
    uint8_t spo = 0;         // short palette (only with a per-vertex position matrix)
    uint8_t valid = 0, fsIdFoldValid = 0;
};

class KeyMemoTable {
public:
    static constexpr uint32_t kSets = 2048;
    static constexpr uint32_t kMaxWays = 4;
    static constexpr uint32_t kEntries = kSets * kMaxWays;

    static uint32_t Set(uint64_t h, uint32_t mask, uint8_t spo) {
        const uint32_t x = static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32) ^ (mask * 0x9E3779B1u) ^ (spo * 0x7F4A7C15u);
        return (x ^ (x >> 15)) & (kSets - 1);
    }
    // The entry for (h, mask, spo), or nullptr. `ways` is 1..kMaxWays (the associativity in use). slot receives the entry's index.
    KeyMemo* Find(uint64_t h, uint32_t mask, uint8_t spo, uint32_t ways, uint32_t& slot) {
        const uint32_t base = Set(h, mask, spo) * kMaxWays;
        for (uint32_t w = 0; w < ways; ++w) {
            KeyMemo& m = e_[base + w];
            if (m.valid && m.h == h && m.vcdMtxMask == mask && m.spo == spo) {
                slot = base + w;
                return &m;
            }
        }
        return nullptr;
    }
    // A cleared entry for (h, mask, spo) in its set (a free way, else the oldest insertion); the caller fills in the ids.
    // Only for a key Find has just missed.
    KeyMemo& Insert(uint64_t h, uint32_t mask, uint8_t spo, uint32_t ways, uint32_t& slot) {
        const uint32_t set = Set(h, mask, spo);
        const uint32_t base = set * kMaxWays;
        uint32_t w = ways;
        for (uint32_t i = 0; i < ways; ++i)
            if (!e_[base + i].valid) { w = i; break; }
        if (w == ways) {
            w = next_[set] % ways;
            next_[set] = static_cast<uint8_t>((w + 1) % ways);
        }
        slot = base + w;
        KeyMemo& m = e_[slot];
        m = KeyMemo{};
        m.h = h; m.vcdMtxMask = mask; m.spo = spo;
        return m;
    }
    KeyMemo& At(uint32_t slot) { return e_[slot]; }

private:
    KeyMemo e_[kEntries];
    uint8_t next_[kSets] = {};
};

} // namespace vgx
