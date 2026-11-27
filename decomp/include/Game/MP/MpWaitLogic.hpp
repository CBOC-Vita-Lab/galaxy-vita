// Galaxy 3.0 multiplayer: the "Waiting for <name>..." banner (uxwait). Pure logic, no game types: tests/mp/wait_test.cpp includes this file.
//
// A player held behind a closed screen by another console (after a Power Star the guest waits for the host's next move) used to see no
// words. The tracker below decides when a small, non-blocking banner explains it: the wait must have lasted kShowAfterMs (a wait
// that ends sooner is never announced: no flicker), it goes the moment the wait ends, and the name follows the host when it changes
// (a host migration while waiting). Times are milliseconds from any monotonic clock.
#ifndef MP_WAIT_LOGIC_HPP
#define MP_WAIT_LOGIC_HPP

#include <cstdint>

namespace MpWaitLogic {
    constexpr int64_t kShowAfterMs = 2000;   // "more than about 2 seconds"

    struct Out {
        bool show;     // the banner is up this frame
        int slot;      // whose name it carries (valid when show)
    };

    // A bubbled player waits for a teammate whose galaxy is still loading: the first such slot (lowest number), -1 when nobody loads.
    // loading[s]: slot s is present, not this console, has no fresh pose of this scene yet.
    inline int firstLoadingSlot(const bool* loading, int count) {
        for (int s = 0; s < count; s++)
            if (loading[s]) return s;
        return -1;
    }

    // The bubbled player's banner: only when the group is really waiting on the loader, i.e. every other player in this scene is caught too
    // (inSceneFree[s]: slot s is in this scene and not in a bubble), and only inside the 40 s window the restart gate also waits
    // (loading[s]: present, no fresh pose of this scene, and still within that window).
    inline int bubbleWaitSlot(const bool* loading, const bool* inSceneFree, int count) {
        for (int s = 0; s < count; s++)
            if (inSceneFree[s]) return -1;
        return firstLoadingSlot(loading, count);
    }

    struct Tracker {
        bool waiting = false;
        int64_t sinceMs = 0;
        int slot = -1;

        void reset() { waiting = false; sinceMs = 0; slot = -1; }

        // Call once per frame. waiting: this console is held by `onSlot`'s console right now. onSlot < 0 (nobody known to wait for) never shows.
        Out step(bool isWaiting, int onSlot, int64_t nowMs) {
            if (!isWaiting || onSlot < 0) { reset(); return {false, -1}; }
            if (!waiting) { waiting = true; sinceMs = nowMs; }
            slot = onSlot;
            const bool show = nowMs - sinceMs >= kShowAfterMs;
            return {show, show ? slot : -1};
        }
    };
}

#endif
