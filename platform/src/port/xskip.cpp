// xskip27 (13 Nov): the accepted skip of a galaxy's opening; see platform/include/port/xskip.h for the lines it writes and the switches.
//
// Owner (c86, console): "when clicking through the white star loading screen flash it caused the game to stutter". Nobody had a marked trace of an accepted skip, so this file
// is first the markers (one story per stage entry, every line with the renderer's queue depths), then three small mechanisms that act only while a skip is open:
//   - the cover hold: the opening's black frame (CinemaFrame, fully black after the skip) is kept closed up to GXV_XSKIP_HOLD_MS while the stage's own programs / the texture and target
//     plan / the warm scope still have work left, and the renderer is told to drain with the closed-screen budget (vgx_xskip_drain) instead of the 2 ms of play;
//   - the pacer's calm window: the frame-skip rule treats the turns from the accept to GXV_XSKIP_CALM_TURNS after the cut like a scene change (drawn at the normal pace, no debt),
//     so a picture that took 100-250 ms is not paid back with turns that draw nothing (the "catch-up burst");
//   - the test injector: PETARI_XSKIP_TEST="phase+turns:mode,..." presses A for the emulator harness (phase scene | opening | starter, mode tap | hold).
// Game thread unless a function says otherwise; the renderer reads port_xskip_active (an atomic).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/port.h"
#include "port/diagnostics.h"
#include "port/xskip.h"
#include "port/lockstep.h"  // (fmt32)
#include "port/mp_gate.h"    // MP3 lodgate: no cover hold in a session
#include "port/mp_bridge.h"  // MP3: port_net_session_lockstep()

extern "C" unsigned port_vgx_backend_pending(void) __attribute__((weak));  // (vgx_galaxy.cpp)

namespace {

bool EnvOn(const char* name, bool fallback) {
    const char* e = getenv(name);
    return e && *e ? atoi(e) != 0 : fallback;
}
long EnvL(const char* name, long fallback) {
    const char* e = getenv(name);
    return e && *e ? atol(e) : fallback;
}
bool LogOn() { static const bool v = EnvOn("GXV_XSKIP_LOG", true); return v; }
// (fmt32: the three mechanisms stand off in a lockstep session: the cover hold delays the game's own cut by this console's clock and renderer work, so two consoles would cut on different turns;
// the markers and the "xskip:" lines stay)
bool ActOn() { static const bool v = EnvOn("GXV_XSKIP", true); return v && !port::lockstep::On(); }
double HoldCapMs() { static const double v = static_cast<double>(EnvL("GXV_XSKIP_HOLD_MS", 400)); return v; }
double StallMs() { static const double v = static_cast<double>(EnvL("GXV_XSKIP_STALL_MS", 150)); return v; }
unsigned CalmTurns() { static const unsigned v = static_cast<unsigned>(EnvL("GXV_XSKIP_CALM_TURNS", 24)); return v; }

const char* PathName(int p) {
    switch (p) {
    case PORT_XSKIP_OPENING: return "opening-camera";
    case PORT_XSKIP_STARTER: return "starter-rail";
    case PORT_XSKIP_FORWARD: return "fast-forward";
    default: return "none";
    }
}

struct Snap {
    PortXskipDepths d;
    unsigned long long reads = 0, syncN = 0;
    double syncMs = 0;
    int64_t ns = 0;
    unsigned turn = 0;
};

void Take(Snap* s, int64_t now, unsigned turn) {
    memset(&s->d, 0, sizeof s->d);
    if (vgx_xskip_depths) vgx_xskip_depths(&s->d);
    if (port_vgx_backend_pending) s->d.backendPending = port_vgx_backend_pending();
    s->reads = s->syncN = 0;
    s->syncMs = 0;
    if (port_dvd_xskip_counters) port_dvd_xskip_counters(&s->reads, &s->syncN, &s->syncMs);
    s->ns = now;
    s->turn = turn;
}

unsigned Work(const PortXskipDepths& d) { return d.ownLeft + d.planLeft + d.prewarmLeft + d.layoutLeft; }

// One stage entry's story (game thread).
struct Story {
    unsigned gen = 0;
    bool seen = false;
    unsigned turn = 0, turnsOpening = 0, turnsStarter = 0;  // game turns since the scene's first update, and in each nerve
    int lastNerve = -1;
    char stage[40] = {};
    int scenario = 0;
    // the skip
    unsigned accepts = 0;
    char paths[48] = {};
    char how[16] = {};
    Snap atAccept, atCut, atControl, atEnd;
    int64_t acceptNs = 0, coverNs = 0, cutNs = 0, picNs = 0, controlNs = 0;
    unsigned acceptTurn = 0, coverTurn = 0, cutTurn = 0, picTurn = 0, controlTurn = 0;
    bool coverSeen = false, cutSeen = false, picSeen = false, controlSeen = false, summaryDone = false;
    int cutPath = 0;
    // the hold
    bool holdOpen = false, holdDone = false;
    int64_t holdStartNs = 0, holdMoveNs = 0;
    double holdMs = 0;
    unsigned holdLeftAtStart = 0, holdLeftAtEnd = 0, holdLast = 0;
    const char* holdEnd = "none";
    // the pictures after control
    int64_t lastDrawnNs = 0;
    double longestMs = 0;
    unsigned drawnAfter = 0, undrawnAfter = 0;
    double bgmMs = 0;
} S;

std::atomic<unsigned> sSceneGen{0};
std::atomic<bool> sActive{false};
std::atomic<bool> sTestA{false};
unsigned sCalmLeft = 0;      // turns of the calm window still to run after the cut
bool sCalmOpen = false;      // accept seen, window not yet over
unsigned sTurnNo = 0;        // game turns since the start (monotonic)

void Depths(char* out, size_t cap, const PortXskipDepths& d) {
    snprintf(out, cap, "queue(own %u, plan %u, warm %u, layouts %u, stage %u, backend pictures %u)", d.ownLeft, d.planLeft, d.prewarmLeft, d.layoutLeft, d.stageLeft,
             d.backendPending);
}
void Line(const char* what, const Snap& s, const char* extra) {
    if (!LogOn()) return;
    char q[160];
    Depths(q, sizeof q, s.d);
    port_log_summary("xskip: %s stage=%s scene-turn=%u%s %s", what, S.stage, S.turn, extra ? extra : "", q);
}

struct TestItem {
    int phase;  // 0 scene, 1 opening, 2 starter
    unsigned at;
    bool hold;
    bool done;
};
TestItem sTest[8];
int sTestN = -1;
unsigned sTestUntil = 0;  // the turn (scene turn counter of its phase) the press ends at
void TestParse() {
    sTestN = 0;
    const char* e = getenv("PETARI_XSKIP_TEST");
    if (!e || !*e) return;
    for (const char* p = e; *p && sTestN < 8;) {
        const char* end = strchr(p, ',');
        const size_t n = end ? static_cast<size_t>(end - p) : strlen(p);
        char item[48];
        snprintf(item, sizeof item, "%.*s", static_cast<int>(n < sizeof item - 1 ? n : sizeof item - 1), p);
        TestItem t{0, 0, false, false};
        char* plus = strchr(item, '+');
        char* colon = strchr(item, ':');
        if (plus) {
            *plus = 0;
            t.phase = strcmp(item, "opening") == 0 ? 1 : strcmp(item, "starter") == 0 ? 2 : 0;
            t.at = static_cast<unsigned>(atoi(plus + 1));
            t.hold = colon && strcmp(colon + 1, "hold") == 0;
            sTestN++;
            sTest[sTestN - 1] = t;
        }
        p = end ? end + 1 : p + n;
    }
    if (sTestN) port_log_summary("xskip: PETARI_XSKIP_TEST armed: %d presses of A per stage entry (%s)", sTestN, e);
}

void Summary(const char* why, int64_t now) {
    if (S.summaryDone || !S.seen) return;
    S.summaryDone = true;
    if (!LogOn()) return;
    const bool skipped = S.accepts > 0;
    char work[200];
    snprintf(work, sizeof work, "own %u, plan %u, warm %u, layouts %u, stage queue %u", S.atAccept.d.ownLeft, S.atAccept.d.planLeft, S.atAccept.d.prewarmLeft,
             S.atAccept.d.layoutLeft, S.atAccept.d.stageLeft);
    const Snap& c0 = S.controlSeen ? S.atControl : S.atEnd;
    const Snap& e = S.atEnd;
    port_log_summary(
        "xskip: summary stage=%s scenario=%d skipped=%s path=%s how=%s accepts=%u work_at_skip=(%s) cover_hold_ms=%.0f hold_end=%s left_at_release=%u "
        "accept_to_control_turns=%d accept_to_control_ms=%.0f scene_to_control_turns=%d longest_picture_3s_ms=%.0f drawn_after=%u undrawn_after=%u bgm_start_ms=%.1f | "
        "ledger accept->control: links+%llu compiles+%llu uploads+%llu targets+%llu reads+%llu syncreads+%llu(%.0f ms) | ledger control->+%s: links+%llu compiles+%llu "
        "uploads+%llu(%.0f ms) targets+%llu reads+%llu syncreads+%llu(%.0f ms) back-end pictures+%llu exec %.0f ms (%s)",
        S.stage, S.scenario, skipped ? "yes" : "no", skipped ? S.paths : "none", skipped ? S.how : "-", S.accepts, skipped ? work : "-", S.holdMs, S.holdEnd, S.holdLeftAtEnd,
        skipped && S.controlSeen ? static_cast<int>(S.controlTurn - S.acceptTurn) : -1,
        skipped && S.controlSeen ? (S.controlNs - S.acceptNs) / 1e6 : 0.0, S.controlSeen ? static_cast<int>(S.controlTurn) : -1, S.longestMs, S.drawnAfter, S.undrawnAfter,
        S.bgmMs,
        skipped && S.controlSeen ? S.atControl.d.links - S.atAccept.d.links : 0ull, skipped && S.controlSeen ? S.atControl.d.compiles - S.atAccept.d.compiles : 0ull,
        skipped && S.controlSeen ? S.atControl.d.uploads - S.atAccept.d.uploads : 0ull, skipped && S.controlSeen ? S.atControl.d.targets - S.atAccept.d.targets : 0ull,
        skipped && S.controlSeen ? S.atControl.reads - S.atAccept.reads : 0ull, skipped && S.controlSeen ? S.atControl.syncN - S.atAccept.syncN : 0ull,
        skipped && S.controlSeen ? S.atControl.syncMs - S.atAccept.syncMs : 0.0, S.controlSeen ? "5s" : "end", e.d.links - c0.d.links, e.d.compiles - c0.d.compiles,
        e.d.uploads - c0.d.uploads, e.d.texMs - c0.d.texMs, e.d.targets - c0.d.targets, e.reads - c0.reads, e.syncN - c0.syncN, e.syncMs - c0.syncMs, e.d.frames - c0.d.frames,
        e.d.execMs - c0.d.execMs, why);
    (void)now;
}

void NewScene(unsigned gen, int64_t now) {
    if (S.seen && !S.summaryDone) {
        Take(&S.atEnd, now, S.turn);
        Summary("scene left", now);
    }
    S = Story();
    S.gen = gen;
    S.seen = true;
    sCalmOpen = false;
    sCalmLeft = 0;
    sActive.store(false, std::memory_order_relaxed);
    sTestUntil = 0;
    for (int i = 0; i < sTestN; ++i) sTest[i].done = false;
    sTestA.store(false, std::memory_order_relaxed);
}

}  // namespace

extern "C" {

void port_xskip_scene_begin(void) { sSceneGen.fetch_add(1, std::memory_order_relaxed); }

unsigned port_xskip_turn_no(void) { return sTurnNo; }

int port_xskip_active(void) { return sActive.load(std::memory_order_relaxed) ? 1 : 0; }

int port_xskip_test_a(void) { return sTestA.load(std::memory_order_relaxed) ? 1 : 0; }

void port_xskip_scene_turn(int nerve, int playing, int blank, const char* stage, int scenario) {
    ++sTurnNo;
    if (sTestN < 0) TestParse();
    const int64_t now = port_host_time_ns();
    const unsigned gen = sSceneGen.load(std::memory_order_relaxed);
    if (!S.seen || S.gen != gen) {
        NewScene(gen, now);
        snprintf(S.stage, sizeof S.stage, "%s", stage ? stage : "?");
        S.scenario = scenario;
    }
    ++S.turn;
    if (nerve == 1) ++S.turnsOpening;
    if (nerve == 2) ++S.turnsStarter;
    S.lastNerve = nerve;
    // the test injector (the harness presses A the way a player would; one press per item and stage entry)
    if (sTestN > 0) {
        bool down = false;
        for (int i = 0; i < sTestN; ++i) {
            TestItem& t = sTest[i];
            const unsigned k = t.phase == 1 ? S.turnsOpening : t.phase == 2 ? S.turnsStarter : S.turn;
            const bool inPhase = t.phase == 0 || (t.phase == 1 && nerve == 1) || (t.phase == 2 && nerve == 2);
            if (!t.done && inPhase && k >= t.at) {
                if (k == t.at && !sTestUntil) {
                    sTestUntil = k + (t.hold ? 120u : 5u);
                    if (LogOn()) port_log_summary("xskip: test: A pressed (%s) at %s+%u, scene turn %u", t.hold ? "hold" : "tap", t.phase == 1 ? "opening" : t.phase == 2 ? "starter" : "scene", t.at, S.turn);
                }
                if (k < sTestUntil) {
                    down = true;
                } else if (sTestUntil) {
                    t.done = true;
                    sTestUntil = 0;
                }
            }
        }
        sTestA.store(down, std::memory_order_relaxed);
    }
    if (S.accepts && !S.coverSeen && blank) {
        S.coverSeen = true;
        S.coverNs = now;
        S.coverTurn = S.turn;
        Snap s;
        Take(&s, now, S.turn);
        char x[96];
        snprintf(x, sizeof x, " %.0f ms after the accept", (now - S.acceptNs) / 1e6);
        Line("cover (cinema frame fully black)", s, x);
    }
    if (!S.controlSeen && playing) {
        S.controlSeen = true;
        S.controlNs = now;
        S.controlTurn = S.turn;
        Take(&S.atControl, now, S.turn);
        char x[128];
        if (S.accepts)
            snprintf(x, sizeof x, " %u turns, %.0f ms after the accept", S.turn - S.acceptTurn, (now - S.acceptNs) / 1e6);
        else
            snprintf(x, sizeof x, " no skip: %u turns after the scene started", S.turn);
        Line("control (first controllable turn)", S.atControl, x);
        S.lastDrawnNs = 0;
    }
    if (S.controlSeen && !S.summaryDone && now - S.controlNs >= 5000000000ll) {
        Take(&S.atEnd, now, S.turn);
        Summary("5 s after control", now);
    }
    if (sCalmOpen && S.cutSeen) {
        if (sCalmLeft > 0) --sCalmLeft;
        if (sCalmLeft == 0) {
            sCalmOpen = false;
            sActive.store(false, std::memory_order_relaxed);
        }
    }
}

void port_xskip_accept(int path) {
    const int64_t now = port_host_time_ns();
    ++S.accepts;
    const size_t n = strlen(S.paths);
    snprintf(S.paths + n, sizeof S.paths - n, "%s%s", n ? "+" : "", PathName(path));
    const int age = port_skip_last_take_age_turns();
    const char* how = path == PORT_XSKIP_FORWARD ? "hold" : (age >= 0 && age <= 2) ? "hold" : "tap";
    snprintf(S.how, sizeof S.how, "%s", how);
    if (S.accepts == 1) {
        S.acceptNs = now;
        S.acceptTurn = S.turn;
        Take(&S.atAccept, now, S.turn);
    }
    char x[96];
    snprintf(x, sizeof x, " path=%s how=%s", PathName(path), how);
    Snap s;
    Take(&s, now, S.turn);
    Line("accepted", s, x);
    S.holdDone = false;
    S.holdOpen = false;
    S.coverSeen = false;
    S.cutSeen = false;
    S.picSeen = false;
    sCalmOpen = ActOn();
    sCalmLeft = CalmTurns();
    sActive.store(ActOn(), std::memory_order_relaxed);
}

void port_xskip_cut(int path) {
    if (!S.accepts) return;
    const int64_t now = port_host_time_ns();
    S.cutSeen = true;
    S.cutNs = now;
    S.cutTurn = S.turn;
    S.cutPath = path;
    Take(&S.atCut, now, S.turn);
    char x[96];
    snprintf(x, sizeof x, " path=%s, %.0f ms after the accept", PathName(path), (now - S.acceptNs) / 1e6);
    Line("cut", S.atCut, x);
}

void port_xskip_frame(int drawn) {
    if (!S.seen) return;
    const int64_t now = port_host_time_ns();
    if (S.cutSeen && !S.picSeen && drawn) {
        S.picSeen = true;
        S.picNs = now;
        S.picTurn = S.turn;
        Snap s;
        Take(&s, now, S.turn);
        char x[96];
        snprintf(x, sizeof x, " %.0f ms after the cut", (now - S.cutNs) / 1e6);
        Line("picture (first drawn after the cut)", s, x);
    }
    if (S.controlSeen && now - S.controlNs <= 3000000000ll) {
        if (drawn) {
            if (S.lastDrawnNs) {
                const double ms = (now - S.lastDrawnNs) / 1e6;
                if (ms > S.longestMs) S.longestMs = ms;
            }
            S.lastDrawnNs = now;
            ++S.drawnAfter;
        } else {
            ++S.undrawnAfter;
        }
    }
}

void port_xskip_bgm(double ms) {
    if (S.accepts && !S.controlSeen) S.bgmMs += ms;
}

int port_xskip_calm_turn(void) { return sCalmOpen ? 1 : 0; }

int port_xskip_cover_hold(void) {
    // (c181's ActOn() test is below the session block on purpose: a hold that was open when a session began is closed by the block even where ActOn() says no, as it does when 3.0 stands alone)
    if (!S.accepts || S.holdDone) return 0;
    if (port_net_session_lockstep() && !port::mpgate::CoverHoldAllowed(true)) {   // MP3 lodgate: the cut after a skip is made at once in a session, not when THIS console's shader / texture work is done
        port_mpgate_note(port::mpgate::kXskip);                   // (the starter's flight, Mario's release and the BGM would start on a different turn on each console)
        if (S.holdOpen) {                                         // (a hold that was already open when the session began: closed, the renderer's drain ended)
            S.holdOpen = false;
            S.holdDone = true;
            if (vgx_xskip_drain) vgx_xskip_drain(0);
        }
        return 0;
    }
    if (!ActOn()) return 0;
    const int64_t now = port_host_time_ns();
    Snap s;
    Take(&s, now, S.turn);
    const unsigned left = Work(s.d);
    if (!S.holdOpen) {
        S.holdOpen = true;
        S.holdStartNs = S.holdMoveNs = now;
        S.holdLast = S.holdLeftAtStart = left;
        if (left == 0) {
            S.holdDone = true;
            S.holdOpen = false;
            S.holdEnd = "nothing left";
            S.holdLeftAtEnd = 0;
            Line("hold (cover not held)", s, " nothing left at the cover");
            return 0;
        }
        char x[128];
        snprintf(x, sizeof x, " the cover is held for %u items of work (cap %.0f ms)", left, HoldCapMs());
        Line("hold (cover held)", s, x);
    }
    if (left != S.holdLast) {
        S.holdLast = left;
        S.holdMoveNs = now;
    }
    const double heldMs = (now - S.holdStartNs) / 1e6;
    const char* end = nullptr;
    if (left == 0) end = "done";
    else if (heldMs >= HoldCapMs()) end = "cap";
    else if ((now - S.holdMoveNs) / 1e6 >= StallMs()) end = "stalled";
    if (end) {
        S.holdDone = true;
        S.holdOpen = false;
        S.holdMs += heldMs;
        S.holdEnd = end;
        S.holdLeftAtEnd = left;
        if (vgx_xskip_drain) vgx_xskip_drain(0);
        char x[128];
        snprintf(x, sizeof x, " released after %.0f ms (%s), %u of %u items left", heldMs, end, left, S.holdLeftAtStart);
        Line("hold (released)", s, x);
        return 0;
    }
    if (vgx_xskip_drain) vgx_xskip_drain(1);
    return 1;
}

}  // extern "C"
