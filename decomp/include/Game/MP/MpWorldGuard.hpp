// Galaxy 3.0 multiplayer: plausibility rules for a guest's world events, applied on the room leader's console before it applies and
// relays them (security review finding 12). Pure logic, no game headers: host tests in tests/mp/world_guard_test.cpp.
//
// Principles: no false refusals in honest play. Every ceiling is several times what a real player can produce; the distance limit is a
// generous bound (the harness logs the measured distances: "world plaus ..." lines) and can be tuned with SMG_MP_PLAUS_DIST on the desktop.
#pragma once

#include <math.h>
#include <stdint.h>

namespace MpWorldGuard {

constexpr int kSlots = 4;
enum Kind : uint8_t { K_SWITCH = 0, K_KILL, K_PURPLE, K_CHIP, K_STAR, K_COUNT };

// ---- per sender, per event type rate limits (token buckets in logic turns, 60 / s) ----------------------------------------------
// What a real player produces: switches: a few a second at most (a cutscene or a tile floor can flip a handful at once: burst 64);
// kills: a spin or a bomb chain kills a handful at once (burst 40, 10 / s sustained); purple coins and chips: about 2 a second while
// running through a trail (burst 24, 8 / s); stars: one every few seconds at the very most (burst 4, 1 / s; a repeat is a ledger duplicate).
struct Limit { uint16_t burst; uint16_t perSecond; };
constexpr Limit kLimit[K_COUNT] = {{64, 30}, {40, 10}, {24, 8}, {24, 8}, {4, 1}};

struct Bucket {
    uint32_t tokens = 0, turn = 0;
    bool init = false;
};

class SenderLimits {
public:
    void reset() { *this = SenderLimits(); }
    // one event of `kind` from `slot` at logic turn `turn`: false = over the ceiling (drop it, count it)
    bool allow(int slot, Kind kind, uint32_t turn) {
        if (slot < 0 || slot >= kSlots || kind >= K_COUNT) return false;
        Bucket& b = mB[slot][kind];
        const Limit& l = kLimit[kind];
        if (!b.init) { b.init = true; b.tokens = l.burst; b.turn = turn; }
        const uint32_t dt = turn - b.turn;
        if (dt > 0x7FFFFFFFu) b.turn = turn;   // a turn counter that went backwards
        else if (dt != 0) {
            const uint64_t v = (uint64_t)b.tokens + (uint64_t)dt * l.perSecond / 60u;
            // (the fraction is kept by moving the clock only by what was paid for)
            const uint32_t paid = (uint32_t)(((uint64_t)(v - b.tokens) * 60u + l.perSecond - 1u) / l.perSecond);
            b.tokens = v >= l.burst ? l.burst : (uint32_t)v;
            b.turn = b.tokens == l.burst ? turn : b.turn + (paid > dt ? dt : paid);
        }
        if (b.tokens == 0) return false;
        b.tokens--;
        return true;
    }
private:
    Bucket mB[kSlots][K_COUNT];
};

// ---- where the sender is -------------------------------------------------------------------------------------------------------
// The distance between the sender's last known pose and the object. Generous: a launch-star flight, a long jump, a Star Bit shot, a bomb
// chain and the network delay (a second of travel at launch-star speed is ~1500 units) must never trip it. Default 30000 units: only
// nonsense (the far end of another galaxy's coordinates) is refused until pair-run traces say more.
constexpr float kDefaultMaxDistance = 30000.0f;
static inline bool nearEnough(const float senderPos[3], const float objPos[3], float maxDist) {
    const float dx = senderPos[0] - objPos[0], dy = senderPos[1] - objPos[1], dz = senderPos[2] - objPos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 == d2 && d2 <= maxDist * maxDist;   // (NaN is never near)
}

// ---- telling the core (smallfix) -----------------------------------------------------------------------------------------------
// A refusal is also reported to the core (mp_core_report_bad), which counts it like a malformed message: 50 in 10 s removes the player.
// Only the room leader reports (a guest's report is ignored anyway). Every rate refusal is reported (an honest console is never that fast).
// A "far" refusal is reported only past twice the limit (a guest's last pose can be a pose interval old, or missing after a hiccup).
// Never report a REGCRC mismatch (a reload legitimately differs). `why` is a short constant: no names, no values.
constexpr const char* kWhyRate = "world rate";
constexpr const char* kWhyFar = "world far";
static inline bool farEnoughToReport(const float senderPos[3], const float objPos[3], float maxDist) {
    const float dx = senderPos[0] - objPos[0], dy = senderPos[1] - objPos[1], dz = senderPos[2] - objPos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float lim = 2.0f * maxDist;
    return d2 == d2 && d2 > lim * lim;   // (NaN: refused, but not reported)
}
// `report(slot, why)` is mp_core_report_bad in the game and a fake in the tests. Returns true when it was called.
template <typename Report> static inline bool reportRate(bool leader, int slot, Report report) {
    if (!leader || slot < 0 || slot >= kSlots) return false;
    report(slot, kWhyRate);
    return true;
}
template <typename Report>
static inline bool reportFar(bool leader, int slot, const float senderPos[3], const float objPos[3], float maxDist, Report report) {
    if (!leader || slot < 0 || slot >= kSlots || !farEnoughToReport(senderPos, objPos, maxDist)) return false;
    report(slot, kWhyFar);
    return true;
}

// ---- REGCRC answers ------------------------------------------------------------------------------------------------------------
// A guest's REGCRC (6 bytes) is answered with the leader's REGCRC and a full SWITCHMAP (~570 bytes). Answered at most once a second per
// guest and once per placement unless the guest's CRC changed (a repeat of the same CRC for the same placement is a request for nothing new).
constexpr int64_t kReplyMinGapNs = 1000000000;
class ReplyGate {
public:
    void reset() { *this = ReplyGate(); }
    void newPlacement() { for (int i = 0; i < kSlots; i++) { mAnswered[i] = false; mRepairInit[i] = false; } }
    void forget(int slot) { if (slot >= 0 && slot < kSlots) mAnswered[slot] = false; }
    void resetSlot(int slot) { if (slot >= 0 && slot < kSlots) { mAnswered[slot] = false; mRepairInit[slot] = false; } }
    // worldsync: a repair request (REGCRC with the repair flag: "my state differs, resend it") is answered at most once a second per guest, from an
    // allowance of kMaxRepairs that starts full at each placement and refills 1 per kRepairRefillNs (10 s). (Before: a repeat REGCRC with the same
    // CRC was never answered again, so a repair request got nothing.)
    enum { kMaxRepairs = 8 };
    static constexpr int64_t kRepairRefillNs = 10000000000LL;
    bool allowRepair(int slot, int64_t nowNs) {
        if (slot < 0 || slot >= kSlots) return false;
        if (!mRepairInit[slot]) { mRepairInit[slot] = true; mRepairTokens[slot] = kMaxRepairs; mRepairAt[slot] = nowNs; mLastRepairNs[slot] = nowNs - kReplyMinGapNs; }
        if (nowNs < mRepairAt[slot]) mRepairAt[slot] = nowNs;   // (clock stepped back)
        const int64_t add = (nowNs - mRepairAt[slot]) / kRepairRefillNs;
        if (add > 0) {
            mRepairTokens[slot] = (int)(mRepairTokens[slot] + add > (int64_t)kMaxRepairs ? (int64_t)kMaxRepairs : mRepairTokens[slot] + add);
            mRepairAt[slot] += add * kRepairRefillNs;
        }
        if (mRepairTokens[slot] <= 0) return false;
        if (nowNs - mLastRepairNs[slot] < kReplyMinGapNs && nowNs >= mLastRepairNs[slot]) return false;
        mRepairTokens[slot]--;
        mLastRepairNs[slot] = nowNs;
        return true;
    }
    bool allow(int slot, uint32_t crc, int64_t nowNs) {
        if (slot < 0 || slot >= kSlots) return false;
        if (mAnswered[slot]) {
            if (crc == mCrc[slot]) return false;                         // once per placement unless the CRC changed
            if (nowNs - mLastNs[slot] < kReplyMinGapNs && nowNs >= mLastNs[slot]) return false;   // and never more than 1 / s
        }
        mAnswered[slot] = true;
        mCrc[slot] = crc;
        mLastNs[slot] = nowNs;
        return true;
    }
private:
    bool mAnswered[kSlots] = {};
    uint32_t mCrc[kSlots] = {};
    int64_t mLastNs[kSlots] = {};
    bool mRepairInit[kSlots] = {};
    int mRepairTokens[kSlots] = {};
    int64_t mRepairAt[kSlots] = {};
    int64_t mLastRepairNs[kSlots] = {};
};

}  // namespace MpWorldGuard
