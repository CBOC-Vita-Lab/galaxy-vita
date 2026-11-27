// Galaxy 3.0 multiplayer: when a games / friends list that comes from the ONLINE service may ask for itself again (uxwait).
// Pure logic, no game types: tests/mp/list_refresh_test.cpp includes this file.
//
// The service's quota is tight (about 60 requests per player-hour on average, server/README.md "Quota defence"), so a list that
// refreshes by itself follows these rules, and only while its screen is up:
//  - one request every kIntervalMs (30 s) at most (friends' online status changes slowly); the first one a full interval after the screen's own request;
//  - an error doubles the wait (60, 120, 240 s: kMaxWaitMs) and after kMaxFails (4) errors in a row it stops by itself;
//  - it stops after kIdleStopMs (3 minutes) without a button press and after kVisitCapRequests (20) requests of its own in one visit;
//    a press wakes the idle stop; a manual refresh starts it again (and counts from zero);
//  - a manual refresh needs kManualGapMs since the last one (else the caller only plays a sound), and no more than kHourCap requests (self-made
//    and manual together, and the screen's own request on entering) in a rolling hour, whatever happens in between: leaving and entering
//    the screen never clears that history. The "hour" is counted in frames since multiplayer was first opened this boot (the
//    menu system runs every frame from then on, also while playing), so it follows wall time closely; it starts over only with the process;
//    above the ceiling the caller also skips the screen's own request on entering and shows the rows it already has;
//  - never while a window or the keyboard is up (the caller passes `blocked`), never with a request already running.
// The local lists (consoles on the same Wi-Fi / nearby) cost the service nothing and are not paced here.
#ifndef MP_LIST_REFRESH_LOGIC_HPP
#define MP_LIST_REFRESH_LOGIC_HPP

#include <cstdint>

namespace MpListRefresh {
    constexpr int64_t kIntervalMs = 30000;
    constexpr int64_t kMaxWaitMs = 240000;
    constexpr int kMaxFails = 4;
    constexpr int64_t kIdleStopMs = 3 * 60 * 1000;
    constexpr int kVisitCapRequests = 20;   // self-made requests per visit
    constexpr int64_t kManualGapMs = 10000;   // a manual refresh at most this often (a held or mashed button is not a flood)
    constexpr int kHourCap = 30;              // requests it makes (self-made and manual) in any rolling hour: survives leaving and entering the screen
    constexpr int64_t kHourMs = 60 * 60 * 1000;

    struct Pacer {
        bool on = false;           // the screen is up
        int64_t enteredMs = 0;
        int64_t lastPressMs = 0;
        int64_t nextAtMs = 0;
        int fails = 0;
        int requests = 0;          // self-made requests this visit
        bool parked = false;       // stopped by itself (errors / idle / visit cap): only a manual refresh or a press wakes it
        int64_t lastManualMs = -kHourMs;       // (survives leave / enter)
        int64_t stamps[kHourCap] = {};         // when the last requests went out (a ring)
        int nStamps = 0, stampAt = 0;
        bool underCeiling(int64_t nowMs) const {
            int n = 0;
            for (int i = 0; i < nStamps; i++)
                if (nowMs - stamps[i] < kHourMs) ++n;
            return n < kHourCap;
        }
        void note(int64_t nowMs) { stamps[stampAt] = nowMs; stampAt = (stampAt + 1) % kHourCap; if (nStamps < kHourCap) ++nStamps; }

        void leave() { on = false; parked = false; fails = 0; }
        // The screen came up and made its own request now (it counts in the hourly ring: Friends > Blocked > back costs one each time).
        void enter(int64_t nowMs) { if (underCeiling(nowMs)) note(nowMs); on = true; enteredMs = lastPressMs = nowMs; nextAtMs = nowMs + kIntervalMs; fails = 0; requests = 0; parked = false; }
        // A button press on the screen (any): ends an idle stop; the list is then asked again when it is due.
        void press(int64_t nowMs) {
            lastPressMs = nowMs;
            if (parked && fails < kMaxFails && requests < kVisitCapRequests) { parked = false; if (nextAtMs > nowMs) nextAtMs = nowMs; }
        }
        // The player asked for a refresh. False = not now (10 s since the last one have not passed, or the hourly ceiling is reached): the caller
        // makes no request. True: the caller makes it; the timing starts over, also after a stop.
        bool manual(int64_t nowMs) {
            if (nowMs - lastManualMs < kManualGapMs || !underCeiling(nowMs)) return false;
            lastManualMs = nowMs;
            note(nowMs);
            enteredMs = lastPressMs = nowMs; nextAtMs = nowMs + kIntervalMs; fails = 0; requests = 0; parked = false;
            return true;
        }
        // The answer of a request this pacer started or saw.
        void answered(int64_t nowMs, bool ok) {
            if (ok) { fails = 0; nextAtMs = nowMs + kIntervalMs; return; }
            ++fails;
            int64_t w = kIntervalMs;
            for (int i = 0; i < fails && w < kMaxWaitMs; i++) w *= 2;
            nextAtMs = nowMs + (w > kMaxWaitMs ? kMaxWaitMs : w);
        }
        // True when the caller should start a request now. Call once per frame; it makes the stop decisions too.
        bool due(int64_t nowMs, bool requestRunning, bool blocked) {
            if (!on || parked) return false;
            if (fails >= kMaxFails || nowMs - lastPressMs >= kIdleStopMs || requests >= kVisitCapRequests) { parked = true; return false; }
            if (requestRunning || blocked) return false;
            if (nowMs < nextAtMs) return false;
            if (!underCeiling(nowMs)) { nextAtMs = nowMs + kIntervalMs; return false; }
            note(nowMs);
            ++requests;
            nextAtMs = nowMs + kIntervalMs;   // (answered() sets the real next time)
            return true;
        }
    };
}

#endif
