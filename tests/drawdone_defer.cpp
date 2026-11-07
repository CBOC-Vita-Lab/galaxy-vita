// GXV_DRAWDONE_DEFER: where the game thread waits for the front end's "draw done" (platform/include/port/drawdone_defer.h).
//
// A model of GXMisc.c's one-bit DrawDone flag and of the front end's finish interrupt stands in for the SDK.  A game thread plays
// scripted turns against the same calls the port makes (MainLoopFramework::waitDrawDoneAndSetAlarm, GXSetDrawDone's settle hook,
// GameSystem::update / calcAnim) and the test checks, for every mode:
//   - where the thread blocks (the trace of waits) against an independent reference of the rules;
//   - that two finishes are never outstanding when a GXSetDrawDone is issued (the flag is one bit);
//   - that nothing is left pending at the end and every finish was waited for;
//   - the same with the front end finishing at random moments (before, during and after the game's wait).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/drawdone_defer.h"

using port::drawdone::Deferral;

static void Require(bool ok, const char* why) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", why);
        abort();
    }
}

struct Model {
    explicit Model(int mode) : d(mode) {}
    Deferral d;
    std::mutex m;
    std::condition_variable cv;
    bool flag = true;            // the SDK's DrawDone
    int outstanding = 0;         // finishes the front end still owes
    std::atomic<bool> inWait{false};
    std::atomic<unsigned> issued{0}, finished{0};
    unsigned overlaps = 0;
    std::vector<std::string> trace;

    // GXSetDrawDone (the settle hook first, as GXMisc.c does)
    void SetDrawDone() {
        Settle(port::drawdone::kSetDrawDone);
        std::lock_guard<std::mutex> lk(m);
        if (outstanding) ++overlaps;
        flag = false;
        ++outstanding;
        ++issued;
    }
    void WaitDrawDone() {
        std::unique_lock<std::mutex> lk(m);
        inWait = true;
        cv.wait(lk, [this] { return flag; });
        inWait = false;
    }
    void FeFinishes() {  // the interrupt handler
        std::lock_guard<std::mutex> lk(m);
        FinishLocked();
    }
    void FinishLocked() {
        if (outstanding) {
            --outstanding;
            ++finished;
        }
        flag = true;
        cv.notify_all();
    }
    // The blocking-phase front end: finishes only while the game thread is asleep in a wait (the worst case for the game).
    bool FeFinishesIfWaiting() {
        std::lock_guard<std::mutex> lk(m);
        if (!inWait || flag) return false;
        FinishLocked();
        return true;
    }
    void Settle(int point) {  // port_gx_drawdone_settle
        if (!d.MustWait(point)) return;
        trace.push_back("wait@" + std::to_string(point));
        WaitDrawDone();
        d.Waited(1000);
    }
    // MainLoopFramework::waitDrawDoneAndSetAlarm
    void EndFrame() {
        if (d.Enabled()) {
            SetDrawDone();
            d.Arm();
            trace.push_back("defer");
        } else {
            SetDrawDone();
            WaitDrawDone();
            trace.push_back("wait@end");
        }
    }
    // A game's own GXDrawDone in the middle of a turn (MarioActorSpecialDraw)
    void MidDrawDone() {
        SetDrawDone();
        WaitDrawDone();
        trace.push_back("mid");
    }
    void Update() { Settle(port::drawdone::kUpdate); }
    void CalcAnim() { Settle(port::drawdone::kCalcAnim); }
};

enum Op { UPDATE, CALCANIM, ENDFRAME, MID };

// The rules, written out separately from the class: which waits a script must make in `mode`.
static std::vector<std::string> Reference(int mode, const std::vector<Op>& script) {
    std::vector<std::string> out;
    bool pending = false;
    auto settle = [&](int point) {
        if (pending && (point == 0 || point >= mode)) {
            out.push_back("wait@" + std::to_string(point));
            pending = false;
        }
    };
    for (Op o : script) {
        switch (o) {
        case UPDATE: settle(1); break;
        case CALCANIM: settle(2); break;
        case MID: settle(0); out.push_back("mid"); break;
        case ENDFRAME:
            settle(0);
            if (mode) { out.push_back("defer"); pending = true; }
            else out.push_back("wait@end");
            break;
        }
    }
    return out;
}

static std::vector<Op> FixedScript() {
    std::vector<Op> s;
    auto drawn = [&](bool mid) { if (mid) s.push_back(MID); s.push_back(UPDATE); s.push_back(CALCANIM); s.push_back(ENDFRAME); };
    auto skipped = [&] { s.push_back(UPDATE); s.push_back(CALCANIM); };
    drawn(false);   // T1
    skipped();      // T2: the wait comes here
    drawn(false);   // T3
    drawn(true);    // T4: a drawn turn after a drawn turn, its mid-frame GXDrawDone with a finish pending
    drawn(false);   // T5
    skipped();
    skipped();
    drawn(false);
    s.push_back(UPDATE);  // (the next turn's update flushes the last one)
    s.push_back(CALCANIM);
    return s;
}

static std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += x + " ";
    return s;
}

// Phase A: the front end only finishes while the game thread is blocked in a wait (or, for a deferred finish, when it gets there).
static void RunBlocking(int mode, const std::vector<Op>& script) {
    Model model(mode);
    std::atomic<bool> done{false};
    std::thread game([&] {
        for (Op o : script) {
            switch (o) {
            case UPDATE: model.Update(); break;
            case CALCANIM: model.CalcAnim(); break;
            case ENDFRAME: model.EndFrame(); break;
            case MID: model.MidDrawDone(); break;
            }
        }
        done = true;
    });
    unsigned blockedObserved = 0;
    while (!done.load()) {
        if (model.FeFinishesIfWaiting()) {
            ++blockedObserved;
        } else {
            std::this_thread::yield();
        }
    }
    game.join();
    const std::vector<std::string> want = Reference(mode, script);
    if (model.trace != want) {
        fprintf(stderr, "mode %d: waits were [%s], expected [%s]\n", mode, Join(model.trace).c_str(), Join(want).c_str());
        Require(false, "the waits are made where the rules put them");
    }
    Require(model.overlaps == 0 && model.d.Overlaps() == 0, "two finishes were never outstanding together");
    Require(model.issued == model.finished, "every finish the game asked for was waited for");
    Require(!model.d.Pending(), "nothing is left pending");
    // With the game blocked at each wait, the number of times it was seen blocked is the number of waits made.
    unsigned waits = 0;
    for (const auto& t : model.trace) waits += t.rfind("wait@", 0) == 0 || t == "mid";
    Require(blockedObserved == waits, "the game slept in exactly the waits that were made (each woken once by the front end)");
    // In the default mode nothing is ever deferred; in the others the drawn turns' end-of-turn waits are all gone.
    if (mode == 0) Require(model.d.Deferred() == 0 && model.d.Waits() == 0, "mode 0 never defers");
    else {
        unsigned ends = 0;
        for (Op o : script) ends += o == ENDFRAME;
        Require(model.d.Deferred() == ends, "mode on: every end of turn is deferred");
        Require(model.d.Waits() == ends, "and settled exactly once");
        for (const auto& t : model.trace) Require(t != "wait@end", "no wait at the end of a turn with the mode on");
    }
}

// Phase B: the front end finishes after a random delay (a finish can come before, during or after the game's wait).
static void RunRandom(int mode, unsigned seed) {
    uint32_t rng = seed;
    auto rnd = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    std::vector<Op> script;
    for (int turn = 0; turn < 120; turn++) {
        if (rnd() % 4 == 0) { script.push_back(MID); }
        script.push_back(UPDATE);
        script.push_back(CALCANIM);
        if (rnd() % 3 != 0) script.push_back(ENDFRAME);
    }
    script.push_back(UPDATE);
    script.push_back(CALCANIM);
    Model model(mode);
    std::atomic<bool> done{false};
    std::thread fe([&] {  // finishes each issued command after 0-300 us
        uint32_t r = seed * 2654435761u + 1;
        unsigned handled = 0;
        while (!done.load() || handled < model.issued.load()) {
            if (handled < model.issued.load()) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                std::this_thread::sleep_for(std::chrono::microseconds(r % 300));
                model.FeFinishes();
                ++handled;
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (Op o : script) {
        switch (o) {
        case UPDATE: model.Update(); break;
        case CALCANIM: model.CalcAnim(); break;
        case ENDFRAME: model.EndFrame(); break;
        case MID: model.MidDrawDone(); break;
        }
    }
    done = true;
    fe.join();
    Require(model.trace == Reference(mode, script), "random front end: the waits are where the rules put them");
    Require(model.overlaps == 0 && model.d.Overlaps() == 0, "random front end: two finishes were never outstanding together");
    Require(model.issued == model.finished && !model.d.Pending(), "random front end: all finishes done, nothing pending");
}

int main() {
    using port::drawdone::ParseMode;
    // The switch: only 1 and 2 turn it on.
    Require(ParseMode(nullptr) == 0 && ParseMode("") == 0, "unset and empty are off");
    Require(ParseMode("0") == 0 && ParseMode("1") == 1 && ParseMode("2") == 2, "0, 1, 2");
    Require(ParseMode("3") == 3 && ParseMode("4") == 0 && ParseMode("-1") == 0 && ParseMode("garbage") == 0 && ParseMode("1x") == 0,
            "other values are off");
    Require(ParseMode("0x1") == 1 && ParseMode("01") == 1, "numbers in the forms strtol reads (the port's other switches do the same)");

    const std::vector<Op> script = FixedScript();
    for (int mode = 0; mode <= 2; mode++) RunBlocking(mode, script);
    for (int mode = 0; mode <= 2; mode++)
        for (unsigned seed = 1; seed <= 12; seed++) RunRandom(mode, seed * 7919u);

    // The expected traces, spelled out for the fixed script (a reader's check of Reference itself).
    Require(Join(Reference(0, script)) == "wait@end wait@end mid wait@end wait@end wait@end ", "mode 0 trace");
    Require(Join(Reference(1, script)) == "defer wait@1 defer wait@0 mid defer wait@1 defer wait@1 defer wait@1 ",
            "mode 1 trace");
    Require(Join(Reference(2, script)) == "defer wait@2 defer wait@0 mid defer wait@2 defer wait@2 defer wait@2 ",
            "mode 2 trace");

    // Mode 3: the turns without a picture never wait (update, calcAnim); the next drawn turn's start does, and a GXSetDrawDone does.
    {
        Deferral d(3);
        d.Arm();
        Require(!d.MustWait(port::drawdone::kUpdate) && !d.MustWait(port::drawdone::kCalcAnim), "mode 3: no wait in turns without a picture");
        Require(!d.MustWait(port::drawdone::kUpdate) && !d.MustWait(port::drawdone::kCalcAnim), "mode 3: still none a turn later");
        Require(d.MustWait(port::drawdone::kDrawnTurn) && !d.MustWait(port::drawdone::kDrawnTurn), "mode 3: one wait at the drawn turn");
        d.Arm();
        Require(d.MustWait(port::drawdone::kSetDrawDone), "mode 3: a GXSetDrawDone waits first");
        Deferral two(2);
        two.Arm();
        Require(two.MustWait(port::drawdone::kCalcAnim) && !two.MustWait(port::drawdone::kDrawnTurn), "mode 2 unchanged; the drawn-turn point finds nothing left");
    }
    // The log line.
    {
        Deferral d(1);
        char line[240];
        for (int i = 0; i < 299; i++) { d.Arm(); Require(d.MustWait(1), "wait"); d.Waited(2000000); }
        Require(!d.Report(line, sizeof line, 300), "no line before 300 waits");
        d.Arm();
        Require(d.MustWait(1), "wait");
        d.Waited(2000000);
        Require(d.Report(line, sizeof line, 300), "a line at 300 waits");
        Require(strstr(line, "mode 1") && strstr(line, "300 deferred") && strstr(line, "2.00 ms each") && strstr(line, "(300 over 1 ms)"), "the line says what happened");
        Require(!d.Report(line, sizeof line, 300), "and starts a new window");
    }
    printf("drawdone defer: modes 0, 1, 2, 3 wait where the rules say, never two finishes outstanding, random front end clean\n");
    return 0;
}
