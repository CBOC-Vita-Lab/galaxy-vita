// hitch25: host test of vgx_stage_room.h (which programs give up a slot for the stage's own).
#include <cstdio>
#include <cstdlib>
#include "../platform/vgx/src/vgx_stage_room.h"
using namespace vgx::stageroom;
static int fails = 0;
#define REQUIRE(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)
int main() {
    std::vector<uint32_t> out;
    {   // the head is never picked, whatever its age
        std::vector<Cand> c = {{1, kOwn, 1}, {2, kPlayer, 1}, {3, kHot, 1}};
        REQUIRE(Pick(c, 10, true, out) == 0 && out.empty(), "head is never given up");
    }
    {   // order: other, shared, menu; oldest first inside a class
        std::vector<Cand> c = {{10, kMenu, 5}, {11, kShared, 9}, {12, kNone, 50}, {13, kShared, 3}, {14, kMenu, 1}, {15, kNone, 7}};
        REQUIRE(Pick(c, 6, true, out) == 6, "all six");
        const uint32_t want[6] = {15, 12, 13, 11, 14, 10};
        for (int i = 0; i < 6; ++i) REQUIRE(out[i] == want[i], "class then age order");
    }
    {   // the menu's programs only on request; want caps the count
        std::vector<Cand> c = {{10, kMenu, 5}, {11, kShared, 9}, {12, kNone, 50}};
        REQUIRE(Pick(c, 10, false, out) == 2 && out[0] == 12 && out[1] == 11, "no menu programs without allowMenu");
        c = {{10, kMenu, 5}, {11, kShared, 9}, {12, kNone, 50}};
        REQUIRE(Pick(c, 1, true, out) == 1 && out[0] == 12, "want caps");
        c = {{10, kMenu, 5}};
        REQUIRE(Pick(c, 0, true, out) == 0, "want 0");
    }
    {   // ties: key order, deterministic; unknown tiers are never picked
        std::vector<Cand> c = {{9, kShared, 4}, {7, kShared, 4}, {8, kShared, 4}, {99, 77, 0}};
        REQUIRE(Pick(c, 5, true, out) == 3 && out[0] == 7 && out[1] == 8 && out[2] == 9, "ties by key, unknown tier skipped");
    }
    {   // the c76 situation in miniature: 970 resident of 976 usable slots, all of them listed by the queue; 40 head programs are not resident
        const size_t usable = 976;
        std::vector<Cand> res;      // resident programs
        for (uint32_t k = 0; k < 970; ++k) res.push_back({k, static_cast<uint8_t>(k < 150 ? kOwn : k < 200 ? kHot : k < 800 ? kMenu : kShared), 1000 + k});
        std::vector<uint32_t> head;
        for (uint32_t k = 5000; k < 5040; ++k) head.push_back(k);
        size_t linked = 0, evicted = 0, evictedHead = 0, menuEvicted = 0, left = 192;
        for (uint32_t k : head) {
            while (res.size() >= usable) {       // refused: make room as StageMakeRoom does (batch of min(open, 48), budget 192)
                const size_t open = head.size() - linked;
                std::vector<Cand> c = res;
                std::vector<uint32_t> pick;
                const size_t want = std::min(std::min<size_t>(open ? open : 1, 48), left);
                if (!Pick(c, want, true, pick)) break;
                for (uint32_t pk : pick) {
                    for (size_t i = 0; i < res.size(); ++i) if (res[i].key == pk) { if (IsHead(res[i].tier)) ++evictedHead; if (res[i].tier == kMenu) ++menuEvicted; res.erase(res.begin() + static_cast<long>(i)); break; }
                    ++evicted; --left;
                }
            }
            if (res.size() >= usable) break;
            res.push_back({k, kOwn, 9000 + k});
            ++linked;
        }
        REQUIRE(linked == 40, "all 40 head programs got a slot");
        REQUIRE(evictedHead == 0, "no head program was evicted");
        REQUIRE(evicted >= 34 && evicted <= 48, "about as many evicted as were missing");
        REQUIRE(menuEvicted == 0, "shared programs (170) were enough: the menu's were kept");
        // without room the first refusal ends it, as on the console
        size_t r = 970, l = 0;
        for (int i = 0; i < 40; ++i) { if (r >= usable) break; ++r; ++l; }
        REQUIRE(l == 6, "without room only 6 of the 40 link");
        // budget: with nothing but menu programs to give, the 192 budget caps the count
        left = 192;
        std::vector<Cand> menuOnly;
        for (uint32_t k = 0; k < 400; ++k) menuOnly.push_back({k, kMenu, k});
        size_t total = 0;
        while (left) { std::vector<Cand> c = menuOnly; std::vector<uint32_t> pick; const size_t want = std::min<size_t>(48, left); if (!Pick(c, want, true, pick)) break; menuOnly.erase(menuOnly.begin(), menuOnly.begin() + static_cast<long>(pick.size())); total += pick.size(); left -= pick.size(); }
        REQUIRE(total == 192, "the stage's budget caps the evictions");
    }
    // dead26: Needed (a queue that fits the slots needs no room-making: MENUSPLIT's ~480 programs) and HoldCapped (GXV_STAGEQ_ROOM_HOLD_MS)
    REQUIRE(!Needed(173, 400, 48), "173 unlinked programs fit 352 free slots above the reserve: no room needed");
    REQUIRE(Needed(173, 200, 48), "173 unlinked do not fit 152: room needed");
    REQUIRE(Needed(10, 48, 48) && Needed(10, 20, 48), "free slots at or under the reserve: room needed");
    REQUIRE(HoldCapped(300, true, 300.0) && !HoldCapped(300, true, 299.0) && !HoldCapped(300, false, 5000.0) && !HoldCapped(0, true, 5000.0), "HoldCapped");
    REQUIRE(IsHead(kOwn) && IsHead(kPlayer) && IsHead(kHot) && !IsHead(kMenu) && !IsHead(kShared) && !IsHead(kNone), "IsHead");
    REQUIRE(TierName(kHot)[0] == 'h' && TierName(200)[0] == '?', "TierName");
    std::puts(fails ? "stageroom_model: FAILED" : "stageroom_model: ok");
    return fails ? 1 : 0;
}
