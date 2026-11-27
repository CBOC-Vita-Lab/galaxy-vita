// bootscr26: host test of vgx_boot_screen.h, the state machine of the Galaxy boot screen and the port-picture guard. tools/test_boot_screen.sh
//
// The machine below is the host's (vgx_host_galaxy_vita.cpp, namespace splash) with the Vita calls taken out: the same Phase numbering, the same transitions, and the SAME
// decision functions (EndDue, PortPicture, LatchPlay) as the host and the back end call. What it does NOT model: the display, the thread's timing, vitaGL (see REPORT.txt).
// The paths of a game present are the host's: 0 the whole present, 1 the picture half (GXV_HUD_NATIVE), 2 the swap half, 3 direct display; the idle loop (Service from the
// render loop's wait) is a fifth way in.
#include "../platform/vgx/src/vgx_boot_screen.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace vgx::bootscreen;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

struct Machine {
    int phase = kOff;
    bool thread = false, threadMayDrawDisplay = false; // the ticker: writes the display only in kDirect
    bool titleSeen = false, faded = false, failed = false, playStarted = false, picsFreed = false;
    double now = 0.0, titleAt = 0.0, limitS = 5.0;
    int dark = 0;
    unsigned bootSwaps = 0, bootSwapsInPlay = 0, directWrites = 0, progressShown = 0, progressRefused = 0;
    std::string endedWhy;
    // transitions (host name in brackets)
    void Begin() { phase = kDirect; thread = true; threadMayDrawDisplay = true; }                 // port_splash_begin: kOff -> kDirect, thread starts
    void Ready() { if (phase == kDirect) { phase = kGl; threadMayDrawDisplay = false; } }         // port_vgx_vita_ready: kDirect -> kGl (render thread)
    void TitleSeen(double t) { if (phase != kOff && !titleSeen) { titleSeen = true; titleAt = t; } } // NoteTitleVisible (ticker or render thread)
    void ThreadTick() {                                                                         // TickerMain loop: draws into memory; writes the display only in kDirect
        if (phase == kDone) { thread = false; return; }
        if (titleSeen && now - titleAt >= kFadeEndS) faded = true;
        if (phase == kDirect && threadMayDrawDisplay && !playStarted) ++directWrites;
    }
    void Finish(const char* why) { if (phase == kDone) return; phase = kDone; thread = false; picsFreed = true; endedWhy = why; } // splash::Finish
    bool EndIfDue() {                                                                           // splash::EndIfDue
        EndInput in; in.phase = phase; in.failed = failed; in.titleSeen = titleSeen; in.faded = faded; in.darkSwaps = dark;
        in.sinceTitleS = titleSeen ? now - titleAt : 0.0; in.limitS = limitS; in.playStarted = playStarted;
        const EndWhy w = EndDue(in);
        if (w == EndWhy::None) return false;
        Finish(EndName(w));
        return true;
    }
    bool PresentBoot() {                                                                        // splash::PresentFrame
        if (picsFreed || !PortPicture(Pic::Boot, phase, playStarted, false, false).ok) return false;
        ++bootSwaps;
        if (playStarted) ++bootSwapsInPlay;
        return true;
    }
    void Service() {                                                                            // splash::Service (render thread)
        if (phase != kGl) return;
        if (EndIfDue()) return;
        const bool shown = PresentBoot();
        if (shown && !failed && faded) ++dark;
        EndIfDue();
    }
    // A game picture arrives by `path`: 0 whole, 1 picture half, 2 swap half, 3 direct display. Returns true when the GAME's picture reaches the display.
    bool GamePresent(int path) {                                                                // PresentDisplayCopy
        EndIfDue();                                                                             // NoteGamePresent: first thing, every path
        if (phase == kGl) {                                                                     // the boot screen is up: the game's picture stays out of sight
            if (path == 0 || path == 2) Service();                                           // (the whole present / swap half reach Service; the picture half returns early; direct display is only offered once the screen is not Active)
            return false;
        }
        return true;
    }
    void Play(bool live) { if (LatchPlay(playStarted, live)) {} }                               // back end: NotePlayStart at frame begin
    bool ProgressRequest(bool screenLive, bool pcsInPlay) {                                     // ShowProgress / playprogress::Show
        const bool ok = PortPicture(Pic::Progress, phase, playStarted, screenLive, pcsInPlay).ok;
        ++(ok ? progressShown : progressRefused);
        return ok;
    }
    void Advance(double dt) { now += dt; ThreadTick(); }
};

static Machine Boot() { Machine m; m.Begin(); m.Advance(0.1); m.Ready(); return m; }

int main() {
    // --- the pure functions
    Require(!PortPicture(Pic::Boot, kGl, true, false, false).ok && !PortPicture(Pic::Dialog, kYielded, true, true, false).ok, "boot and dialog: never after play started");
    Require(PortPicture(Pic::Boot, kGl, false, false, false).ok && PortPicture(Pic::Boot, kDirect, false, false, false).ok, "boot: allowed while it is up");
    Require(!PortPicture(Pic::Boot, kDone, false, false, false).ok && !PortPicture(Pic::Boot, kOff, false, false, false).ok, "boot: not after it ended");
    Require(!PortPicture(Pic::Progress, kDone, true, true, false).ok && !PortPicture(Pic::PlainBar, kDone, true, true, false).ok, "progress and plain bar: never over a live picture after play started, no switch needed");
    Require(PortPicture(Pic::Progress, kDone, true, false, false).ok, "progress: a closed or quiet screen after play started keeps its picture (stage load)");
    Require(PortPicture(Pic::Progress, kDone, false, true, false).ok, "progress: before play started (the title, the file select) as before");
    Require(!PortPicture(Pic::Progress, kDone, false, true, true).ok, "progress: noprog26's switchable rule still refuses");
    { bool st = false; Require(!LatchPlay(st, false) && !st, "latch: not for a picture that is not live"); Require(LatchPlay(st, true) && st, "latch: set once"); Require(!LatchPlay(st, true) && st, "latch: and only once, never cleared"); }
    { EndInput in; in.phase = kDone; in.playStarted = true; Require(EndDue(in) == EndWhy::None, "EndDue: nothing to end once done"); in.phase = kDirect; Require(EndDue(in) == EndWhy::None, "EndDue: not in kDirect"); }
    { EndInput in; in.phase = kGl; in.limitS = 0.0; in.titleSeen = true; in.sinceTitleS = 9.0; Require(EndDue(in) == EndWhy::None, "EndDue: GXV_BOOT_FINISH_SEC=0 waits until the hard limit...");
      in.sinceTitleS = 10.0; Require(EndDue(in) == EndWhy::Deadline, "...and cannot keep the screen past it"); in.limitS = 600.0; Require(EndDue(in) == EndWhy::Deadline, "a huge limit is cut to the hard limit too"); }

    // --- 1. normal finish: the title, the fade, three dark swaps, then the game
    {
        Machine m = Boot();
        for (int i = 0; i < 30; ++i) { m.Advance(0.1); m.GamePresent(0); }
        Require(m.phase == kGl && !m.GamePresent(0), "1: before the title the game's pictures are hidden");
        m.TitleSeen(m.now);
        bool gameSeen = false;
        for (int i = 0; i < 50 && !gameSeen; ++i) { m.Advance(0.05); gameSeen = m.GamePresent(0); }
        Require(gameSeen && m.phase == kDone && m.endedWhy == EndName(EndWhy::FadedDark), "1: the fade, three dark swaps, Done, and the game's pictures show");
        Require(!m.thread && m.picsFreed, "1: thread stopped, buffers gone");
        const unsigned swaps = m.bootSwaps;
        for (int i = 0; i < 100; ++i) { m.Advance(0.05); m.Service(); m.GamePresent(0); }
        Require(m.bootSwaps == swaps, "1: after Done no boot picture is swapped, ever");
    }
    // --- 2. the alternate path: after the title only the picture half (path 1) and direct display (3) arrive; the swap that would count dark pictures never comes
    {
        Machine m = Boot();
        m.TitleSeen(m.now);
        bool gameSeen = false;
        for (int i = 0; i < 400 && !gameSeen; ++i) { m.Advance(0.05); gameSeen = m.GamePresent(i % 2 ? 1 : 3); }
        Require(gameSeen && m.phase == kDone, "2: ends at a game present on a path that never reaches Service (the Strikers failure)");
        Require(m.endedWhy == EndName(EndWhy::FadedGrace), "2: by the grace rule: faded and a game picture waiting");
        Require(m.now - m.titleAt <= kFadeEndS + kGraceS + 0.2, "2: within the grace time after the fade, not after the 5 s deadline");
        Require(!m.thread && m.picsFreed, "2: thread stopped, buffers gone");
    }
    // --- 2b. GXV_BOOT_FINISH_SEC=0 and nothing but path 1 and a title that never fades (thread starved): the hard limit ends it
    {
        Machine m = Boot();
        m.limitS = 0.0;
        m.TitleSeen(m.now);
        bool gameSeen = false;
        for (int i = 0; i < 400 && !gameSeen; ++i) { m.now += 0.05; /* the ticker never runs: not faded */ gameSeen = m.GamePresent(1); }
        Require(gameSeen && m.endedWhy == EndName(EndWhy::Deadline) && m.now - m.titleAt <= kHardLimitS + 0.1, "2b: a starved thread and the switch off: the hard limit ends the screen");
    }
    // --- 3. a long present after the finish: nothing is drawn, the machine stays Done
    {
        Machine m = Boot();
        m.TitleSeen(m.now);
        for (int i = 0; i < 40; ++i) { m.Advance(0.05); m.GamePresent(0); }
        Require(m.phase == kDone, "3: precondition: done");
        const unsigned swaps = m.bootSwaps, writes = m.directWrites;
        m.Advance(4.4);                      // one present that took 4.4 s (a target resize and 92 uploads)
        m.ThreadTick(); m.Service(); m.GamePresent(2); m.GamePresent(0);
        for (int i = 0; i < 20; ++i) { m.Advance(0.1); m.ThreadTick(); m.Service(); }
        Require(m.bootSwaps == swaps && m.directWrites == writes && m.phase == kDone && !m.thread, "3: a long present after the finish draws nothing and the thread is not alive");
    }
    // --- 3b. the same long present BEFORE the finish (title seen, thread starved): ends by the deadline at the next game present, then never again
    {
        Machine m = Boot();
        m.TitleSeen(m.now);
        m.now += 4.4;                        // the starved ticker has not faded it; a 4.4 s present
        m.GamePresent(1);
        Require(m.phase == kGl, "3b: 4.4 s after the title is inside the 5 s default");
        m.now += 1.0;
        Require(m.GamePresent(1) && m.phase == kDone, "3b: past the deadline the next game present ends it");
    }
    // --- 4. progress requests after play started
    {
        Machine m = Boot();
        m.TitleSeen(m.now);
        for (int i = 0; i < 40; ++i) { m.Advance(0.05); m.GamePresent(0); }
        Require(m.phase == kDone, "4: precondition: done");
        Require(m.ProgressRequest(false, false), "4: before play: a closed screen's progress picture is allowed (the title, a load)");
        m.Play(false);
        Require(!m.playStarted, "4: a closed screen does not start play");
        m.Play(true);
        Require(m.playStarted, "4: the first live picture starts play");
        Require(!m.ProgressRequest(true, true) && !m.ProgressRequest(true, false), "4: in play, with the noprog26 rule on or off, refused");
        Require(m.ProgressRequest(false, false), "4: a closed screen after play (a stage load) still gets its picture");
        Require(m.progressRefused == 2 && m.bootSwapsInPlay == 0, "4: counted, and no boot picture in play");
        m.Play(false);
        Require(m.playStarted, "4: PlayStarted is never cleared");
    }
    // --- 5. play starts while the boot screen is somehow still up (a PETARI_STAGE direct entry whose title never shows): the screen ends at once and is never swapped again
    {
        Machine m = Boot();
        m.Play(true);
        m.Service();
        Require(m.phase == kDone && m.endedWhy == EndName(EndWhy::PlayStarted) && m.bootSwapsInPlay == 0, "5: play started: the boot screen ends, nothing was swapped over play");
    }
    // --- 6. a direct-phase thread can not touch the display once play has started
    {
        Machine m;
        m.Begin();
        m.playStarted = true;
        const unsigned before = m.directWrites;
        m.ThreadTick();
        Require(m.directWrites == before, "6: the ticker's direct display write is refused after play started");
    }
    // --- 7. the start-up failure text stays up until play
    {
        Machine m = Boot();
        m.failed = true;
        m.TitleSeen(m.now);
        for (int i = 0; i < 400; ++i) { m.Advance(0.05); m.GamePresent(0); }
        Require(m.phase == kGl, "7: a failure text is not taken down by the clock");
        m.Play(true);
        m.GamePresent(0);
        Require(m.phase == kDone, "7: but never over a game that is being played");
    }
    std::printf("PASS boot screen model\n");
    return 0;
}
