// precomp26 (GXV_STAGE_ENUM): host test of the pure parts, platform/vgx/src/vgx_stage_enum.h (Inbox, record keys, Decide, HitTracker).
// Not covered here (needs the console or the full front-end harness): the game's J3D walk, the dry draw's keys against real draws, the worker.
#include "vgx_stage_enum.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace vgx::senum;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    // keys
    Rec a; a.vs = 1; a.fs = 2; a.lb = 3;
    Rec b = a; b.lb = 4;
    Rec c = a; c.stage = 9; c.seq = 5; // stage and seq are not part of the key
    CHECK(RecKey(a) != RecKey(b));
    CHECK(RecKey(a) == RecKey(c));
    CHECK(ProgKey(1, 2) != ProgKey(2, 1));
    // inbox: order, cap, drain
    {
        Inbox in;
        for (unsigned i = 0; i < kMaxRecs + 10; ++i) { Rec r; r.vs = i; r.seq = i; in.Push(r); }
        CHECK(in.Size() == kMaxRecs && in.Dropped() == 10 && in.Pushed() == kMaxRecs);
        std::vector<Rec> out; in.Drain(out);
        CHECK(out.size() == kMaxRecs && out[0].vs == 0 && out[kMaxRecs - 1].vs == kMaxRecs - 1 && in.Size() == 0);
        Inbox t; // two producers, one consumer
        std::thread p1([&] { for (int i = 0; i < 3000; ++i) { Rec r; r.vs = 1; t.Push(r); } });
        std::thread p2([&] { for (int i = 0; i < 3000; ++i) { Rec r; r.vs = 2; t.Push(r); } });
        size_t got = 0; std::vector<Rec> o;
        while (got < 6000) { o.clear(); t.Drain(o); got += o.size(); std::this_thread::yield(); }
        p1.join(); p2.join();
        CHECK(got == 6000);
    }
    // Decide
    {
        ActIn i; i.haveWorker = true; i.freeSlots = 200; i.reserve = 100;
        i.linked = true; CHECK(Decide(i) == Act::Done); i.linked = false;
        i.cache = Cache::Hit; CHECK(Decide(i) == Act::Link);
        i.live = true; i.workerBusy = true; CHECK(Decide(i) == Act::HoldBack);
        i.workerBusy = false; CHECK(Decide(i) == Act::Link);
        i.freeSlots = 100; CHECK(Decide(i) == Act::NoSlot); // at the reserve: no slot
        i.freeSlots = 101; CHECK(Decide(i) == Act::Link);
        i.cache = Cache::Miss; CHECK(Decide(i) == Act::Ask);
        i.inFlight = 1; CHECK(Decide(i) == Act::WaitWorker);
        i.inFlight = 0; i.pacedOut = true; CHECK(Decide(i) == Act::WaitWorker);
        i.pacedOut = false; i.asked = true; CHECK(Decide(i) == Act::WaitWorker);
        i.asked = false; i.haveWorker = false; CHECK(Decide(i) == Act::NoWorker); // never a compile on the render thread for a guess
        i.cache = Cache::Unknown; CHECK(Decide(i) == Act::NoWorker);
    }
    // hit tracker
    {
        HitTracker h;
        h.Predict(10, 100); h.Predict(11, 110);
        CHECK(h.NoteProgram(10, false) == 1);   // predicted, not in lists
        CHECK(h.NoteProgram(10, false) == -1);  // counted once
        CHECK(h.NoteProgram(12, false) == 2);   // not predicted
        CHECK(h.NoteProgram(13, true) == 0);    // in lists
        CHECK(h.NoteProgram(11, true) == 0);    // in lists wins
        CHECK(h.pHit() == 1 && h.pMiss() == 1 && h.pList() == 2);
        CHECK(h.NoteVariant(100, false) == 1 && h.NoteVariant(999, false) == 2 && h.NoteVariant(110, true) == 0);
        CHECK(std::abs(HitTracker::Rate(h.pHit(), h.pMiss()) - 50.0) < 1e-9);
        CHECK(HitTracker::Rate(0, 0) < 0);
        h.Reset(); CHECK(h.Predicted() == 0 && h.pHit() == 0);
    }
    // seedrun31 A4: the can-draw tag (the record a harvest keeps) and the tier's place behind the stage's queue
    {
        using namespace vgx::firstvisit;
        const uint32_t w = CanDrawWord(0x16c51u | slowTag | (3u << 26), ScenarioBits(3));
        CHECK(w == (0x16c51u | canDrawTag | ScenarioBits(3)) && (w & drawMask) == 0x16c51u && CanDraw(w) && ScenarioOf(w) == 3);
        CHECK(!Priority(w | slowTag, 3) && !Priority(w | slowTag, 0)); // (a can-draw record never goes to the head)
        CHECK(KeepAsCanDraw(true, false, true) && !KeepAsCanDraw(false, false, true) && !KeepAsCanDraw(true, true, true) && !KeepAsCanDraw(true, false, false));
        // a stage's queue (own A, B; player P) and its tier from four enumerated variants: one is A's draw (the queue has it), one is another scenario's
        struct R { uint32_t vs, fs, lb; };
        Queue q;
        const std::vector<R> queued = {{1, 1, 0x400}, {2, 2, 0x800}, {9, 9, 0x400}};
        for (const R& r : queued) CHECK(q.First(r.vs, r.fs, r.lb));
        const std::vector<R> cand = {{1, 1, CanDrawWord(0x400, 0)}, {3, 3, CanDrawWord(0x400, ScenarioBits(2))}, {4, 4, CanDrawWord(0xc00, 0)}, {5, 5, CanDrawWord(0x400, ScenarioBits(3))}};
        std::vector<uint32_t> order;
        ScenarioFirst(cand, 3, order);
        std::vector<uint32_t> tier;
        for (const uint32_t i : order) if (!q.Seen(cand[i].vs, cand[i].fs, cand[i].lb)) tier.push_back(cand[i].vs);
        CHECK(tier == (std::vector<uint32_t>{4, 5, 3})); // (this scenario's and unhinted first, the other scenario's last; A's draw not taken twice)
        CHECK(q.First(4, 4, 0xc00)); // (the tier claims nothing: a late own record of the same draw still joins the queue's head)
        // the place: only behind the whole queue, only under a cover of the game's, never in play
        CanDrawIn in; in.cached = true; in.covered = true; in.queueEmpty = false;
        CHECK(CanDrawGate(in) == CanDrawStep::Wait);
        in.queueEmpty = true; CHECK(CanDrawGate(in) == CanDrawStep::Take);
        in.live = true; CHECK(CanDrawGate(in) == CanDrawStep::Wait);
        in.live = false; in.covered = false; CHECK(CanDrawGate(in) == CanDrawStep::Wait);
        in.covered = true; in.cached = false; CHECK(CanDrawGate(in) == CanDrawStep::Skip);
    }
    if (fails) { std::printf("RESULT: FAIL (%d)\n", fails); return 1; }
    std::printf("RESULT: PASS\n");
    return 0;
}
