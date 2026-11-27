// GXV_DRAWPIPE (drawpipe24, 10 Nov 2026; default 0): the first pieces of a pipelined game thread, in which the draw side of a turn
// (the J3D draw traversal and the other draw lists that write the GX stream) runs on another core while the game thread already
// runs the next update. DESIGN.md / REPORT.txt of /home/claude/g23/agents/drawpipe24 have the data map and the staged plan.
//
// What a draw running beside the next update needs is that nothing it READS is WRITTEN by that update. J3D keeps two things in
// double buffers for the graphics processor's sake (the draw / normal matrices, J3DMtxBuffer's swaps; the material and differed
// lists, J3DDisplayListObj's swap) and the port keeps that discipline against the front end (GXV_DRAWDONE_DEFER 3/4: only the
// drawn turn's flushes write them, port_gx_view_write_from waits otherwise). Everything else the draw traversal reads is read
// live: the draw-buffer lists (which actors, in which order), the light of each actor, the packets' and models' flags, j3dSys.
//
//   GXV_DRAWPIPE=0  nothing of this runs (one switch read at each of the drawn turn's four points).
//   GXV_DRAWPIPE=2  the census / verify run, pictures unchanged: on one drawn turn in GXV_DRAWPIPE_SAMPLE (default 4) everything the
//                   J3D draw passes read is hashed, object by object and class by class (Class below), at four points: the
//                   hand-over (after the flushes, before the draw), after the draw, after update(), after calcAnim(); and once
//                   more at the next drawn turn's hand-over. An object whose hash moved in an interval was written in it: by
//                   the draw itself, by the update that would overlap the draw, by calc-anim, by the flushes. "drawpipe:"
//                   lines every 300 turns give, per interval and class, the sampled turns with a write, the objects written a
//                   turn and the first one by name. The lists are also frozen at the hand-over (Frame below; its time and
//                   bytes are the snapshot's cost) and compared with the live ones at each pass: a pass that would have drawn
//                   something else from the frozen lists is counted.
//   GXV_DRAWPIPE=1  the pipeline as far as it is built: the J3D passes draw from the frame frozen at the hand-over (the lists, the
//                   light each actor loads) instead of the live lists. The draw itself stays on the game thread: the other
//                   draw lists (shadows, particles, layouts, ...) still read live game state (the log says so, and how many
//                   there are). Exact while nothing between the hand-over and a pass changes a list: a list change in between
//                   (the lists' epoch) makes that pass draw from the live lists, counted. The overlap itself is proven only on
//                   the host (tests/draw_pipe.cpp: the frozen passes on a second thread beside the next update).
//
//
// dpengine (11 Nov 2026) continues it (DESIGN.md / REPORT.txt of /home/claude/g23/agents/dpengine):
//   GXV_DRAWPIPE=1  is now the SERIAL REHEARSAL of the overlap's data flow: besides the frozen lists and lights, each pass is drawn
//                   with the draw side's own J3DSys and J3D statics (j3dSys as it stood at the hand-over, plus the words the game
//                   thread's draw lists changed up to the pass: Tokens / ApplyChanged below), with the camera's matrices, the
//                   player's and the point light and the fog / scene serial frozen at the hand-over, and what the passes wrote
//                   goes back into j3dSys only when the draw is over (MergeChanged). Still on the game thread, in today's order.
//   GXV_DRAWPIPE=3  the WHOLE serial draw (beginRender / draw() / endRender) on the draw thread while the game thread waits at the
//                   hand-over (port/draw_thread.h). No gain; it proves the thread, the GX owner hand-off and the guest-OS rules.
//   GXV_DRAWPIPE=4  stage A: the game thread runs the draw with the GX library recording (port/gx_capture.h), each J3D pass a
//                   token; the draw thread replays the recording and draws each pass from the frozen frame at its token, while the
//                   game thread runs update(); joined before calcAnim().
//   GXV_DRAWPIPE=2  the census also hashes the new frozen fields (camera, scene lights, the scene / fog / fade serial, the star
//                   pointer, the flags that pick GameScene::draw's branches) and counts the drawn turns in which the draw itself
//                   changed one of them after the hand-over.
// Header only; no allocation with operator new (the game's operator new is a guest heap): malloc / realloc, kept.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// (snprintf is called unqualified below: the game's build routes it through a macro of its own)

extern "C++" {  // (may be included inside an extern "C" block, as port/par_async.h is)
namespace port {
namespace drawpipe {

// What the J3D draw passes read, by who owns it. The comments give the reader in the draw and the writers found in the code.
enum Class : int {
    kLists,    // a group's active models and each model's count: DrawBufferGroup::drawOpa; written by show / hide / clip (active, deactive)
    kPackets,  // a drawer's packets and light loaders, in order: DrawBufferShapeDrawer::draw; written by add / remove / resetLightSort
    kMatDL,    // a material's list (pointer, size, bytes): the drawer's call; written by the material flush (and outside it after a wait)
    kDiffDL,   // a packet's differed list: the same, per actor
    kShape,    // a shape packet's words and its shape's flags / matrix words: J3DShapePacket::prepareDraw, J3DShape::drawFast
    kModel,    // a model's flags, cull stamp, matrix-buffer pointers, vertex arrays: prepareDraw
    kDrawMtx,  // the current draw / normal matrix halves: read by the front end through the stream (and by immediate loads)
    kJoints,   // the joint and envelope matrices: the view flush, the shapes that concatenate the view in the draw
    kLight,    // what an actor's light loader loads: ActorLightCtrl::loadLight; written by ActorLightCtrl::update in movement
    kActor,    // an actor's flag bytes (dead, hidden, clipped, ...)
    kSys,      // j3dSys (the view matrix, the model / packet / flags the draw and the calc both write)
    // (dpengine) the small state frozen at the hand-over; one object each
    kCamera,   // the camera's view, inverse view and projection matrices and its pose: lights, environment maps, the view matrix
    kSceneLight,  // the player's light and the point light: MR::loadLight(player) in a pass; written by movement
    kExt,      // the scene / fog / fade serial GXV_J3DNATIVE's brackets carry: FogCtrl::update in movement
    kPointer,  // the star pointer: mode and position (drawn by the draw's tail; written by the pad read in update)
    kBranch,   // the flags that pick GameScene::draw's branches (3D on / off, movie, pause, mirror, captures, HOME, warnings)
    kClasses
};
constexpr int kFirstSmall = kCamera;  // (the first of the small state's classes)
inline const char* ClassName(int c) {
    static const char* const names[kClasses] = {"lists", "packets", "material lists", "differed lists", "shape words", "model words",
                                                "draw matrices", "joints", "lights", "actor flags", "j3dSys",
                                                "camera", "scene lights", "scene serial", "star pointer", "draw branches"};
    return c >= 0 && c < kClasses ? names[c] : "?";
}
// The four points of a drawn turn, and the intervals between them (the last one runs to the next drawn turn's hand-over).
enum Point : int { kPtHandOver, kPtDrawn, kPtUpdated, kPtCalced, kPoints };
enum Interval : int { kIvDraw, kIvUpdate, kIvCalc, kIvFlush, kIntervals };
inline const char* IntervalName(int i) {
    static const char* const names[kIntervals] = {"the draw", "update", "calc-anim", "the flushes"};
    return i >= 0 && i < kIntervals ? names[i] : "?";
}

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
// (a content hash for change detection, not for security: 8 bytes a step)
inline uint64_t HashBytes(const void* p, size_t n, uint64_t h = 0x2545F4914F6CDD1Dull) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    h = Mix(h, n);
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, b, 8);
        h = Mix(h, w);
        b += 8;
        n -= 8;
    }
    if (n) {
        uint64_t w = 0;
        std::memcpy(&w, b, n);
        h = Mix(h, w);
    }
    return h;
}
template <class T> inline uint64_t HashValue(uint64_t h, const T& v) {
    uint64_t w = 0;
    std::memcpy(&w, &v, sizeof v < 8 ? sizeof v : 8);
    return Mix(h, w);
}

// One point's entries: (class, key) -> hash, each key once, in walk order.
struct Entry {
    uintptr_t key;
    uint64_t hash;
    uint32_t cls;
    uint32_t tag;  // (the object's name: Names below; its index among the model's actors in the top 12 bits)
};
class Set {
public:
    void Clear() {
        mCount = 0;
        if (mSlots) std::memset(mSlots, 0xFF, sizeof(uint32_t) * mSlotCount);
        std::memset(objects, 0, sizeof objects);
        std::memset(bytes, 0, sizeof bytes);
        valid = false;
    }
    // false: the key is already in the set (an actor in two groups) or there is no memory (the entry is then left out and counted).
    bool Add(int cls, const void* key, uint64_t hash, uint32_t tag, uint32_t nbytes) {
        if ((mCount + 1) * 2 > mSlotCount && !Grow()) {
            ++dropped;
            return false;
        }
        const uintptr_t k = reinterpret_cast<uintptr_t>(key);
        uint32_t s = Slot(cls, k);
        while (mSlots[s] != 0xFFFFFFFFu) {
            const Entry& e = mEntries[mSlots[s]];
            if (e.key == k && e.cls == static_cast<uint32_t>(cls)) return false;
            s = (s + 1) & (mSlotCount - 1);
        }
        mSlots[s] = mCount;
        Entry& e = mEntries[mCount++];
        e.key = k;
        e.hash = hash;
        e.cls = static_cast<uint32_t>(cls);
        e.tag = tag;
        ++objects[cls];
        bytes[cls] += nbytes;
        return true;
    }
    const Entry* Find(uint32_t cls, uintptr_t k) const {
        if (!mSlotCount) return nullptr;
        uint32_t s = Slot(static_cast<int>(cls), k);
        while (mSlots[s] != 0xFFFFFFFFu) {
            const Entry& e = mEntries[mSlots[s]];
            if (e.key == k && e.cls == cls) return &e;
            s = (s + 1) & (mSlotCount - 1);
        }
        return nullptr;
    }
    uint32_t Count() const { return mCount; }
    const Entry& At(uint32_t i) const { return mEntries[i]; }
    uint32_t objects[kClasses] = {};
    uint64_t bytes[kClasses] = {};
    uint32_t dropped = 0;
    bool valid = false;
    uint32_t turn = 0;  // (the drawn turn it was taken in)

private:
    uint32_t Slot(int cls, uintptr_t k) const {
        return static_cast<uint32_t>(Mix(static_cast<uint64_t>(k), static_cast<uint64_t>(cls) + 1)) & (mSlotCount - 1);
    }
    bool Grow() {
        const uint32_t slots = mSlotCount ? mSlotCount * 2 : 4096;
        if (slots > (1u << 22)) return false;
        Entry* e = static_cast<Entry*>(std::realloc(mEntries, sizeof(Entry) * (slots / 2)));
        if (!e) return false;
        mEntries = e;
        uint32_t* s = static_cast<uint32_t*>(std::realloc(mSlots, sizeof(uint32_t) * slots));
        if (!s) return false;
        mSlots = s;
        mSlotCount = slots;
        std::memset(mSlots, 0xFF, sizeof(uint32_t) * slots);
        for (uint32_t i = 0; i < mCount; ++i) {
            uint32_t at = Slot(static_cast<int>(mEntries[i].cls), mEntries[i].key);
            while (mSlots[at] != 0xFFFFFFFFu) at = (at + 1) & (slots - 1);
            mSlots[at] = i;
        }
        return true;
    }
    Entry* mEntries = nullptr;
    uint32_t* mSlots = nullptr;
    uint32_t mSlotCount = 0;
    uint32_t mCount = 0;
};

// The names of what the entries belong to (a model's resource name, copied: the scene's strings go with the scene).
class Names {
public:
    enum { kMax = 1024, kLen = 24 };
    uint32_t Of(const void* owner, const char* name) {
        const uintptr_t k = reinterpret_cast<uintptr_t>(owner);
        for (uint32_t i = mLast; i < mCount; ++i) {
            if (mKey[i] == k) return mLast = i;
        }
        for (uint32_t i = 0; i < mLast && i < mCount; ++i) {
            if (mKey[i] == k) return mLast = i;
        }
        if (mCount >= kMax) return 0;
        if (!mKey) {
            mKey = static_cast<uintptr_t*>(std::calloc(kMax, sizeof(uintptr_t)));
            mText = static_cast<char*>(std::calloc(kMax, kLen));
            if (!mKey || !mText) {
                mCount = kMax;
                return 0;
            }
            mKey[0] = 0;
            snprintf(mText, kLen, "?");
            mCount = 1;
        }
        mKey[mCount] = k;
        // (a name of the game may be Shift-JIS: the log gets its ASCII, a '?' for the rest)
        char* t = mText + mCount * kLen;
        int n = 0;
        for (const char* p = name ? name : "?"; *p && n < kLen - 1; ++p) t[n++] = (*p >= 0x20 && *p < 0x7F) ? *p : '?';
        t[n] = 0;
        return mLast = mCount++;
    }
    const char* Text(uint32_t tag) const {
        const uint32_t i = tag & 0xFFFFF;
        return mText && i < mCount ? mText + i * kLen : "?";
    }
    void Clear() { mCount = mCount ? 1 : 0, mLast = 0; }

private:
    uintptr_t* mKey = nullptr;
    char* mText = nullptr;
    uint32_t mCount = 0, mLast = 0;
};
inline uint32_t Tag(uint32_t name, uint32_t index) { return (name & 0xFFFFF) | ((index > 0xFFF ? 0xFFF : index) << 20); }

// The census's sums over the sampled turns of one log window.
struct IntervalStat {
    uint32_t samples = 0;
    uint32_t turnsWritten[kClasses] = {};  // sampled turns in which at least one object of the class was written
    uint64_t written[kClasses] = {};       // objects whose hash moved
    uint64_t came[kClasses] = {};          // objects in the read set after the interval and not before it
    uint64_t went[kClasses] = {};
    uint32_t firstTag[kClasses] = {};      // the first written object of the window
    uint32_t firstTurn[kClasses] = {};
    bool firstSet[kClasses] = {};
};
// `before` against `after`: an entry of `after` with another hash was written; one not in `before` came; the rest of `before` went.
inline void Compare(const Set& before, const Set& after, IntervalStat& st) {
    uint32_t matched[kClasses] = {};
    bool any[kClasses] = {};
    for (uint32_t i = 0; i < after.Count(); ++i) {
        const Entry& e = after.At(i);
        const Entry* b = before.Find(e.cls, e.key);
        if (!b) {
            ++st.came[e.cls];
            if (e.cls == kLists || e.cls == kPackets) any[e.cls] = true;
            continue;
        }
        ++matched[e.cls];
        if (b->hash != e.hash) {
            ++st.written[e.cls];
            any[e.cls] = true;
            if (!st.firstSet[e.cls]) {
                st.firstSet[e.cls] = true;
                st.firstTag[e.cls] = e.tag;
                st.firstTurn[e.cls] = after.turn;
            }
        }
    }
    for (int c = 0; c < kClasses; ++c) {
        st.went[c] += before.objects[c] - matched[c];
        if ((c == kLists || c == kPackets) && before.objects[c] != matched[c]) any[c] = true;
        if (any[c]) ++st.turnsWritten[c];
    }
    ++st.samples;
}

// ---------------------------------------------------------------------------------------------------------------- the frozen frame
// What the J3D passes read of the lists, copied at the hand-over: per group its active models in order, per model its drawers
// (opaque first), per drawer its packets in order with what each light loader loads. The game's types are opaque here
// (Game/System/DrawBufferHolder.cpp fills and draws it); the arrays are reused from turn to turn.
struct FrozenPacket {
    void* shapePacket;  // J3DShapePacket*
    int32_t light;      // -2: no light loader; -1: a loader that loads nothing (no area light); else the offset of its light's bytes
    // (dpengine) what the pass asks of the packet, its shape and its model before it draws, as the hand-over saw it: bit 0 the
    // shape is hidden; bit 1 the model is culled in the picture this hand-over draws; bit 2 the packet itself is hidden. The next
    // update writes them (the zoom cull's stamp for the picture after, ClippingActorHolder.cpp; MR::hideMaterial / showMaterial
    // in movement).
    uint32_t flags;
};
enum : uint32_t { kFrozenHidden = 1, kFrozenCulled = 2, kFrozenPacketHidden = 4 };
struct FrozenDrawer {
    const void* drawer;  // DrawBufferShapeDrawer*
    uint32_t first, count;
    uint32_t serial;     // (the drawer's list version, DrawBufferShapeDrawer::mPortSerial, as the pass would have read it)
};
struct FrozenExec {
    const void* exec;  // DrawBufferExecuter*
    int32_t lightType, numActors;
    uint32_t firstDrawer, numOpa, numDrawers;
};
struct FrozenGroup {
    uint32_t firstExec = 0, numExec = 0;
    int32_t lightLoadType = -1;
    uint64_t hash = 0;  // (of everything frozen for the group: the verify's pass-time compare)
};
template <class T> class Arena {
public:
    T* Push(uint32_t n = 1) {
        if (mCount + n > mCap) {
            uint32_t cap = mCap ? mCap * 2 : 256;
            while (cap < mCount + n) cap *= 2;
            T* p = static_cast<T*>(std::realloc(mItems, sizeof(T) * cap));
            if (!p) return nullptr;
            mItems = p;
            mCap = cap;
        }
        T* at = mItems + mCount;
        mCount += n;
        return at;
    }
    void Clear() { mCount = 0; }
    uint32_t Count() const { return mCount; }
    T& operator[](uint32_t i) { return mItems[i]; }
    const T& operator[](uint32_t i) const { return mItems[i]; }

private:
    T* mItems = nullptr;
    uint32_t mCount = 0, mCap = 0;
};
struct Frame {
    Arena<FrozenGroup> groups;
    Arena<FrozenExec> execs;
    Arena<FrozenDrawer> drawers;
    Arena<FrozenPacket> packets;
    Arena<unsigned char> lights;
    const void* owner = nullptr;  // the DrawBufferHolder it was frozen from (another holder's passes are drawn live)
    uint32_t epoch = 0;           // the lists' epoch at the freeze
    bool valid = false;           // frozen in full (no memory ran out)
    bool guardEpoch = true;       // a pass draws live when the lists' epoch moved since the freeze (the serial order's net; the
                                  // host's threaded overlap turns it off: there the lists may move, that is the point)
    uint32_t extSerial = 0;       // (dpengine) J3DPortExtSerial() at the hand-over: what a frozen pass puts in its brackets
    uint32_t Bytes() const {
        return static_cast<uint32_t>(groups.Count() * sizeof(FrozenGroup) + execs.Count() * sizeof(FrozenExec) + drawers.Count() * sizeof(FrozenDrawer) +
                                     packets.Count() * sizeof(FrozenPacket) + lights.Count());
    }
    void Clear() {
        groups.Clear(), execs.Clear(), drawers.Clear(), packets.Clear(), lights.Clear();
        valid = false;
    }
};

// ------------------------------------------------------------------------------------------- why a turn is serial (dpengine)
// The fall-back states of DESIGN.md 2.5 (drawpipe24's numbers in brackets), each a test at the hand-over; plus the engine's own.
enum Reason : int {
    kGo = 0,
    kWhyOff,         // the watchdog (or a guest-OS call on the draw thread) turned the pipeline off for the session
    kWhyNoScene,     // [1] no scene, a scene change, another list executor than last turn
    kWhySettling,    // [1] the first turns after any other reason ended (GXV_DRAWPIPE_SETTLE, default 8)
    kWhyLoading,     // [2] another guest thread ran since the last hand-over (a loading thread; the scene's init)
    kWhySkip,        // [3] a skipped cutscene
    kWhyDemo,        // [4] a movie, a demo or cutscene, any GameScene nerve but the action nerve
    kWhySceneClass,  // [5] not a GameScene (title, file select, star select, galaxy map, staff roll)
    kWhyPause,       // [6] the pause menu (and PETARI_PAUSE_FREEZE's pictures)
    kWhySystem,      // [7] HOME / settings, the error watcher, a system warning, the save sequence, reset / power
    kWhyCapture,     // [8] the mirror pass, VR, an ODH capture, a screen-capture timing, the screen preserver
    kWhyFormSwitch,  // [9] a player form switch on this turn or the two before
    kWhyGuestCall,   // [10] the draw thread made a guest-OS call in the last overlap (counted; the pipeline goes off)
    kWhyLogicGx,     // [11] the logic used GX, or wrote a list / matrix half outside the flushes, in the last turns
    kWhyConfig,      // [12] GXV_DRAWDONE_DEFER below 4, no draw thread, no capture layer or one without the token handler / abandon
    kWhyReview,      // [13] a fur draw (FurCtrl writes shape words in its draw) in the last turns
    kWhyListMoved,   // a list changed between the hand-over and its pass (the draw itself): the capture was ended and replayed here
    kWhyDrawWrite,   // a matrix half or a list was written inside the draw, outside the flushes (the recorded calls point at it)
    kWhyBusy,        // the capture layer said "not now" (port_gx_capture_begin returned false: GXV_GXCAPTURE=0, a display list open, ...)
    kWhyAbandoned,   // the capture layer abandoned the capture (what it had recorded was executed in order, on the game thread)
    kWhyNoMemory,    // no memory for the frame or the tokens
    kWhyNoPass,      // (pipe27) the picture had no J3D pass to hand over
    kReasons
};
inline const char* ReasonName(int r) {
    static const char* const names[kReasons] = {"-", "off", "no scene", "settling", "another guest thread ran", "skipped cutscene", "demo / movie / nerve",
                                                "scene class", "pause", "HOME / warning / save / reset", "mirror / capture", "form switch",
                                                "guest call on the draw thread", "logic used GX", "configuration", "fur", "a list moved in the draw",
                                                "a matrix or list written in the draw",
                                                "capture busy", "capture abandoned", "no memory", "no J3D pass"};
    return r >= 0 && r < kReasons ? names[r] : "?";
}

// ----------------------------------------------------------------------------------------------------------- tokens (dpengine)
// A draw whose GX calls are recorded (port/gx_capture.h) cannot draw a J3D pass where it stands: the pass is the draw thread's.
// It leaves a token instead: "this pass happens here". The id handed to the capture layer is the token's index.
enum TokenKind : uint32_t { kTokPass = 1 };
struct TokenRec {
    uint32_t kind;
    int32_t a, b;   // the pass: buffer type, translucent
    uint32_t ctx;   // where its J3D context snapshot starts in Tokens::bytes
};
struct Tokens {
    Arena<TokenRec> recs;
    Arena<unsigned char> bytes;
    void Clear() { recs.Clear(), bytes.Clear(); }
};
// `cur` against `prev`, word by word: a word that changed is written into dst. (What the game thread's draw lists wrote into a
// context between two passes, carried over into the draw side's own copy.)
inline void ApplyChanged(void* dst, const void* prev, const void* cur, size_t bytes) {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* p = static_cast<const unsigned char*>(prev);
    const unsigned char* c = static_cast<const unsigned char*>(cur);
    size_t i = 0;
    for (; i + 4 <= bytes; i += 4) {
        if (std::memcmp(p + i, c + i, 4) != 0) std::memcpy(d + i, c + i, 4);
    }
    for (; i < bytes; ++i) {
        if (p[i] != c[i]) d[i] = c[i];
    }
}
// The context after the draw (par_dl.h's MergeWords rule): `live` is the game thread's (its draw lists and its update wrote into
// it since `pre`), `mine` the draw side's. A word the draw side changed is taken, unless the game thread changed it as well: then
// the game thread's stands (it wrote later in the serial order: the draw's tail, the update). *taken / *conflicts: counted.
inline void MergeChanged(void* live, const void* pre, const void* mine, size_t bytes, uint32_t* taken, uint32_t* conflicts) {
    unsigned char* l = static_cast<unsigned char*>(live);
    const unsigned char* p = static_cast<const unsigned char*>(pre);
    const unsigned char* m = static_cast<const unsigned char*>(mine);
    size_t i = 0;
    for (; i + 4 <= bytes; i += 4) {
        if (std::memcmp(m + i, p + i, 4) != 0) {
            if (std::memcmp(l + i, p + i, 4) != 0) {
                ++*conflicts;
            } else {
                std::memcpy(l + i, m + i, 4);
                ++*taken;
            }
        }
    }
    for (; i < bytes; ++i) {
        if (m[i] != p[i]) {
            if (l[i] != p[i]) {
                ++*conflicts;
            } else {
                l[i] = m[i];
                ++*taken;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------- the driver
// The four points of a drawn turn (GameSystem::frameLoop), the sampling and the log. The game's glue (DrawBufferHolder.cpp
// port_drawpipe_point) and the host test both drive it; `Host` gives the walk over the live read set, the freeze and the log.
struct Host {
    virtual void Walk(Set& set, Names& names) = 0;                  // everything the J3D passes read, hashed
    virtual bool Freeze(Frame& frame) = 0;                          // the lists into the frame (false: nothing to freeze)
    virtual void UseFrozen(const Frame* frame) = 0;                 // the passes draw from it (nullptr: from the live lists)
    virtual int64_t NowNs() = 0;
    virtual const unsigned char* StreamPos() = 0;                   // the GX stream's write position
    virtual void OpenLists(uint32_t* lists, uint32_t* objects) = 0; // the draw lists that are not J3D passes: how many, how many objects
    virtual void Log(const char* line) = 0;
    // (dpengine) the small state: copied at the hand-over, with the frame; which of its fields (a bit per Class from kFirstSmall)
    // are no longer what was frozen; the draw of this hand-over is still outstanding on the draw thread (the frame stays in use)
    virtual void FreezeSmall() {}
    virtual uint32_t SmallDiffer() { return 0; }
    virtual bool DrawOutstanding() { return false; }

protected:
    ~Host() {}
};
struct PassStats {  // (counted by the passes themselves; plain counters of the thread that draws)
    uint32_t frozenPasses = 0, livePasses = 0, epochFallbacks = 0, verifyPasses = 0, verifyDiffer = 0;
    // (dpengine) passes that left other J3D statics than their token expected (the game thread's draw lists go on from what the
    // token expects); j3dSys words a pass left otherwise than expected (the two matrix-array words apart, mtxMiss: a shape with
    // several matrix groups sets them again as it draws, and nothing reads them before the next shape sets them); of all those,
    // the words put right in j3dSys after the draw, and the words the game thread had written since (its value stands)
    uint32_t predictMiss = 0, sysMiss = 0, mtxMiss = 0, sysTaken = 0, sysConflicts = 0;
    uint32_t mergedDeferred = 0;  // merged lists (GXV_J3D_MATMERGE) a pass on the draw thread left for the game thread to make
    // (dpengine 2) passes asked for whose buffer type had no model at the hand-over (no token is made for them); and the time
    // inside the passes themselves, measured on the turns the driver marks (timeThis): the part of the draw that can leave the
    // game thread
    uint32_t emptyPasses = 0;
    uint64_t passNs = 0;
    bool timeThis = false;
    char firstDiffer[Names::kLen] = {};
};
class Driver {
public:
    Frame frame;
    PassStats pass;
    // (dpengine 2) set before the hand-over's Point by a caller that already knows this turn's draw is serial (modes 3 / 4,
    // a turn that does not qualify): nothing is frozen, the passes are drawn from the live lists, the turn pays no bookkeeping.
    bool skipFreeze = false;
    // `mode`: GXV_DRAWPIPE; `sample`: one drawn turn in this many is hashed (mode 2); `sceneOk`: a scene with draw buffers is up
    // and is not changing. Called on the thread that runs the turn.
    void Point(Host& host, int point, int mode, int sample, bool sceneOk) {
        if (mode <= 0) return;
        if (point == kPtHandOver) {
            ++mTurn;
            host.UseFrozen(nullptr);
            pass.timeThis = false;
            if (!sceneOk) {
                Reset();
                ++mNoScene;
            } else if (skipFreeze) {
                frame.Clear();
                mSmallFrozen = false;
                ++mNotFrozen;
            } else {
                const int64_t t0 = host.NowNs();
                frame.Clear();
                const bool ok = host.Freeze(frame) && frame.valid;
                mFreezeNs += static_cast<uint64_t>(host.NowNs() - t0);
                if (ok) {
                    ++mFreezes;
                    mFreezeBytes += frame.Bytes();
                    mFreezePackets += frame.packets.Count();
                    uint32_t lists = 0, objects = 0;
                    host.OpenLists(&lists, &objects);
                    mOpenLists += lists;
                    mOpenObjects += objects;
                    // (mode 1 draws from the frame; mode 2 keeps drawing live and compares at each pass: the frame is handed
                    // over either way, the passes ask the mode)
                    host.FreezeSmall();
                    // (the passes' own time: clock reads around each pass on every eighth frozen turn)
                    pass.timeThis = mode != 2 && (mFreezes & 7u) == 0;
                    if (pass.timeThis) ++mTimedTurns;
                    host.UseFrozen(&frame);
                    mSmallFrozen = true;
                }
            }
        } else if (point == kPtDrawn) {
            if (mSmallFrozen && mode == 2) {
                // (the draw itself changed a frozen field after the hand-over: a copy made there is not what its reader saw)
                const uint32_t differ = host.SmallDiffer();
                for (int c = kFirstSmall; c < kClasses; ++c) mSmallDiffer[c - kFirstSmall] += (differ >> (c - kFirstSmall)) & 1u;
            }
            mSmallFrozen = false;
            // (GXV_DRAWPIPE=4: the draw thread still draws from the frame; the join ends its use)
            if (!host.DrawOutstanding()) host.UseFrozen(nullptr);
        }
        if (mode != 2) {
            if (point == kPtCalced) MaybeLog(host, mode);
            return;
        }
        // ---- the census
        const unsigned char* pos = host.StreamPos();
        if (point == kPtHandOver) {
            // (the interval that ends here began at the last sampled turn's calc-anim point, if that was the turn before)
            const bool tail = sceneOk && mSampling && mSets[kPtCalced].valid && mSets[kPtCalced].turn + 1 == mTurn;
            mSampling = sceneOk && (sample <= 1 || mTurn % static_cast<uint32_t>(sample) == 0);
            if (tail || mSampling) {
                Take(host, mScratch);
                if (tail && sceneOk) Compare(mSets[kPtCalced], mScratch, mStat[kIvFlush]);
                Swap(mSets[kPtHandOver], mScratch);
            }
            for (int p = kPtDrawn; p < kPoints; ++p) mSets[p].valid = false;
            if (!mSampling) mSets[kPtHandOver].valid = false;
        } else if (mSampling) {
            if (!sceneOk) {
                // (a scene change inside the turn: what is left of it is not compared)
                mSampling = false;
                for (int p = 0; p < kPoints; ++p) mSets[p].valid = false;
            } else {
                Take(host, mSets[point]);
                if (mSets[point - 1].valid) Compare(mSets[point - 1], mSets[point], mStat[point - 1]);
            }
        }
        // (bytes the phases wrote into the GX stream: the draw's are the picture; update's and calc-anim's are GX use by the logic,
        // which a draw on another thread would have to own or wait for. A segment change in between gives no figure.)
        if (point != kPtHandOver && mSampling && mLastPos && pos >= mLastPos && pos - mLastPos < (8 << 20)) {
            mStreamBytes[point - 1] += static_cast<uint64_t>(pos - mLastPos);
            if (pos != mLastPos) ++mStreamTurns[point - 1];
        }
        mLastPos = pos;
        if (point == kPtCalced) MaybeLog(host, mode);
    }
    // A scene went (its heaps with it): nothing kept from it may be compared or named again.
    void Reset() {
        for (int p = 0; p < kPoints; ++p) mSets[p].valid = false;
        mSampling = false;
        mSmallFrozen = false;
        frame.Clear();
        frame.owner = nullptr;
    }
    // A turn without a picture ran: the interval from the last sampled turn's calc-anim to the next hand-over is not the flushes' alone.
    void Undrawn() { mSets[kPtCalced].valid = false; }
    // The sums since the start (the log's windows added up, and the open one): what the host test checks.
    IntervalStat Total(int interval) const {
        IntervalStat t = mTotal[interval];
        Add(t, mStat[interval]);
        return t;
    }
    uint32_t Turn() const { return mTurn; }
    // (the drawn turns, since the start, in which the draw changed the small field of class c after the hand-over)
    uint32_t SmallDifferTotal(int c) const { return mSmallTotal[c - kFirstSmall] + mSmallDiffer[c - kFirstSmall]; }
    // The window's lines now (the host test reads them at its end).
    void Flush(Host& host, int mode) {
        if (mWindowTurns) LogWindow(host, mode);
    }

private:
    static void Swap(Set& a, Set& b) {
        Set t = a;
        a = b;
        b = t;
    }
    void Take(Host& host, Set& set) {
        const int64_t t0 = host.NowNs();
        set.Clear();
        set.turn = mTurn;
        host.Walk(set, mNames);
        set.valid = true;
        mWalkNs += static_cast<uint64_t>(host.NowNs() - t0);
        ++mWalks;
        uint64_t b = 0;
        uint32_t o = 0;
        for (int c = 0; c < kClasses; ++c) b += set.bytes[c], o += set.objects[c];
        mWalkBytes += b;
        mWalkObjects += o;
        mDropped += set.dropped;
        set.dropped = 0;
    }
    void MaybeLog(Host& host, int mode) {
        if (++mWindowTurns < 300) return;
        LogWindow(host, mode);
    }
    void LogWindow(Host& host, int mode) {
        char line[1300];
        const double fr = mFreezes ? 1.0 / mFreezes : 0.0;
        int o = snprintf(line, sizeof line,
                              "drawpipe: GXV_DRAWPIPE=%d, %u drawn turns (%u without a scene to freeze, %u serial ones not frozen); the frozen lists a "
                              "turn: %.0f packets, %.1f KiB, %.3f ms; J3D passes from the frozen lists %u (%u of them with nothing to draw: no "
                              "token), from the live lists %u (%u because a list changed after the hand-over); the passes themselves %.3f ms a "
                              "frozen turn (timed on %u turns: the part of the draw that can leave the game thread); "
                              "%.1f draw lists a turn with %.1f objects read live game state (they stay with the game thread); what the passes "
                              "left against what their tokens expected: J3D statics wrong in %u passes, %u j3dSys words wrong, %u matrix-array "
                              "words (harmless: set again before they are read); %u words put right after the draw, %u written by the game "
                              "thread since; merged lists left to the game thread %u",
                              mode, mWindowTurns, mNoScene, mNotFrozen, mFreezePackets * fr, mFreezeBytes * fr / 1024.0, mFreezeNs * fr / 1e6,
                              pass.frozenPasses, pass.emptyPasses, pass.livePasses, pass.epochFallbacks,
                              mTimedTurns ? pass.passNs / 1e6 / mTimedTurns : 0.0, mTimedTurns, mOpenLists * fr, mOpenObjects * fr, pass.predictMiss, pass.sysMiss,
                              pass.mtxMiss, pass.sysTaken, pass.sysConflicts, pass.mergedDeferred);
        if (mode == 2) {
            snprintf(line + o, sizeof line - static_cast<size_t>(o),
                          "; verify: %u passes compared with their frozen lists, %u differed%s%s", pass.verifyPasses, pass.verifyDiffer,
                          pass.verifyDiffer ? " (first: " : "", pass.verifyDiffer ? pass.firstDiffer : "");
            if (pass.verifyDiffer) std::strncat(line, ")", sizeof line - std::strlen(line) - 1);
        }
        host.Log(line);
        if (mode == 2) {
            // (drawn turns in which the draw changed a field after it was frozen: must all be 0 for the frozen copy to be exact)
            o = snprintf(line, sizeof line, "drawpipe: frozen at the hand-over and changed by the draw itself (drawn turns):");
            for (int c = kFirstSmall; c < kClasses; ++c) {
                o += snprintf(line + o, sizeof line - static_cast<size_t>(o), " %s %u;", ClassName(c), mSmallDiffer[c - kFirstSmall]);
            }
            host.Log(line);
        }
        if (mode == 2) {
            const double wk = mWalks ? 1.0 / mWalks : 0.0;
            snprintf(line, sizeof line,
                          "drawpipe: census: %u sampled turns of %u; the J3D passes' read set at a point: %.0f objects, %.0f KiB hashed in %.2f ms "
                          "(%u entries left out); GX stream bytes a sampled turn: the draw %.0f, update %.0f (%u turns), calc-anim %.0f (%u turns)",
                          mStat[kIvDraw].samples, mWindowTurns, mWalkObjects * wk, mWalkBytes * wk / 1024.0, mWalkNs * wk / 1e6, mDropped,
                          Per(mStreamBytes[kIvDraw], mStat[kIvDraw].samples), Per(mStreamBytes[kIvUpdate], mStat[kIvUpdate].samples),
                          mStreamTurns[kIvUpdate], Per(mStreamBytes[kIvCalc], mStat[kIvCalc].samples), mStreamTurns[kIvCalc]);
            host.Log(line);
            for (int iv = 0; iv < kIntervals; ++iv) {
                const IntervalStat& st = mStat[iv];
                o = snprintf(line, sizeof line, "drawpipe: %s wrote (%u samples; class: turns with a write, objects a turn [came/went], first):",
                                  IntervalName(iv), st.samples);
                bool any = false;
                for (int c = 0; c < kClasses && o < static_cast<int>(sizeof line) - 120; ++c) {
                    if (!st.turnsWritten[c] && !st.written[c] && !st.came[c] && !st.went[c]) continue;
                    any = true;
                    o += snprintf(line + o, sizeof line - static_cast<size_t>(o), " %s %u, %.1f [%.1f/%.1f]", ClassName(c), st.turnsWritten[c],
                                       Per(st.written[c], st.samples), Per(st.came[c], st.samples), Per(st.went[c], st.samples));
                    if (st.firstSet[c]) {
                        o += snprintf(line + o, sizeof line - static_cast<size_t>(o), " '%s' #%u turn %u", mNames.Text(st.firstTag[c]),
                                           st.firstTag[c] >> 20, st.firstTurn[c]);
                    }
                    o += snprintf(line + o, sizeof line - static_cast<size_t>(o), ";");
                }
                if (!any) snprintf(line + o, sizeof line - static_cast<size_t>(o), " nothing");
                host.Log(line);
            }
        }
        mWindowTurns = 0;
        mNoScene = 0;
        mNotFrozen = 0;
        mTimedTurns = 0;
        mFreezes = 0;
        mFreezeBytes = mFreezePackets = mFreezeNs = 0;
        mOpenLists = mOpenObjects = 0;
        mWalks = 0;
        mWalkNs = mWalkBytes = mWalkObjects = 0;
        mDropped = 0;
        for (int c = 0; c < kClasses - kFirstSmall; ++c) mSmallTotal[c] += mSmallDiffer[c], mSmallDiffer[c] = 0;
        pass = PassStats();
        for (int iv = 0; iv < kIntervals; ++iv) {
            Add(mTotal[iv], mStat[iv]);
            mStat[iv] = IntervalStat();
            mStreamBytes[iv] = 0;
            mStreamTurns[iv] = 0;
        }
    }
    static double Per(uint64_t v, uint32_t n) { return n ? static_cast<double>(v) / n : 0.0; }
    static void Add(IntervalStat& to, const IntervalStat& from) {
        to.samples += from.samples;
        for (int c = 0; c < kClasses; ++c) {
            to.turnsWritten[c] += from.turnsWritten[c];
            to.written[c] += from.written[c];
            to.came[c] += from.came[c];
            to.went[c] += from.went[c];
            if (!to.firstSet[c] && from.firstSet[c]) {
                to.firstSet[c] = true;
                to.firstTag[c] = from.firstTag[c];
                to.firstTurn[c] = from.firstTurn[c];
            }
        }
    }

    Set mSets[kPoints], mScratch;
    Names mNames;
    IntervalStat mStat[kIntervals], mTotal[kIntervals];
    uint64_t mStreamBytes[kIntervals] = {};
    uint32_t mStreamTurns[kIntervals] = {};
    const unsigned char* mLastPos = nullptr;
    uint32_t mTurn = 0, mWindowTurns = 0, mNoScene = 0, mFreezes = 0, mWalks = 0, mDropped = 0;
    uint32_t mNotFrozen = 0, mTimedTurns = 0;
    uint64_t mFreezeBytes = 0, mFreezePackets = 0, mFreezeNs = 0, mOpenLists = 0, mOpenObjects = 0;
    uint64_t mWalkNs = 0, mWalkBytes = 0, mWalkObjects = 0;
    bool mSampling = false;
    bool mSmallFrozen = false;
    uint32_t mSmallDiffer[kClasses - kFirstSmall] = {}, mSmallTotal[kClasses - kFirstSmall] = {};
};

}  // namespace drawpipe
}  // namespace port
}  // extern "C++"
