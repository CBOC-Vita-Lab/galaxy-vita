// Galaxy 3.0 multiplayer: the automatic puppet quality governor, pure logic (owner: perf3; no game headers, host tested in
// tests/mp/quality_test.cpp). The game side is MpQuality.cpp.
//
// Up to three remote players cost the console extra work every picture (their joints, draw calls, shadow volumes). In a scene
// that already fills the frame budget that can tip the pictures over it. The governor watches the time between pictures while
// a session runs and, when they miss the target for a whole window, takes one step down; it climbs back one step at a time when
// there is room again. Nothing to set: single player never runs it (MpPuppets::update returns before it without a session).
//
// optfix (owner rule: never lower visible quality for speed, no 30 fps assumption): the governor's CEILING is level 0 by default, so by
// default it never changes anything: level 0 is the puppets' own level of detail (beyond 5500 units, where a puppet is a few pixels:
// no shadow, joints on every other picture) and the other savings (MpLod: culling, tiny skip, distance bands) are not the governor's.
// Levels 1-4 trade visible quality (shadows, animation rate, nearest-two) for speed: they are a TEST SWITCH only
// (SMG_MP_QUALITY_AUTO=<ceiling 1..4> lets the governor use them; SMG_MP_QUALITY=<n> holds one). The target picture time defaults to
// 60 fps (mp_frame_target_us); 30 only when a real 30 fps cap is engaged.
//
// Levels (each keeps everything the one before it gave up):
//   0  the default: the puppets' own level of detail (beyond the far distance no shadow and joints on every other picture)
//   1  no puppet shadows
//   2  distant puppets (beyond the "half rate" distance) compute their joints on every other picture
//   3  at most the two nearest remote puppets at full rate; the rest on every other picture
//   4  a shorter far distance (far puppets: no shadow, every other picture; with 2-3 even the nearest go half rate beyond it)
//
// Windows: pictures are collected for windowUs (3 s); a window with fewer than minFrames pictures, or one cut by a gap (a load, a
// system pause: one interval longer than gapUs), gives no verdict. A window's p95 picture interval against the target:
//   MISS  p95 > target * missPct %          and remote puppets were drawn in at least half of its pictures: one step down
//   ROOM  p95 < target * roomPct %          (the pictures come faster than the target: an uncapped desktop), or the renderer's
//         own work estimate a picture < target * workRoomPct % (under a frame cap the intervals sit at the target whatever the
//         load: only the work says how much is left): upWindows such windows in a row, one step up
//   CALM  neither: no misses, but no proof of room either. trialWindows calm windows in a row (x4 when the work estimate is known
//         and says "no room"): one trial step up.
// A step up that misses within two judged windows steps down again and doubles the wait before the next step up (backoff, up to
// maxBackoff); a step up that holds for two judged windows resets it (HELD windows, late without puppets on screen, judge nothing). At most one step per window, either way. Every change is reported (the caller
// logs "quality level N (frame p95 X ms)").
#pragma once

#include <stdint.h>

namespace MpQuality {
    enum { LEVEL_MAX = 4 };

    // What a level asks of the puppets (MpRemotePlayer.cpp lodWantsCalc / updateInteraction). Distances from the camera, in units.
    struct LevelParams {
        bool shadows;          // remote puppets cast shadows (the far ones never do)
        float farIn, farOut;   // far LOD (hysteresis: far beyond farIn, near again inside farOut)
        float halfIn, halfOut; // "distant": joints on every other picture beyond halfIn (0: no such rule)
        bool nearestTwo;       // only the two nearest remote puppets may run at full rate
    };

    static inline LevelParams paramsFor(int level) {
        LevelParams p;
        p.shadows = level < 1;
        p.farIn = level >= 4 ? 2500.0f : 5500.0f;   // (5500 / 4500: the puppets' own far LOD, as before the governor)
        p.farOut = level >= 4 ? 2000.0f : 4500.0f;
        p.halfIn = level >= 2 ? 3500.0f : 0.0f;
        p.halfOut = level >= 2 ? 3000.0f : 0.0f;
        p.nearestTwo = level >= 3;
        return p;
    }

    // Picture intervals of one window: a histogram of 0.5 ms bins up to 127.5 ms (the last bin holds everything longer).
    // One increment a picture; no sorting, no heap.
    class FrameWindow {
    public:
        enum { BINS = 256, BIN_US = 500 };
        FrameWindow() { reset(); }
        void reset() {
            for (int i = 0; i < BINS; i++) mBins[i] = 0;
            mCount = 0;
            mMaxUs = 0;
        }
        void add(uint32_t us) {
            uint32_t b = us / BIN_US;
            if (b >= (uint32_t)BINS) b = BINS - 1;
            mBins[b]++;
            mCount++;
            if (us > mMaxUs) mMaxUs = us;
        }
        uint32_t count() const { return mCount; }
        uint32_t maxUs() const { return mMaxUs; }
        // The interval below which pct % of the pictures came (the upper edge of its bin; the window's maximum when that is lower).
        uint32_t percentileUs(uint32_t pct) const {
            if (mCount == 0) return 0;
            const uint32_t rank = (mCount * pct + 99) / 100;   // ceil: p95 of 60 pictures = the 57th
            uint32_t seen = 0;
            for (int i = 0; i < BINS; i++) {
                seen += mBins[i];
                if (seen >= rank) {
                    const uint32_t edge = (uint32_t)(i + 1) * BIN_US;
                    return i == BINS - 1 || edge > mMaxUs ? mMaxUs : edge;
                }
            }
            return mMaxUs;
        }

    private:
        uint16_t mBins[BINS];
        uint32_t mCount, mMaxUs;
    };

    struct Config {
        uint32_t windowUs = 3000000;  // a window: 3 s of pictures
        uint32_t minFrames = 30;      // fewer pictures in a window: no verdict
        uint32_t gapUs = 250000;      // a longer interval (a load, the system menu, a hitch): the window starts again
        uint16_t missPct = 112;       // p95 above target * 1.12 (30: 37.3 ms; 5 % of pictures a vblank late) misses
        uint16_t roomPct = 85;        // p95 below target * 0.85: room (pictures faster than the target)
        uint16_t workRoomPct = 80;    // the renderer's work estimate below target * 0.80: room (under a cap)
        uint8_t upWindows = 2;        // ROOM windows in a row for a step up (x backoff)
        uint8_t trialWindows = 5;     // CALM windows in a row for a trial step up (x backoff; x4 when the work says "no room")
        uint8_t maxBackoff = 32;      // the wait after failed step ups doubles up to this factor
        uint8_t maxLevel = 0;         // optfix: the deepest level the governor may step to (0: it never changes the level; tests use LEVEL_MAX)
    };

    enum Verdict { V_NONE = 0, V_MISS, V_HELD, V_CALM, V_ROOM };

    class Governor {
    public:
        explicit Governor(const Config& c = Config()) : mCfg(c) { reset(); }

        void reset() {
            mWin.reset();
            mTargetUs = 16667;   // (60 fps: optfix, no 30 fps assumption)
            mLevel = 0;
            mStart = 0;
            mShown = 0;
            mWorkSum = 0;
            mWorkN = 0;
            mRoom = mCalm = 0;
            mBackoff = 1;
            mSinceChange = 0;
            mLastUp = false;
            mLastP95 = 0;
            mLastVerdict = V_NONE;
            mChanges = 0;
            mWindows = 0;
        }
        void setMaxLevel(int level) { mCfg.maxLevel = (uint8_t)(level < 0 ? 0 : level > LEVEL_MAX ? LEVEL_MAX : level); }
        void setTargetUs(uint32_t us) { mTargetUs = us != 0 ? us : 16667; }
        uint32_t targetUs() const { return mTargetUs; }

        // A new scene (or anything that makes the old pictures meaningless): the window starts again, the streaks and the backoff
        // too (a new load); the level stays (the same players, most likely a similar cost).
        void restart() {
            mWin.reset();
            mStart = 0;
            mShown = 0;
            mWorkSum = 0;
            mWorkN = 0;
            mRoom = mCalm = 0;
            mBackoff = 1;
            mSinceChange = 0;
            mLastUp = false;
        }

        // One picture. nowUs: a monotonic clock; intervalUs: the time since the last picture; puppetsShown: remote puppets were
        // drawn in it; workUs: the renderer's work estimate a picture (0: unknown). True when the level changed.
        bool onPicture(uint64_t nowUs, uint32_t intervalUs, bool puppetsShown, uint32_t workUs) {
            if (intervalUs > mCfg.gapUs) {
                mWin.reset();
                mStart = 0;
                mShown = 0;
                mWorkSum = 0;
                mWorkN = 0;
                return false;
            }
            if (mStart == 0) mStart = nowUs ? nowUs : 1;
            mWin.add(intervalUs);
            if (puppetsShown) mShown++;
            if (workUs != 0) {
                mWorkSum += workUs;
                mWorkN++;
            }
            if (nowUs - mStart < mCfg.windowUs) return false;
            return closeWindow();
        }

        int level() const { return mLevel; }
        uint32_t lastP95Us() const { return mLastP95; }
        Verdict lastVerdict() const { return mLastVerdict; }
        uint32_t changes() const { return mChanges; }
        uint32_t windows() const { return mWindows; }   // windows closed (with or without a verdict)
        uint32_t backoff() const { return mBackoff; }

    private:
        bool closeWindow() {
            const uint32_t n = mWin.count();
            const uint32_t p95 = mWin.percentileUs(95);
            const bool shown = mShown * 2 >= n;
            const bool workKnown = mWorkN * 2 >= n;
            const uint64_t workAvg = mWorkN ? mWorkSum / mWorkN : 0;
            mWin.reset();
            mStart = 0;
            mShown = 0;
            mWorkSum = 0;
            mWorkN = 0;
            mWindows++;
            if (n < mCfg.minFrames) {
                mLastVerdict = V_NONE;
                return false;
            }
            mLastP95 = p95;
            const uint64_t t = mTargetUs;
            Verdict v;
            if ((uint64_t)p95 * 100 > t * mCfg.missPct) {
                v = shown ? V_MISS : V_HELD;   // (late pictures without puppets on screen: nothing to give up, and no room either)
            } else if ((uint64_t)p95 * 100 < t * mCfg.roomPct || (workKnown && workAvg * 100 < t * mCfg.workRoomPct)) {
                v = V_ROOM;
            } else {
                v = V_CALM;
            }
            mLastVerdict = v;
            if (v != V_HELD) mSinceChange++;   // (windows that judged the level: HELD ones say nothing about a step up)
            const int before = mLevel;
            switch (v) {
            case V_MISS:
                if (mLastUp && mSinceChange <= 2) {
                    mBackoff = mBackoff * 2 > mCfg.maxBackoff ? mCfg.maxBackoff : mBackoff * 2;   // that step up did not hold
                }
                mLastUp = false;
                mRoom = mCalm = 0;
                if (mLevel < (int)mCfg.maxLevel && mLevel < LEVEL_MAX) mLevel++;
                break;
            case V_HELD:
                mRoom = mCalm = 0;
                break;
            case V_ROOM:
            case V_CALM:
                if (mLastUp && mSinceChange >= 2) {
                    mBackoff = 1;   // the last step up held for two windows
                    mLastUp = false;
                }
                if (v == V_ROOM) {
                    mRoom++;
                } else {
                    mRoom = 0;
                }
                mCalm++;
                if (mLevel > 0) {
                    const uint32_t needRoom = (uint32_t)mCfg.upWindows * mBackoff;
                    const uint32_t needCalm = (uint32_t)mCfg.trialWindows * mBackoff * (workKnown ? 4u : 1u);
                    if (mRoom >= needRoom || mCalm >= needCalm) {
                        mLevel--;
                        mLastUp = true;
                        mRoom = mCalm = 0;
                    }
                }
                break;
            default:
                break;
            }
            if (mLevel != before) {
                mSinceChange = 0;
                mChanges++;
                return true;
            }
            return false;
        }

        Config mCfg;
        FrameWindow mWin;
        uint32_t mTargetUs;
        int mLevel;
        uint64_t mStart;
        uint32_t mShown;
        uint64_t mWorkSum;
        uint32_t mWorkN;
        uint32_t mRoom, mCalm;
        uint32_t mBackoff;
        uint32_t mSinceChange;
        bool mLastUp;
        uint32_t mLastP95;
        Verdict mLastVerdict;
        uint32_t mChanges;
        uint32_t mWindows;
    };

    // Level 3: which remote slots may run at full rate (the two nearest of those drawn). d2[s] < 0: not drawn / not a remote
    // puppet. Bit s of the result set: full rate allowed. Ties go to the lower slot (stable from turn to turn).
    static inline uint32_t nearestTwoMask(const float* d2, int slots) {
        int a = -1, b = -1;
        for (int s = 0; s < slots; s++) {
            if (d2[s] < 0.0f) continue;
            if (a < 0 || d2[s] < d2[a]) {
                b = a;
                a = s;
            } else if (b < 0 || d2[s] < d2[b]) {
                b = s;
            }
        }
        return (a >= 0 ? 1u << a : 0u) | (b >= 0 ? 1u << b : 0u);
    }

    // A slot's "distant" state with hysteresis (level 2): distant beyond halfIn, near again inside halfOut.
    static inline bool distant(bool was, float d2, const LevelParams& p) {
        if (p.halfIn <= 0.0f) return false;
        return was ? d2 > p.halfOut * p.halfOut : d2 > p.halfIn * p.halfIn;
    }
}  // namespace MpQuality
