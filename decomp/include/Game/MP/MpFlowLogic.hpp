// Galaxy 3.0 multiplayer: the pure part of the game flow (MpFlow / MpRace): wire codecs of the flow messages (0x30..0x3A), the scene
// start barrier, the cutscene skip vote, the star ledger, the Star Race hold and ranking, and the race pose digest.
// No game headers and no global state: the game glue (decomp/src/Game/MP/MpFlow.cpp, MpRace.cpp) owns the instances, and the host
// unit tests (tests/mp/flow_test.cpp) drive them directly. Times are milliseconds from any monotonic clock.
#pragma once

#include "Game/MP/MpSeriesLogic.hpp"
#include "port/mp_proto.h"

namespace MpFlowLogic {

// ---- serials ---------------------------------------------------------------------------------------------------------------------
// Scene serials wrap at 16 bits: a is newer than b when it is ahead by less than half the range.
static inline bool serialNewer(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }

static inline int popcount4(unsigned m) { return (int)(m & 1) + (int)((m >> 1) & 1) + (int)((m >> 2) & 1) + (int)((m >> 3) & 1); }

// ---- wire codecs (field order = DESIGN.md section 3; little-endian through MpWriter / MpReader) ------------------------------------
enum : uint8_t { kUnset8 = 0xFF };
enum MoveType : uint8_t { MOVE_DIRECT = 0, MOVE_STAR_SELECT = 2, MOVE_BACK_TO_DOME = 3, MOVE_AFTER_CLEAR = 4, MOVE_AFTER_LOAD = 6 };
static inline bool moveTypeValid(uint8_t t) {
    return t == MOVE_DIRECT || t == MOVE_STAR_SELECT || t == MOVE_BACK_TO_DOME || t == MOVE_AFTER_CLEAR || t == MOVE_AFTER_LOAD;
}

// joinfix: the host's progress snapshot at the start of the flow session. presenceCheck offers it on a join EDGE; a session that opens
// with the guest already present (both consoles in play) has no edge, so the start offers it: host, its scene up, past the file select
// (MOVE_AFTER_LOAD offers there), and a guest to send it to.
static inline bool offerSnapshotAtSessionStart(bool isHost, bool sceneUp, bool atFileSelect, bool guestPresent) {
    return isHost && sceneUp && !atFileSelect && guestPresent;
}

// WHERE (8 bytes) and MOVE (9 bytes). scenario/selected 0 = none (-1 in the game), zone 0xFF = none (-1).
struct Where {
    uint8_t galaxy;      // index in the scenario data list (MR::makeBeginScenarioDataIter order)
    uint8_t scenario;    // placed scenario (GalaxyMoveArgument::mScenarioNo)
    uint8_t selected;    // selected scenario (GalaxyMoveArgument::_C)
    int16_t startId;     // JMapIdInfo::_0, -1 = none
    uint8_t zone;        // JMapIdInfo::mZoneID, 0xFF = none
    uint16_t serial;
    uint8_t moveType;    // MOVE only
};
enum { kWhereBytes = 8, kMoveBytes = 9 };
static inline void writeWhere(MpWriter& w, const Where& v, bool isMove) {
    w.u8(v.galaxy); w.u8(v.scenario); w.u8(v.selected); w.s16(v.startId); w.u8(v.zone); w.u16(v.serial);
    if (isMove) w.u8(v.moveType);
}
// Format checks only; the game checks the ranges against its scenario data with checkWhereRanges.
static inline bool readWhere(const void* data, size_t len, Where* v, bool isMove) {
    MpReader r(data, len);
    v->galaxy = r.u8(); v->scenario = r.u8(); v->selected = r.u8(); v->startId = r.s16(); v->zone = r.u8(); v->serial = r.u16();
    v->moveType = isMove ? r.u8() : (uint8_t)MOVE_DIRECT;
    if (!r.done()) return false;
    if (isMove && !moveTypeValid(v->moveType)) return false;
    if (v->startId < -1) return false;
    return true;
}
// Ranges against the receiving console's data: galaxyCount = scenario data entries; scenarioNum / zoneNum of that galaxy.
// A direct move (and a WHERE) needs a real scenario; the other move types let the story executor decide (scenario 0 allowed).
static inline bool checkWhereRanges(const Where& v, int galaxyCount, int scenarioNum, int zoneNum, bool isMove) {
    if (galaxyCount <= 0 || v.galaxy >= galaxyCount || galaxyCount > 255) return false;
    if (scenarioNum <= 0 || scenarioNum > 255) return false;
    const bool needScenario = !isMove || v.moveType == MOVE_DIRECT;
    if (v.scenario > scenarioNum || (needScenario && v.scenario == 0)) return false;
    if (v.selected > scenarioNum) return false;
    if (v.zone != kUnset8 && (zoneNum <= 0 || v.zone >= zoneNum)) return false;
    return true;
}

// LOADED / DEAD (2 bytes): serial.
static inline bool readSerialOnly(const void* data, size_t len, uint16_t* serial) {
    MpReader r(data, len);
    *serial = r.u16();
    return r.done();
}
// GO (12 bytes; 6 without the series block): serial, turn, then (race) the host's series block: who has the head start in this race
// (MpSeriesLogic). Only the host sends GO (mp_msg_host_only); every console keeps the block (a migrated host continues from it).
// startAtTurn is informational (the host's turn when it sent GO); the start is the receipt of GO (RaceStartGate).
struct Go { uint16_t serial; uint32_t startAtTurn; bool hasSeries = false; MpSeriesLogic::Block series = MpSeriesLogic::Block(); };
enum { kGoBytes = 12, kGoBytesPlain = 6 };
static inline void writeGo(MpWriter& w, const Go& g) {
    w.u16(g.serial); w.u32(g.startAtTurn);
    if (g.hasSeries) MpSeriesLogic::writeBlock(w, g.series);
}
static inline bool readGo(const void* data, size_t len, Go* g) {
    if (len != kGoBytes && len != kGoBytesPlain) return false;
    MpReader r(data, len);
    g->serial = r.u16(); g->startAtTurn = r.u32();
    g->hasSeries = len == kGoBytes;
    g->series = MpSeriesLogic::Block();
    if (g->hasSeries && !MpSeriesLogic::readBlock(r, &g->series)) return false;
    return r.done();
}
// STAR (12 bytes, proto 5). bySlot is informational: receivers use MpMsg::fromSlot. galaxyHash / scenario (starfix) say which scene the claimer
// was in; the HOST uses them only as a cross-check against the scene it judges (a late claim must name the scene the host left), never to choose
// what is credited. galaxyHash = mp_name_hash15 of the galaxy name (the same on every console, not an index).
static const uint8_t kMaxStarId = 8;   // scenarios per galaxy in the save data (u8 flags, 8 max-coin entries)
struct Star { uint16_t serial; uint8_t starId, grand, bySlot; uint32_t turns; uint16_t galaxyHash = 0; uint8_t scenario = 0; };
static const size_t kStarBytes = 12;
static inline void writeStar(MpWriter& w, const Star& s) {
    w.u16(s.serial); w.u8(s.starId); w.u8(s.grand); w.u8(s.bySlot); w.u32(s.turns); w.u16(s.galaxyHash); w.u8(s.scenario);
}
static inline bool readStar(const void* data, size_t len, Star* s) {
    MpReader r(data, len);
    s->serial = r.u16(); s->starId = r.u8(); s->grand = r.u8(); s->bySlot = r.u8(); s->turns = r.u32(); s->galaxyHash = r.u16(); s->scenario = r.u8();
    // review: starId becomes a scenario number in the receiver's game data (GameDataSomeGalaxyStorage: u8 star flags, u16
    // mMaxCoinNum[8]): anything above 8 wrote out of bounds on every console of the session
    return r.done() && s->starId >= 1 && s->starId <= kMaxStarId && s->grand <= 1 && s->bySlot < MP_MAX_PLAYERS && s->galaxyHash < 0x8000 && s->scenario >= 1;
}
// STARV (host only, reliable, 11 bytes): the host's verdict on a star, sent for EVERY verdict (a live claim accepted or refused, a late claim, the
// host's own star). Guests credit ONLY on a credited STARV. epoch = the host epoch it was sent in: a console accepts it only from the CURRENT host.
enum StarvVerdict : uint8_t { SV_CREDITED = 1, SV_REFUSED = 2, SV_NO_STAR = 3, SV_NOT_THERE = 4, SV_TOO_LATE = 5, SV_GRAND_LATE = 6, SV_DROPPED = 7, SV_LAST = 7 };   // 7: dropped without judgement (rate cap, no scene known): answered to the sender only
struct Starv { uint16_t serial, epoch; uint8_t starId, by, grand; uint16_t galaxyHash; uint8_t scenario, verdict; };
static const size_t kStarvBytes = 11;
static inline void writeStarv(MpWriter& w, const Starv& v) {
    w.u16(v.serial); w.u16(v.epoch); w.u8(v.starId); w.u8(v.by); w.u8(v.grand); w.u16(v.galaxyHash); w.u8(v.scenario); w.u8(v.verdict);
}
static inline bool readStarv(const void* data, size_t len, Starv* v) {
    MpReader r(data, len);
    v->serial = r.u16(); v->epoch = r.u16(); v->starId = r.u8(); v->by = r.u8(); v->grand = r.u8(); v->galaxyHash = r.u16(); v->scenario = r.u8(); v->verdict = r.u8();
    return r.done() && v->starId >= 1 && v->starId <= kMaxStarId && v->by < MP_MAX_PLAYERS && v->grand <= 1 && v->galaxyHash < 0x8000 && v->scenario >= 1 &&
           v->verdict >= SV_CREDITED && v->verdict <= SV_LAST;
}
// A STARV counts only from the current host slot AND the host epoch it was sent in (a stale host's or a stranger's is dropped).
static inline bool starvAccepted(int fromSlot, int hostSlot, uint16_t epoch, uint16_t hostEpoch) { return fromSlot == hostSlot && hostSlot >= 0 && epoch == hostEpoch; }
// gamefix: ScenarioData.bcsv "PowerStarId" of a scenario is the MASK of the stars it gives (bit id-1: 1 = star 1, 4 = star 3,
// 36 = stars 3 and 6; 0 = none). A STAR names a real star of the scenario only when its bit is set. The wire range check above
// (1..8) is not enough: a stranger's STAR with an id the scenario does not have would otherwise run the synthetic star-get
// sequence, which records that star as collected in the player's save.
static inline bool starInMask(uint32_t powerStarIdMask, uint8_t starId) {
    return starId >= 1 && starId <= kMaxStarId && ((powerStarIdMask >> (starId - 1)) & 1u) != 0;
}
// glitchfix S1: progress goes into a single-player file only from events the console's own simulation produced or verified. On a
// console that plays on its single-player file ("Play on my file"), another player's STAR claim counts only when the console's own scene
// holds that PowerStar and it has appeared there; the scenario mask alone (a bare claim, no proof) is refused. A claim that is refused
// only for "not appeared yet" is retried for kClaimRetryMs (object sync lag: the star can appear on the claimer's console first).
static const int64_t kClaimRetryMs = 3000;
enum class ClaimVerdict { Accept, Retry, Refuse };
// fix2 (rule in docs/STAR-CLAIM-RULE.md): some honest stars are only ever spawned by the CLAIMER's console, never by the host's
// own simulation, so "it appeared in the host's scene" can never come true for them: a star whose object the scene holds but whose
// conditions (a hidden spot, a boss, each console runs its own) were met on the other console, and a grand star that has no holder
// entry at all. For those kinds the claim counts when the star id belongs to the scenario being played (synthOk), the claimer's
// console is in the same scene serial (claimerSameScene; the STAR's serial was checked by the caller and the claimer's last pose must
// agree) and the claim passed the per-sender rate cap (the caller's). Every other kind keeps the strict rule.
static inline bool consoleOnlyStar(bool holderHas, bool grand) { return holderHas || grand; }
static inline ClaimVerdict claimVerdict(bool spFileSession, bool fromLocal, bool holderHas, bool holderAppeared, bool synthOk,
                                        bool grand = false, bool claimerSameScene = false) {
    if (!spFileSession || fromLocal) return ClaimVerdict::Accept;   // co-op file: the old rules (mask or holder) stand
    if (holderHas && holderAppeared) return ClaimVerdict::Accept;
    if (synthOk && claimerSameScene && consoleOnlyStar(holderHas, grand)) return ClaimVerdict::Accept;
    return (holderHas || synthOk) ? ClaimVerdict::Retry : ClaimVerdict::Refuse;
}
// fix2: claims waiting for the star to appear here. The table holds kClaimRetryMax; a claim for a star already waiting replaces
// that entry; a claim that finds no room is reported (add() false) so the caller refuses it out loud instead of losing it silently.
static const int kClaimRetryMax = 4;
struct ClaimRetryEntry { bool valid = false; Star s = Star(); int from = 0; bool synthOk = false; int64_t untilMs = 0; };
struct ClaimRetryTable {
    ClaimRetryEntry slot[kClaimRetryMax];
    bool add(const Star& s, int from, bool synthOk, int64_t untilMs) {
        int freeAt = -1;
        for (int i = 0; i < kClaimRetryMax; i++) {
            if (slot[i].valid && slot[i].s.starId == s.starId && slot[i].s.serial == s.serial) { slot[i] = ClaimRetryEntry{true, s, from, synthOk, untilMs}; return true; }
            if (!slot[i].valid && freeAt < 0) freeAt = i;
        }
        if (freeAt < 0) return false;
        slot[freeAt] = ClaimRetryEntry{true, s, from, synthOk, untilMs};
        return true;
    }
    int count() const { int n = 0; for (int i = 0; i < kClaimRetryMax; i++) n += slot[i].valid ? 1 : 0; return n; }
};
// glitchfix G2: a second, different star credited while this console is already in its own star demo (or has a delivery running)
// cannot get a demo of its own (the scene is left behind the first demo, and the game's result sequence holds one star). It is
// never dropped silently: a star of the scenario is recorded directly in the game data (Record), any other (grand, holder-only) is
// kept in the ledger and announced (NoticeOnly).
enum class LateStar { Deliver, Record, NoticeOnly };
static inline LateStar lateStarPlan(bool localClear, bool deliveryActive, bool grand, bool synthOk) {
    if (!localClear && !deliveryActive) return LateStar::Deliver;
    return (synthOk && !grand) ? LateStar::Record : LateStar::NoticeOnly;
}
// glitchfix G3: what the console that collects a star tells the others. Real stars carry ids 1..7 (the cooked stage data has two
// PowerStar placements with Obj_arg0 -1, which the game treats as "no id": it records nothing for them), so id <= 0 is possible.
enum class OwnStar { Skip, Send, NoId };
static inline OwnStar ownStarRule(bool together, int starId, bool serialKnown) {
    if (!together || !serialKnown) return OwnStar::Skip;
    return (starId >= 1 && starId <= kMaxStarId) ? OwnStar::Send : OwnStar::NoId;
}
// A guest that collected a star nobody was told about (NoId) must not wait for the host's clear move: it ends the hold at once.
enum class ClearHold { Hold, ReleaseMove, ReleaseNoShare, Timeout };
static inline ClearHold clearHoldPlan(bool pendingMove, bool noShare, int64_t heldMs, int64_t timeoutMs) {
    if (pendingMove) return ClearHold::ReleaseMove;
    if (noShare) return ClearHold::ReleaseNoShare;
    return heldMs > timeoutMs ? ClearHold::Timeout : ClearHold::Hold;
}
// glitchfix G4: a star delivered to a player who is bound to a launch star / pull star / driver waits for the bind to end (bounded; the
// delivery's own 15 s limit still applies, so the bind wait stays well inside it).
static const int64_t kBindWaitMs = 12000;
static inline bool bindHoldsDelivery(bool inBind, int64_t sinceDeliveryStartMs) { return inBind && sinceDeliveryStartMs < kBindWaitMs; }
// FINISH (6 bytes): serial, frames from GO.
struct Finish { uint16_t serial; uint32_t frames; };
static const uint32_t kMaxRaceFrames = 60u * 60u * 60u * 10u;   // 10 hours: anything longer is nonsense
static inline void writeFinish(MpWriter& w, const Finish& f) { w.u16(f.serial); w.u32(f.frames); }
static inline bool readFinish(const void* data, size_t len, Finish* f) {
    MpReader r(data, len);
    f->serial = r.u16(); f->frames = r.u32();
    return r.done() && f->frames > 0 && f->frames < kMaxRaceFrames;
}
// RESULTS: serial u16, count u8, entries {slot u8, frames u32 (0xFFFFFFFF = DNF), place u8 (0 = DNF)}, then optionally the tail
// (MpSeriesLogic::Tail, 8 bytes: the series after this race, the host's choice for the next step, the next scenario). `frames` of an
// entry is the room's clock (the player's own frames plus its head start): the order of the places. The host sends RESULTS again,
// unchanged but for the choice, when it chooses. Only the host sends it (mp_msg_host_only).
static const uint32_t kDnf = 0xFFFFFFFFu;
struct ResultEntry { uint8_t slot; uint32_t frames; uint8_t place; };
struct Results {
    uint16_t serial; uint8_t count; ResultEntry e[MP_MAX_PLAYERS];
    bool hasTail = false; MpSeriesLogic::Tail tail = MpSeriesLogic::Tail();
};
static inline void writeResults(MpWriter& w, const Results& res) {
    w.u16(res.serial); w.u8(res.count);
    for (int i = 0; i < res.count && i < MP_MAX_PLAYERS; i++) { w.u8(res.e[i].slot); w.u32(res.e[i].frames); w.u8(res.e[i].place); }
    if (res.hasTail) MpSeriesLogic::writeTail(w, res.tail);
}
static inline bool readResults(const void* data, size_t len, Results* res) {
    MpReader r(data, len);
    res->serial = r.u16(); res->count = r.u8();
    res->hasTail = false;
    if (!r.ok() || res->count > MP_MAX_PLAYERS) return false;
    unsigned seen = 0;
    for (int i = 0; i < res->count; i++) {
        ResultEntry& e = res->e[i];
        e.slot = r.u8(); e.frames = r.u32(); e.place = r.u8();
        if (!r.ok() || e.slot >= MP_MAX_PLAYERS || (seen & (1u << e.slot))) return false;
        seen |= 1u << e.slot;
        if (e.place > res->count) return false;
        if ((e.place == 0) != (e.frames == kDnf)) return false;
        if (e.frames != kDnf && (e.frames == 0 || e.frames >= kMaxRaceFrames)) return false;
    }
    if (r.ok() && r.len > r.n) {
        if (r.len - r.n != (size_t)MpSeriesLogic::kTailBytes || !MpSeriesLogic::readTail(r, &res->tail)) return false;
        res->hasTail = true;
        // the series' lines of the players of this race are all there (a result for a slot with no line is nonsense)
        for (int i = 0; i < res->count; i++) if (!(res->tail.series.in & (1u << res->e[i].slot))) return false;
    }
    return r.done();
}
// SKIPVOTE (6 bytes): serial, demo hash.
struct SkipVoteMsg { uint16_t serial; uint32_t hash; };
static inline void writeSkipVote(MpWriter& w, const SkipVoteMsg& v) { w.u16(v.serial); w.u32(v.hash); }
static inline bool readSkipVote(const void* data, size_t len, SkipVoteMsg* v) {
    MpReader r(data, len);
    v->serial = r.u16(); v->hash = r.u32();
    return r.done() && v->hash != 0;
}
// WARPREQ (1 byte): target slot.
static inline bool readWarpReq(const void* data, size_t len, uint8_t* target) {
    MpReader r(data, len);
    *target = r.u8();
    return r.done() && *target < MP_MAX_PLAYERS;
}
// HOLD (5 bytes): reason, turns left (relative: receivers' clocks differ).
enum HoldReason : uint8_t { HOLD_BARRIER = 0, HOLD_RACE_WAIT = 1 };
struct Hold { uint8_t reason; uint32_t turnsLeft; };
static inline void writeHold(MpWriter& w, const Hold& h) { w.u8(h.reason); w.u32(h.turnsLeft); }
static inline bool readHold(const void* data, size_t len, Hold* h) {
    MpReader r(data, len);
    h->reason = r.u8(); h->turnsLeft = r.u32();
    return r.done() && h->reason <= HOLD_RACE_WAIT && h->turnsLeft < kMaxRaceFrames;
}

// ---- scene start barrier -----------------------------------------------------------------------------------------------------------
// Every console holds the closed intermission screen until every present member has sent LOADED for this serial (or timeoutMs).
// LOADED is relayed by the host, so each console decides on its own with the same rule.
struct Barrier {
    bool armed = false;
    uint16_t serial = 0;
    int64_t armedMs = 0;
    int64_t readyMs = -1;            // first time the local scene was ready (the wait counts from here)
    bool haveLoaded[MP_MAX_PLAYERS] = {};
    uint16_t loadedSerial[MP_MAX_PLAYERS] = {};

    void arm(uint16_t s, int64_t nowMs) { armed = true; serial = s; armedMs = nowMs; readyMs = -1; }
    void disarm() { armed = false; }
    // Idempotent: a repeated LOADED changes nothing; an older one is ignored.
    void noteLoaded(int slot, uint16_t s) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS) return;
        if (!haveLoaded[slot] || !serialNewer(loadedSerial[slot], s)) { haveLoaded[slot] = true; loadedSerial[slot] = s; }
    }
    bool slotLoaded(int slot) const {
        return slot >= 0 && slot < MP_MAX_PLAYERS && haveLoaded[slot] && !serialNewer(serial, loadedSerial[slot]);
    }
    int loadedCount(unsigned presentMask) const {
        int n = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if ((presentMask & (1u << i)) && slotLoaded(i)) n++;
        return n;
    }
    // Called each turn once the local scene is ready. True = start the scene now.
    bool shouldRelease(unsigned presentMask, int64_t nowMs, int64_t timeoutMs) {
        if (!armed) return true;
        if (readyMs < 0) readyMs = nowMs;
        if (loadedCount(presentMask) >= popcount4(presentMask)) return true;
        return nowMs - readyMs >= timeoutMs;
    }
    int waitedMs(int64_t nowMs) const { return readyMs < 0 ? 0 : (int)(nowMs - readyMs); }
};

// ---- cutscene skip vote ------------------------------------------------------------------------------------------------------------
// Votes per (serial, demo hash). Skip when every eligible player voted, or after majorityAfterMs from the first vote when a strict
// majority of the eligible players voted. Eligible = present players in the same cutscene (the local one always).
struct SkipVote {
    enum { kRecords = 4 };
    struct Rec { bool used; uint16_t serial; uint32_t hash; unsigned votes; int64_t firstMs; bool done; };
    Rec rec[kRecords] = {};

    Rec* find(uint16_t serial, uint32_t hash) {
        for (Rec& r : rec) if (r.used && r.serial == serial && r.hash == hash) return &r;
        return nullptr;
    }
    // Idempotent per (slot, serial, hash). Returns true when this was a new vote.
    bool vote(int slot, uint16_t serial, uint32_t hash, int64_t nowMs) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS || hash == 0) return false;
        Rec* r = find(serial, hash);
        if (!r) {
            Rec* victim = &rec[0];
            for (Rec& c : rec) {
                if (!c.used) { victim = &c; break; }
                if (c.firstMs < victim->firstMs) victim = &c;
            }
            *victim = Rec{true, serial, hash, 0u, nowMs, false};
            r = victim;
        }
        if (r->votes & (1u << slot)) return false;
        r->votes |= 1u << slot;
        return true;
    }
    int votes(uint16_t serial, uint32_t hash, unsigned eligibleMask) {
        Rec* r = find(serial, hash);
        return r ? popcount4(r->votes & eligibleMask) : 0;
    }
    // True once per (serial, hash) when the group decided to skip.
    bool decide(uint16_t serial, uint32_t hash, unsigned eligibleMask, int64_t nowMs, int64_t majorityAfterMs) {
        Rec* r = find(serial, hash);
        if (!r || r->done || eligibleMask == 0) return false;
        const int have = popcount4(r->votes & eligibleMask), need = popcount4(eligibleMask);
        bool skip = have >= need || (nowMs - r->firstMs >= majorityAfterMs && have * 2 > need);
        if (skip) r->done = true;
        return skip;
    }
    bool isDone(uint16_t serial, uint32_t hash) { Rec* r = find(serial, hash); return r && r->done; }
    void clear() { for (Rec& r : rec) r.used = false; }
};

// ---- star ledger ---------------------------------------------------------------------------------------------------------------------
// Keyed by (galaxy, scenario, starId, scene serial): the first claim wins, a repeated STAR of the same scene (serial) is a no-op.
// gamefix: the same star in a LATER scene (the mission replayed in the same session) is a fresh collection.
struct StarLedger {
    enum { kEntries = 16 };
    struct Entry { uint8_t galaxy, scenario, starId, bySlot; uint16_t serial; uint8_t grand = 0; };   // grand: a grand star has no flag to set again (replayLedgerFlags)
    Entry e[kEntries] = {};
    int n = 0, next = 0;
    const Entry* find(uint8_t galaxy, uint8_t scenario, uint8_t starId) const {
        for (int i = 0; i < n; i++) if (e[i].galaxy == galaxy && e[i].scenario == scenario && e[i].starId == starId) return &e[i];
        return nullptr;
    }
    // Returns true when this is the first claim (the caller acts on it).
    bool claim(uint8_t galaxy, uint8_t scenario, uint8_t starId, int bySlot, uint16_t serial, bool grand = false) {
        if (const Entry* f = find(galaxy, scenario, starId)) {
            if (f->serial == serial) return false;
            Entry& old = e[f - e];   // same star, another scene: collected again, the entry is renewed (no growth)
            old = Entry{galaxy, scenario, starId, (uint8_t)bySlot, serial, (uint8_t)(grand ? 1 : 0)};
            return true;
        }
        Entry& d = e[n < kEntries ? n++ : (next++ % kEntries)];
        d = Entry{galaxy, scenario, starId, (uint8_t)bySlot, serial, (uint8_t)(grand ? 1 : 0)};
        return true;
    }
    // A claimer whose star was refused takes its own entry back (only the entry of that very claim: same key and serial).
    bool erase(uint8_t galaxy, uint8_t scenario, uint8_t starId, uint16_t serial) {
        for (int i = 0; i < n; i++) {
            if (e[i].galaxy != galaxy || e[i].scenario != scenario || e[i].starId != starId || e[i].serial != serial) continue;
            e[i] = e[n - 1];
            n--;
            if (next > n) next = 0;
            return true;
        }
        return false;
    }
    void clear() { n = 0; next = 0; }
};

// A clock that advances only while this console pumps messages (review: the window must measure ARRIVAL, not handling; a Vita loads for 25 s+ with
// nothing read, and a STAR that arrived 100 ms after the MOVE is first handled when the load ends). tick() is called wherever messages are pumped,
// before the first one is handled; one step never adds more than capMs, so a gap with no pump counts as at most one step.
struct FlowClock {
    int64_t t = 0, last = -1;
    int64_t tick(int64_t realNowMs, int64_t capMs) {
        if (last >= 0 && realNowMs > last) t += (realNowMs - last) < capMs ? (realNowMs - last) : capMs;
        last = realNowMs;
        return t;
    }
};
static const int64_t kFlowStepCapMs = 500;

// ---- starfix (DESIGN-star revision 4): the scene the console just LEFT, and the host's judgement of a late claim --------------------------
// A STAR that arrives after the host moved on carries the old scene serial. The host judges it against ITS OWN memory of the scene it left
// (taken at the MOVE, before the teardown), never against the scene it is in now and never against what the claimer says. Masks: bit (starId-1).
// kLateClaimMs: the window after the MOVE (this console's own clock) in which a late claim of the scene just left is still judged.
static const int64_t kLateClaimMs = 5000;
struct LeftScene {
    bool valid = false;
    uint8_t galaxy = 0, scenario = 0;      // the scene's key (galaxy index in this console's scenario data, scenario number)
    uint16_t galaxyHash = 0;               // mp_name_hash15 of the galaxy name (what a STAR carries)
    uint16_t serial = 0;                   // the serial being left
    int64_t leftAtMs = 0;                  // this console's own clock at the MOVE
    uint8_t hasMask = 0;                   // the PowerStarHolder had a request for the star
    uint8_t appearedMask = 0;              // ... and it had appeared (info->mIsAppear)
    uint8_t grandMask = 0;                 // a grand star was placed (informational; a late grand star is refused)
    uint8_t scenMask = 0;                  // the scenario's static PowerStarId mask (low 8 bits)
    uint8_t inSerialMask = 0;              // bit slot: the slot's last pose carried the serial being left (plus our own slot); frozen at the MOVE
    void clear() { *this = LeftScene(); }
    void slotGone(int slot) { if (slot >= 0 && slot < MP_MAX_PLAYERS) inSerialMask &= (uint8_t)~(1u << slot); }   // a player who takes the slot later does not inherit the bit
};
enum class LateResult : uint8_t { Credit, NoScene, WrongSerial, TooLate, SlotAbsent, WrongKey, GrandLate, NoSuchStar, NotInScene, NotAppeared };
static inline uint8_t lateVerdictCode(LateResult r) {
    switch (r) {
    case LateResult::Credit: return SV_CREDITED;
    case LateResult::GrandLate: return SV_GRAND_LATE;
    case LateResult::NoSuchStar: case LateResult::WrongKey: return SV_NO_STAR;
    case LateResult::NotInScene: case LateResult::NotAppeared: return SV_NOT_THERE;
    default: return SV_TOO_LATE;
    }
}
// spFile: the console plays on its single-player file (claimVerdict's strict rule). Nothing here reads the live scene or a live pose.
// (b) a Retry verdict is an immediate refusal: the left scene is frozen, nothing can appear in it any more.
// fromSlot: the REAL sender (MpMsg::fromSlot), never s.bySlot (informational, the claimer's own word); nowMs: the flow clock (FlowClock), which counts
// only time this console spent pumping messages, so a long scene load between the MOVE and the claim does not age the claim.
static inline LateResult lateClaimJudge(const LeftScene& L, const Star& s, int fromSlot, bool slotPresent, int64_t nowMs, bool spFile) {
    if (!L.valid) return LateResult::NoScene;
    if (s.serial != L.serial) return LateResult::WrongSerial;                         // equality (serials wrap 65535 -> 1), not "one less"
    const int64_t age = nowMs - L.leftAtMs;
    if (age < 0 || age > kLateClaimMs) return LateResult::TooLate;
    if (!slotPresent) return LateResult::SlotAbsent;
    if (s.galaxyHash != L.galaxyHash || s.scenario != L.scenario) return LateResult::WrongKey;   // cross-check only
    if (s.grand) return LateResult::GrandLate;                                        // (c) the record path cannot save a grand star
    if (s.starId < 1 || s.starId > kMaxStarId || fromSlot < 0 || fromSlot >= MP_MAX_PLAYERS) return LateResult::NoSuchStar;
    const uint8_t bit = (uint8_t)(1u << (s.starId - 1));
    const bool synthOk = (L.scenMask & bit) != 0, has = (L.hasMask & bit) != 0, appeared = (L.appearedMask & bit) != 0;
    if (!synthOk) return LateResult::NoSuchStar;                                      // only a star of the scenario can be recorded
    if (!(L.inSerialMask & (1u << fromSlot))) return LateResult::NotInScene;          // (d) from the frozen mask, by the real sender
    const ClaimVerdict cv = claimVerdict(spFile, false, has, appeared, synthOk, false, true);
    return cv == ClaimVerdict::Accept ? LateResult::Credit : LateResult::NotAppeared;
}
// The claimer's own STARs (6 at once; one more is still sent and answered, only its 60 s timeout is lost) wait for the host's STARV; on a host change they are re-sent to the new host; 60 s without a verdict -> stop.
struct StarPending {
    enum { kEntries = 6 };
    struct Entry { bool valid = false; Star s = Star(); int64_t sinceMs = 0, sentMs = 0; };
    Entry e[kEntries];
    // False when every slot holds an unanswered claim: the caller tells the player (nothing is overwritten silently).
    bool add(const Star& s, int64_t nowMs) {
        for (Entry& x : e) if (x.valid && x.s.serial == s.serial && x.s.starId == s.starId) { x.s = s; x.sinceMs = x.sentMs = nowMs; return true; }
        for (Entry& x : e) if (!x.valid) { x = Entry{true, s, nowMs, nowMs}; return true; }
        return false;
    }
    bool answer(uint16_t serial, uint8_t starId) {
        for (Entry& x : e) if (x.valid && x.s.serial == serial && x.s.starId == starId) { x.valid = false; return true; }
        return false;
    }
    // One expired entry at a time (the caller shows its toast); false when none.
    bool takeExpired(int64_t nowMs, int64_t timeoutMs, Star* out) {
        for (Entry& x : e) if (x.valid && (nowMs < x.sinceMs || nowMs - x.sinceMs >= timeoutMs)) { *out = x.s; x.valid = false; return true; }
        return false;
    }
    // An unanswered claim is sent again every intervalMs (a STARV dropped on the way, or at a host change before the epoch was known, cannot lose it).
    bool takeResend(int64_t nowMs, int64_t intervalMs, Star* out) {
        for (Entry& x : e) if (x.valid && (nowMs < x.sentMs || nowMs - x.sentMs >= intervalMs)) { x.sentMs = nowMs; *out = x.s; return true; }
        return false;
    }
    int count() const { int n = 0; for (const Entry& x : e) n += x.valid ? 1 : 0; return n; }
    void clear() { for (Entry& x : e) x.valid = false; }
};
static const int64_t kStarResendMs = 10000;
// The host already credited this very claim (same scene serial, same star): a repeat (a resend) is answered "credited" again, never judged afresh
// (a resend after the 5 s window would otherwise turn a credited star into a refusal).
static inline bool starAlreadyCredited(const StarLedger& l, uint8_t galaxy, uint8_t scenario, const Star& s) {
    const StarLedger::Entry* f = l.find(galaxy, scenario, s.starId);
    return f && f->serial == s.serial;
}
// After a snapshot is applied (it may predate this session's credits: the core keeps the last complete image ready while a newer one arrives), the flags of
// this session's credited non-grand stars are set again (design principle 3: crediting is idempotent). maskOf(galaxy, scenario) = the scenario's PowerStarId mask;
// setFlag(galaxy, scenario, starId). Returns how many flags were set.
template <class MaskOf, class SetFlag>
static inline int replayLedgerFlags(const StarLedger& l, MaskOf maskOf, SetFlag setFlag) {
    int n = 0;
    for (int i = 0; i < l.n; i++) {
        const StarLedger::Entry& x = l.e[i];
        if (x.grand || !starInMask(maskOf(x.galaxy, x.scenario), x.starId)) continue;   // a grand / holder-only star has no flag to set (the rule forbids writing one directly)
        setFlag(x.galaxy, x.scenario, x.starId);
        n++;
    }
    return n;
}

// The 60 s no-verdict expiry of the claimer's own claim: it is taken back like a refusal (its ledger entry goes; if the host did credit it, its snapshot carries the
// star, and a replay must not set a flag the host never confirmed). idxOfHash: hash -> galaxy index (-1 unknown). Returns true when an entry expired.
template <class IdxOfHash>
static inline bool expireStarClaim(StarPending& p, StarLedger& l, int64_t nowMs, int64_t timeoutMs, IdxOfHash idxOfHash, Star* out) {
    if (!p.takeExpired(nowMs, timeoutMs, out)) return false;
    const int gi = idxOfHash(out->galaxyHash);
    if (gi >= 0) l.erase((uint8_t)gi, out->scenario, out->starId, out->serial);
    return true;
}
// A guest's own new claim: it waits for the host's verdict in the pending table (which also gives it the 60 s expiry and the resend). When all slots hold an unanswered
// claim (a 7th at once) it cannot be tracked, and an untracked entry would stay in the ledger for ever (no expiry would ever take it back): the ledger entry of that very
// claim is erased then (the claim is still sent; its verdict, when it comes, is handled like any other). Returns true when it is tracked.
template <class IdxOfHash>
static inline bool trackOwnClaim(StarPending& p, StarLedger& l, const Star& s, int64_t nowMs, IdxOfHash idxOfHash) {
    if (p.add(s, nowMs)) return true;
    const int gi = idxOfHash(s.galaxyHash);
    if (gi >= 0) l.erase((uint8_t)gi, s.scenario, s.starId, s.serial);
    return false;
}
// A private "dropped" reply (rate cap, no scene known, an already credited claim answered again) goes to one sender at most once a second.
struct ReplyCap {
    int64_t last[MP_MAX_PLAYERS] = {};
    bool allow(int slot, int64_t nowMs) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS) return false;
        if (last[slot] && nowMs >= last[slot] && nowMs - last[slot] < 1000) return false;
        last[slot] = nowMs ? nowMs : 1;
        return true;
    }
};
// What a console that becomes the host sends for the claims it had pending (its own play credited them): the STARVs, and the table is emptied.
static inline int becameHostAnswers(StarPending& p, int localSlot, uint16_t epoch, Starv* out, int max) {
    int n = 0;
    for (StarPending::Entry& e : p.e) {
        if (!e.valid) continue;
        if (n < max) out[n++] = Starv{e.s.serial, epoch, e.s.starId, (uint8_t)localSlot, e.s.grand, e.s.galaxyHash, e.s.scenario, SV_CREDITED};
        e.valid = false;
    }
    return n;
}

// ---- the host's decision on a STAR (MpFlow::handleStar calls this; tests/mp/flow_test.cpp drives the same function) ---------------------------------------------
// The caller reads the scene (probes) and does the effects (ledger claim, flag, toast, STARV, retry table); the DECISION is here.
struct StarClaimIn {
    Star s; int from = 0;
    bool sceneKnown = false;
    bool slotPresent = false;      // the sender's slot is in the room
    bool spFile = false;           // the host plays on its single-player file
    int64_t flowNow = 0;           // FlowClock::t
    int claimGalaxyIdx = -1;       // galaxy index of s.galaxyHash on this console (-1 unknown)
    uint16_t hostSerial = 0;
    // live probes, read only when s.serial == hostSerial
    bool liveReal = false, liveSynthOk = false, liveHas = false, liveAppeared = false, claimerSameScene = false, fromLocal = false;
};
enum class StarAct : uint8_t { DropReply, AnswerAgain, RefuseNoStar, RefuseVerdict, Retry, CreditLive, CreditLate, RefuseLate };
struct StarDecision { StarAct act; uint8_t verdict; LateResult late; };
template <class RateFn>
static inline StarDecision hostDecideStar(const StarClaimIn& in, const StarLedger& led, const LeftScene& left, RateFn rateAllows) {
    if (!in.sceneKnown) return {StarAct::DropReply, SV_DROPPED, LateResult::NoScene};
    // a repeat of a claim already credited is answered "credited" again, ahead of the rate cap (never "dropped")
    if (in.claimGalaxyIdx >= 0 && starAlreadyCredited(led, (uint8_t)in.claimGalaxyIdx, in.s.scenario, in.s)) return {StarAct::AnswerAgain, SV_CREDITED, LateResult::Credit};
    if (!rateAllows()) return {StarAct::DropReply, SV_DROPPED, LateResult::NoScene};
    if (in.s.serial != in.hostSerial) {
        const LateResult r = lateClaimJudge(left, in.s, in.from, in.slotPresent, in.flowNow, in.spFile);
        return r == LateResult::Credit ? StarDecision{StarAct::CreditLate, SV_CREDITED, r} : StarDecision{StarAct::RefuseLate, lateVerdictCode(r), r};
    }
    if (!in.liveReal) return {StarAct::RefuseNoStar, SV_NO_STAR, LateResult::NoSuchStar};
    const ClaimVerdict cv = claimVerdict(in.spFile, in.fromLocal, in.liveHas, in.liveAppeared, in.liveSynthOk, in.s.grand != 0, in.claimerSameScene);
    if (cv == ClaimVerdict::Refuse) return {StarAct::RefuseVerdict, SV_NOT_THERE, LateResult::NotAppeared};
    if (cv == ClaimVerdict::Retry) return {StarAct::Retry, 0, LateResult::NotAppeared};
    return {StarAct::CreditLive, SV_CREDITED, LateResult::Credit};
}
// ---- every console's decision on a STARV (MpFlow::handleStarv) -----------------------------------------------------------------------------------------------------
struct StarvIn {
    Starv v; int fromSlot = 0, hostSlot = -1; uint16_t hostEpoch = 0;
    bool isHost = false, together = true; int localSlot = 0;
    int galaxyIdx = -1;            // galaxy index of v.galaxyHash (-1 unknown)
    uint32_t scenMask = 0;         // the STATIC PowerStarId mask of (galaxy, scenario)
    bool live = false;             // we are in that very scene (serial, galaxy, scenario)
    bool liveHolderReal = false;   // (read only when live) the scene holds that star
};
enum class StarvAct : uint8_t { Drop, Ignore, RefusedOther, RefusedOwn, RefusedOwnToast, CreditedOwn, Veto, CreditLive, RecordLate, NotRecorded };
static inline StarvAct decideStarv(const StarvIn& in) {
    if (!starvAccepted(in.fromSlot, in.hostSlot, in.v.epoch, in.hostEpoch)) return StarvAct::Drop;
    if (in.isHost || !in.together) return StarvAct::Ignore;
    const bool mine = (int)in.v.by == in.localSlot;
    if (in.v.verdict != SV_CREDITED) return mine ? (in.v.verdict == SV_DROPPED ? StarvAct::RefusedOwnToast : StarvAct::RefusedOwn) : StarvAct::RefusedOther;
    const bool synthOk = in.galaxyIdx >= 0 && starInMask(in.scenMask, in.v.starId);
    if (mine) return StarvAct::CreditedOwn;
    if (!(synthOk || (in.live && in.liveHolderReal))) return StarvAct::Veto;
    if (in.live) return StarvAct::CreditLive;
    if (in.galaxyIdx < 0 || in.v.grand) return StarvAct::NotRecorded;
    return StarvAct::RecordLate;
}

// ---- Star Race synchronized start (racefix) ----------------------------------------------------------------------------------------
// Every console takes control away when its OWN scene reaches Action, and says READY (RACEREADY, serial only, guest -> host, resent every
// kReadyResendMs until GO is known, and again on a host change). The HOST sends GO once every present racer is ready (its own entry counts
// when it reaches Action), or kGoWaitMs after the host itself reached Action (a stuck or very slow console must not hold the room). Each
// console starts its countdown on GO (not on its own Action entry); a console that reaches Action after GO starts at once. A console that
// never gets a GO (lost host) starts alone kGuestGoWaitMs after its own Action entry. GO's startAtTurn is informational (the host's turn): nothing reads it.
static const int64_t kRaceGoWaitMs = 10000;       // the host's wait for the slowest present racer, from the host's own Action entry
static const int64_t kRaceGuestGoWaitMs = 25000;  // a guest's wait for GO before it starts alone (longer than the host's, plus one migration)
static const int64_t kRaceReadyResendMs = 2000;
struct RaceStartGate {
    uint16_t serial = 0;
    unsigned ready = 0;          // slots that reported READY (the host's own bit once it is in Action)
    int64_t waitSinceMs = -1;    // the host's wait began (its own Action entry, or a host change)
    bool goSent = false;
    void reset(uint16_t s) { *this = RaceStartGate(); serial = s; }
    void noteReady(int slot, uint16_t s) { if (s == serial && slot >= 0 && slot < MP_MAX_PLAYERS) ready |= 1u << slot; }
    // the host reached Action (idempotent)
    void hostEntered(int hostSlot, int64_t nowMs) {
        if (hostSlot >= 0 && hostSlot < MP_MAX_PLAYERS) ready |= 1u << hostSlot;
        if (waitSinceMs < 0) waitSinceMs = nowMs;
    }
    // a new host: the guests report again, so the wait begins again (they cannot be judged late for the old host's time)
    void hostChanged(int64_t nowMs) { if (waitSinceMs >= 0) waitSinceMs = nowMs; }
    bool allReady(unsigned presentMask) const { return presentMask != 0 && (presentMask & ~ready) == 0; }
    // the host's decision: GO now? (only after the host itself is in Action, once)
    bool shouldSendGo(unsigned presentMask, int64_t nowMs) const {
        if (goSent || waitSinceMs < 0) return false;
        return allReady(presentMask) || nowMs - waitSinceMs >= kRaceGoWaitMs;
    }
    // the slots that count as ready: a slot that left (or whose number was reused by a newcomer) loses its bit; the host's own stays
    void prune(unsigned presentMask, int hostSlot) { ready &= presentMask | (hostSlot >= 0 && hostSlot < MP_MAX_PLAYERS ? 1u << hostSlot : 0u); }
    // READY handler (host only): this READY counts (right session, right race, a real slot)
    static bool acceptReady(bool isHost, bool raceActive, uint16_t raceSerial, uint16_t s, int fromSlot) {
        return isHost && raceActive && s == raceSerial && fromSlot >= 0 && fromSlot < MP_MAX_PLAYERS;
    }
    // READY from a slot after GO went out: the host sends the stored GO to that slot alone (until the results exist)
    static bool answerLateReady(bool isHost, bool goSent, bool resultsKnown, int goLen, int fromSlot, int localSlot) {
        return isHost && goSent && !resultsKnown && goLen > 0 && fromSlot >= 0 && fromSlot < MP_MAX_PLAYERS && fromSlot != localSlot;
    }
    // GO handler (a guest, or any console that has not heard it): the first GO of this race counts; a late or repeated one changes nothing
    static bool acceptGo(bool raceActive, uint16_t raceSerial, uint16_t s, bool goKnown) { return raceActive && s == raceSerial && !goKnown; }
    // a console that became host: say GO again when it knows it, or started alone (the others are released by it)
    static bool resendGoAsNewHost(bool goKnown, bool countdownStarted) { return goKnown || countdownStarted; }
    // the host's GO may go out whatever its own scene does now (paused, dead, between scenes): only its Action entry and the gate count
    // a console that lost its player object (a miss during the countdown or the head start wait) holds control again in the new scene
    static bool needsRehold(bool countdownStarted, bool racing, bool finished, bool controlHeld, bool inAction, bool alive) {
        return countdownStarted && !racing && !finished && !controlHeld && inAction && alive;
    }
    // a console's decision: start the countdown? inAction = it has taken control away (its own opening is over)
    static bool consoleStarts(bool inAction, bool goKnown, int64_t actionSinceMs, int64_t nowMs) {
        if (!inAction) return false;
        return goKnown || nowMs - actionSinceMs >= kRaceGuestGoWaitMs;
    }
    // a guest's READY: send now (the first time) or again after the resend period, until GO is known
    static bool shouldSendReady(bool inAction, bool goKnown, int64_t lastSentMs, int64_t nowMs) {
        if (!inAction || goKnown) return false;
        return lastSentMs < 0 || nowMs - lastSentMs >= kRaceReadyResendMs;
    }
};

// ---- Star Race hold and ranking ----------------------------------------------------------------------------------------------------
struct RaceBoard {
    uint16_t serial = 0;
    bool active = false;
    uint32_t frames[MP_MAX_PLAYERS] = {};   // 0 = not finished: the player's OWN frames, from its own start
    uint32_t headStart[MP_MAX_PLAYERS] = {};   // race: the head start (frames) each slot waited after the countdown (from GO)
    int64_t firstFinishMs = -1;

    // The room's clock for a finished slot: own frames + head start (who crossed the line first); clamped to stay a valid time.
    uint32_t roomFrames(int slot) const {
        if (slot < 0 || slot >= MP_MAX_PLAYERS || frames[slot] == 0) return 0;
        const uint64_t t = (uint64_t)frames[slot] + headStart[slot];
        return t >= kMaxRaceFrames ? kMaxRaceFrames - 1 : (uint32_t)t;
    }

    void reset(uint16_t s) { *this = RaceBoard(); serial = s; active = true; }
    // Idempotent: the first time per slot counts.
    bool noteFinish(int slot, uint32_t f, int64_t nowMs) {
        if (!active || slot < 0 || slot >= MP_MAX_PLAYERS || f == 0 || f >= kMaxRaceFrames || frames[slot] != 0) return false;
        frames[slot] = f;
        if (firstFinishMs < 0) firstFinishMs = nowMs;
        return true;
    }
    int finishedCount(unsigned presentMask) const {
        int n = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if ((presentMask & (1u << i)) && frames[i]) n++;
        return n;
    }
    // limitMs <= 0: "no limit", which still ends kNoLimitCapMs after the first finisher (fix2: finishers are never held forever
    // behind the star wipe by a player who stopped racing). Results when every present player finished, or the limit ran out.
    static constexpr int64_t kNoLimitCapMs = 10 * 60 * 1000;
    static constexpr int64_t kNoLimitShowMs = 60 * 1000;   // "no limit": the countdown shows in the cap's last minute only
    static int64_t effectiveLimitMs(int64_t limitMs) { return limitMs > 0 ? limitMs : kNoLimitCapMs; }
    bool shouldPublish(unsigned presentMask, int64_t limitMs, int64_t nowMs) const {
        if (!active || presentMask == 0) return false;
        const int fin = finishedCount(presentMask);
        if (fin == 0) return false;
        if (fin >= popcount4(presentMask)) return true;
        return nowMs - firstFinishMs >= effectiveLimitMs(limitMs);
    }
    int secondsLeft(int64_t limitMs, int64_t nowMs) const {
        if (firstFinishMs < 0) return -1;
        int64_t left = effectiveLimitMs(limitMs) - (nowMs - firstFinishMs);
        if (limitMs <= 0 && left > kNoLimitShowMs) return -1;
        return left <= 0 ? 0 : (int)((left + 999) / 1000);
    }
    // Ranking of the present players: finishers by the room's clock (own frames + head start; ties share a place, lower slot listed
    // first), then DNF. The entries' frames are the room's clock.
    void rank(unsigned presentMask, Results* out) const {
        out->serial = serial; out->count = 0; out->hasTail = false;
        int order[MP_MAX_PLAYERS], n = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if ((presentMask & (1u << i)) && frames[i]) order[n++] = i;
        for (int a = 1; a < n; a++) {   // insertion sort: stable, slot order on ties
            int v = order[a], b = a - 1;
            while (b >= 0 && roomFrames(order[b]) > roomFrames(v)) { order[b + 1] = order[b]; b--; }
            order[b + 1] = v;
        }
        for (int k = 0; k < n; k++) {
            ResultEntry& e = out->e[out->count++];
            e.slot = (uint8_t)order[k]; e.frames = roomFrames(order[k]);
            e.place = (uint8_t)((k > 0 && roomFrames(order[k - 1]) == e.frames) ? out->e[out->count - 2].place : k + 1);
        }
        for (int i = 0; i < MP_MAX_PLAYERS; i++)
            if ((presentMask & (1u << i)) && !frames[i]) out->e[out->count++] = ResultEntry{(uint8_t)i, kDnf, 0};
    }
};

// ---- race pose digest: four CRC32 lanes (different seeds) over the local player's position stream -> 16 bytes ----------------
static inline uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
struct PoseDigest {
    uint32_t lane[4] = {0x00000000u, 0x9E3779B9u, 0x7F4A7C15u, 0x85EBCA6Bu};
    uint32_t count = 0;
    void reset() { *this = PoseDigest(); }
    void add(float x, float y, float z, uint32_t frame) {
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        w.f32(x); w.f32(y); w.f32(z); w.u32(frame);
        for (int i = 0; i < 4; i++) lane[i] = crc32Update(lane[i], b, sizeof b);
        count++;
    }
    void out(uint8_t d[16]) const {
        for (int i = 0; i < 4; i++) { d[i * 4] = (uint8_t)lane[i]; d[i * 4 + 1] = (uint8_t)(lane[i] >> 8); d[i * 4 + 2] = (uint8_t)(lane[i] >> 16); d[i * 4 + 3] = (uint8_t)(lane[i] >> 24); }
    }
};

// 32-bit FNV-1a over a name (bytes as uint8_t); never 0 (0 = no vote).
static inline uint32_t hashName(const char* s, uint32_t salt) {
    uint32_t h = 2166136261u ^ salt;
    for (const unsigned char* q = (const unsigned char*)s; q && *q; q++) { h ^= *q; h *= 16777619u; }
    return h ? h : 1u;
}

}  // namespace MpFlowLogic

// ---- refused guest galaxy starts: ONE rate-limited path for every refusal site (the file select's move, the dome's Yes, the dome
// jump-out, the stage cubes). A held button / a sequence that retries every frame is ONE episode: one log line, one toast, nothing sent.
// An episode ends when the site has been quiet for kRefusalQuietMs (the player let go / walked away); the next call starts a new one.
namespace MpFlow {
constexpr int64_t kRefusalQuietMs = 1000;
constexpr int kRefusalKinds = 8;
struct RefusalGate {
    int64_t last[kRefusalKinds];
    uint32_t count[kRefusalKinds];
    bool init = false;
    void reset() { init = true; for (int i = 0; i < kRefusalKinds; i++) { last[i] = -1000000000LL; count[i] = 0; } }
    // a refusal at `kind` (a small site id) at `nowMs`: true = a new episode (log it, toast it); `repeats` = the refusals the episode before it had
    bool call(int kind, int64_t nowMs, uint32_t* repeats = nullptr) {
        if (!init) reset();
        if (kind < 0 || kind >= kRefusalKinds) kind = kRefusalKinds - 1;
        const bool fresh = nowMs < last[kind] || nowMs - last[kind] >= kRefusalQuietMs;   // (a clock that went back starts over)
        if (repeats) *repeats = fresh ? count[kind] : 0;
        if (fresh) count[kind] = 0;
        count[kind]++;
        last[kind] = nowMs;
        return fresh;
    }
};
}  // namespace MpFlow
