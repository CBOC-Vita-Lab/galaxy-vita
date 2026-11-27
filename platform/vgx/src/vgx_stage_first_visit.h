#pragma once

#include "vgx_stage_room.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vgx::firstvisit {
// Stage records can refer to shader sources that have not been registered yet.
// Register() is a notification only; TakeReady() is called by the GL owner under the wipe.
struct Variant {
    uint32_t vs, fs, lb;
    uint8_t tier;
};
constexpr uint32_t missing = 0x10000;
// The record word (a stage record's lb, in memory and in the stage file), bit by bit:
//   0..12   blend key, 13..24 vertex layout mask: the real draw (drawMask). Every reader takes the draw from these bits alone.
//   25      can draw (seedrun31 A4, canDrawTag): the stage's loaded materials can make this draw; no visit was seen making it. Written only by a harvest run
//           (GXV_SEEDWALK=1 with GXV_STAGE_ENUM) and kept apart from the stage's own records: a tier of its own behind the whole queue (CanDrawGate).
//   26..27  menusplit's class (vgx_menu_split.h), menu records only.
//   28      slow tag: this draw's first link was costly (learned in play, LearnSlow, or set by the seed tools): queued first at the stage's next load.
//   29..31  scenario hint (seedrun31 A3): the scenario (1..7) the draw was seen in; 0 = any or not known (every record before seedrun31).
// Readers before seedrun31 ignore bits 25 and 28..31 (they take a can-draw record for an own record and a hinted record for an unhinted one).
constexpr uint32_t drawMask = 0x1ffffffu;
constexpr uint32_t canDrawTag = 1u << 25;
constexpr uint32_t slowTag = 1u << 28;
constexpr unsigned scenarioShift = 29;
constexpr uint32_t scenarioMask = 7u << scenarioShift;
constexpr unsigned slowLimit = 12;
inline bool LearnSlow(double ms, unsigned count) { return ms >= 100.0 && count < slowLimit; }
inline unsigned ScenarioOf(uint32_t lb) { return (lb & scenarioMask) >> scenarioShift; }
// The hint bits of a scenario number as the game counts them (1..7); any other value (none, -1, 8 and above) is "any".
inline uint32_t ScenarioBits(int scenario) { return scenario >= 1 && scenario <= 7 ? static_cast<uint32_t>(scenario) << scenarioShift : 0u; }
// A record belongs to the load of scenario `cur` when it has no hint, when the scenario being loaded is not known (cur 0), or when the two are equal.
inline bool ScenarioMatch(uint32_t lb, unsigned cur) {
    const unsigned s = ScenarioOf(lb);
    return s == 0 || cur == 0 || s == cur;
}
inline bool CanDraw(uint32_t lb) { return (lb & canDrawTag) != 0; }
// A record that goes in front of the queue at a load of scenario `cur` (if its program is not linked): slow-tagged, of this scenario or of any.
// This is the whole rule: there are no built-in priority hashes (C182 / C183's nine historical records are tags in the packaged list now: tools/seedrun/stagetags.py).
inline bool Priority(uint32_t lb, unsigned cur) { return (lb & slowTag) && !CanDraw(lb) && ScenarioMatch(lb, cur); }
// The order a stage's records are queued in: those of the scenario being loaded (or of any) first, then those of other scenarios, each part in record order.
// `order` gets the indices; the result is how many of them are of the first part. With cur 0 (not known) or no hints this is the record order itself.
template <class Rec>
size_t ScenarioFirst(const std::vector<Rec>& recs, unsigned cur, std::vector<uint32_t>& order) {
    order.clear();
    order.reserve(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) if (ScenarioMatch(static_cast<uint32_t>(recs[i].lb), cur)) order.push_back(static_cast<uint32_t>(i));
    const size_t first = order.size();
    for (size_t i = 0; i < recs.size(); ++i) if (!ScenarioMatch(static_cast<uint32_t>(recs[i].lb), cur)) order.push_back(static_cast<uint32_t>(i));
    return first;
}
// (GXV_STAGEQ_SCENARIO) The label a draw's new record gets, from what the stage's list holds of the same draw already: unhinted (haveAny), with the
// scenario being played (haveCur), with another scenario only (haveOther). -1: nothing to add. A draw seen in a second scenario gets an unhinted record
// (so a draw is held at most twice: its first scenario's, then "any"); cur 0 (not known) records as before seedrun31 (unhinted).
inline int RecordLabel(unsigned cur, bool haveAny, bool haveCur, bool haveOther) {
    if (haveAny) return -1;
    if (cur == 0 || cur > 7) return 0;
    if (haveCur) return -1;
    return haveOther ? 0 : static_cast<int>(cur);
}
// The head of a stage's queue (its first `seg` entries: hot, own, player) as the build leaves it, both moves stable: entries whose program is not linked first
// (GXV_STAGEWARM_OWNFIRST, `ownFirst`), then the priority variants (`prio`: the build's g_stageFirstIds and still open) in front of all. Returns the number of
// priority entries now at the front. Only the order changes: nothing is added or dropped.
template <class Q, class Unlinked, class Prio>
size_t ArrangeHead(Q& q, size_t seg, bool ownFirst, bool anyPrio, Unlinked unlinked, Prio prio) {
    seg = std::min(seg, static_cast<size_t>(q.size()));
    if (ownFirst) std::stable_partition(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(seg), unlinked);
    if (!anyPrio) return 0;
    return static_cast<size_t>(std::stable_partition(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(seg), prio) - q.begin());
}
// The stage's own records of other scenarios go behind the player list when the player list is part of the head (GXV_PLAYERWARM_HOLD): Mario's actions are drawn
// in the first seconds of any scenario, another scenario's draws maybe never. `at`: where the other scenarios' block starts, `n` its length, `m` the player list's.
template <class Q>
void OthersBehindPlayer(Q& q, size_t at, size_t n, size_t m) {
    if (!n || !m || at + n + m > static_cast<size_t>(q.size())) return;
    std::rotate(q.begin() + static_cast<std::ptrdiff_t>(at), q.begin() + static_cast<std::ptrdiff_t>(at + n), q.begin() + static_cast<std::ptrdiff_t>(at + n + m));
}
// (seedrun31 A4) What the can-draw tier does with its front entry. It runs only once the stage's whole queue is done, in frames whose screen is closed for something
// else (the game's own load, or a hold that waits for other work): it never extends a hold, never makes room (no eviction), never compiles (a variant whose shaders
// no cache has is passed over: its draw pays the compile, as without the tier), and stops for good at the slot reserve (what does not fit stays compiled, unlinked).
enum class CanDrawStep : uint8_t { Wait, Take, Skip };
struct CanDrawIn {
    bool queueEmpty = true;  // the stage's queue (hot, own, player, menu, shared) has nothing left
    bool live = false;       // the picture is shown (play)
    bool covered = false;    // the screen is closed for the game's load or held for other work
    bool linked = false;     // the variant's program is linked (a new layout / blend of it: the patcher's work only)
    bool cached = false;     // both of its shaders are in a cache (a link is a file read)
};
inline CanDrawStep CanDrawGate(const CanDrawIn& in) {
    if (!in.queueEmpty || in.live || !in.covered) return CanDrawStep::Wait;
    if (in.linked || in.cached) return CanDrawStep::Take;
    return CanDrawStep::Skip;
}
// (c183fix F5) How a variant resolved late in the visit joins the stage's queue. Own, player and seed records go to the front (C183's rule). A menu-tier record
// follows the build's menu rule: one variant per program and none for a program the queue already holds, at the back (the build put the menu tier behind
// the stage's own and player records). Skip: nothing to add.
enum class Late { Skip, Front, Back };
inline Late LatePlace(uint8_t tier, bool programQueued) {
    if (tier == stageroom::kMenu) return programQueued ? Late::Skip : Late::Back;
    return Late::Front;
}
// The tier a late record leaves on its program. C183's rule for own and player records (own wins; player replaces anything but own), and a menu record
// only raises a program's protection (never takes a player's, hot's or own program down to the menu's class).
inline uint8_t LateTier(uint8_t have, uint8_t late) {
    if (have == stageroom::kOwn) return have;
    if (late == stageroom::kOwn || late == stageroom::kPlayer) return late;
    return stageroom::Class(late) > stageroom::Class(have) ? late : have;
}
// Whether the late record joins the hold's "own open" set (C183's rule: own ones, and player ones with GXV_PLAYERWARM_HOLD; a menu record never).
inline bool LateHolds(uint8_t tier, bool playerHold) { return tier == stageroom::kOwn || (tier == stageroom::kPlayer && playerHold); }
inline uint64_t Identity(uint32_t vs, uint32_t fs, uint32_t lb) {
    return (uint64_t(vs) << 48) | (uint64_t(fs) << 32) | (lb & drawMask);
}
class Queue {
    using Links = std::unordered_multimap<uint64_t, size_t>;
    Links links[2]; // by stage kind (VS/FS), full 64-bit key hash (seedrun31: the low-32-bit links of the built-in player seed are gone with it)
    std::vector<Variant> held;
    std::vector<size_t> ready;
    std::unordered_set<uint64_t> seen;
    size_t unresolved = 0;
public:
    void Clear() {
        for (auto& map : links) map.clear();
        held.clear(); ready.clear(); seen.clear(); unresolved = 0;
    }
    bool First(uint32_t vs, uint32_t fs, uint32_t lb) { return seen.insert(Identity(vs, fs, lb)).second; }
    bool Seen(uint32_t vs, uint32_t fs, uint32_t lb) const { return seen.count(Identity(vs, fs, lb)) != 0; } // (the can-draw tier: queued already, without claiming it)
    void Wait(uint64_t vsKey, uint64_t fsKey, Variant v) {
        const size_t i = held.size();
        if (v.vs == missing) links[0].emplace(vsKey, i);
        if (v.fs == missing) links[1].emplace(fsKey, i);
        held.push_back(v);
        ++unresolved;
    }
    void Register(bool fragment, uint64_t key, uint32_t id) {
        if (!unresolved || id >= missing) return;
        auto& map = links[fragment];
        if (map.empty()) return;
        auto range = map.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) {
            Variant& v = held[it->second];
            (fragment ? v.fs : v.vs) = id;
            if (v.vs != missing && v.fs != missing) { ready.push_back(it->second); --unresolved; }
        }
        map.erase(range.first, range.second);
    }
    bool HasReady() const { return !ready.empty(); }
    size_t Waiting() const { return unresolved; }
    template<class Fn> void TakeReady(Fn take) {
        // Registration order/hash bucket order must not reorder the stage's own list behind player hints.
        std::sort(ready.begin(), ready.end());
        for (size_t i : ready) take(held[i]);
        ready.clear();
    }
};
} // namespace vgx::firstvisit
