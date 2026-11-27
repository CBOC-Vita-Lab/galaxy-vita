// Galaxy 3.0 multiplayer: shared world state (Play Together), pure logic. No game headers (host unit tests build this file).
#include "Game/MP/MpWorldLogic.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace MpWorldLogic {

// ---- hashing / CRC ---------------------------------------------------------------------------------------------------------
uint32_t crc32(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t nameHash32(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* q = (const unsigned char*)s; q && *q; q++) {
        h ^= *q;
        h *= 16777619u;
    }
    return h;
}

// ---- registry --------------------------------------------------------------------------------------------------------------
int compareKey(const MpObjKey& a, const MpObjKey& b) {
    if (a.zone != b.zone) return a.zone < b.zone ? -1 : 1;
    if (a.nameHash != b.nameHash) return a.nameHash < b.nameHash ? -1 : 1;
    if (a.lid != b.lid) return a.lid < b.lid ? -1 : 1;
    if (a.sub != b.sub) return a.sub < b.sub ? -1 : 1;
    return 0;
}

static bool keyEqual(const MpObjKey& a, const MpObjKey& b) { return compareKey(a, b) == 0; }

static uint32_t ptrHash(const void* p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 17;
    v *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(v >> 32);
}

void Registry::clear() {
    mCount = 0;
    mDups = 0;
    mCrc = 0;
    mReady = false;
    mOverflow = false;
}

bool Registry::add(const MpObjKey& key, uint32_t classHash, const void* actor, uint8_t flags) {
    if (mReady) return false;                 // finalized: the scene's registry is closed
    if (mCount >= kMax) {
        mOverflow = true;
        return false;
    }
    Entry& e = mEntries[mCount++];
    e.key = key;
    e.flags = flags;
    e.classHash = classHash;
    e.actor = actor;
    return true;
}

void Registry::finalize() {
    // Shell sort (stable enough: equal keys are flagged as duplicates anyway). n <= 2560, once per scene.
    int n = mCount;
    int gap = 1;
    while (gap < n / 3) gap = gap * 3 + 1;
    for (; gap > 0; gap /= 3) {
        for (int i = gap; i < n; i++) {
            Entry t = mEntries[i];
            int j = i;
            while (j >= gap && compareKey(t.key, mEntries[j - gap].key) < 0) {
                mEntries[j] = mEntries[j - gap];
                j -= gap;
            }
            mEntries[j] = t;
        }
    }
    mDups = 0;
    for (int i = 0; i < n; i++) {
        bool dup = (i > 0 && keyEqual(mEntries[i].key, mEntries[i - 1].key)) ||
                   (i + 1 < n && keyEqual(mEntries[i].key, mEntries[i + 1].key));
        if (dup) {
            mEntries[i].flags |= ENTRY_DUP;
            mDups++;
        }
    }
    for (int i = 0; i < kHashSize; i++) mHash[i] = 0xFFFF;
    for (int i = 0; i < n; i++) {
        if (!mEntries[i].actor) continue;
        uint32_t h = ptrHash(mEntries[i].actor) & (kHashSize - 1);
        while (mHash[h] != 0xFFFF) h = (h + 1) & (kHashSize - 1);
        mHash[h] = (uint16_t)i;
    }
    uint32_t c = 0;
    uint8_t rec[10];
    for (int i = 0; i < n; i++) {
        const Entry& e = mEntries[i];
        MpWriter w(rec, sizeof rec);
        mp_write_key(w, e.key);
        w.u32(e.classHash);
        c = crc32(c, rec, w.n);
    }
    uint8_t tail[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)mOverflow, 0};
    mCrc = crc32(c, tail, sizeof tail);
    mReady = true;
}

int Registry::findKey(const MpObjKey& key) const {
    if (!mReady) return -1;
    int lo = 0, hi = mCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = compareKey(mEntries[mid].key, key);
        if (c == 0) return (mEntries[mid].flags & ENTRY_DUP) ? -1 : mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int Registry::findActor(const void* actor) const {
    if (!mReady || !actor) return -1;
    uint32_t h = ptrHash(actor) & (kHashSize - 1);
    for (int probes = 0; probes < kHashSize; probes++) {
        uint16_t i = mHash[h];
        if (i == 0xFFFF) return -1;
        if (mEntries[i].actor == actor) return i;
        h = (h + 1) & (kHashSize - 1);
    }
    return -1;
}

bool Registry::setFlagsForActor(const void* actor, uint8_t flags) {
    int i = findActor(actor);
    if (i < 0) return false;
    mEntries[i].flags |= flags;
    return true;
}

// ---- policy tables ---------------------------------------------------------------------------------------------------------
namespace {
    enum Cond : uint8_t { COND_ALWAYS = 0, COND_ARG0_SET = 1 };
    struct PolicyRow {
        const char* name;       // placement name (exact)
        const char* field;      // switch field, nullptr = any
        uint8_t cond;
    };
    // Switches whose state depends on THIS console's player or that are derived from other switches on every console.
    const PolicyRow kLocalRows[] = {
        {"SwitchCube", "SW_A", COND_ARG0_SET},        // SwitchArea level mode: offSwitchA() every frame while the player is outside
        {"SwitchCylinder", "SW_A", COND_ARG0_SET},
        {"SwitchSphere", "SW_A", COND_ARG0_SET},
        {"Tamakoro", "SW_A", COND_ALWAYS},            // riding the star ball
        {"TamakoroWithTutorial", "SW_A", COND_ALWAYS},
        {"Fluff", "SW_A", COND_ALWAYS},               // holding a dandelion fluff
        {"MarioLauncher", "SW_B", COND_ALWAYS},       // aiming in the launcher
        {"SwitchSynchronizer", "SW_A", COND_ALWAYS},  // derived from SW_B on every console
        {"SwitchSynchronizerReverse", "SW_A", COND_ALWAYS},
        {"GroupSwitchWatcher", "SW_A", COND_ALWAYS},  // derived from SW_B.. on every console
        {"TimerSwitch", "SW_A", COND_ALWAYS},         // derived from SW_B after a delay on every console
    };
    const char* const kBossPrefixes[] = {
        "Boss",          // BossBegoman, BossStinkBug, BossKameck
        "DinoPackun", "Dodoryu", "Koopa", "OtaKing", "Polta", "SkeletalFish", "TombSpider", "TripodBoss", "TriPodBoss",
    };

    bool streq(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *a == *b) { a++; b++; }
        return *a == *b;
    }
    bool startsWith(const char* s, const char* prefix) {
        if (!s) return false;
        while (*prefix) {
            if (*s != *prefix) return false;
            s++;
            prefix++;
        }
        return true;
    }
}  // namespace

SwitchPolicy switchPolicy(const char* placementName, const char* switchName, bool hasArg0, int32_t arg0) {
    if (!placementName) return SW_SYNC;
    for (size_t i = 0; i < sizeof kLocalRows / sizeof kLocalRows[0]; i++) {
        const PolicyRow& r = kLocalRows[i];
        if (!streq(r.name, placementName)) continue;
        if (r.field && !streq(r.field, switchName)) continue;
        if (r.cond == COND_ARG0_SET && !(hasArg0 && arg0 != -1)) continue;
        return SW_LOCAL;
    }
    return SW_SYNC;
}

bool isBossName(const char* placementName) {
    for (size_t i = 0; i < sizeof kBossPrefixes / sizeof kBossPrefixes[0]; i++) {
        if (startsWith(placementName, kBossPrefixes[i])) return true;
    }
    return false;
}

// ---- switch mirror ---------------------------------------------------------------------------------------------------------
int zoneSlot(int wireZone) {
    if (wireZone == kGlobalWire) return kGlobalSlot;
    if (wireZone >= 0 && wireZone < kGlobalSlot) return wireZone;
    return -1;
}

int slotWire(int slot) { return slot == kGlobalSlot ? kGlobalWire : slot; }

void SwitchMirror::reset(uint16_t serial) {
    memset(mOn, 0, sizeof mOn);
    resetScene();
    mSerial = serial;
    mValid = true;
}

void SwitchMirror::resetScene() {
    memset(mLocal, 0, sizeof mLocal);
    memset(mGuard, 0, sizeof mGuard);
    memset(mWinStart, 0, sizeof mWinStart);
    memset(mWinCount, 0, sizeof mWinCount);
}

bool SwitchMirror::oscEdge(int slot, int bit, uint32_t now, bool* tripped) {
    if (tripped) *tripped = false;
    if (mGuard[slot].get(bit)) return true;
    uint16_t now16 = (uint16_t)now;
    if (mWinCount[slot][bit] == 0 || (uint16_t)(now16 - mWinStart[slot][bit]) >= kOscWindow) {
        mWinStart[slot][bit] = now16;
        mWinCount[slot][bit] = 0;
    }
    mWinCount[slot][bit]++;
    if (mWinCount[slot][bit] > kOscEdges) {
        mGuard[slot].set(bit, true);
        if (tripped) *tripped = true;
        return true;
    }
    return false;
}

static uint32_t crcSlot(uint32_t c, int slot, const BitSet128& b) {
    uint8_t rec[kMapEntry];
    MpWriter w(rec, sizeof rec);
    w.u8((uint8_t)slotWire(slot));
    for (int i = 0; i < 4; i++) w.u32(b.w[i]);
    return crc32(c, rec, w.n);
}

uint32_t SwitchMirror::crc() const {
    uint32_t c = 0;
    for (int s = 0; s < kZoneSlots; s++) {
        if (mOn[s].any()) c = crcSlot(c, s, mOn[s]);
    }
    return c;
}

int SwitchMirror::zonesWithBits() const {
    int n = 0;
    for (int s = 0; s < kZoneSlots; s++) n += mOn[s].any() ? 1 : 0;
    return n;
}

// ---- messages --------------------------------------------------------------------------------------------------------------
size_t writeSwitch(void* buf, size_t cap, const SwitchMsg& m) {
    MpWriter w(buf, cap);
    w.u16(m.serial); w.u8(m.zone); w.u8(m.bit); w.u8(m.on);
    return w.ok() ? w.n : 0;
}

size_t writeObj(void* buf, size_t cap, const ObjMsg& m) {
    MpWriter w(buf, cap);
    w.u16(m.serial);
    mp_write_key(w, m.key);
    return w.ok() ? w.n : 0;
}

size_t writeRegCrc(void* buf, size_t cap, const RegCrcMsg& m) {
    MpWriter w(buf, cap);
    w.u16(m.serial); w.u32(m.crc);
    if (m.flags) w.u8(m.flags);
    return w.ok() ? w.n : 0;
}

size_t writeMap(void* buf, size_t cap, uint16_t serial, const SwitchMirror& mirror, bool full) {
    MpWriter w(buf, cap);
    w.u16(serial);
    w.u32(mirror.crc());
    if (!full) {
        w.u8(0);
        return w.ok() ? w.n : 0;
    }
    w.u8((uint8_t)mirror.zonesWithBits());
    for (int s = 0; s < kZoneSlots; s++) {
        const BitSet128& b = mirror.onBits(s);
        if (!b.any()) continue;
        w.u8((uint8_t)slotWire(s));
        for (int i = 0; i < 4; i++) w.u32(b.w[i]);
    }
    return w.ok() ? w.n : 0;
}

static bool zoneOk(int zone, int zoneCount) {
    if (zone == kGlobalWire) return true;
    return zone < zoneCount && zone < kGlobalSlot;
}

bool readSwitch(const void* data, size_t len, int zoneCount, SwitchMsg* out) {
    MpReader r(data, len);
    out->serial = r.u16(); out->zone = r.u8(); out->bit = r.u8(); out->on = r.u8();
    return r.done() && zoneOk(out->zone, zoneCount) && out->bit < kBits && out->on <= 1;
}

bool readObj(const void* data, size_t len, int zoneCount, ObjMsg* out) {
    MpReader r(data, len);
    out->serial = r.u16();
    bool keyOk = mp_read_key(r, &out->key);
    return keyOk && r.done() && out->key.zone < zoneCount && out->key.zone < kGlobalSlot;
}

bool readRegCrc(const void* data, size_t len, RegCrcMsg* out) {
    MpReader r(data, len);
    out->serial = r.u16(); out->crc = r.u32(); out->flags = 0;
    if (r.ok() && len == (size_t)kRegCrcLenFlags) out->flags = r.u8();
    return r.done() && (len == (size_t)kRegCrcLen || out->flags == (uint8_t)kRegCrcRepair);   // canonical: 6 bytes, or 7 with exactly the repair flag
}

bool readMap(const void* data, size_t len, int zoneCount, MapMsg* out) {
    MpReader r(data, len);
    out->serial = r.u16(); out->crc = r.u32(); out->count = r.u8();
    if (!r.ok() || out->count > kMapMaxZones) return false;
    bool seen[kZoneSlots] = {};
    for (int i = 0; i < out->count; i++) {
        out->zone[i] = r.u8();
        for (int k = 0; k < 4; k++) out->bits[i].w[k] = r.u32();
        if (!r.ok() || !zoneOk(out->zone[i], zoneCount)) return false;
        int s = zoneSlot(out->zone[i]);
        if (s < 0 || seen[s] || !out->bits[i].any()) return false;   // canonical maps hold no duplicates and no empty zones
        seen[s] = true;
    }
    if (!r.done()) return false;
    // A full map must be self-consistent (its crc is the crc of its entries, in canonical ascending order).
    if (out->count > 0) {
        for (int i = 1; i < out->count; i++) {
            if (zoneSlot(out->zone[i]) <= zoneSlot(out->zone[i - 1])) return false;
        }
        if (mapCrc(*out) != out->crc) return false;
    }
    return true;
}

uint32_t mapCrc(const MapMsg& m) {
    uint32_t c = 0;
    for (int i = 0; i < m.count; i++) c = crcSlot(c, zoneSlot(m.zone[i]), m.bits[i]);
    return c;
}

bool mapIsFull(const MapMsg& m) { return m.count > 0 || m.crc == 0; }

int applyMap(SwitchMirror& mirror, const MapMsg& m, bool authoritative, ApplyFn apply, void* ctx) {
    BitSet128 target[kZoneSlots];
    memset(target, 0, sizeof target);
    for (int i = 0; i < m.count; i++) target[zoneSlot(m.zone[i])] = m.bits[i];
    int changed = 0;
    for (int s = 0; s < kZoneSlots; s++) {
        for (int b = 0; b < kBits; b++) {
            if (mirror.isLocal(s, b)) continue;
            bool want = target[s].get(b);
            bool have = mirror.getOn(s, b);
            if (want == have || (!want && !authoritative)) continue;
            mirror.setOn(s, b, want);
            changed++;
            if (apply && !mirror.isGuarded(s, b)) apply(ctx, s, b, want);
        }
    }
    return changed;
}

// ---- pending ---------------------------------------------------------------------------------------------------------------
bool PendingQueue::push(uint8_t kind, uint16_t index, uint32_t deadline, Item* evicted) {
    for (int i = 0; i < mCount; i++) {
        if (mItems[i].kind == kind && mItems[i].index == index) {
            mItems[i].deadline = deadline;
            return false;
        }
    }
    bool dropped = false;
    if (mCount >= kMax) {
        if (evicted) *evicted = mItems[0];
        memmove(&mItems[0], &mItems[1], sizeof(Item) * (kMax - 1));
        mCount = kMax - 1;
        dropped = true;
    }
    mItems[mCount].kind = kind;
    mItems[mCount].index = index;
    mItems[mCount].deadline = deadline;
    mCount++;
    return dropped;
}

bool EarlyQueue::push(uint8_t type, uint8_t fromSlot, uint16_t serial, const MpObjKey& key, uint32_t deadline, Item* evicted) {
    for (int i = 0; i < mCount; i++) {
        const Item& it = mItems[i];
        if (it.type == type && it.fromSlot == fromSlot && it.serial == serial && compareKey(it.key, key) == 0) { mItems[i].deadline = deadline; return false; }
    }
    bool dropped = false;
    int victim = -1;
    if (countFrom(fromSlot) >= kPerSender) {
        for (int i = 0; i < mCount; i++) if (mItems[i].fromSlot == fromSlot) { victim = i; break; }   // this sender's oldest
    } else if (mCount >= kMax) {
        victim = 0;
    }
    if (victim >= 0) {
        if (evicted) *evicted = mItems[victim];
        memmove(&mItems[victim], &mItems[victim + 1], sizeof(Item) * (size_t)(mCount - victim - 1));
        mCount--;
        dropped = true;
    }
    Item& d = mItems[mCount++];
    d.type = type; d.fromSlot = fromSlot; d.serial = serial; d.key = key; d.deadline = deadline;
    return dropped;
}

// ---- object state map --------------------------------------------------------------------------------------------------------
static inline bool bitGet(const uint8_t* b, int i) { return (b[i >> 3] >> (i & 7)) & 1u; }
void packObjState(const Registry& reg, ObjState* out, uint8_t share, ActorDeadFn isDead) {
    memset(out, 0, sizeof *out);
    const int n = reg.count();
    out->usedBytes = (n + 7) / 8;
    for (int i = 0; i < n && i < Registry::kMax; i++) {
        const uint8_t f = reg.at(i).flags;
        if (f & ENTRY_DUP) continue;
        const bool k = (share & SHARE_KILL) && (f & ENTRY_ENEMY) && (f & ENTRY_KILLED) && (!isDead || isDead(reg.at(i).actor));
        const bool p = (share & SHARE_PURPLE) && (f & ENTRY_PURPLE) && (f & ENTRY_GOT);
        const bool c = (share & SHARE_CHIP) && (f & ENTRY_CHIP) && (f & ENTRY_GOT);
        const bool v[kObjKinds] = {k, p, c};
        for (int t = 0; t < kObjKinds; t++) {
            if (!v[t]) continue;
            out->bits[t][i >> 3] |= (uint8_t)(1u << (i & 7));
            out->count[t]++;
        }
    }
    uint32_t c = 0;
    for (int t = 0; t < kObjKinds; t++) c = crc32(c, out->bits[t], (size_t)out->usedBytes);
    out->crc = c;
}

void carryClear(ObjCarry* c) { memset(c, 0, sizeof *c); }
void carryDrop(ObjCarry* c) { carryClear(c); c->blocked = true; }
void carryArm(ObjCarry* c) { c->blocked = false; }
bool carryMayApply(bool isHost, int othersInScene) { return isHost && othersInScene > 0; }

void carryNote(ObjCarry* c, uint16_t serial, uint32_t regCrc, int kind, int idx) {
    if (c->blocked || kind < OM_KILL || kind > OM_CHIP || idx < 0 || idx >= Registry::kMax) return;
    if (!c->valid || c->serial != serial || c->regCrc != regCrc) {
        carryClear(c);
        c->valid = true;
        c->serial = serial;
        c->regCrc = regCrc;
    }
    uint8_t& b = c->st.bits[kind - 1][idx >> 3];
    const uint8_t m = (uint8_t)(1u << (idx & 7));
    if (b & m) return;
    b |= m;
    c->st.count[kind - 1]++;
}

int carryApply(ObjCarry* c, uint16_t serial, uint32_t regCrc, int regCount, uint8_t share, ObjApplyFn apply, void* ctx) {
    if (!c->valid) return 0;
    if (c->serial != serial || c->regCrc != regCrc) { carryClear(c); return 0; }
    static const uint8_t kShare[kObjKinds] = {SHARE_KILL, SHARE_PURPLE, SHARE_CHIP};
    int calls = 0;
    for (int t = 0; t < kObjKinds; t++) {
        if (!(share & kShare[t])) continue;
        for (int i = 0; i < regCount && i < Registry::kMax; i++) {
            if (!(c->st.bits[t][i >> 3] & (1u << (i & 7)))) continue;
            apply(ctx, t + 1, i);
            calls++;
        }
    }
    return calls;
}

bool carryBound(const ObjCarry& c, uint16_t serial, uint32_t regCrc) { return c.valid && c.serial == serial && c.regCrc == regCrc; }

bool carryOutstanding(const ObjCarry& c, const Registry& reg, int kind, int idx) {
    if (kind < OM_KILL || kind > OM_CHIP || idx < 0 || idx >= reg.count() || idx >= Registry::kMax) return false;
    if (!(c.st.bits[kind - 1][idx >> 3] & (1u << (idx & 7)))) return false;
    const uint8_t f = reg.at(idx).flags;
    if (f & ENTRY_DUP) return false;
    if (kind == OM_KILL) return (f & ENTRY_ENEMY) && !(f & ENTRY_KILLED);
    if (kind == OM_PURPLE) return (f & ENTRY_PURPLE) && !(f & ENTRY_GOT);
    return (f & ENTRY_CHIP) && !(f & ENTRY_GOT);
}

int carryPrune(ObjCarry* c, const Registry& reg, ActorDeadFn isDead) {
    if (!c->valid || !isDead) return 0;
    int cleared = 0;
    for (int i = 0; i < reg.count() && i < Registry::kMax; i++) {
        const uint8_t m = (uint8_t)(1u << (i & 7));
        uint8_t& b = c->st.bits[0][i >> 3];
        if (!(b & m)) continue;
        const Entry& e = reg.at(i);
        if ((e.flags & ENTRY_KILLED) && !isDead(e.actor)) { b &= (uint8_t)~m; if (c->st.count[0]) c->st.count[0]--; cleared++; }
    }
    return cleared;
}

int carryPass(ObjCarry* c, const Registry& reg, uint8_t share, int maxTries, CarryTryFn tryFn, void* ctx, int* remaining) {
    static const uint8_t kShare[kObjKinds] = {SHARE_KILL, SHARE_PURPLE, SHARE_CHIP};
    int done = 0, left = 0;
    if (c->valid) {
        for (int t = 0; t < kObjKinds; t++) {
            if (!(share & kShare[t])) continue;
            for (int i = 0; i < reg.count() && i < Registry::kMax; i++) {
                if (!carryOutstanding(*c, reg, t + 1, i)) continue;
                // (only successes count against the bound: a bit that cannot be put back yet - a coin that has not appeared - must not use up the
                // turn's tries and starve the ones behind it; a failing try is cheap and the failures are at most the outstanding bits)
                if (done < maxTries && tryFn(ctx, t + 1, i)) { done++; continue; }
                left++;
            }
        }
    }
    if (remaining) *remaining = left;
    return done;
}

void carryMerge(const ObjCarry& c, uint16_t serial, uint32_t regCrc, uint8_t share, const Registry& reg, ObjState* st) {
    if (!carryBound(c, serial, regCrc)) return;
    static const uint8_t kShare[kObjKinds] = {SHARE_KILL, SHARE_PURPLE, SHARE_CHIP};
    for (int t = 0; t < kObjKinds; t++) {
        if (!(share & kShare[t])) continue;
        for (int i = 0; i < st->usedBytes * 8 && i < reg.count(); i++) {
            if (carryOutstanding(c, reg, t + 1, i)) st->bits[t][i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }
    uint32_t crc = 0;
    for (int t = 0; t < kObjKinds; t++) {
        unsigned n = 0;
        for (int i = 0; i < st->usedBytes && i < kObjMapMaxBytes; i++) n += (unsigned)__builtin_popcount(st->bits[t][i]);
        st->count[t] = (uint16_t)n;
        crc = crc32(crc, st->bits[t], (size_t)st->usedBytes);
    }
    st->crc = crc;
}

size_t writeObjSummary(void* buf, size_t cap, uint16_t serial, uint32_t regCrc, const ObjState& st) {
    MpWriter w(buf, cap);
    w.u16(serial); w.u32(regCrc); w.u8(OM_SUMMARY);
    for (int t = 0; t < kObjKinds; t++) w.u16(st.count[t]);
    w.u32(st.crc);
    return w.ok() ? w.n : 0;
}

size_t writeObjChunk(void* buf, size_t cap, uint16_t serial, uint32_t regCrc, int kind, const ObjState& st, int start, int n) {
    if (kind < OM_KILL || kind > OM_CHIP || start < 0 || n < 1 || n > kObjMapChunkBytes || start + n > st.usedBytes || start + n > kObjMapMaxBytes) return 0;
    MpWriter w(buf, cap);
    w.u16(serial); w.u32(regCrc); w.u8((uint8_t)kind); w.u16((uint16_t)start); w.u8((uint8_t)n);
    w.bytes(st.bits[kind - 1] + start, (size_t)n);
    return w.ok() ? w.n : 0;
}

bool objChunkHasBits(const ObjState& st, int kind, int start, int n) {
    if (kind < OM_KILL || kind > OM_CHIP || start < 0 || n < 0 || start + n > kObjMapMaxBytes) return false;
    for (int i = 0; i < n; i++) if (st.bits[kind - 1][start + i]) return true;
    return false;
}

bool readObjMap(const void* data, size_t len, ObjMapMsg* out) {
    MpReader r(data, len);
    memset(out, 0, sizeof *out);
    out->serial = r.u16(); out->regCrc = r.u32(); out->kind = r.u8();
    if (!r.ok() || out->kind > OM_CHIP) return false;
    if (out->kind == OM_SUMMARY) {
        for (int t = 0; t < kObjKinds; t++) {
            out->count[t] = r.u16();
            if (out->count[t] > Registry::kMax) return false;
        }
        out->crc = r.u32();
        return r.done();
    }
    out->start = r.u16(); out->n = r.u8();
    if (!r.ok() || out->n < 1 || out->n > kObjMapChunkBytes || (int)out->start + out->n > kObjMapMaxBytes) return false;
    r.bytes(out->bytes, out->n);
    return r.done();
}

int applyObjChunk(const ObjMapMsg& m, int regCount, ObjApplyFn apply, void* ctx) {
    if (m.kind < OM_KILL || m.kind > OM_CHIP || !apply) return 0;
    int calls = 0;
    for (int i = 0; i < m.n; i++) {
        if (!m.bytes[i]) continue;
        for (int b = 0; b < 8; b++) {
            if (!((m.bytes[i] >> b) & 1u)) continue;
            const int idx = ((int)m.start + i) * 8 + b;
            if (idx >= regCount) continue;
            apply(ctx, m.kind, idx);
            calls++;
        }
    }
    return calls;
}

bool objStateBehind(const ObjMapMsg& h, const ObjState& mine) {
    if (h.kind != OM_SUMMARY) return false;
    bool same = true;
    for (int t = 0; t < kObjKinds; t++) {
        if (h.count[t] > mine.count[t]) return true;
        if (h.count[t] != mine.count[t]) same = false;
    }
    return same && h.crc != mine.crc;
}

// ---- drop accounting ---------------------------------------------------------------------------------------------------------
int EarlyQueue::countFrom(uint8_t fromSlot) const {
    int n = 0;
    for (int i = 0; i < mCount; i++) n += mItems[i].fromSlot == fromSlot ? 1 : 0;
    return n;
}

bool isPeriodicCheck(uint8_t msgType, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    if (msgType == MP_MSG_SWITCHMAP) {                                                              // serial 2, crc 4, count 0 (and crc != 0: else an empty full map)
        if (len != (size_t)kMapHeader || d[6] != 0) return false;
        return (d[2] | d[3] | d[4] | d[5]) != 0;
    }
    if (msgType == MP_MSG_OBJMAP) return len == (size_t)kObjSummaryLen && d[6] == OM_SUMMARY;      // serial 2, regCrc 4, kind 0
    return false;
}

const char* dropTypeName(int t) {
    static const char* const n[DT_COUNT] = {"switch", "kill", "purple", "chip", "other"};
    return (t >= 0 && t < DT_COUNT) ? n[t] : "?";
}
const char* dropReasonName(int r) {
    static const char* const n[DR_COUNT] = {"serial", "not-placed", "reg-off", "no-crc", "crc-mismatch", "queue-full", "expired",
                                            "held-full", "unknown-key", "wrong-kind", "malformed", "rate", "far", "no-mem", "not-host"};
    return (r >= 0 && r < DR_COUNT) ? n[r] : "?";
}
DropType dropTypeOfMsg(uint8_t msgType) {
    switch (msgType) {
    case MP_MSG_SWITCH: return DT_SWITCH;
    case MP_MSG_KILL: return DT_KILL;
    case MP_MSG_PURPLE: return DT_PURPLE;
    case MP_MSG_CHIP: return DT_CHIP;
    default: return DT_OTHER;
    }
}

void DropStats::reset() {
    memset(mN, 0, sizeof mN);
    memset(mSeen, 0, sizeof mSeen);
    mTokens = kLogBurst;
    mRefillAt = 0;
    mSummaryAt = 0;
    mSummaryTotal = 0;
    mInit = false;
}

bool DropStats::note(DropType t, DropReason r, uint32_t nowTurn) {
    if (t >= DT_COUNT || r >= DR_COUNT) return false;
    if (mN[t][r] != 0xFFFFFFFFu) mN[t][r]++;
    if (!mInit) { mInit = true; mTokens = kLogBurst; mRefillAt = nowTurn; }
    const uint32_t gap = nowTurn - mRefillAt;
    if ((int32_t)gap >= (int32_t)kLogRefillTurns) {
        const uint32_t add = gap / kLogRefillTurns;
        mTokens = (mTokens + add > (uint32_t)kLogBurst) ? (uint32_t)kLogBurst : mTokens + add;
        mRefillAt = nowTurn;
    }
    const uint16_t bit = (uint16_t)(1u << r);
    if (!(mSeen[t] & bit)) { mSeen[t] |= bit; return true; }   // the first of each (type, reason): always said
    if (mTokens == 0) return false;
    mTokens--;
    return true;
}
bool DropStats::summaryDue(uint32_t nowTurn) {
    const uint32_t t = total();
    if (t == mSummaryTotal) return false;                      // nothing new since the last summary
    if (mSummaryTotal == 0 && mSummaryAt == 0) mSummaryAt = nowTurn;   // the first drop starts the clock
    if ((int32_t)(nowTurn - mSummaryAt) < (int32_t)kSummaryTurns) return false;
    mSummaryAt = nowTurn;
    mSummaryTotal = t;
    return true;
}
uint32_t DropStats::typeTotal(DropType t) const {
    uint32_t n = 0;
    if (t < DT_COUNT) for (int r = 0; r < DR_COUNT; r++) n += mN[t][r];
    return n;
}
uint32_t DropStats::reasonTotal(DropReason r) const {
    uint32_t n = 0;
    if (r < DR_COUNT) for (int t = 0; t < DT_COUNT; t++) n += mN[t][r];
    return n;
}
uint32_t DropStats::total() const {
    uint32_t n = 0;
    for (int t = 0; t < DT_COUNT; t++) n += typeTotal((DropType)t);
    return n;
}
static void putf(char* buf, size_t cap, size_t* n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void putf(char* buf, size_t cap, size_t* n, const char* fmt, ...) {
    if (*n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (w > 0) *n = (*n + (size_t)w < cap) ? *n + (size_t)w : cap - 1;
}
size_t DropStats::format(char* buf, size_t cap, const char* scene) const {
    if (!buf || cap == 0) return 0;
    size_t n = 0;
    buf[0] = 0;
    putf(buf, cap, &n, "world drops scene=%s switch=%u kill=%u purple=%u chip=%u other=%u reason=", scene ? scene : "-", (unsigned)typeTotal(DT_SWITCH),
        (unsigned)typeTotal(DT_KILL), (unsigned)typeTotal(DT_PURPLE), (unsigned)typeTotal(DT_CHIP), (unsigned)typeTotal(DT_OTHER));
    bool any = false;
    for (int r = 0; r < DR_COUNT; r++) {
        const uint32_t c = reasonTotal((DropReason)r);
        if (!c) continue;
        putf(buf, cap, &n, "%s%s:%u", any ? "," : "", dropReasonName(r), (unsigned)c);
        any = true;
    }
    if (!any) putf(buf, cap, &n, "none");
    return n;
}

}  // namespace MpWorldLogic
