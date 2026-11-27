// Host test of GXV_DIRECT_DISPLAY (direct24): the decision (Decide), the buffer rotation (Machine) and the census, against a fake world. No GL, no SDK.
// What the fake checks that the console would punish: a buffer is never the EFB's, a spare or the successor while it is in flight; the buffer the EFB goes
// on with was flipped at least two pictures before (a scene begun on a buffer whose release depends on an entry not queued yet waits for ever); every buffer
// is somewhere exactly once (EFB, spares, successor, flights).
#include "vgx_directdisplay.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vgx::dd;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Fake {
    unsigned nextId = 100;
    unsigned efb = 1;
    std::vector<unsigned> spares;
    std::map<unsigned, long> lastFlip;   // buffer -> picture of its last flip
    long picture = 0;
    long minGap = 1000000;               // smallest (picture now - picture of last flip) over every buffer that became the EFB
    bool noBuffers = false;              // TakeSuccessor finds none and cannot make one
    uint32_t failures = 0;
    int made = 0, released = 0, givenBack = 0, rotates = 0;
    std::set<unsigned> owned{1};
    unsigned TakeSuccessor() {
        if (!spares.empty()) { unsigned id = spares.back(); spares.pop_back(); return id; }
        if (noBuffers) return 0;
        ++made;
        owned.insert(nextId);
        return nextId++;
    }
    void GiveBack(unsigned id) { spares.push_back(id); ++givenBack; }
    void Rotate(unsigned oldEfb, unsigned next) {
        CHECK(oldEfb == efb);
        lastFlip[oldEfb] = picture;
        auto it = lastFlip.find(next);
        if (it != lastFlip.end()) minGap = std::min(minGap, picture - it->second);
        efb = next;
        ++rotates;
    }
    bool sizeChanged = false;
    int discarded = 0;
    bool Fits(unsigned) { return !sizeChanged; }
    void Discard(unsigned id) { owned.erase(id); ++discarded; }
    void Release(unsigned id) { spares.push_back(id); ++released; }
    uint32_t BeginFailures() { return failures; }
};

static Inputs Good() {
    Inputs i;
    i.mode = 1; i.api = true; i.x = 0; i.y = 0; i.w = 960; i.h = 544; i.rtW = 960; i.rtH = 544; i.screenW = 960; i.screenH = 544; i.dx0 = 0; i.dx1 = 960;
    i.clearWhole = true; i.clearEnable = true; i.displayable = true;
    return i;
}

static void TestDecide() {
    CHECK(Decide(Good()) == kDirect);
    { Inputs i = Good(); i.mode = 0; CHECK(Decide(i) == kSwitchOff); i.mode = 2; CHECK(Decide(i) == kSwitchOff); }
    { Inputs i = Good(); i.api = false; CHECK(Decide(i) == kNoApi); }
    { Inputs i = Good(); i.selfOff = true; CHECK(Decide(i) == kSelfOff); }
    { Inputs i = Good(); i.keepRt = true; CHECK(Decide(i) == kNotDisplayable); }
    { Inputs i = Good(); i.hostOk = false; CHECK(Decide(i) == kHost); }
    { Inputs i = Good(); i.hudEngaged = true; CHECK(Decide(i) == kHud); }
    { Inputs i = Good(); i.phaseZero = false; CHECK(Decide(i) == kHud); }
    { Inputs i = Good(); i.samples = 2; CHECK(Decide(i) == kMsaa); }
    { Inputs i = Good(); i.capture = true; CHECK(Decide(i) == kCapture); }
    { Inputs i = Good(); i.rtW = 784; i.rtH = 440; i.w = 784; i.h = 440; CHECK(Decide(i) == kSize); }   // (a render below the screen's: upscaled, not a copy)
    { Inputs i = Good(); i.x = 1; CHECK(Decide(i) == kSize); }
    { Inputs i = Good(); i.w = 958; CHECK(Decide(i) == kSize); }
    { Inputs i = Good(); i.dx0 = 8; i.dx1 = 952; CHECK(Decide(i) == kSize); }
    { Inputs i = Good(); i.sharpen = 0.05f; CHECK(Decide(i) == kSharpen); i.sharpen = 0.049f; CHECK(Decide(i) == kDirect); }
    { Inputs i = Good(); i.edgeAA = true; CHECK(Decide(i) == kSharpen); }
    { Inputs i = Good(); i.chara = true; CHECK(Decide(i) == kSharpen); }
    { Inputs i = Good(); i.clearWhole = false; CHECK(Decide(i) == kClear); }                     // (a partial clear)
    { Inputs i = Good(); i.clearEnable = false; i.clearWhole = false; i.planOn = false; CHECK(Decide(i) == kNoPlan); } // (no clear, nothing can prove the next start dead)
    { Inputs i = Good(); i.clearEnable = false; i.clearWhole = false; i.planOn = true; CHECK(Decide(i) == kDirect); }  // (the game's own pattern: no clear on the copy)
    { Inputs i = Good(); i.displayable = false; CHECK(Decide(i) == kNotDisplayable); }
    // the order: the first doubt names the reason (the census must be stable)
    { Inputs i = Good(); i.mode = 0; i.api = false; i.clearWhole = false; CHECK(Decide(i) == kSwitchOff); }
    std::printf("PASS decide matrix\n");
}

static void TestFlow() {
    Fake f;
    Machine<Fake> m(f, 2);
    Reason why = kHost;
    // picture 1: offered, flipped: the EFB goes on with a new buffer
    f.picture = 1;
    unsigned id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id == 1 && why == kDirect && m.Next() != 0);
    m.Done(true, kDirect);
    CHECK(f.efb != 1 && f.efb >= 100 && m.InFlight() == 1 && m.census.flips == 1 && m.census.direct == 1);
    // picture 2: refused by the host (vglSetNextFlip said no): same EFB, successor kept, reason counted
    f.picture = 2;
    const unsigned efb2 = f.efb;
    id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id == efb2);
    const unsigned succ = m.Next();
    m.Done(false, kRefused);
    CHECK(f.efb == efb2 && m.Next() == succ && m.census.reason[kRefused] == 1 && m.census.flips == 1);
    // picture 3: not eligible (a sharpened present): nothing flips, nothing moves, flight 1 (picture 1's) is released now (2 pictures old)
    f.picture = 3;
    Inputs bad = Good(); bad.sharpen = 0.5f;
    id = m.Offer(bad, f.efb, true, &why);
    CHECK(id == 0 && why == kSharpen);
    m.Done(false, kHost);
    CHECK(m.census.reason[kSharpen] == 1 && f.released == 1 && m.InFlight() == 0);
    // picture 4: the successor is reused, no new buffer
    const int made = f.made;
    f.picture = 4;
    id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id != 0 && f.made == made);
    m.Done(true, kDirect);
    CHECK(m.census.flips == 2 && m.census.pictures == 4);
    std::printf("PASS flow, refusal, fallback, reuse\n");
}

static void TestNoSuccessor() {
    Fake f;
    f.noBuffers = true;
    Machine<Fake> m(f, 2);
    Reason why = kDirect;
    f.picture = 1;
    unsigned id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id == 0 && why == kNoSuccessor);
    m.Done(false, kHost);
    CHECK(m.census.reason[kNoSuccessor] == 1 && f.efb == 1 && f.rotates == 0);
    // a buffer appears: it works afterwards
    f.noBuffers = false;
    f.picture = 2;
    id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id == 1 && why == kDirect);
    m.Done(true, kDirect);
    CHECK(f.efb != 1 && f.rotates == 1);
    std::printf("PASS no successor falls back\n");
}

static void TestSizeChange() {
    Fake f;
    Machine<Fake> m(f, 2);
    Reason why = kHost;
    f.picture = 1;
    m.Offer(Good(), f.efb, true, &why);
    m.Done(false, kRefused); // (a successor is prepared and kept)
    const unsigned old = m.Next();
    CHECK(old != 0);
    f.sizeChanged = true;    // the render size changed: the prepared buffer has the wrong size
    f.picture = 2;
    unsigned id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id != 0 && f.discarded == 1 && m.Next() != 0 && m.Next() != old);
    m.Done(true, kDirect);
    CHECK(f.efb != 1 && f.efb != old);
    std::printf("PASS a successor of the wrong size is let go, never used\n");
}

static void TestSelfOff() {
    Fake f;
    Machine<Fake> m(f, 2);
    Reason why = kHost;
    f.picture = 1;
    m.Offer(Good(), f.efb, true, &why);
    m.Done(true, kDirect);
    f.picture = 2;
    m.Offer(Good(), f.efb, true, &why); // (successor prepared)
    CHECK(m.Next() != 0);
    m.Done(false, kRefused);
    f.failures = 1; // a scene failed to begin
    f.picture = 3;
    unsigned id = m.Offer(Good(), f.efb, true, &why);
    CHECK(id == 0 && why == kSelfOff && m.selfOff && m.Next() == 0 && f.givenBack == 1);
    m.Done(false, kHost);
    for (int p = 4; p < 20; ++p) { f.picture = p; id = m.Offer(Good(), f.efb, true, &why); CHECK(id == 0); m.Done(false, kHost); }
    CHECK(m.census.flips == 1 && m.census.reason[kSelfOff] == 17);
    // the failure counter's value at the start does not count (a console that had failures before the mode went on)
    Fake g; g.failures = 5;
    Machine<Fake> n(g, 2);
    g.picture = 1;
    id = n.Offer(Good(), g.efb, true, &why);
    CHECK(id != 0 && !n.selfOff);
    std::printf("PASS self-disable on a scene begin failure\n");
}

static void TestRandom() {
    Fake f;
    Machine<Fake> m(f, 2);
    unsigned seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 16) & 0x7fff; };
    uint64_t flips = 0;
    std::set<unsigned> everywhere;
    for (int p = 1; p <= 20000; ++p) {
        f.picture = p;
        Inputs in = Good();
        const unsigned r = rnd() % 10;
        if (r == 0) in.sharpen = 0.5f;
        if (r == 1) in.clearWhole = false;
        if (r == 2) in.samples = 2;
        f.noBuffers = rnd() % 50 == 0;
        Reason why = kHost;
        const unsigned efbBefore = f.efb;
        unsigned id = m.Offer(in, f.efb, true, &why);
        if (id) {
            CHECK(id == efbBefore);
            const bool flip = rnd() % 8 != 0; // (the host refuses some)
            m.Done(flip, flip ? kDirect : kRefused);
            if (flip) ++flips;
        } else {
            m.Done(false, kHost);
        }
        // every buffer exactly once: EFB, spares, successor, flights
        std::multiset<unsigned> all;
        all.insert(f.efb);
        for (unsigned s : f.spares) all.insert(s);
        if (m.Next()) all.insert(m.Next());
        for (auto& fl : m.Flights()) all.insert(fl.first);
        CHECK(all.size() == f.owned.size());
        for (unsigned b : f.owned) CHECK(all.count(b) == 1);
        // a buffer in flight is not the EFB's
        for (auto& fl : m.Flights()) CHECK(fl.first != f.efb);
        if (g_fail) break;
    }
    CHECK(f.minGap >= 2);
    CHECK(m.census.flips == flips);
    CHECK(f.owned.size() <= 6); // (the EFB, two in flight, the successor, a spare or two: no growth)
    std::printf("PASS random 20000 pictures: %llu flips, %zu buffers made, smallest gap between a flip and the buffer's next use as the EFB: %ld pictures\n",
                static_cast<unsigned long long>(flips), f.owned.size(), f.minGap);
}

// r3: a game whose copies do not clear: the successor is owed the colour; pictures whose next start needs it back wait after four in a row.
static void TestNoClearBackoff() {
    Fake f;
    Machine<Fake> m(f);
    Inputs in = Good();
    in.clearEnable = false; in.clearWhole = false; in.planOn = true;
    Reason why;
    int flips = 0, live = 0;
    for (int pic = 0; pic < 40; ++pic) {
        unsigned id = m.Offer(in, f.efb, true, &why);
        if (id) { m.Done(true, kDirect); ++flips; m.Settled(true); } else { m.Done(false, why); live += why == kLive; }
    }
    CHECK(flips == 4 && live == 36);                    // 4 flips with a live start each, then the back-off (120 pictures) holds the rest
    CHECK(m.census.reason[kLive] == 36 && m.census.startsRestored == 4);
    // a game whose starts are dead never backs off
    Fake g;
    Machine<Fake> m2(g);
    flips = 0;
    for (int pic = 0; pic < 40; ++pic) {
        unsigned id = m2.Offer(in, g.efb, true, &why);
        if (id) { m2.Done(true, kDirect); ++flips; m2.Settled(false); } else m2.Done(false, why);
    }
    CHECK(flips == 40 && m2.census.startsDead == 40 && m2.census.reason[kLive] == 0);
    std::printf("PASS no-clear copies: dead starts flip every picture, live starts back off\n");
}

static void TestCensus() {
    Census c;
    char line[512];
    CHECK(c.Line(line, sizeof line, 600, 1, false, 0) == 0);
    for (int i = 0; i < 600; ++i) c.Note(i % 3 == 0 ? kDirect : (i % 3 == 1 ? kSharpen : kClear));
    c.flips = 200;
    const int n = c.Line(line, sizeof line, 600, 1, false, 7);
    CHECK(n > 0);
    const std::string s = line;
    std::printf("%s\n", line);
    CHECK(s.find("direct-display:") == 0 && s.find("600 pictures, 200 shown directly") != std::string::npos && s.find("sharpen 200") != std::string::npos &&
          s.find("clear 200") != std::string::npos && s.find("vitaGL refused 7") != std::string::npos);
    CHECK(c.Line(line, sizeof line, 600, 1, false, 0) == 0);              // (not due again)
    CHECK(c.Line(line, sizeof line, 0, 1, false, 0) == 0);                // (every=0: never)
    for (int i = 0; i < 600; ++i) c.Note(kDirect);
    CHECK(c.Line(line, sizeof line, 600, 1, true, 0) > 0 && std::string(line).find("SELF-DISABLED") != std::string::npos && std::string(line).find("none") == std::string::npos);
    {   // the top fallback's state is named
        Census d;
        Inputs st = Good();
        st.clearEnable = false; st.clearWhole = false; st.haveCopy = true; st.clearColor = 1; st.clearAlpha = 1;
        for (int i = 0; i < 600; ++i) d.Note(kClear, &st);
        char l2[600];
        CHECK(d.Line(l2, sizeof l2, 600, 1, false, 0) > 0);
        const std::string t = l2;
        std::printf("%s\n", l2);
        CHECK(t.find("clear 600") != std::string::npos && t.find("top: clear [copy clear enable 0 color 1 alpha 1") != std::string::npos);
    }
    std::printf("PASS census line\n");
}

static void TestHrwGuard() {   // sg12: never under GXV_HRW != 0
    CHECK(EffectiveMode(1, 0) == 1);            // 60 Assist off (GXV_HRW=0): on
    CHECK(EffectiveMode(1, 5) == 0);            // 60 Assist on: off
    CHECK(EffectiveMode(1, 1) == 0 && EffectiveMode(1, -1) == 0);   // any non-zero GXV_HRW
    CHECK(EffectiveMode(1, 5, 1) == 1);         // the diagnostic lift
    CHECK(EffectiveMode(1, 5, 2) == 0 && EffectiveMode(1, 5, 0) == 0);
    CHECK(EffectiveMode(0, 0) == 0 && EffectiveMode(0, 5, 1) == 0);
    CHECK(EffectiveMode(2, 5) == 2 && EffectiveMode(2, 0) == 2);    // the probe mode never flips: untouched
    std::printf("PASS hrw guard\n");
}

// dd24 GXV_DD_DYN: the guard is lifted for the run, every picture decides. A game-thread model asks for pairs at random; the pair opens only after the machine reports drained
// (held, refused, nothing in flight: what DirectDisplayOffer publishes); the pair lasts a few pictures; the direct display resumes kHrwLead presents after its last picture.
static void TestDyn() {
    CHECK(EffectiveMode(1, 5, 0, 1) == 1 && EffectiveMode(1, 0, 0, 1) == 1 && EffectiveMode(0, 5, 0, 1) == 0 && EffectiveMode(1, 5, 0, 0) == 0);
    CHECK(HrwHold(true, false, 100, 0) && HrwHold(false, true, 100, 0) && !HrwHold(false, false, 100, 0));
    CHECK(HrwHold(false, false, 103, 100) && !HrwHold(false, false, 104, 100));            // kHrwLead = 4 presents after the last pair picture
    Inputs h = Good(); h.hrwHold = true;
    CHECK(Decide(h) == kHrw && std::string(ReasonName(kHrw)) == "hrw");
    h.hostOk = false;
    CHECK(Decide(h) == kHost);                                                              // (the host's own refusal first)
    Fake f;
    Machine<Fake> m(f);
    unsigned long long seed = 12345;
    auto rnd = [&]() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<unsigned>(seed >> 33); };
    bool gameWant = false, pairOpen = false;
    int pairLeft = 0, episodes = 0, directWithinLead = 0, pairWithFlight = 0, flipDuringPair = 0, waits = 0;
    uint64_t presents = 0, lastPair = 0, lastFlip = 0;
    int openedTooSoon = 0;
    bool quiet = false;
    for (int pic = 0; pic < 6000; ++pic) {
        ++f.picture;
        // the game thread's side, once a picture
        if (!gameWant && !pairOpen && rnd() % 90 == 0) { gameWant = true; ++episodes; }
        if (gameWant && !pairOpen) { if (quiet) { if (presents < lastFlip + kHrwLead) ++openedTooSoon; pairOpen = true; pairLeft = 3 + static_cast<int>(rnd() % 12); gameWant = false; } else ++waits; }
        const bool gameHold = gameWant || pairOpen;
        ++presents;
        const bool pairPic = pairOpen;
        if (pairPic) lastPair = presents;
        Inputs in = Good();
        in.hrwHold = HrwHold(gameHold, pairPic, presents, lastPair);
        Reason why = kDirect;
        const unsigned r = m.Offer(in, f.efb, true, &why);
        if (r != 0) lastFlip = presents;                                                    // (fix24: DirectDisplayOffer's g_lastFlip)
        quiet = in.hrwHold && r == 0 && m.InFlight() == 0 && presents >= lastFlip + kHrwLead;
        if (pairPic) { if (m.InFlight() != 0) ++pairWithFlight; if (r != 0) ++flipDuringPair; }
        if (r != 0 && lastPair != 0 && presents < lastPair + kHrwLead) ++directWithinLead;
        m.Done(r != 0, why);
        if (pairOpen && --pairLeft <= 0) pairOpen = false;
    }
    CHECK(episodes > 20 && openedTooSoon == 0 && pairWithFlight == 0 && flipDuringPair == 0 && directWithinLead == 0);
    CHECK(f.minGap >= 2);                                                                   // the buffer the EFB goes on with was flipped >= 2 pictures before, as without pairs
    CHECK(m.census.direct > m.census.pictures / 2 && m.census.reason[kHrw] > 0 && m.census.switches >= static_cast<uint64_t>(episodes));
    char line[600];
    m.census.lastLog = 0;
    CHECK(m.census.Line(line, sizeof line, 1, 1, false, 0, true) > 0 && std::string(line).find("DD_DYN:") != std::string::npos);
    std::printf("%s\n", line);
    std::printf("PASS dyn: %d pair episodes, %d waits for the drain, %.1f %% direct, %llu switches, no pair picture with a flight, no flip within the lead\n", episodes, waits,
                100.0 * static_cast<double>(m.census.direct) / static_cast<double>(m.census.pictures), static_cast<unsigned long long>(m.census.switches));
}


// ddfix25 (night60): the production world's DynDeny policy against a fake that counts live display textures (vitaGL takes 16: VGL_DD_MAX) and free video memory. Refusals mid-run and repeated mode
// switches: the machine never goes off, the live count never passes the cap, a refusal is the old present at once (no wait), and after the cool-down the direct display resumes.
struct CapFake : Fake {
    unsigned live = 1;               // display textures alive (the EFB's included)
    uint64_t freeVram = 40ull << 20;
    uint64_t presents = 0, cool = 0;
    int denied = 0, refusedHard = 0, hardRefuseAt = -1, maxLive = 1;
    bool Deny() {                    // directd::DynDeny
        if (presents < cool) { ++denied; return true; }                                              // (inside the cool-down: no new look, as DynDeny)
        if (DynMakeDenied(live, freeVram, presents, cool)) { cool = presents + kDynCooldown; ++denied; return true; }
        return false;
    }
    bool letGo = false;              // the spares are full: a buffer back from the display is deleted (the production Release)
    void Release(unsigned id) { if (letGo) { --live; owned.erase(id); ++released; } else Fake::Release(id); }
    unsigned TakeSuccessor() {
        if (!spares.empty()) { unsigned id = spares.back(); spares.pop_back(); return id; }
        if (Deny()) return 0;
        if (hardRefuseAt >= 0 && static_cast<long>(presents) >= hardRefuseAt) { hardRefuseAt = -1; ++refusedHard; cool = presents + 5 * kDynCooldown; return 0; } // (vitaGL refused: ridden out)
        ++live; maxLive = std::max<int>(maxLive, static_cast<int>(live));
        ++made;
        owned.insert(nextId);
        return nextId++;
    }
};
static void TestDynCapAndRefusal() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        CapFake f;
        Machine<CapFake> m(f);
        std::vector<unsigned> held;           // display buffers a texture copy (EfbSwap) holds for good: the leak of the night60 run
        unsigned long long seed = 777 + static_cast<unsigned>(scenario);
        (void)seed;
        if (scenario == 2) f.hardRefuseAt = 500;              // a refusal by vitaGL in the middle of the run
        int directAfterDeny = 0, tookWhileOld = 0, switches = 0;
        bool hold = false;
        for (int pic = 0; pic < 6000; ++pic) {
            ++f.picture; ++f.presents;
            if (scenario == 1 && pic % 400 == 0) hold = !hold, ++switches;  // repeated mode switches (a pair held / released)
            if (scenario == 1 && pic % 25 == 0) hold = (pic / 25) % 3 == 0;
            // a texture copy takes the EFB's buffer for good and the EFB takes a spare or a new one (EfbSwap): modelled as an extra buffer made while allowed
            if (scenario != 0 && pic > 300 && pic < 2000 && pic % (scenario == 2 ? 2 : 7) == 0 && held.size() < 30) {
                if (!f.spares.empty()) { f.spares.pop_back(); held.push_back(1); }   // (a spare is taken by the texture)
                else if (!f.Deny()) { ++f.live; f.maxLive = std::max<int>(f.maxLive, static_cast<int>(f.live)); held.push_back(1); }
            }
            if (scenario == 2) {
                f.letGo = (pic >= 600 && pic < 700) || (pic >= 2500 && pic < 2600);
                if (pic == 600 || pic == 2500) { f.live -= static_cast<unsigned>(f.spares.size()); f.spares.clear(); } // (no spare: a new texture is needed)
            }
            if (scenario == 2 && pic == 2500) f.freeVram = 8ull << 20;      // video memory runs low
            if (pic == 2200) { for (size_t k = 0; k < held.size(); ++k) --f.live; held.clear(); } // the held textures are let go: the direct display must come back
            if (scenario == 2 && pic == 3500) f.freeVram = 40ull << 20;
            Inputs in = Good();
            in.hrwHold = hold;
            Reason why = kDirect;
            const unsigned r = m.Offer(in, f.efb, true, &why);
            if (hold) CHECK(r == 0);
            if (!hold && r == 0 && why == kNoSuccessor) ++tookWhileOld;  // the old present, at once
            if (r != 0 && f.denied) ++directAfterDeny;
            m.Done(r != 0, why);
            CHECK(!m.selfOff);                                           // no refusal, count or memory shortage switches the machine off
            CHECK(f.live <= kDynMaxTextures + 1);                        // the cap holds (one more only by the EFB made before the policy ran)
        }
        CHECK(f.maxLive <= static_cast<int>(kDynMaxTextures) + 1);
        CHECK(m.census.direct > m.census.pictures / 4);
        if (scenario != 0) CHECK(f.denied > 0 && tookWhileOld > 0 && directAfterDeny > 0);   // denied, shown the old way, and the direct display came back
        if (scenario == 2) CHECK(f.refusedHard == 1 && directAfterDeny > 0);
        // the flights always came back: nothing in flight forever, every buffer owned once
        CHECK(m.InFlight() <= 4);
        std::printf("PASS dyn cap scenario %d: max live %d, denied %d, hard refusals %d, %.1f %% direct, switches %d\n", scenario, f.maxLive, f.denied, f.refusedHard,
                    100.0 * static_cast<double>(m.census.direct) / static_cast<double>(m.census.pictures), switches);
    }
    // the pure rule
    CHECK(!DynMakeDenied(5, 40ull << 20, 100, 0) && DynMakeDenied(12, 40ull << 20, 100, 0) && DynMakeDenied(5, 15ull << 20, 100, 0) && DynMakeDenied(5, 40ull << 20, 100, 101) && !DynMakeDenied(5, 40ull << 20, 101, 101));
    // the verify line carries the new figures
    Census c; c.pictures = 600; c.direct = 500; c.dynDenied = 7; c.dynRefused = 1; c.dynReturned = 3; c.dynHeld = 2;
    char line[700];
    CHECK(c.Line(line, sizeof line, 600, 1, false, 0, true) > 0 && std::string(line).find("no new display texture 7") != std::string::npos && std::string(line).find("refusals ridden out 1") != std::string::npos);
    CHECK(std::string(line).find("returned to the spares (not adopted) 3, held by plain textures now 2") != std::string::npos);   // (pace25)
}

int main() {
    TestHrwGuard();
    TestDyn();
    TestDynCapAndRefusal();
    TestDecide();
    TestFlow();
    TestNoSuccessor();
    TestSizeChange();
    TestSelfOff();
    TestRandom();
    TestNoClearBackoff();
    TestCensus();
    if (g_fail) { std::printf("direct-display: %d FAILED\n", g_fail); return 1; }
    std::printf("direct-display: all host tests passed\n");
    return 0;
}
