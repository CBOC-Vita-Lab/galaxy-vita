// GXV_EFBSWAP2_BACKOFF (dd24): the length of a texture's "converted later" hold (GXV_EFBSWAP2_HOLD) as a backoff. Pure, host-tested (tests/efbswap_backoff_test.cpp).
// The hold only chooses the ordinary copy over the zero-copy swap (a speed rule, the r84 Good Egg slowdown); it guards no picture.
// First conversion of a texture: kMin pictures. Another conversion within kClean pictures after that hold ended: double, capped at kCap. Later than that (kClean clean pictures): kMin again.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace vgx::swapback {
constexpr uint32_t kMin = 16, kCap = 300, kClean = 60;
struct Entry { uint32_t texId; uint64_t until; uint32_t len; };
struct Backoff {
    std::vector<Entry> e;
    uint32_t starts = 0, doubled = 0, longest = 0;   // (the verify line)
    uint32_t Next(uint32_t texId, uint64_t now) {
        for (Entry& b : e)
            if (b.texId == texId) {
                if (now <= b.until + kClean) { b.len = std::min<uint32_t>(b.len * 2u, kCap); ++doubled; }
                else b.len = kMin;
                b.until = now + b.len;
                longest = std::max(longest, b.len);
                return b.len;
            }
        if (e.size() < 64) e.push_back(Entry{texId, now + kMin, kMin});
        else { // (fix24) full: the entry whose hold ended longest ago is replaced, so a new id is tracked (and backs off) like the first ones; never worse than c51's fixed hold
            size_t o = 0;
            for (size_t i = 1; i < e.size(); ++i)
                if (e[i].until < e[o].until) o = i;
            e[o] = Entry{texId, now + kMin, kMin};
        }
        ++starts;
        longest = std::max(longest, kMin);
        return kMin;
    }
};
}  // namespace vgx::swapback
