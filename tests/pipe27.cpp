// pipe27 host test: the PRODUCTION engine of GXV_PIPE25 (platform/include/port/draw_thread.h, class Late: the hand-over, the helper's
// job, the join with its take-overs, the abandon, verify) over a model of what the game does for it (a LateHost whose passes are walks of
// a small "live state" that the next turn's logic writes). The stream the engine produces must equal the serial turn's bytes.
//   pipe27_test [turns] [passes]    env: P27_SLEEP=1   widens the windows (the logic and the passes nap)
//                                        P27_NEG=unfenced   the helper reads the live state, not the snapshot: the stream / verify must differ
// Scenarios (each runs `turns` turns): normal; late start (the helper starts after the join: taken back); slow helper (the join asks it to
// stop at a pass boundary and finishes the rest itself); abandon at pass k (the passes made so far run in place, the rest is serial);
// scene change (join straight after the hand-over, no update); an early join by the logic (GameEnter); verify (in place + re-run +
// compare), with the unfenced control.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "port/draw_thread.h"
using namespace port::drawpipe;
using namespace port::pipe25;

static bool gSleep = false, gUnfenced = false;
static int gFail = 0;
static void Nap(int us = 20) { if (gSleep) std::this_thread::sleep_for(std::chrono::microseconds(us)); }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++gFail; } } while (0)

static constexpr int kWords = 16;
static uint32_t gLive[kWords];  // what update() writes and a pass reads
static void Logic(uint32_t turn) { for (int i = 0; i < kWords; ++i) { gLive[i] = turn * 1009u + i * 31u; if (i == 7) Nap(); } }
static void WalkPass(const uint32_t* st, uint32_t pass, std::vector<uint8_t>& out) {
    for (int i = 0; i < kWords; ++i) {
        uint32_t w = st[i] ^ (pass * 0x9E3779B9u);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&w);
        out.insert(out.end(), p, p + 4);
        if (i == 7) Nap();
    }
}
static void Fill(std::vector<uint8_t>& s, uint32_t tag, uint32_t n) { for (uint32_t i = 0; i < n; ++i) s.push_back(static_cast<uint8_t>(tag + i)); }

struct Scenario {
    uint32_t abandonAt = 0xFFFFFFFFu;  // abandon when this pass is asked for
    uint32_t slowUs = 0;               // every pass on the helper takes this much longer
    uint32_t startDelayUs = 0;         // the helper starts the job this late
    bool noUpdate = false;             // scene change: join at once
    bool logicGx = false;              // the logic uses GX after a while: GameEnter joins
    bool verify = false;
    uint32_t updateUs = 3000;          // update() takes this long (the helper's window)
};

struct Model : LateHost {
    Late* late = nullptr;
    Scenario sc;
    uint32_t passes = 6, turn = 0;
    std::vector<uint8_t> stream, ref;
    // the hand-over's store
    std::vector<std::vector<uint32_t>> snaps;
    std::vector<std::vector<uint8_t>> chain;  // Bi
    std::vector<uint8_t> a;
    std::vector<std::vector<uint8_t>> inPlace;  // verify
    uint32_t tokens = 0;
    bool verifyMode = false, dropped = false;
    VerifyStats* vs = nullptr;
    std::atomic<uint32_t> stepsHelper{0}, stepsGame{0};
    std::thread::id gameId = std::this_thread::get_id();

    void SerialRef() { ref.clear(); Fill(ref, 0xA0, 100); for (uint32_t i = 0; i < passes; ++i) { WalkPass(gLive, i, ref); Fill(ref, static_cast<uint8_t>(i), 300 + i * 70); } Fill(ref, 0xEE, 40); }
    // ---- the draw (the game thread)
    void DrawBegin() override { a.clear(); Fill(a, 0xA0, 100); }
    // One pass: the glue (portLateTokenOut) in miniature.
    void Pass(uint32_t i) {
        if (i == sc.abandonAt && late->Capturing()) late->Abandon(*this, kWhyDrawWrite);
        std::vector<uint8_t> b;
        Fill(b, static_cast<uint8_t>(i), 300 + i * 70);
        if (late->Capturing()) {
            snaps.emplace_back(gLive, gLive + kWords);
            late->CountToken();
            if (verifyMode) { inPlace.emplace_back(); WalkPass(gLive, i, inPlace.back()); WalkPass(gLive, i, tail()); chainOpen(b); }
            else { chain.emplace_back(); chain.back() = b; ++tokens; }
        } else {
            // in place (serial, or after an abandon): the pass, then the open list's bytes
            WalkPass(gLive, i, tail());
            chainOpen(b);
        }
    }
    std::vector<uint8_t>& tail() { return open_; }
    void chainOpen(const std::vector<uint8_t>& b) { open_.insert(open_.end(), b.begin(), b.end()); }
    std::vector<uint8_t> open_;  // the stream as the game thread writes it before the hand-over
    void DrawMiddle() override { for (uint32_t i = 0; i < passes; ++i) Pass(i); }
    void DrawEnd() override { Fill(stream_end(), 0xEE, 40); }
    std::vector<uint8_t>& stream_end() { return stream; }
    bool Open(bool verify) override {
        verifyMode = verify; dropped = false; tokens = 0;
        snaps.clear(); chain.clear(); inPlace.clear(); open_.clear(); stream.clear();
        return true;
    }
    uint32_t Close(bool verify, bool abandoned) override {
        if (verify) {
            // the stream is whole (passes in place); the draw's end is made by the engine right after
            stream = a; stream.insert(stream.end(), open_.begin(), open_.end());
            if (!dropped && !abandoned) ++vs->pictures;
            return dropped || abandoned ? 0 : static_cast<uint32_t>(snaps.size());
        }
        if (abandoned) {
            // AbandonNow ran everything into the stream; what came after was in place
            stream.insert(stream.end(), open_.begin(), open_.end());
            return 0;
        }
        if (snaps.empty()) { stream = a; stream.insert(stream.end(), open_.begin(), open_.end()); return 0; }
        stream = a;  // the splice starts at the tail of what the game thread wrote before the first token (all of A)
        return static_cast<uint32_t>(snaps.size());
    }
    void AbandonNow(bool verify) override {
        if (verify) { dropped = true; return; }
        // the passes made so far, in order, here: A, then Pi Bi
        stream = a;
        for (uint32_t i = 0; i < snaps.size(); ++i) { WalkPass(snaps[i].data(), i, stream); stream.insert(stream.end(), chain[i].begin(), chain[i].end()); }
        snaps.clear(); chain.clear(); tokens = 0;
    }
    // ---- the helper (or the game thread at a take-over)
    uint32_t Begin(bool) override { return static_cast<uint32_t>(snaps.size()); }
    void Step(uint32_t i, bool verify) override {
        (std::this_thread::get_id() == gameId ? stepsGame : stepsHelper).fetch_add(1);
        if (sc.slowUs && std::this_thread::get_id() != gameId) std::this_thread::sleep_for(std::chrono::microseconds(sc.slowUs));
        std::vector<uint8_t> p;
        WalkPass(gUnfenced ? gLive : snaps[i].data(), i, p);
        if (verify) {
            PassRec r; r.id = i; r.length = static_cast<uint32_t>(inPlace[i].size()); r.models = {{0, "model"}};
            ComparePass(r, inPlace[i].data(), p.data(), static_cast<uint32_t>(p.size()), *vs, turn);
            return;
        }
        stream.insert(stream.end(), p.begin(), p.end());
        stream.insert(stream.end(), chain[i].begin(), chain[i].end());
    }
    void End(bool) override {}
    bool OverlapBegin() override { return true; }
    void OverlapEnd() override {}
    void Poll() override {}
    int64_t NowNs() override { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void Log(const char* line) override { if (getenv("P27_LOG")) printf("%s\n", line); }
};

struct Result { uint32_t bad = 0, taken = 0, takenOver = 0, early = 0, abandoned = 0, helperSteps = 0, gameSteps = 0, late = 0; VerifyStats vs; };

static Result Run(const Scenario& sc, uint32_t turns, uint32_t passes, bool verifyMode) {
    Result r;
    Late& late = *new Late;  // (never destroyed: its thread waits on it for ever, as in the game)
    late.takeoverUs = 800;
    late.watchdogUs = 5000000;
    CHECK(late.worker.Start(1, -1, 512u << 10));
    late.worker.testStartDelayUs.store(sc.startDelayUs);
    late.netLoss = false;  // (the control turns of the net-loss rule are drawn serially without Open: this model builds its stream in Open)
    Model m;
    m.late = &late; m.sc = sc; m.passes = passes; m.vs = &r.vs;
    Logic(0);
    for (uint32_t t = 0; t < turns; ++t) {
        m.turn = t;
        Logic(t * 2);                   // the flushes: the live state at the hand-over
        m.SerialRef();                  // today's serial turn, from the live state of this hand-over
        const int how = late.Draw(m, verifyMode ? 2 : 1, kGo);
        if (how == 0) {
            // serial (also after an abandon): the stream is whole but for the draw's end the engine made already
            if (m.stream != m.ref) { ++r.bad; if (r.bad == 1) printf("  serial-path stream differs, turn %u\n", t); }
            Logic(t * 2 + 1);
            continue;
        }
        if (sc.noUpdate) {
            late.Join(m);               // scene change: nothing runs between the hand-over and the join
        } else {
            Logic(t * 2 + 1);           // update(): writes the live state beside the helper
            std::this_thread::sleep_for(std::chrono::microseconds(sc.updateUs));
            if (sc.logicGx && (t % 3) == 1) { CHECK(late.GameEnter(m)); }   // the logic needs GX: joined first
            late.Join(m);               // before calcAnim
        }
        if (m.stream != m.ref) { ++r.bad; if (r.bad == 1) printf("  stream differs, turn %u (%zu vs %zu bytes)\n", t, m.stream.size(), m.ref.size()); }
        CHECK(!late.Overlapped());
    }
    r.taken = late.c.taken; r.takenOver = late.c.takenOver; r.early = late.c.early; r.abandoned = late.c.abandoned;
    r.helperSteps = m.stepsHelper; r.gameSteps = m.stepsGame;
    return r;
}

int main(int argc, char** argv) {
    const uint32_t turns = argc > 1 ? static_cast<uint32_t>(atoi(argv[1])) : 150;
    const uint32_t passes = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 6;
    gSleep = getenv("P27_SLEEP") != nullptr;
    const char* neg = getenv("P27_NEG");
    gUnfenced = neg && !strcmp(neg, "unfenced");
    if (gUnfenced) {
        // the negative control: the helper reads the live state. The stream is not the serial one, and verify names it.
        Scenario sc;
        Result a = Run(sc, turns, passes, false);
        Scenario sv; sv.verify = true;
        Result b = Run(sv, turns, passes, true);
        printf("pipe27 negative control unfenced: stream mismatches %u, verify mismatches %llu (%s)\n", a.bad, (unsigned long long)b.vs.mismatches, b.vs.first);
        const bool caught = a.bad > 0 && b.vs.mismatches > 0;
        printf("%s\n", caught ? "caught (as it must)" : "NOT CAUGHT (test broken)");
        return caught ? 0 : 1;
    }
    {
        // revB: the verify mode's GXData end-state compare
        uint32_t a[64], b[64];
        for (int i = 0; i < 64; ++i) a[i] = b[i] = 0x1000u + static_cast<uint32_t>(i);
        port::pipe25::VerifyStats vs;
        CHECK(port::pipe25::CompareGxEnd(a, b, 64, vs, 7, 2));
        CHECK(vs.passesState == 1 && vs.stateDiffer == 0);
        b[37] ^= 4u;
        CHECK(!port::pipe25::CompareGxEnd(a, b, 64, vs, 8, 3));
        CHECK(vs.passesState == 2 && vs.stateDiffer == 1 && strstr(vs.stateFirst, "picture 8 pass 3 word 37 of 64") != nullptr);
        b[5] ^= 1u;
        CHECK(!port::pipe25::CompareGxEnd(a, b, 64, vs, 9, 0));
        CHECK(vs.stateDiffer == 2 && strstr(vs.stateFirst, "picture 8") != nullptr);  // (the first one stays)
    }
    struct { const char* name; Scenario sc; } list[8];
    int n = 0;
    list[n].name = "normal"; ++n;
    list[n].name = "late start (job taken back)"; list[n].sc.startDelayUs = 30000; ++n;
    list[n].name = "slow helper (stop at a pass boundary, game thread finishes)"; list[n].sc.slowUs = 2500; ++n;
    list[n].name = "abandon at pass 3"; list[n].sc.abandonAt = 3; ++n;
    list[n].name = "abandon at pass 0"; list[n].sc.abandonAt = 0; ++n;
    list[n].name = "scene change (join at once)"; list[n].sc.noUpdate = true; ++n;
    list[n].name = "logic uses GX (early join)"; list[n].sc.logicGx = true; ++n;
    for (int k = 0; k < n; ++k) {
        Result r = Run(list[k].sc, turns, passes, false);
        printf("pipe27 %-58s: %u bad streams of %u; helper steps %u, game-thread steps %u, taken back %u, take-overs %u, early joins %u, abandoned %u\n", list[k].name, r.bad,
               turns, r.helperSteps, r.gameSteps, r.taken, r.takenOver, r.early, r.abandoned);
        CHECK(r.bad == 0);
        if (k == 0) CHECK(r.helperSteps > 0);
        if (k == 1) CHECK(r.taken > 0 && r.gameSteps > 0);
        if (k == 2) CHECK(r.takenOver > 0 && r.gameSteps > 0);
        if (k == 3 || k == 4) CHECK(r.abandoned > 0);
        if (k == 6) CHECK(r.early > 0);
    }
    Scenario sv; sv.verify = true;
    Result v = Run(sv, turns, passes, true);
    printf("pipe27 verify: %llu pictures, %llu passes compared, %llu mismatches\n", (unsigned long long)v.vs.pictures, (unsigned long long)v.vs.compared, (unsigned long long)v.vs.mismatches);
    CHECK(v.vs.mismatches == 0 && v.vs.compared > 0);
    Scenario sa; sa.verify = true; sa.abandonAt = 2;
    Result va = Run(sa, turns, passes, true);
    printf("pipe27 verify with an abandon (picture dropped from the compare): %llu compared, %llu mismatches, bad streams %u\n", (unsigned long long)va.vs.compared, (unsigned long long)va.vs.mismatches, va.bad);
    CHECK(va.vs.mismatches == 0 && va.bad == 0);
    printf("pipe27 engine test: %s\n", gFail == 0 ? "PASS" : "FAIL");
    return gFail == 0 ? 0 : 1;
}
