// Galaxy 3.0 multiplayer: the team Star Bit count, pure logic (owner: costar). No game headers and no global state: the game side is
// MpStarBits.cpp, the host tests are tests/mp/starbits_test.cpp.
//
// Play Together shares ONE Star Bit count. It is the game's own count (MR::getStarPieceNum: the in-galaxy count, or the file's stock in
// the Observatory), so the HUD counter, the Hungry Lumas, the 1-Up at each 50 and the end-of-stage reflection into the file all keep
// working natively; this module only keeps that number equal on every console.
//
//   Wire (rides in TALLY, 0x3D, whose old form is exactly 8 bytes; these two have other lengths so nothing else changes):
//     DELTA  guest -> host, reliable, 7 bytes:  serial u16, kind u8 = 1, seq u16, delta s16.
//            "my Star Bit count changed by delta" (coalesced: at most one per kBatchTurns). seq counts this sender's DELTAs of the
//            scene from 1, so a repeat (re-send after a host migration, relay echo) is applied once.
//     TOTAL  host -> everyone, reliable, 16 bytes:  serial u16, kind u8 = 2, epoch u8, rev u16, total u16, acks u16[4].
//            The authoritative count, the host's epoch (host migration counter, low 8 bits), a revision (+1 per change), and for every
//            slot the highest DELTA seq already folded into the total. Sent when the total changed (at most one per kBatchTurns)
//            and as a beacon every kBeaconTurns, and at once when someone joined. A guest shows
//            total + its own DELTAs the host has not acknowledged yet (so its own pick-ups never wait for the network).
//   Host loss: every console keeps the last TOTAL. The new host starts from its last TOTAL plus its own unacknowledged DELTAs, bumps the
//   epoch and broadcasts; guests accept a newer epoch at once, drop what the new host acknowledges and re-send the rest.
//   (A DELTA the old host applied in its last ~100 ms before it vanished, and that the new host never heard of, is lost: everyone
//   then agrees on the new host's number.)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "port/mp_proto.h"

namespace MpStarBitsLogic {

enum : uint8_t { kKindDelta = 1, kKindTotal = 2 };
enum { kDeltaBytes = 7, kTotalBytes = 16 };
static constexpr int kCapGalaxy = 999;      // the game's in-galaxy count clamp
static constexpr int kCapStock = 9999;      // the file's stock (the Observatory); the game stops counting there
static constexpr int kMaxMsgDelta = 100;    // |delta| a DELTA may carry
static constexpr uint32_t kBatchTurns = 6;      // 100 ms at 60 turns/s
static constexpr uint32_t kBeaconTurns = 120;   // 2 s
static constexpr uint32_t kResendTurns = 180;   // a DELTA nobody acknowledged for 3 s is sent again
static constexpr int kRing = 16;                // unacknowledged DELTAs a guest keeps
// A host accepts at most this much movement per slot: a bucket of kBucketMax, refilled kBucketRefillPer2Turns per 2 turns (~120 / s).
static constexpr int kBucketMax = 600;
static constexpr int kBucketRefillPer2Turns = 4;
// Spending (negative DELTAs: shooting a bit is -1, a Hungry Luma's meal a few at a time) is bounded much tighter than collecting: what one
// player can really spend is ~a few Star Bits a second (the pointer fires about 4 a second), a Luma's feeding a bit more. A peer that only
// subtracts cannot drain the team: 200 burst, then 30 / s.
static constexpr int kSpendMax = 200;
static constexpr int kSpendRefillPer2Turns = 1;

static inline bool serialNewer(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }
static inline bool epochNewer(uint8_t a, uint8_t b) { return (int8_t)(uint8_t)(a - b) > 0; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

struct Delta {
    uint16_t serial;
    uint16_t seq;
    int16_t delta;
};
struct Total {
    uint16_t serial;
    uint8_t epoch;
    uint16_t rev;
    uint16_t total;
    uint16_t acks[MP_MAX_PLAYERS];
};

static inline void writeDelta(MpWriter& w, const Delta& d) { w.u16(d.serial); w.u8(kKindDelta); w.u16(d.seq); w.s16(d.delta); }
static inline bool readDelta(const void* data, size_t len, Delta* d) {
    if (len != kDeltaBytes) return false;
    MpReader r(data, len);
    d->serial = r.u16();
    const uint8_t kind = r.u8();
    d->seq = r.u16();
    d->delta = r.s16();
    return r.done() && kind == kKindDelta && d->seq != 0 && d->delta != 0 && d->delta >= -kMaxMsgDelta && d->delta <= kMaxMsgDelta;
}
static inline void writeTotal(MpWriter& w, const Total& t) {
    w.u16(t.serial); w.u8(kKindTotal); w.u8(t.epoch); w.u16(t.rev); w.u16(t.total);
    for (int i = 0; i < MP_MAX_PLAYERS; i++) w.u16(t.acks[i]);
}
static inline bool readTotal(const void* data, size_t len, Total* t) {
    if (len != kTotalBytes) return false;
    MpReader r(data, len);
    t->serial = r.u16();
    const uint8_t kind = r.u8();
    t->epoch = r.u8();
    t->rev = r.u16();
    t->total = r.u16();
    for (int i = 0; i < MP_MAX_PLAYERS; i++) t->acks[i] = r.u16();
    return r.done() && kind == kKindTotal && t->total <= kCapStock;
}
// What is this TALLY-typed payload? 0 = an old 8-byte tally (not ours), kKindDelta / kKindTotal, 0xFF = ours by length but malformed.
static inline uint8_t classify(const void* data, size_t len) {
    if (len == kDeltaBytes) { Delta d; return readDelta(data, len, &d) ? kKindDelta : 0xFF; }
    if (len == kTotalBytes) { Total t; return readTotal(data, len, &t) ? kKindTotal : 0xFF; }
    return len == 8 ? 0 : 0xFF;
}

// Remote changes are added to the game's own count in steps that stop at every multiple of 50, so the 1-Up rule (which looks at
// one add at a time) sees each crossing. cur/want = the game's count and the count to reach; returns the next signed step (0: done).
static inline int nextStep(int cur, int want) {
    if (want > cur) {
        const int toNext = 50 - (cur % 50);
        const int d = want - cur;
        return d < toNext ? d : toNext;
    }
    return want < cur ? want - cur : 0;
}

// ---- the host ---------------------------------------------------------------------------------------------------------------------
struct Host {
    uint16_t serial;
    uint8_t epoch;
    uint16_t rev;
    int total;
    int cap;
    uint16_t acks[MP_MAX_PLAYERS];
    uint32_t bucket[MP_MAX_PLAYERS];     // tokens (star bits of movement) per slot
    uint32_t bucketTurn[MP_MAX_PLAYERS];
    uint32_t spend[MP_MAX_PLAYERS];      // tokens for spending (negative deltas) per slot
    uint32_t spendTurn[MP_MAX_PLAYERS];
    uint32_t contrib[MP_MAX_PLAYERS];    // positive movement accepted per slot (statistics)
    bool dirty;
    bool forceNext;
    uint32_t lastSendTurn;
    bool valid;

    // startRev: the revision this host's first TOTAL carries. The caller continues the count of the host that was (across scenes and
    // re-inits in one epoch), so a TOTAL after a re-init is never "not newer" than one a guest took before it.
    void reset(uint16_t ser, uint8_t ep, int cap_, int startTotal, uint32_t turn, uint16_t startRev = 1) {
        serial = ser; epoch = ep; rev = startRev; cap = cap_; total = clampi(startTotal, 0, cap_);
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            acks[i] = 0; bucket[i] = kBucketMax; bucketTurn[i] = turn; contrib[i] = 0; spend[i] = kSpendMax; spendTurn[i] = turn;
        }
        dirty = true; forceNext = true; lastSendTurn = turn; valid = true;
    }
    // The same scene's count read again (the host first had no scene, or its first read was wrong): the guests' acknowledged DELTAs stay
    // acknowledged (a DELTA is never applied twice) and the revision goes on, so every guest takes the corrected TOTAL.
    void reinit(int cap_, int startTotal, uint32_t turn) {
        cap = cap_; total = clampi(startTotal, 0, cap_);
        rev++; dirty = true; forceNext = true; lastSendTurn = turn; valid = true;
    }
    enum Result { APPLIED, DUPLICATE, REJECTED_SCENE, REJECTED_SLOT, RATE_LIMITED };
    static void refillOne(uint32_t& tokens, uint32_t& since, uint32_t turn, uint32_t maxTokens, uint32_t per2) {
        const uint32_t dt = turn - since;
        if (dt == 0) return;
        if (dt > 0x7FFFFFFFu) { since = turn; return; }   // a turn counter that went backwards
        const uint32_t pairs = dt / 2;                    // (an odd turn stays for the next call)
        since += pairs * 2;
        const uint64_t v = (uint64_t)tokens + (uint64_t)pairs * per2;
        tokens = v >= (uint64_t)maxTokens ? maxTokens : (uint32_t)v;
        if (tokens == maxTokens) since = turn;
    }
    void refill(int slot, uint32_t turn) {
        refillOne(bucket[slot], bucketTurn[slot], turn, (uint32_t)kBucketMax, (uint32_t)kBucketRefillPer2Turns);
        refillOne(spend[slot], spendTurn[slot], turn, (uint32_t)kSpendMax, (uint32_t)kSpendRefillPer2Turns);
    }
    void move(int delta, int slot) {
        const int before = total;
        total = clampi(total + delta, 0, cap);
        if (total != before) { rev++; dirty = true; }
        if (delta > 0 && slot >= 0 && slot < MP_MAX_PLAYERS) contrib[slot] += (uint32_t)delta;
    }
    // A DELTA from a guest. A repeat (seq not newer than what is acknowledged) is a no-op; a seq that jumps ahead is taken (the reliable
    // channel does not lose a DELTA; after a migration the gap is the old host's business). Over the rate: acknowledged and dropped.
    Result applyDelta(int slot, const Delta& d, uint32_t turn) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS) return REJECTED_SLOT;
        if (!valid || d.serial != serial) return REJECTED_SCENE;
        if ((int16_t)(uint16_t)(d.seq - acks[slot]) <= 0) return DUPLICATE;
        acks[slot] = d.seq;
        rev++;            // (the acks changed: a TOTAL that differs in anything is a newer revision)
        dirty = true;
        refill(slot, turn);
        const uint32_t cost = (uint32_t)(d.delta < 0 ? -d.delta : d.delta);
        if (cost > bucket[slot]) return RATE_LIMITED;
        if (d.delta < 0) {
            if (cost > spend[slot]) return RATE_LIMITED;   // (and never below what the team has: move() stops at 0)
            spend[slot] -= cost;
        }
        bucket[slot] -= cost;
        move(d.delta, slot);
        return APPLIED;
    }
    // The host's own pick-ups.
    void applyLocal(int delta, int slot) { if (valid && delta) move(delta, slot); }
    void noteJoin() { forceNext = true; }
    // True = send a TOTAL now (fills *out).
    bool poll(uint32_t turn, Total* out) {
        if (!valid) return false;
        const uint32_t since = turn - lastSendTurn;
        const bool due = forceNext || (dirty && since >= kBatchTurns) || since >= kBeaconTurns;
        if (!due) return false;
        lastSendTurn = turn;
        dirty = false;
        forceNext = false;
        out->serial = serial; out->epoch = epoch; out->rev = rev; out->total = (uint16_t)total;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) out->acks[i] = acks[i];
        return true;
    }
};

// ---- a guest ----------------------------------------------------------------------------------------------------------------------
struct Guest {
    struct Sent {
        uint16_t seq;
        int16_t delta;
        uint32_t turn;       // last (re)send
        bool resend;         // send again at the next poll
    };
    uint16_t serial;
    bool known;              // a TOTAL of this scene was accepted
    int hostTotal;
    uint16_t rev;
    uint8_t epoch;
    uint16_t acks[MP_MAX_PLAYERS];
    uint16_t lastSeq;        // the last seq used
    int pending;             // collected, not yet in a DELTA
    uint32_t lastSendTurn;
    Sent ring[kRing];
    int n;                   // used entries (oldest first)
    int cap;
    bool valid;

    void reset(uint16_t ser, int cap_, uint32_t turn) {
        serial = ser; known = false; hostTotal = 0; rev = 0; epoch = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) acks[i] = 0;
        lastSeq = 0; pending = 0; lastSendTurn = turn; n = 0; cap = cap_; valid = true;
    }
    // The local player's count changed by delta (already clamped by the game's own count).
    void localAdd(int delta) {
        if (!valid) return;
        pending = clampi(pending + delta, -kCapStock, kCapStock);
    }
    int unacked() const {
        int s = pending;
        for (int i = 0; i < n; i++) s += ring[i].delta;
        return s;
    }
    // What this console should show; only meaningful when known.
    int view() const { return clampi(hostTotal + unacked(), 0, cap); }

    // Send at most one DELTA: new (pending, when the batch time passed and the ring has room) or a due re-send. Returns true and fills *out.
    bool poll(uint32_t turn, Delta* out) {
        if (!valid) return false;
        for (int i = 0; i < n; i++) {
            if (ring[i].resend || (turn - ring[i].turn >= kResendTurns && turn - ring[i].turn < 0x7FFFFFFFu)) {
                ring[i].resend = false;
                ring[i].turn = turn;
                out->serial = serial; out->seq = ring[i].seq; out->delta = ring[i].delta;
                return true;
            }
        }
        if (pending != 0 && turn - lastSendTurn >= kBatchTurns && n < kRing) {
            int d = clampi(pending, -kMaxMsgDelta, kMaxMsgDelta);
            pending -= d;
            lastSeq = (uint16_t)(lastSeq + 1 == 0 ? 1 : lastSeq + 1);
            ring[n++] = Sent{lastSeq, (int16_t)d, turn, false};
            lastSendTurn = turn;
            out->serial = serial; out->seq = lastSeq; out->delta = (int16_t)d;
            return true;
        }
        return false;
    }
    // A TOTAL from the host. Returns true when taken. myslot = this console's slot.
    bool onTotal(const Total& t, int myslot, uint32_t turn) {
        (void)turn;
        if (!valid || t.serial != serial || myslot < 0 || myslot >= MP_MAX_PLAYERS) return false;
        const bool newEpoch = !known || epochNewer(t.epoch, epoch);
        if (!newEpoch && !(t.epoch == epoch && serialNewer(t.rev, rev))) return false;   // old or repeated
        const bool migrated = known && t.epoch != epoch;
        known = true;
        epoch = t.epoch; rev = t.rev;
        hostTotal = clampi(t.total, 0, kCapStock);   // (the scene's own cap is applied by view(): a TOTAL may come before this console's scene exists)
        for (int i = 0; i < MP_MAX_PLAYERS; i++) acks[i] = t.acks[i];
        if (t.acks[myslot] != 0 && (lastSeq == 0 || (int16_t)(uint16_t)(t.acks[myslot] - lastSeq) > 0)) lastSeq = t.acks[myslot];
        // drop what the host has folded in
        int w = 0;
        for (int i = 0; i < n; i++) {
            if ((int16_t)(uint16_t)(ring[i].seq - t.acks[myslot]) > 0) ring[w++] = ring[i];
        }
        n = w;
        // a new host: everything it does not acknowledge goes out again
        if (migrated) for (int i = 0; i < n; i++) ring[i].resend = true;
        return true;
    }
    // This console is the new host: build the authoritative state from what it knows. The ring is folded in (the other guests
    // re-send theirs). newEpoch = the migration counter's low 8 bits.
    void becomeHost(Host* h, int myslot, uint8_t newEpoch, uint32_t turn) {
        const int t = clampi(hostTotal + unacked(), 0, cap);   // (not known yet: just what this console collected)
        h->reset(serial, newEpoch, cap, t, turn);
        for (int i = 0; i < MP_MAX_PLAYERS; i++) h->acks[i] = acks[i];
        if (myslot >= 0 && myslot < MP_MAX_PLAYERS) h->acks[myslot] = (uint16_t)(lastSeq);
        h->rev = (uint16_t)(rev + 1);
        // epoch must look newer than the last one we accepted
        if (!epochNewer(newEpoch, epoch) && newEpoch == epoch) h->epoch = (uint8_t)(epoch + 1);
        n = 0;
        pending = 0;
    }
};

// The order at a scene change. The scene serial changes while the old scene still exists (its Star Bit count is the OLD scene's), so nothing
// may be initialised, sent or shown until the old scene has ended (sceneEnd: init/waiting cleared) and the new one exists.
constexpr uint32_t kSceneWaitMax = 900;   // turns: if sceneEnd never comes, go on
enum GateAct { GATE_IDLE = 0, GATE_START = 1 };
inline GateAct gateStep(bool& init, bool& waiting, uint16_t& serial, uint32_t& waitTurn, uint16_t cur, uint32_t turn) {
    if (init && cur != serial) { init = false; waiting = true; waitTurn = turn; return GATE_IDLE; }
    if (waiting && turn - waitTurn < kSceneWaitMax) return GATE_IDLE;
    if (!init) { waiting = false; init = true; serial = cur; return GATE_START; }
    return GATE_IDLE;
}

}  // namespace MpStarBitsLogic
