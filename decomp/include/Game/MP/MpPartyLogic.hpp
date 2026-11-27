// Galaxy 3.0 multiplayer: the pure part of the party strip and the team star celebration (owner: celebrate). No game headers, no
// global state: MpTeam.cpp (game glue) owns the instances; tests/mp/party_test.cpp and the ui test drive them directly.
//
//  - TALLY (0x3D): each console's own counters for the star just won (star bits, coins, "helped") and whether its player was near the
//    star. Sent once by every console when it learns of the STAR; the host relays it. Counters are local and only ever shown.
//  - Counter: the local counters, fed once a turn with the game's own coin / star bit numbers (only increases count).
//  - Ledger: the tallies of this scene serial, and the "TEAM STAR!" decision (everyone present was near the star).
//  - partyState / Flash: what each head of the party strip shows.
#pragma once

#include "Game/MP/MpPanelPlan.hpp"
#include "port/mp_proto.h"

namespace MpPartyLogic {

// ---- TALLY ------------------------------------------------------------------------------------------------------------------------
enum : unsigned { kMaxCount = 9999, kMaxHelped = 99, kTallyBytes = 8 };
struct Tally {
    uint16_t serial;       // the scene serial of the star (events of another serial are dropped)
    uint16_t starBits;     // star bits collected in this scene
    uint16_t coins;        // coins collected in this scene
    uint8_t helped;        // bubbles popped + friends warped to in this scene
    uint8_t nearStar;      // 1: this player was near the star when it was collected
};
static inline void writeTally(MpWriter& w, const Tally& t) {
    w.u16(t.serial); w.u16(t.starBits); w.u16(t.coins); w.u8(t.helped); w.u8(t.nearStar);
}
static inline bool readTally(const void* data, size_t len, Tally* t) {
    MpReader r(data, len);
    t->serial = r.u16(); t->starBits = r.u16(); t->coins = r.u16(); t->helped = r.u8(); t->nearStar = r.u8();
    return r.done() && t->starBits <= kMaxCount && t->coins <= kMaxCount && t->helped <= kMaxHelped && t->nearStar <= 1;
}

// ---- local counters ---------------------------------------------------------------------------------------------------------------
struct Counter {
    int lastCoins, lastBits;   // -1: no baseline yet
    unsigned coins, bits, helped;
    void reset() { lastCoins = lastBits = -1; coins = bits = helped = 0; }
    // Once a turn with the game's numbers: only increases count (a coin counter that resets, star bits fed to a Luma, lose nothing).
    void observe(int coinsNow, int bitsNow) {
        if (coinsNow < 0 || bitsNow < 0) return;
        if (lastCoins >= 0 && coinsNow > lastCoins) coins += (unsigned)(coinsNow - lastCoins);
        if (lastBits >= 0 && bitsNow > lastBits) bits += (unsigned)(bitsNow - lastBits);
        lastCoins = coinsNow, lastBits = bitsNow;
        if (coins > kMaxCount) coins = kMaxCount;
        if (bits > kMaxCount) bits = kMaxCount;
    }
    void help() { if (helped < kMaxHelped) helped++; }
    Tally tally(uint16_t serial, bool nearStar) const {
        return Tally{serial, (uint16_t)bits, (uint16_t)coins, (uint8_t)helped, (uint8_t)(nearStar ? 1 : 0)};
    }
};

// ---- near the star ----------------------------------------------------------------------------------------------------------------
// fix3: measured on the cooked stage data (StageObjInfo zone offsets + placement), Good Egg: every star sits on its own planet or arena,
// 700..2818 units from that planet's centre (Dino Piranha 1150: a teammate anywhere on the planet is within ~2300; King Kaliente's
// bath 2818, its arena edges (the ShootingStarArea corners) 3870..6310 from the star), and the nearest OTHER planet's centre is
// 7409 (the egg next to star 5) .. 17535 (Dino Piranha's planet from the fortress) away. 6000 takes in the whole arena of a star but
// the far rim of King Kaliente's bath, never a teammate on another planet. (The first guess, 3500, left out half of that bath.)
// The Observatory has no star; its domes are further apart than this, so two players at different domes are not "near".
constexpr float kNearRadius = 6000.0f;   // game units (Mario is about 150 tall)
static inline float nearDistance(const float a[3], const float b[3]) {   // -1: a bad position
    for (int i = 0; i < 3; i++) if (!mp_pose_float_ok(a[i]) || !mp_pose_float_ok(b[i])) return -1.0f;
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 > 0.0f ? __builtin_sqrtf(d2) : 0.0f;
}
static inline bool isNear(const float a[3], const float b[3]) {
    const float d = nearDistance(a, b);
    return d >= 0.0f && d <= kNearRadius;
}

// ---- the ledger of one star ---------------------------------------------------------------------------------------------------------
struct Ledger {
    uint16_t serial;
    bool have[MP_MAX_PLAYERS];
    Tally t[MP_MAX_PLAYERS];
    int by;   // the slot that collected the star, -1 not known
    void reset(uint16_t s) {
        serial = s;
        by = -1;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) have[i] = false;
    }
    // A tally from `slot` (the transport's sender, never the payload). False: another serial, or a repeat (the first one stays).
    bool note(int slot, const Tally& v) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS || v.serial != serial || have[slot]) return false;
        have[slot] = true;
        t[slot] = v;
        return true;
    }
    // TEAM STAR: two or more players present, every one of them sent a tally and was near the star.
    bool team(unsigned presentMask) const {
        int n = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!(presentMask & (1u << i))) continue;
            n++;
            if (!have[i] || !t[i].nearStar) return false;
        }
        return n >= 2;
    }
};

// ---- the party strip ---------------------------------------------------------------------------------------------------------------
// MP_POSE_BUBBLE: the bubble module's pose flag (a player floating in a bubble shows as the floating head).
constexpr uint16_t kPoseBubble = MP_POSE_BUBBLE;

static inline uint8_t partyState(bool havePose, uint16_t poseFlags, bool quiet) {
    if (havePose && kPoseBubble != 0 && (poseFlags & kPoseBubble)) return MpPanel::PARTY_BUBBLE;
    if (havePose && (poseFlags & MP_POSE_DEAD)) return MpPanel::PARTY_DOWN;
    if (quiet) return MpPanel::PARTY_QUIET;
    return MpPanel::PARTY_NORMAL;
}

// touches: the connection of one player as three bars (good / weak / lost), no numbers. From the core's own per-peer data: the round
// trip time (mp_slot_ping_ms, -1 unknown = good) and "quiet" (no packet for 3 s = lost). Weak from 250 ms, good again under 170 ms, so
// the bars do not flicker on a ping that hovers; lost clears at once when a packet comes. (The core's "quiet" flag is the lost verdict.)
constexpr int kWeakPingMs = 250, kGoodPingMs = 170;
struct LinkGauge {
    uint8_t state = MpPanel::LINK_GOOD;
    uint8_t update(int pingMs, bool quiet) {
        if (quiet) return state = MpPanel::LINK_LOST;
        if (state == MpPanel::LINK_LOST) state = MpPanel::LINK_GOOD;   // (a packet came: judged by the ping again, below)
        if (pingMs >= kWeakPingMs) state = MpPanel::LINK_WEAK;
        else if (pingMs >= 0 && pingMs < kGoodPingMs) state = MpPanel::LINK_GOOD;   // (-1: no ping yet, the state stays; a new gauge is good)
        return state;
    }
};

// The star flash of each head: starts when that player gets a Power Star, runs kFlashFrames logic turns.
constexpr int kFlashFrames = 110;
struct Flash {
    int age[MP_MAX_PLAYERS];   // -1 none
    void reset() { for (int& a : age) a = -1; }
    void start(int slot) { if (slot >= 0 && slot < MP_MAX_PLAYERS) age[slot] = 0; }
    void tick() { for (int& a : age) if (a >= 0 && ++a >= kFlashFrames) a = -1; }
    float progress(int slot) const {
        if (slot < 0 || slot >= MP_MAX_PLAYERS || age[slot] < 0) return -1.0f;
        return age[slot] / (float)kFlashFrames;
    }
};

// The strip's model: the other present players, in slot order (at most 3).
struct PartyInput {
    int localSlot;
    unsigned presentMask;
    uint8_t look[MP_MAX_PLAYERS];
    uint8_t state[MP_MAX_PLAYERS];    // MpPanel::PartyState
    float flash[MP_MAX_PLAYERS];      // -1 none, else 0..1
    uint8_t link[MP_MAX_PLAYERS];     // MpPanel::LinkQuality (touches)
    bool away[MP_MAX_PLAYERS];        // touches: "Be right back" / idle
};
// ingame: the life counter's own picture (PlayerLeft's PicMario) is Mario's or Luigi's head: every other character (Toad 2, Cosmic
// Mario 3, Yellow Toad 4, Cosmic Luigi 5) has its own head drawn over it.
static inline bool counterNeedsHead(uint8_t look) { return look >= 2 && look < MpPanel::kCharacters; }
// ingame: which picture the life counter shows: the game's own Mario or Luigi picture (PicMario, its "Luigi" animation), or none (the
// game's picture pane hidden and the character's own head drawn in its place, following that pane's place and bounce). Exactly one.
enum CounterPane : uint8_t { COUNTER_MARIO = 0, COUNTER_LUIGI = 1, COUNTER_OWN_HEAD = 2 };
static inline CounterPane counterPane(int look) {
    if (look == 1) return COUNTER_LUIGI;
    if (look >= 2 && look < MpPanel::kCharacters) return COUNTER_OWN_HEAD;
    return COUNTER_MARIO;   // Mario, and anything out of range
}
static inline void makeStrip(const PartyInput& in, float appear, MpPanel::Strip* out) {
    out->count = 0;
    out->appear = appear;
    out->localLook = -1;
    if (in.localSlot >= 0 && in.localSlot < MP_MAX_PLAYERS && (in.presentMask & (1u << in.localSlot)) && counterNeedsHead(in.look[in.localSlot]))
        out->localLook = in.look[in.localSlot];   // touches: Toad (2) / Yellow Toad (4), ingame: Cosmic Mario (3) / Cosmic Luigi (5): the counter's picture shows Mario or Luigi only
    for (int s = 0; s < MP_MAX_PLAYERS && out->count < MpPanel::kMaxStrip; s++) {
        if (s == in.localSlot || !(in.presentMask & (1u << s))) continue;
        MpPanel::StripMember& m = out->m[out->count++];
        m.look = in.look[s] < MpPanel::kCharacters ? in.look[s] : 0;
        m.state = in.state[s];
        m.flash = in.flash[s];
        m.link = in.link[s];
        m.away = in.away[s];
    }
}

// ingame: arrivals and departures without words. A player who joins slides into the strip (the head grows in from below, its
// neighbours make room); one who leaves slides out the same way, kept in the strip with their last look until gone. kSlideTurns
// each way. Call once a logic turn after makeStrip.
constexpr int kSlideTurns = 14;
struct StripMotion {
    float t[MP_MAX_PLAYERS];      // 0 out .. 1 in
    uint8_t look[MP_MAX_PLAYERS]; // the last look seen (a leaver keeps theirs)
    bool known = false;           // false: the first call takes everyone present as already in (a scene start, the session's start)
    void reset() { for (int s = 0; s < MP_MAX_PLAYERS; s++) { t[s] = 0.0f; look[s] = 0; } known = false; }
    void apply(const PartyInput& in, MpPanel::Strip* out) {
        const float d = 1.0f / (float)kSlideTurns;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            const bool here = s != in.localSlot && (in.presentMask & (1u << s));
            if (here) look[s] = in.look[s] < MpPanel::kCharacters ? in.look[s] : 0;
            if (!known) t[s] = here ? 1.0f : 0.0f;
            else t[s] = here ? (t[s] + d > 1.0f ? 1.0f : t[s] + d) : (t[s] - d < 0.0f ? 0.0f : t[s] - d);
            if (s == in.localSlot) t[s] = 0.0f;
        }
        known = true;
        // rebuild in slot order: present heads with their state (from makeStrip), leavers with a plain head
        MpPanel::StripMember made[MpPanel::kMaxStrip];
        const int madeCount = out->count;
        for (int i = 0; i < madeCount && i < MpPanel::kMaxStrip; i++) made[i] = out->m[i];
        int mi = 0;
        float pos = 0.0f;
        out->count = 0;
        for (int s = 0; s < MP_MAX_PLAYERS && out->count < MpPanel::kMaxStrip; s++) {
            if (s == in.localSlot) continue;
            const bool here = (in.presentMask & (1u << s)) != 0;
            if (!here && t[s] <= 0.0f) continue;
            MpPanel::StripMember m;
            if (here && mi < madeCount) {
                m = made[mi++];
            } else {
                m.look = look[s];
                m.state = MpPanel::PARTY_NORMAL;
                m.flash = -1.0f;
            }
            const float e = t[s];
            m.enter = e;
            m.pos = pos;
            pos += 1.0f - (1.0f - e) * (1.0f - e);   // (eased: the neighbours glide)
            out->m[out->count++] = m;
        }
    }
};

}  // namespace MpPartyLogic
