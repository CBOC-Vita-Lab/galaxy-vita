// GXV_UNDRAWN_LEAN (undrawn26, 12 Nov): the pure parts of the lean undrawn turn. Header only, no game types, so the host test (tests/undrawn_lean_test.cpp)
// compiles it as it is.
//
// What a turn without a picture may leave out: a PRODUCT of the turn (a joint matrix set, a shadow cast) that is recomputed from the actor's current
// state every turn and that only the next picture, or nothing, reads. Such a product is left out ("skipped") and made once, just before the next
// picture ("flushed"). Nothing may read it in between. This header holds
//   - the mode parsing (0 off, 1 on, 2 verify),
//   - Book: the products skipped (mode 1) or that WOULD be skipped (mode 2), by object, "dirty" until the next flush; one flush = one counter step,
//   - ExportSet: the objects whose product the game's logic asked for at any time (joint matrices handed out by MR::getJointMtx etc.): never skipped,
//   - the two eligibility predicates as pure functions of facts (so the host test can state the rule),
//   - Fnv64: the hash of the gameplay state two console runs are diffed by,
//   - Census: the counters of the "undrawn-lean:" line.
// Everything is allocation free (fixed tables); a full table answers "conservative" (exported / not skippable), never "free".
#pragma once
#include <stdint.h>
#include <string.h>

namespace port {
namespace undrawn {

enum Product : unsigned { kShadow = 0, kJoint = 1, kProducts = 2 };

// "0" / unset: 0, "1": 1, "2": 2 (anything else: 0).
inline int ParseMode(const char* e) {
    if (e == nullptr || e[0] == 0 || e[1] != 0) return 0;
    return e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0;
}

// Open addressing, linear probing, no erase. key 0 = empty. Ensure fails (-1) when the table is three quarters full.
template <unsigned LogN>
struct PtrTable {
    static constexpr unsigned N = 1u << LogN;
    uintptr_t key[N];
    uint32_t val[N];
    const void* tag[N];
    unsigned used;
    void Clear() {
        memset(key, 0, sizeof key);
        memset(val, 0, sizeof val);
        memset(tag, 0, sizeof tag);
        used = 0;
    }
    static unsigned Slot(uintptr_t k) { return static_cast<unsigned>((static_cast<uint32_t>(k >> 3) * 0x9E3779B1u) >> (32 - LogN)); }
    // The slot of p, or -1 when absent.
    int Find(const void* p) const {
        const uintptr_t k = reinterpret_cast<uintptr_t>(p);
        if (k == 0) return -1;
        for (unsigned i = 0, s = Slot(k); i < N; ++i, s = (s + 1) & (N - 1)) {
            if (key[s] == k) return static_cast<int>(s);
            if (key[s] == 0) return -1;
        }
        return -1;
    }
    // The slot of p (made if absent), or -1 when full.
    int Ensure(const void* p) {
        const uintptr_t k = reinterpret_cast<uintptr_t>(p);
        if (k == 0) return -1;
        for (unsigned i = 0, s = Slot(k); i < N; ++i, s = (s + 1) & (N - 1)) {
            if (key[s] == k) return static_cast<int>(s);
            if (key[s] == 0) {
                if (used * 4 >= N * 3) return -1;
                key[s] = k;
                val[s] = 0;
                tag[s] = nullptr;
                ++used;
                return static_cast<int>(s);
            }
        }
        return -1;
    }
};

// Objects whose product the logic has asked for. Never expires (the scene's drop clears it). A full table: Exported() answers true for everything.
template <unsigned LogN>
struct ExportSet {
    PtrTable<LogN> t;
    bool full;
    void Clear() {
        t.Clear();
        full = false;
    }
    // Returns true when p was not in the set before (a first export).
    bool Add(const void* p) {
        if (t.Find(p) >= 0) return false;
        if (t.Ensure(p) < 0) {
            full = true;
            return false;
        }
        return true;
    }
    bool Exported(const void* p) const { return full || t.Find(p) >= 0; }
};

// Products skipped since the last flush, by object. Mark(p, owner) sets p dirty; Flush() makes all clean in one step.
template <unsigned LogN>
struct Book {
    PtrTable<LogN> t;
    uint32_t epoch;  // a mark is dirty when its value is epoch + 1
    bool full;
    void Clear() {
        t.Clear();
        epoch = 0;
        full = false;
    }
    void Mark(const void* p, const void* owner) {
        const int s = t.Ensure(p);
        if (s < 0) {
            full = true;
            return;
        }
        t.val[s] = epoch + 1;
        t.tag[s] = owner;
    }
    bool Dirty(const void* p) const {
        const int s = t.Find(p);
        return s >= 0 && t.val[s] == epoch + 1;
    }
    const void* Owner(const void* p) const {
        const int s = t.Find(p);
        return s >= 0 ? t.tag[s] : nullptr;
    }
    // Everything marked so far is clean (the product has been made). The table is cleared now and then so that dead objects do not fill it.
    void Flush() {
        ++epoch;
        if (epoch >= 0x7FFFFF00u || t.used * 2 >= PtrTable<LogN>::N) Clear();
    }
};

// ---- eligibility ------------------------------------------------------------------------------------------------------------------
// MR::CalcAnimType values the rule looks at (the raw category of the calc-anim list the actor is in).
enum { kCatEnvironment = 4, kCatMapObj = 5, kCatNPC = 6, kCatEnemy = 8, kCatPlayer = 9, kCatPlayerDeco = 10, kCatMapObjDeco = 11, kCatItem = 16 };

struct JointFacts {
    int category;        // the raw calc-anim category, -1 unknown
    bool collision;      // the actor has collision parts of its own (their matrices follow the joints)
    bool camera;         // an ActorPadAndCameraCtrl
    bool exported;       // MR::getJointMtx & co. handed out (or read) a joint of this model at any time
    bool jointCallback;  // a J3DJoint of the model has a callback (it may write game state while the joints are computed)
    bool stale;          // already deferred by an older rule (the old rules decide)
    bool onFrameThread;  // false: built on the loading thread, computed at once
    bool posedOnce;      // the pose has been made at least once (mCrPosed)
};
// Why not (index into Census::refused), or -1 when the joints may be left out of a turn and made once before the next picture.
inline int JointRefusal(const JointFacts& f) {
    if (!f.onFrameThread || !f.posedOnce || f.stale) return 4;
    if (f.collision) return 0;
    if (f.camera) return 1;
    if (f.exported) return 2;
    if (f.jointCallback) return 3;
    switch (f.category) {
        case kCatEnvironment: case kCatMapObj: case kCatNPC: case kCatMapObjDeco: case kCatEnemy: case kCatItem: return -1;
        default: return 4;  // players, layouts, collision lists, unknown
    }
}
inline bool JointDefers(const JointFacts& f) { return JointRefusal(f) < 0; }

struct ShadowFacts {
    bool logicRead;     // an accessor of ActorShadowUtil that the logic uses has been called for this controller (mPbLogicRead)
    unsigned collMode;  // _60: 0 off, 1 every turn, 2 one-time (counted)
    unsigned gravMode;  // _61: 0 off, 1 drop gravity, 2 one-time, 3 off (private), 4 private gravity, 5 private one-time
    bool pbShadow;      // the Performance-mode far clip is on (its hysteresis state is not idempotent)
    unsigned age;       // turns since the controller first asked to cast
    unsigned minAge;
};
// May this controller's cast be left out of a turn and made once before the next picture?
inline bool ShadowDefers(const ShadowFacts& f) {
    if (f.logicRead || f.pbShadow) return false;
    if (f.collMode == 2 || f.gravMode == 2 || f.gravMode == 5) return false;  // counted casts: leaving one out changes how many are made
    return f.age >= f.minAge;
}

// ---- hash ---------------------------------------------------------------------------------------------------------------------------
struct Fnv64 {
    uint64_t h = 14695981039346656037ull;
    void Byte(uint8_t b) { h = (h ^ b) * 1099511628211ull; }
    void U32(uint32_t v) {
        for (int i = 0; i < 4; ++i) Byte(static_cast<uint8_t>(v >> (8 * i)));
    }
    void F32(float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        U32(v);
    }
};

// ---- census -------------------------------------------------------------------------------------------------------------------------
struct Census {
    uint32_t turns, undrawnTurns, flushes;
    uint32_t skipped[kProducts];     // mode 1: products left out
    uint32_t wouldSkip[kProducts];   // mode 2: products that mode 1 would have left out
    uint32_t flushed[kProducts];     // products made by a flush
    uint32_t violations[kProducts];  // mode 2: logic read a dirty product
    uint32_t lateReads[kProducts];   // mode 1: logic asked for a product that was left out (it is exported from then on)
    uint32_t refused[5];             // joints: why an actor was not eligible (JointRefusal's index)
};


// ---- the tracker: everything perf.cpp's GXV_UNDRAWN_LEAN entry points do apart from reading the environment and writing the log ---------------
struct Tracker {
    ExportSet<12> exports;
    Book<11> book[kProducts];
    PtrTable<11> seen;  // shadow controllers: the turn each first asked
    Census c;
    unsigned pend[kProducts];  // distinct products left out since the last flush
    uint32_t turn;             // game turns of this stage
    void Reset() {  // a new stage: its objects are new
        exports.Clear();
        book[0].Clear();
        book[1].Clear();
        seen.Clear();
        pend[0] = pend[1] = 0;
        turn = 0;
    }
    void Init() {
        Reset();
        memset(&c, 0, sizeof c);
    }
    // May this model's joints be left out of the turn? facts.exported is filled here. mode 1: true = leave out; mode 2: counted and marked, false.
    bool JointDecide(int mode, const void* model, const void* owner, JointFacts f) {
        if (mode == 0 || model == nullptr) return false;
        f.exported = exports.Exported(model);
        const int why = JointRefusal(f);
        if (why >= 0) {
            ++c.refused[why];
            return false;
        }
        auto& b = book[kJoint];
        if (!b.Dirty(model)) ++pend[kJoint];
        b.Mark(model, owner);
        if (mode == 2) {
            ++c.wouldSkip[kJoint];
            return false;
        }
        ++c.skipped[kJoint];
        return true;
    }
    bool ShadowDecide(int mode, const void* ctrl, const void* owner, ShadowFacts f) {
        if (mode == 0 || ctrl == nullptr) return false;
        const int s = seen.Ensure(ctrl);
        unsigned age = 0;
        if (s >= 0) {
            if (seen.val[s] == 0) seen.val[s] = turn + 1;
            age = turn + 1 - seen.val[s];
        }
        f.logicRead = f.logicRead || exports.Exported(ctrl);
        f.age = age;
        if (!ShadowDefers(f)) return false;
        auto& b = book[kShadow];
        if (!b.Dirty(ctrl)) ++pend[kShadow];
        b.Mark(ctrl, owner);
        if (mode == 2) {
            ++c.wouldSkip[kShadow];
            return false;
        }
        ++c.skipped[kShadow];
        return true;
    }
    // The logic reads a product of obj. True: it was left out (mode 2: a VIOLATION; mode 1: a late read). The object is exported from now on.
    bool Read(unsigned product, int mode, const void* obj) {
        if (obj == nullptr) return false;
        const bool dirty = book[product].Dirty(obj);
        if (dirty) {
            if (mode == 2) ++c.violations[product];
            else ++c.lateReads[product];
        }
        exports.Add(obj);
        return dirty;
    }
    // A picture is about to be drawn: everything left out has been made.
    void Flush() {
        for (unsigned p = 0; p < kProducts; ++p) {
            c.flushed[p] += pend[p];
            pend[p] = 0;
            book[p].Flush();
        }
        ++c.flushes;
    }
};

}  // namespace undrawn
}  // namespace port
