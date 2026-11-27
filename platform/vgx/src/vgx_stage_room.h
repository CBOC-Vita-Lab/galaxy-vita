// hitch25 (GXV_STAGEQ_ROOM, default 0): which resident programs may give up their program slot for the stage's own programs, in what order. Pure, standard library only
// (the backend and tests/stageroom_model.cpp share it).
//
// c76 (console, 12 Nov): vitaGL has 1,024 program slots, the stage warm-up's queue stops 48 short of them, and every stage's queue holds 1,015-1,148 programs (its own
// records, the player list, the 600+ menu programs, the shared ones of other stages' lists). All of them are spared by the stage warm-up's eviction, so once about 970 programs are
// resident (from the 4th stage of a tour on) the queue's first entry that needs a slot is refused for good, the entries behind it never run, and the stage's own programs
// that were not resident (46-204 a stage) are linked by the draws that need them in play (56 in a 22-stage tour, 8-40 ms each). With the switch, a refused entry of the head
// of the queue (own, player, hot) makes room from the programs the stage listed only as filler: other stages' shared ones first, then the menu's.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vgx::stageroom {

enum Tier : uint8_t { kNone = 0, kOwn = 1, kPlayer = 2, kMenu = 3, kShared = 4, kHot = 5, kTierN = 6 };
inline const char* TierName(unsigned t) {
    static const char* const n[kTierN] = {"other", "own", "player", "menu", "shared", "hot"};
    return t < kTierN ? n[t] : "?";
}
// the head of the queue: what the first frames of the stage draw
inline bool IsHead(uint8_t t) { return t == kOwn || t == kPlayer || t == kHot; }

struct Cand {
    uint32_t key;
    uint8_t tier;
    uint64_t age;   // the frame of its last draw, or of its link when it was never drawn (smaller: older)
};
// Cost class: programs the stage's queue does not list at all go first, then the shared ones, then the menu's; the head is never given up.
inline int Class(uint8_t t) { return t == kNone ? 0 : t == kShared ? 1 : t == kMenu ? 2 : 3; }

// Picks up to `want` keys from `c` (which it reorders), oldest first within a class. The menu's programs only when allowMenu.
inline size_t Pick(std::vector<Cand>& c, size_t want, bool allowMenu, std::vector<uint32_t>& out) {
    out.clear();
    c.erase(std::remove_if(c.begin(), c.end(), [&](const Cand& x) { return IsHead(x.tier) || x.tier >= kTierN || (x.tier == kMenu && !allowMenu); }), c.end());
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) {
        const int ca = Class(a.tier), cb = Class(b.tier);
        if (ca != cb) return ca < cb;
        if (a.age != b.age) return a.age < b.age;
        return a.key < b.key;
    });
    for (size_t i = 0; i < c.size() && out.size() < want; ++i) out.push_back(c[i].key);
    return out.size();
}

// dead26: does the room-making have anything to do for a stage? Only a queue whose unlinked programs do not fit the free slots above the queue's reserve can be refused
// (`slack` programs are the back end's own links meanwhile). GXV_STAGEQ_MENUSPLIT=1 queues ~480 programs: then it does not.
inline bool Needed(unsigned unlinked, unsigned freeSlots, unsigned reserve, unsigned slack = 16) {
    return !(freeSlots > reserve && unlinked + slack <= freeSlots - reserve);
}
// dead26: GXV_STAGEQ_ROOM_HOLD_MS. The closed-screen hold of a stage whose room-making has acted ends after capMs (0: no cap); the rest of the queue goes on behind play.
inline bool HoldCapped(long capMs, bool roomActed, double heldMs) { return capMs > 0 && roomActed && heldMs >= static_cast<double>(capMs); }

}  // namespace vgx::stageroom
