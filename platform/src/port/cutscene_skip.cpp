// Holding A skips cutscenes.
//
// The game reports each update which kind of cutscene runs (port_skip_context,
// from GameScene::update).  Holding A for skip_hold seconds (petari_vr.ini,
// 1 s by default, 0 turns skipping off) fills a ring the VR layer draws, then
// skips:
// - movies, the galaxy intro camera and the fly-in end through the game's own
//   skip paths, which take the request with port_skip_take;
// - in-engine demos have no general way to end early (they run their side
//   effects part by part), so the game logic runs fast-forward, unrendered and
//   silent, until the demo is over; so do dialogues outside cutscenes (most
//   people's talk, which is no demo: holding A there only finished the
//   sentence on screen, the next one wanting a new press).  A is pressed for
//   it in a held, held,
//   released cycle so the demo's text boxes advance: prompts that wait for a
//   press take the first update of each cycle, and cutscene dialogue
//   (TalkStateEvent) turns a page only while A is held on from the update
//   before, which a press every other update never gives.  A yes/no choice
//   stops it (port_skip_interrupt): the answer is the player's.
// A hold that began before the cutscene does not count.  After a skip the
// game reads A as released until the player lets go, so the held A neither
// starts a new skip nor reaches the game as a fresh press (a jump); but a
// cutscene or dialogue that follows within a second while A is still held
// (the next part of the same scene) is skipped on at once.
#include <atomic>

#include "port/heap_routing.h"
#include "port/mp_bridge.h"  // MP3: skip vote in a multiplayer session
#include "port/mp_gate.h"    // MP3 lodgate: SkipHoldWaits
#include "port/port.h"
#include "port/xskip.h"  // (xskip27)
#include "port/lockstep.h"  // (fmt32)

// MP3: (decomp/src/Game/MP/MpFlow.cpp, game thread) kind > 0: the local hold is complete, vote and return whether everyone agreed;
// kind < 0: only ask whether the group decided to skip the cutscene -kind. Weak reference: absent in programs without the game.
namespace MpFlow {
bool skipVote(int contextKind) __attribute__((weak));
}
static bool mpSkipVote(int kind) { return MpFlow::skipVote == nullptr || MpFlow::skipVote(kind); }
static bool mpSkipQuery(int kind) { return MpFlow::skipVote != nullptr && MpFlow::skipVote(-kind); }

// (defined by the Vita build's renderer host: free memory and the renderer's own holdings; absent elsewhere)
extern "C" void port_vgx_vita_log_memory(const char* when) __attribute__((weak));
extern "C" void port_vgx_census_log(const char* when) __attribute__((weak));  // (the renderer's, on every build that has one)

static void logRendererMemory(const char* when) {
    if (port_vgx_vita_log_memory) port_vgx_vita_log_memory(when);
    else if (port_vgx_census_log) port_vgx_census_log(when);
}

namespace {

// Game thread: the hold and the fast-forward.
float sHoldSeconds = 1.0f;
int sKind = PORT_SKIP_NONE;
int64_t sHoldStart = 0;
bool sNeedRelease = false;
int sPending = PORT_SKIP_NONE;
int64_t sPendingAt = 0;
int64_t sBlockedAt = 0;  // last port_skip_interrupt (a choice on screen)
unsigned sForwardUpdates = 0;
unsigned sContextCalls = 0;       // (xskip27) port_skip_context calls = game turns
unsigned sTakeAtCall = 0;         // (xskip27) the call count when port_skip_take last handed a skip over
bool sTookOnce = false;
unsigned sInjectPhase = 0;  // A held on phases 0 and 1, released on 2
int64_t sForwardStart = 0;
int64_t sForwardEndAt = 0;  // when a fast-forward last ended with its cutscene
// The longest a fast-forward runs: a demo that does not end by then (one
// waiting for something the fast-forward cannot give it) plays on normally.
const unsigned kMaxForwardUpdates = 60 * 180;

// Shared with the input and render threads.
std::atomic<bool> sForward{false};
std::atomic<bool> sInjectA{false};
std::atomic<bool> sMaskA{false};       // A reads as released until let go
std::atomic<float> sProgress{0.0f};
std::atomic<int64_t> sContextAt{0};    // last port_skip_context call
std::atomic<bool> sMoviePlaying{false};
std::atomic<unsigned> sSkips{0};       // skips triggered so far

const char* kindName(int kind) {
    switch (kind) {
    case PORT_SKIP_MOVIE:
        return "movie";
    case PORT_SKIP_OPENING:
        return "intro camera";
    case PORT_SKIP_STARTER:
        return "fly-in";
    case PORT_SKIP_DEMO:
        return "cutscene";
    case PORT_SKIP_TALK:
        return "dialogue";
    default:
        return "?";
    }
}

// The game stopped reporting (another scene): nothing to skip.
bool contextStale(int64_t now) { return now - sContextAt.load() > 200000000; }

void startForward(int64_t now) {
    sForwardUpdates = 0;
    sForwardStart = now;
    sInjectPhase = 2;  // released first: the next update presses A anew
    sInjectA.store(false);
    sForward.store(true);  // before sSkips: see port_skip_indicator
    port_xskip_accept(PORT_XSKIP_FORWARD);  // (xskip27: the marker; a fast-forward is the generic path)
    port_host_heap_log("at the start of a fast-forward");
    logRendererMemory("at the start of a fast-forward");
}

void endForward(const char* why) {
    port_log("skip: fast-forward ended after %u updates in %.1f s (%s)", sForwardUpdates,
             (port_host_time_ns() - sForwardStart) / 1e9, why);
    port_host_heap_log("at the end of a fast-forward");
    logRendererMemory("at the end of a fast-forward");
    sForward.store(false);
    sInjectA.store(false);
    sProgress.store(0.0f);
    sHoldStart = 0;
    sNeedRelease = true;
}

}  // namespace

extern "C" {

void port_skip_set_hold_seconds(float seconds) { sHoldSeconds = seconds; }

int port_skip_movie_playing(void) { return sMoviePlaying.load(std::memory_order_relaxed) ? 1 : 0; }

void port_skip_context(int kind) {
    int64_t now = port_host_time_ns();
    ++sContextCalls;
    bool held = port_input_real_a() != 0;
    bool resumed = contextStale(now);  // first update of a scene, or after a stall
    sContextAt.store(now);
    sMoviePlaying.store(kind == PORT_SKIP_MOVIE, std::memory_order_relaxed);

    // (merge 3.0 on c181) c181's fmt32 switched the fast-forward off in a lockstep session here (one that ran ended, none started, no hold-skip for the two fast-forward kinds).
    // The 3.0 line decided the opposite (Fable's checks83 review, 14 Nov: the fast-forward draws nothing and runs the same turns packed tighter, like catch-up turns; gated, an accepted
    // session skip becomes a black wait at normal speed; the vote below owns whether a skip happens, and a skip held up by a reconnect waits in SkipHoldWaits). The 3.0 behaviour is
    // kept; single player is unchanged (lockstep::On() was false there).
    if (sForward.load()) {
        if (resumed) {
            endForward("the scene changed");
        } else if (kind != PORT_SKIP_DEMO && kind != PORT_SKIP_TALK) {
            endForward("the cutscene is over");
            sForwardEndAt = now;
        } else if (++sForwardUpdates > kMaxForwardUpdates) {
            endForward("gave up");
        } else {
            sInjectPhase = (sInjectPhase + 1) % 3;
            sInjectA.store(sInjectPhase != 2);
            return;
        }
    }

    // The next part of the scene just skipped, A still held: on it goes.
    // Held means never let go since the skip (A is still masked): a fresh
    // press is the player's own, such as the one confirming a galaxy in the
    // dome right after its lecture was skipped, and must not skip what it
    // starts.
    // MP3 lodgate: continuing a skip the session voted (active) or one in a dialogue is fine; held but not active (a reconnect) it must not start a local skip, it waits like the full hold below.
    const bool sessionHeld = port_net_session_lockstep() != 0;
    const bool sessionActive = mp_active();
    if (!sForward.load() && (kind == PORT_SKIP_DEMO || kind == PORT_SKIP_TALK) && held && sMaskA.load() && sForwardEndAt &&
        now - sForwardEndAt < 1000000000 && !port::mpgate::SkipHoldWaits(sessionHeld, sessionActive, kind == PORT_SKIP_TALK)) {
        port_log("skip: %s following, still skipping", kindName(kind));
        sKind = kind;
        sForwardEndAt = 0;
        sMaskA.store(true);
        startForward(now);
        sSkips.fetch_add(1);
        return;
    }
    if (kind != sKind) {
        if (kind != PORT_SKIP_NONE) {
            port_log("skip: %s playing", kindName(kind));
        }
        sKind = kind;
        sHoldStart = 0;
        sNeedRelease = held;  // a hold from before the cutscene does not count
    }
    if (sPending != PORT_SKIP_NONE && now - sPendingAt > 1000000000) {
        sPending = PORT_SKIP_NONE;  // the game never took it (not skippable just then)
    }
    if (!held) {
        sNeedRelease = false;
    }
    bool blocked = now - sBlockedAt < 100000000;
    // MP3: in a session the others' votes can decide the skip without a local hold (dialogue stays local)
    bool groupSkip = false;
    if (sessionActive && kind != PORT_SKIP_NONE && kind != PORT_SKIP_TALK && !blocked && mpSkipQuery(kind)) {
        groupSkip = true;
        port_log("skip: %s (players voted)", kindName(kind));
    }
    if (!groupSkip && (kind == PORT_SKIP_NONE || sHoldSeconds <= 0.0f || !held || sNeedRelease || blocked)) {
        sHoldStart = 0;
        sProgress.store(0.0f);
        return;
    }
    if (!groupSkip) {
        if (!sHoldStart) {
            sHoldStart = now;
        }
        float p = (float)((now - sHoldStart) / (sHoldSeconds * 1e9));
        sProgress.store(p < 1.0f ? p : 1.0f);
        if (p < 1.0f) {
            return;
        }
        // MP3: a full hold is this player's vote; the skip waits for the others (the ring stays full)
        if (port::mpgate::SkipHoldWaits(sessionHeld, sessionActive, kind == PORT_SKIP_TALK)) {
            return;   // MP3 lodgate: a reconnect (held, not active): the ring stays full, no vote; the hold skips again once the session is back, or as in single player if it gave up
        }
        if (sessionActive && !mpSkipVote(kind)) {
            return;
        }
        port_log("skip: %s", kindName(kind));
    }
    sHoldStart = 0;
    sNeedRelease = true;
    sProgress.store(0.0f);
    sMaskA.store(true);
    if (kind == PORT_SKIP_DEMO || kind == PORT_SKIP_TALK) {
        startForward(now);
    } else {
        sPending = kind;
        sPendingAt = now;
    }
    sSkips.fetch_add(1);
}

// What the game last reported (PORT_SKIP_NONE when it has stopped reporting): the input layer asks whether a dialogue or a
// cutscene is on screen.
int port_skip_context_kind(void) { return contextStale(port_host_time_ns()) ? PORT_SKIP_NONE : sKind; }

int port_skip_take(int kind) {
    if (sPending != kind) {
        return 0;
    }
    sPending = PORT_SKIP_NONE;
    sTakeAtCall = sContextCalls;  // (xskip27)
    sTookOnce = true;
    return 1;
}

// (xskip27) game turns since a held-A skip was handed over (-1: never): the intro camera's / starter's accept tells a hold of A from a tap
int port_skip_last_take_age_turns(void) { return sTookOnce ? static_cast<int>(sContextCalls - sTakeAtCall) : -1; }

int port_skip_interrupt(void) {
    sBlockedAt = port_host_time_ns();
    if (!sForward.load()) {
        return 0;
    }
    endForward("a choice for the player");
    return 1;
}

int port_skip_fast_forwarding(void) { return sForward.load() && !contextStale(port_host_time_ns()); }

void port_skip_scene_change(void) {
    if (sForward.load()) {
        endForward("a scene change begins");
    }
}

// Input thread: A as the game should read it (-1: as pressed).
int port_skip_inject_a(void) {
    if (port_skip_fast_forwarding()) {
        return sInjectA.load() ? 1 : 0;
    }
    if (sMaskA.load()) {
        if (port_input_real_a()) {
            return 0;
        }
        sMaskA.store(false);
    }
    return -1;
}

// Render thread: the indicator.  *progress: how far the hold has come (0 =
// hidden), *forwarding: a fast-forward runs, *skips: skips triggered so far
// (a change: one just happened).
void port_skip_indicator(float* progress, int* forwarding, unsigned* skips) {
    // sSkips first: a new count guarantees a demo's sForward is seen with it.
    *skips = sSkips.load();
    bool stale = contextStale(port_host_time_ns());
    *progress = stale ? 0.0f : sProgress.load();
    *forwarding = !stale && sForward.load();
}

}  // extern "C"
