// Galaxy 3.0 multiplayer: shared world state (Play Together), pure logic. No game headers: host unit tests build this file
// (tests/mp/world_*). The game glue lives in decomp/src/Game/MP/MpWorld.cpp. See DESIGN.md section 4 "World".
#pragma once

#include "port/mp_proto.h"

namespace MpWorldLogic {

// ---- hashing / CRC ---------------------------------------------------------------------------------------------------------
uint32_t crc32(uint32_t crc, const void* data, size_t len);   // IEEE (zlib) CRC-32; start with crc = 0
uint32_t nameHash32(const char* s);                            // FNV-1a over the bytes as uint8_t (signedness independent)

// ---- placement registry ----------------------------------------------------------------------------------------------------
enum EntryFlag : uint8_t {
    ENTRY_ENEMY = 1 << 0,    // sub 0, body sensor in the enemy range, not a boss: KILL eligible
    ENTRY_PURPLE = 1 << 1,   // a purple Coin (found in the PurpleCoinHolder)
    ENTRY_CHIP = 1 << 2,     // a star chip (found in a ChipGroup)
    ENTRY_DUP = 1 << 3,      // key not unique in this scene: never synced
    ENTRY_GOT = 1 << 4,      // purple coin / chip already counted on this console
    ENTRY_KILLED = 1 << 5,   // worldsync: an enemy defeat the world sync has seen (local or remote): the late joiner / repair state
};

struct Entry {
    MpObjKey key;
    uint8_t flags;
    uint32_t classHash;      // nameHash32 of the actor's NameObj name
    const void* actor;       // scene object (compared, dereferenced only by the glue while the scene lives)
};

// Actors a shared pool creates inside a placement row's window (SceneObjHolder creates a scene object lazily, in the first row that asks for it:
// CoinHolder makes its 32 shared Coins in whichever row declares coins first; MapPartsRailGuideHolder makes the rail guide points the same way).
// Root cause: placement order is not fixed. PlacementInfoOrdered::sort() ranks each row set by whether its archives are already loaded (fromDVD) and
// breaks ties with an unstable shell sort, so the order depends on what the process already has loaded: a same-process reload, a late joiner and a console
// arriving from another galaxy can each place rows differently. Whatever "the first row that asks" creates is therefore attributed to different rows on
// different consoles / loads. Those actors stay out of the keys and the CRC, and the row's own children are numbered without them.
class PoolRanges {
public:
    enum { kMax = 32 };
    void clear() { mN = 0; mDepth = 0; mOverflow = false; }
    bool overflowed() const { return mOverflow; }
    // Start of a scene object's creation: returns a token for end(). -1 = not placing (nothing recorded), -2 = nested inside another creation.
    int begin(int groupCount, bool placing) {
        if (!placing) return -1;
        if (mDepth++ > 0) return -2;
        return groupCount;
    }
    void end(int token, int groupCount) {
        if (token == -1) return;
        if (mDepth > 0) mDepth--;
        if (token < 0 || groupCount <= token) return;
        if (mN > 0 && mTo[mN - 1] == token) { mTo[mN - 1] = groupCount; return; }   // adjacent to the previous range: one range
        if (mN < kMax) { mFrom[mN] = token; mTo[mN] = groupCount; mN++; }
        else mOverflow = true;
    }
    bool contains(int idx) const { for (int i = 0; i < mN; i++) if (idx >= mFrom[i] && idx < mTo[i]) return true; return false; }
    int count() const { return mN; }
    // sub key of group index `idx` inside a row that starts at `before`: its position among the row's own actors (pool actors are not counted)
    int subFor(int before, int idx, int base) const {
        int sub = base + (idx - before);
        for (int i = 0; i < mN; i++) {
            int lo = mFrom[i] > before ? mFrom[i] : before, hi = mTo[i] < idx ? mTo[i] : idx;
            if (hi > lo) sub -= hi - lo;
        }
        return sub;
    }
private:
    int mFrom[kMax];
    int mTo[kMax];
    int mN = 0;
    int mDepth = 0;
    bool mOverflow = false;
};

int compareKey(const MpObjKey& a, const MpObjKey& b);         // order: zone, nameHash, lid, sub

class Registry {
public:
    enum { kMax = 2560, kHashSize = 4096 };

    void clear();
    // Adds one actor (placement order). Returns false when full (the registry is then marked overflowed: sync off).
    bool add(const MpObjKey& key, uint32_t classHash, const void* actor, uint8_t flags);
    // Sorts by key, marks duplicate keys, builds the pointer index and the CRC. Call once after placement.
    void finalize();
    int findKey(const MpObjKey& key) const;                    // index or -1 (missing or duplicate)
    int findActor(const void* actor) const;                    // index or -1
    bool setFlagsForActor(const void* actor, uint8_t flags);   // after finalize
    Entry& at(int i) { return mEntries[i]; }
    const Entry& at(int i) const { return mEntries[i]; }
    int count() const { return mCount; }
    bool ready() const { return mReady; }
    bool overflowed() const { return mOverflow; }
    uint32_t crc() const { return mCrc; }
    int dupCount() const { return mDups; }

private:
    Entry mEntries[kMax];
    uint16_t mHash[kHashSize];  // indices into mEntries (sorted order), 0xFFFF = empty
    int mCount = 0;
    int mDups = 0;
    uint32_t mCrc = 0;
    bool mReady = false;
    bool mOverflow = false;
};

// ---- policy tables ---------------------------------------------------------------------------------------------------------
enum SwitchPolicy : uint8_t { SW_SYNC = 0, SW_LOCAL = 1 };
// Placement name (ASCII, from MR::getObjectName), switch field ("SW_A", ...), Obj_arg0 (hasArg0 false when absent).
SwitchPolicy switchPolicy(const char* placementName, const char* switchName, bool hasArg0, int32_t arg0);
// Boss classes (decomp/src/Game/Boss/*) are never kill-synced: each console fights its own boss.
bool isBossName(const char* placementName);

// ---- switch mirror ---------------------------------------------------------------------------------------------------------
// Bits index: zone 0..31 -> 0..31, global (wire 0xFF) -> 32.
enum { kZoneSlots = 33, kGlobalSlot = 32, kGlobalWire = 0xFF, kBits = 128 };
int zoneSlot(int wireZone);                                    // -1 if not representable
int slotWire(int slot);

struct BitSet128 {
    uint32_t w[4];
    bool get(int b) const { return (w[b >> 5] >> (b & 31)) & 1u; }
    void set(int b, bool on) { if (on) w[b >> 5] |= 1u << (b & 31); else w[b >> 5] &= ~(1u << (b & 31)); }
    bool any() const { return (w[0] | w[1] | w[2] | w[3]) != 0; }
};

class SwitchMirror {
public:
    enum { kOscEdges = 6, kOscWindow = 60 };                  // > 6 local edges in 60 turns (1 s) => local for the scene

    void reset(uint16_t serial);                               // new scene serial: forget everything
    void resetScene();                                         // scene (re)load with the same serial: keep `on`, clear the rest
    uint16_t serial() const { return mSerial; }
    bool valid() const { return mValid; }

    // Static local bits (a local-policy writer refers to them): never in the map, never written from a map.
    void markLocal(int slot, int bit) { mLocal[slot].set(bit, true); }
    bool isLocal(int slot, int bit) const { return mLocal[slot].get(bit); }
    bool isGuarded(int slot, int bit) const { return mGuard[slot].get(bit); }
    // A local edge that would be sent: counts it; returns true when the bit is (or just became) guarded = do not send.
    // *tripped is set when this edge tripped the guard.
    bool oscEdge(int slot, int bit, uint32_t now, bool* tripped);

    bool getOn(int slot, int bit) const { return mOn[slot].get(bit); }
    void setOn(int slot, int bit, bool on) { if (!mLocal[slot].get(bit)) mOn[slot].set(bit, on); }
    const BitSet128& onBits(int slot) const { return mOn[slot]; }
    BitSet128& onBitsMutable(int slot) { return mOn[slot]; }

    uint32_t crc() const;                                      // CRC of the on-bits (canonical: slots with bits, ascending)
    int zonesWithBits() const;

private:
    BitSet128 mOn[kZoneSlots];
    BitSet128 mLocal[kZoneSlots];
    BitSet128 mGuard[kZoneSlots];
    uint16_t mWinStart[kZoneSlots][kBits];
    uint8_t mWinCount[kZoneSlots][kBits];
    uint16_t mSerial = 0;
    bool mValid = false;
};

// ---- messages --------------------------------------------------------------------------------------------------------------
enum { kSwitchLen = 5, kObjLen = 8, kRegCrcLen = 6, kRegCrcLenFlags = 7, kMapHeader = 7, kMapEntry = 17, kMapMaxZones = kZoneSlots };

struct SwitchMsg { uint16_t serial; uint8_t zone; uint8_t bit; uint8_t on; };
struct ObjMsg { uint16_t serial; MpObjKey key; };
// flags bit0 (kRegCrcRepair): "my state differs from yours, resend it" (a repair request; plain 6 bytes = the placement announcement)
enum { kRegCrcRepair = 1 };
struct RegCrcMsg { uint16_t serial; uint32_t crc; uint8_t flags = 0; };
struct MapMsg {
    uint16_t serial;
    uint32_t crc;
    uint8_t count;
    uint8_t zone[kMapMaxZones];       // wire zone ids
    BitSet128 bits[kMapMaxZones];
};

size_t writeSwitch(void* buf, size_t cap, const SwitchMsg& m);
size_t writeObj(void* buf, size_t cap, const ObjMsg& m);
size_t writeRegCrc(void* buf, size_t cap, const RegCrcMsg& m);
// full = false: header only (count 0, crc of the whole mirror) for the 1 s check.
size_t writeMap(void* buf, size_t cap, uint16_t serial, const SwitchMirror& mirror, bool full);

// Readers: zoneCount = MR::getZoneNum() of the receiver. All return false (drop the whole message) on any problem.
bool readSwitch(const void* data, size_t len, int zoneCount, SwitchMsg* out);
bool readObj(const void* data, size_t len, int zoneCount, ObjMsg* out);
bool readRegCrc(const void* data, size_t len, RegCrcMsg* out);   // 6 bytes, or 7 with flags (only kRegCrcRepair may be set)
bool readMap(const void* data, size_t len, int zoneCount, MapMsg* out);
uint32_t mapCrc(const MapMsg& m);                              // CRC of the entries as SwitchMirror::crc computes it
bool mapIsFull(const MapMsg& m);                               // count > 0, or an empty full map (crc of nothing)

// Applies a full map to a mirror. authoritative = false: only turns bits on. true: the map is the whole truth for bits the
// mirror had on (they are turned off when absent). Calls apply(slot, bit, on) for every mirror bit that changed and is not
// guarded (the glue writes the real switch). Returns the number of changed bits.
typedef void (*ApplyFn)(void* ctx, int slot, int bit, bool on);
int applyMap(SwitchMirror& mirror, const MapMsg& m, bool authoritative, ApplyFn apply, void* ctx);

// ---- object state map (worldsync: late joiner / rebuilt registry / repair) ---------------------------------------------------------
// The host's kill / purple coin / chip state of the scene as bitsets over the registry index (every console with the same registry CRC
// has the same index order). Sent as OBJMAP (0x46, host only, reliable) in chunks of kObjMapChunkBytes, only the chunks that hold a set
// bit, plus one SUMMARY (counts + CRC) that is also repeated about once a second so a guest notices a difference. Applied idempotently,
// "on" only: a defeat or a collected coin never comes back inside a scene. Every message is far below 1444 bytes (largest: 138).
enum { kObjKinds = 3, kObjMapMaxBytes = Registry::kMax / 8, kObjMapChunkBytes = 128, kObjMapHeader = 10, kObjSummaryLen = 17 };
enum ObjMapKind : uint8_t { OM_SUMMARY = 0, OM_KILL = 1, OM_PURPLE = 2, OM_CHIP = 3 };
struct ObjState {
    uint8_t bits[kObjKinds][kObjMapMaxBytes];
    uint16_t count[kObjKinds];
    uint32_t crc;
    int usedBytes;                          // ceil(registry count / 8)
};
// Which kinds this session shares (mp_settings: sharedKills, flags bit1 purple coins, bit2 star chips). A kind that is not shared is never packed
// (the host's map must not give a guest coins / chips / defeats the rules of the session keep separate) and never applied.
enum ShareMask : uint8_t { SHARE_KILL = 1, SHARE_PURPLE = 2, SHARE_CHIP = 4, SHARE_ALL = 7 };
typedef bool (*ActorDeadFn)(const void* actor);
// Fills `out` from the registry flags (kill: ENEMY and KILLED and the actor is dead NOW (isDead; an enemy that came back, e.g. Meramera's
// recovery, is not a defeat any more); purple: PURPLE and GOT; chip: CHIP and GOT; duplicates never; only the kinds in `share`).
void packObjState(const Registry& reg, ObjState* out, uint8_t share = SHARE_ALL, ActorDeadFn isDead = nullptr);
struct ObjMapMsg {
    uint16_t serial;
    uint32_t regCrc;
    uint8_t kind;                           // ObjMapKind
    uint16_t start;                         // chunk: byte offset into the bitset
    uint8_t n;                              // chunk: byte count (1..kObjMapChunkBytes)
    uint8_t bytes[kObjMapChunkBytes];
    uint16_t count[kObjKinds];              // summary
    uint32_t crc;                           // summary
};
size_t writeObjSummary(void* buf, size_t cap, uint16_t serial, uint32_t regCrc, const ObjState& st);
// kind 1..3; the chunk [start, start + n) of that bitset. 0 when the arguments are out of range or cap is too small.
size_t writeObjChunk(void* buf, size_t cap, uint16_t serial, uint32_t regCrc, int kind, const ObjState& st, int start, int n);
bool objChunkHasBits(const ObjState& st, int kind, int start, int n);
bool readObjMap(const void* data, size_t len, ObjMapMsg* out);   // false (drop the whole message) on any problem
typedef void (*ObjApplyFn)(void* ctx, int kind, int idx);
// Calls apply(kind, registry index) for every set bit of a chunk whose index is below regCount. Returns the number of calls.
int applyObjChunk(const ObjMapMsg& m, int regCount, ObjApplyFn apply, void* ctx);
// True when the host's summary says this console lacks something: the host has more of a kind than we do, or (same counts) another set.
bool objStateBehind(const ObjMapMsg& hostSummary, const ObjState& mine);

// ---- reload carry (hostreload) ---------------------------------------------------------------------------------------------------
// A console that reloads its own scene after a miss (MOVE types 5 / 7: the SAME serial, no session MOVE) rebuilds the registry: every
// ENTRY_KILLED / ENTRY_GOT flag is gone and the new actors are fresh. The carry remembers what the world sync saw in this scene (local and
// remote defeats, purple coins, chips) as bits over the registry index, bound to (serial, registry CRC): the same scene placed again has the
// same CRC, so the same indices. The host re-applies it after its placement (so it does not show what the guests cleared as alive and does
// not hand a joiner an empty state); a new serial or another registry drops it. Bits only ever turn on inside a scene.
struct ObjCarry {
    bool valid;
    uint16_t serial;
    uint32_t regCrc;
    ObjState st;                            // bits / count per kind (usedBytes and crc are not kept up to date)
    bool blocked;                           // dropped by a group restart: no note is taken until the next placement (carryArm)
};
void carryClear(ObjCarry* c);
// A GROUP restart (everyone caught: the RESTART path) is a restart from the checkpoint on every console: enemies, coins, purple coins and chips
// are reset, so the carry is dropped and takes no notes (the last events of the wipe) until the next placement calls carryArm.
void carryDrop(ObjCarry* c);
void carryArm(ObjCarry* c);
// Only the HOST re-applies, and only for an individual reload (Try Again) while at least one other present player is still in the scene of
// this serial (the shared world continues); alone, or in a group restart, the scene is a fresh one.
bool carryMayApply(bool isHost, int othersInScene);
// kind OM_KILL..OM_CHIP, idx 0..Registry::kMax-1 (anything else is ignored). A note for another (serial, CRC) replaces the old binding.
void carryNote(ObjCarry* c, uint16_t serial, uint32_t regCrc, int kind, int idx);
// Calls apply(kind, idx) for every carried bit below regCount of a kind in `share` when (serial, regCrc) match; a mismatch clears the carry.
int carryApply(ObjCarry* c, uint16_t serial, uint32_t regCrc, int regCount, uint8_t share, ObjApplyFn apply, void* ctx);

// Host: ORs the matching carry (kinds in `share`) into a packed state and recounts it (count, crc), so what goes out right after a reload
// already holds what the re-applied carry will have restored by the time the scene runs (its kills wait for the live scene). No match: unchanged.
// Only OUTSTANDING bits are merged: carried, and the registry entry is not yet flagged KILLED / GOT (what the reload has not put back yet).
void carryMerge(const ObjCarry& c, uint16_t serial, uint32_t regCrc, uint8_t share, const Registry& reg, ObjState* st);
bool carryBound(const ObjCarry& c, uint16_t serial, uint32_t regCrc);   // valid and bound to exactly this (serial, registry CRC)
// A carried bit the reload still has to put back: the bit is set, the entry has the kind's flag, is not a duplicate and is not flagged yet.
bool carryOutstanding(const ObjCarry& c, const Registry& reg, int kind, int idx);
// Enemies that come back by design (Meramera::tryRecovery) are flagged KILLED but alive: their carry kill bit is cleared (never re-killed on a
// guest in ordinary play, nor by a later host reload). Returns the bits cleared.
int carryPrune(ObjCarry* c, const Registry& reg, ActorDeadFn isDead);
// One re-apply pass: puts back at most maxTries outstanding bits (a failing try does not count against the bound); calls tryFn(kind, idx) for outstanding bits of the kinds in `share` (true = done: the glue flagged the entry);
// *remaining = outstanding bits left after the pass (0: finished). Returns the tries that returned true. A bit whose try fails stays for the next pass.
typedef bool (*CarryTryFn)(void* ctx, int kind, int idx);
int carryPass(ObjCarry* c, const Registry& reg, uint8_t share, int maxTries, CarryTryFn tryFn, void* ctx, int* remaining);
// Host: a console's map request is answered at once, or (placement not done for this serial, or a group restart is in progress: the carry is
// blocked and the registry still holds the pre-restart flags) kept for the host's own placementDone.
inline bool replyDeferred(bool placedThisSerial, bool carryBlocked) { return !placedThisSerial || carryBlocked; }

// ---- drop accounting (worldsync) -------------------------------------------------------------------------------------------
// Every world event that is not applied (serial mismatch, not placed yet, queue full, expired kill, held pool full, ...) is counted per
// scene by (type, reason) and its log line is rate limited; the scene end prints one summary. Pure logic: host tested.
enum DropType : uint8_t { DT_SWITCH = 0, DT_KILL, DT_PURPLE, DT_CHIP, DT_OTHER, DT_COUNT };   // OTHER: maps, REGCRC, OBJMAP
enum DropReason : uint8_t {
    DR_SERIAL = 0,     // the event's scene serial is not this console's
    DR_NOT_PLACED,     // this console has not finished placement (or never built a registry for this scene)
    DR_REG_OFF,        // registry overflowed / not ready: object sync is off
    DR_NO_CRC,         // the other side's registry CRC is not known yet
    DR_CRC_MISMATCH,   // the registries differ: object sync is off for that sender
    DR_QUEUE_FULL,     // a bounded queue was full: the oldest entry was dropped
    DR_EXPIRED,        // a queued event ran out of time
    DR_HELD_FULL,      // the held-message pool (scene not live yet) was full
    DR_UNKNOWN_KEY,    // no object with that key here
    DR_WRONG_KIND,     // the key is not an enemy / purple coin / chip (or the setting is off)
    DR_MALFORMED,
    DR_RATE,           // host: a guest over its ceiling
    DR_FAR,            // host: the guest was too far from the object
    DR_NO_MEM,
    DR_NOT_HOST,
    DR_COUNT
};
const char* dropTypeName(int t);     // "switch" "kill" "purple" "chip" "other"
const char* dropReasonName(int r);   // "serial" "not-placed" ...
DropType dropTypeOfMsg(uint8_t msgType);   // MP_MSG_SWITCH -> DT_SWITCH, KILL, PURPLE, CHIP; every other id DT_OTHER

class DropStats {
public:
    DropStats() { reset(); }
    enum { kLogBurst = 4, kLogRefillTurns = 1800, kSummaryTurns = 1800 };   // after the first line of each (type, reason): 4 lines, then 1 per 30 s; and a summary every 30 s
    void reset();
    // Counts one drop; true when the caller should log it (the first of each (type, reason) of the scene, then the bucket).
    bool note(DropType t, DropReason r, uint32_t nowTurn);
    void noteMany(DropType t, DropReason r, uint32_t n) { if (t < DT_COUNT && r < DR_COUNT && n) mN[t][r] += n; }
    uint32_t count(DropType t, DropReason r) const { return (t < DT_COUNT && r < DR_COUNT) ? mN[t][r] : 0; }
    uint32_t typeTotal(DropType t) const;
    uint32_t reasonTotal(DropReason r) const;
    uint32_t total() const;
    // True once every kSummaryTurns while drops keep happening (the caller logs format(): the steady state is one summary per 30 s, not a line per drop).
    bool summaryDue(uint32_t nowTurn);
    // "world drops scene=<scene> switch=n kill=n purple=n chip=n other=n reason=serial:3,not-placed:2" (reason=none when nothing).
    // Returns the length written (always NUL terminated when cap > 0).
    size_t format(char* buf, size_t cap, const char* scene) const;
private:
    uint32_t mN[DT_COUNT][DR_COUNT];
    uint16_t mSeen[DT_COUNT];
    uint32_t mTokens;
    uint32_t mRefillAt;
    uint32_t mSummaryAt;
    uint32_t mSummaryTotal;
    bool mInit;
};

// ---- pending remote applies ------------------------------------------------------------------------------------------------
enum PendingKind : uint8_t { PEND_KILL = 1, PEND_PURPLE = 2, PEND_CHIP = 3 };
class PendingQueue {
public:
    enum { kMax = 64 };
    struct Item { uint8_t kind; uint16_t index; uint32_t deadline; };   // deadline 0 = until scene end
    void clear() { mCount = 0; }
    // Dedups (kind, index); drops the oldest when full. Returns true when the oldest entry was dropped (*evicted = it).
    bool push(uint8_t kind, uint16_t index, uint32_t deadline, Item* evicted = nullptr);
    int count() const { return mCount; }
    // Calls fn for each item; fn returns true when done (removed). Items past deadline are removed without a call to fn (expiredFn(item)
    // is called for each of them when given: the drop accounting).
    template < class F > void run(uint32_t now, F fn) { run(now, fn, [](const Item&) {}); }
    template < class F, class E > void run(uint32_t now, F fn, E expiredFn) {
        int w = 0;
        for (int i = 0; i < mCount; i++) {
            Item it = mItems[i];
            bool expired = it.deadline != 0 && (int32_t)(now - it.deadline) > 0;
            if (expired) { expiredFn(it); continue; }
            if (fn(it)) continue;
            mItems[w++] = it;
        }
        mCount = w;
    }
private:
    Item mItems[kMax];
    int mCount = 0;
};

// ---- early object events (worldsync) -----------------------------------------------------------------------------------------
// KILL / PURPLE / CHIP that cannot be judged yet (this console has not finished placement, the serial is already the next scene's, the
// peer's registry CRC has not arrived) wait here instead of being dropped. Bounded (kMax), no heap; the oldest is dropped (and reported)
// when full. An event is kept by (type, serial, key): a repeat refreshes it. Survives scene boundaries: the glue prunes by serial.
class EarlyQueue {
public:
    enum { kMax = 96, kPerSender = 24 };       // one sender can never hold more than kPerSender of the 96
    enum Verdict : int { EQ_KEEP = 0, EQ_DONE = 1 };            // fn: keep waiting / remove (applied or dropped and counted by fn)
    struct Item { uint8_t type; uint8_t fromSlot; uint16_t serial; MpObjKey key; uint32_t deadline; };   // deadline 0 = no limit
    void clear() { mCount = 0; }
    int count() const { return mCount; }
    // Dedups by (type, sender, serial, key). A sender at its kPerSender loses ITS oldest entry; a full queue loses the oldest overall.
    // Returns true when an item was dropped to make room (*evicted = it).
    bool push(uint8_t type, uint8_t fromSlot, uint16_t serial, const MpObjKey& key, uint32_t deadline, Item* evicted = nullptr);
    int countFrom(uint8_t fromSlot) const;
    int dropSerial(uint16_t serial) { int w = 0; for (int i = 0; i < mCount; i++) if (mItems[i].serial != serial) mItems[w++] = mItems[i]; const int n = mCount - w; mCount = w; return n; }   // hostreload: a restart while loading forgets this scene's early events
    template < class F, class E > void run(uint32_t now, F fn, E expiredFn) {
        int w = 0;
        for (int i = 0; i < mCount; i++) {
            Item it = mItems[i];
            if (it.deadline != 0 && (int32_t)(now - it.deadline) > 0) { expiredFn(it); continue; }
            if (fn(it) == EQ_DONE) continue;
            mItems[w++] = it;
        }
        mCount = w;
    }
private:
    Item mItems[kMax];
    int mCount = 0;
};
// 16-bit scene serials with wrap: true when a is newer than b.
inline bool serialAhead(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }
// An event may wait for its scene only when it is the current scene's or the very next one's (a stranger's serial far ahead is junk).
inline bool serialWaitable(uint16_t eventSerial, uint16_t currentSerial) { return eventSerial == currentSerial || eventSerial == (uint16_t)(currentSerial + 1); }
// F: the host's 1 s check messages (a CRC-only SWITCHMAP, the OBJMAP summary) are worthless for a console that is still loading: never held.
// (a CRC-only SWITCHMAP is count 0 with a NON-zero crc: an empty FULL map has the same bytes with crc 0 and is an answer, not a check.)
bool isPeriodicCheck(uint8_t msgType, const void* data, size_t len);
// Is the object kind of this event (KILL / PURPLE / CHIP) shared in this session (ShareMask)? Other types: true.
inline bool eventKindShared(uint8_t msgType, uint8_t share) {
    return msgType == MP_MSG_KILL ? (share & SHARE_KILL) != 0 : msgType == MP_MSG_PURPLE ? (share & SHARE_PURPLE) != 0 : msgType == MP_MSG_CHIP ? (share & SHARE_CHIP) != 0 : true;
}
// A guest's repair requests (map or object state): an allowance of kMax that refills 1 per kRefillTurns (10 s of turns) and is full again at reset().
// (Before: a fixed 5 per scene that only a matching CRC gave back.)
struct RepairBudget {
    enum { kMax = 5, kRefillTurns = 600 };
    int tokens = kMax;
    uint32_t at = 0;                 // valid while tokens < kMax: the turn the refill clock last advanced
    void reset() { tokens = kMax; at = 0; }
    bool take(uint32_t nowTurn) {
        if (tokens < kMax) {
            const uint32_t elapsed = nowTurn - at;
            if ((int32_t)elapsed >= (int32_t)kRefillTurns) {
                const uint32_t add = elapsed / kRefillTurns;
                tokens = (int)add >= kMax - tokens ? (int)kMax : tokens + (int)add;
                at += add * kRefillTurns;
            }
        }
        if (tokens <= 0) return false;
        if (tokens == kMax) at = nowTurn;   // the clock starts when a full allowance is first used
        tokens--;
        return true;
    }
};

// A console's knowledge of the OTHER side's registry CRC. The scene serial alone does not identify a placement: a reload after
// "Everyone was caught" places the SAME serial again, and the CRC stored for the first placement must not be judged against the
// second one's objects. So the knowledge is dropped when a placement begins, and a disagreement is only reported when it lasts
// kMismatchGrace turns (the peer's fresh CRC is in flight for a moment; sync itself is judged live, so it heals by itself).
constexpr uint32_t kMismatchGrace = 120;
struct PeerCrc {
    bool known = false;
    uint16_t serial = 0;
    uint32_t crc = 0;
    uint32_t badTurns = 0;
    bool logged = false;
    void beginPlacement() { *this = PeerCrc(); }
    void received(uint16_t s, uint32_t c) { known = true; serial = s; crc = c; badTurns = 0; logged = false; }
    bool agrees(uint16_t placedSerial, uint32_t mine) const { return known && serial == placedSerial && crc == mine; }
    bool disagrees(uint16_t placedSerial, uint32_t mine) const { return known && serial == placedSerial && crc != mine; }
    // once per turn while placed: true exactly once when a disagreement has lasted kMismatchGrace turns (log it then)
    bool tick(bool placed, uint16_t placedSerial, uint32_t mine) {
        if (!placed || !disagrees(placedSerial, mine)) { badTurns = 0; logged = false; return false; }
        if (badTurns < kMismatchGrace) badTurns++;
        if (badTurns >= kMismatchGrace && !logged) { logged = true; return true; }
        return false;
    }
};

// optfix item 16: a block that exists only while a Together session needs it (the registry 49 KB, the switch mirror 14 KB, the
// local-writer table 4 KB used to be static bss on every console, single player included). Allocated once at a scene start (never per
// frame), released when the session is over. A failed allocation is remembered until release() or retryNext() (called at each scene start): no retry every message, and every
// user checks get() == nullptr (object / switch sync is simply off for that session; the game plays on).
template < class T >
class LazyMem {
public:
    typedef T* (*AllocFn)();
    typedef void (*FreeFn)(T*);
    T* get() const { return mP; }
    T* acquire(AllocFn alloc) {   // alloc() returns a zeroed T or nullptr
        if (mP == nullptr && !mFailed) {
            mP = alloc();
            if (mP == nullptr) mFailed = true;
        }
        return mP;
    }
    bool failed() const { return mFailed; }
    void retryNext() { mFailed = false; }   // fix2: a scene start tries the allocation again (a failure no longer lasts the whole session)
    void release(FreeFn fr) {
        if (mP != nullptr) fr(mP);
        mP = nullptr;
        mFailed = false;
    }

private:
    T* mP = nullptr;
    bool mFailed = false;
};

}  // namespace MpWorldLogic
