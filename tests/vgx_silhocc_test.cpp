// across26: host test of GXV_SILH_OCC (platform/vgx/src/vgx_silhocc.h, vgx_silhocc_gl.h with a mock GL) and of the GXV_ACROSS table (platform/include/port/across.h).
// No GL, no game. Negative controls are marked NEG: each of them must make a check fail when the rule it tests is broken.
#define VGX_SILHOCC_MOCKGL 1
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); }
}

// ---- the mock GL: a query becomes available `latency` presents after it was ended, with the result the test planted for its begin ----
namespace mock {
struct Q { bool begun = false, ended = false; unsigned pictureOrder = 0, endedAtPresent = 0, result = 0; bool inUse = false; };
static std::vector<Q> qs(1);  // index 0 unused (0 = no query)
static int open = -1, presents = 0, latency = 2, begins = 0, ends = 0, begunWhileOpen = 0;
static unsigned plantedResult = 0;  // what the next begun query will count
static std::vector<unsigned> plantQueue;
}
void glGenQueries(int n, unsigned int* ids) { for (int i = 0; i < n; ++i) { mock::qs.push_back(mock::Q()); ids[i] = static_cast<unsigned>(mock::qs.size() - 1); } }
void glBeginQuery(unsigned int, unsigned int id) {
    if (mock::open >= 0) ++mock::begunWhileOpen;
    mock::open = static_cast<int>(id);
    mock::Q& q = mock::qs[id];
    q.begun = true; q.ended = false; ++mock::begins;
    q.result = mock::plantQueue.empty() ? mock::plantedResult : mock::plantQueue.front();
    if (!mock::plantQueue.empty()) mock::plantQueue.erase(mock::plantQueue.begin());
}
void glEndQuery(unsigned int) { mock::qs[mock::open].ended = true; mock::qs[mock::open].endedAtPresent = mock::presents; mock::open = -1; ++mock::ends; }
void glGetQueryObjectuiv(unsigned int id, unsigned int pname, unsigned int* p) {
    const mock::Q& q = mock::qs[id];
    if (pname == 0x8867) *p = (q.ended && mock::presents >= static_cast<int>(q.endedAtPresent) + mock::latency) ? 1u : 0u;
    else *p = q.result;
}

#include "../platform/vgx/src/vgx_silhocc_gl.h"
#include "../platform/include/port/across.h"

using namespace vgx;
using namespace vgx::silhocc;

static void TestParamsAndCheck() {
    History h;
    const Params p1 = ParamsFor(1), p2 = ParamsFor(2);
    Check(p1.hold == 12 && p1.needProof && p2.hold == 5 && !p2.needProof, "level parameters");
    Check(h.Check(100, p1) == kNoProof, "level 1 without a hit ever: no proof");
    Check(h.Check(100, p2) == kNoFlow, "level 2 without any result: not flowing (fail closed)");
    h.OnResult(10, 0, 1);  // a result of 0 samples, frame 10
    Check(h.Check(12, p2) == kAllowed, "level 2, a clear result 2 pictures ago: allowed");
    Check(h.Check(10 + 12, p2) == kAllowed && h.Check(10 + 13, p2) == kNoFlow, "results older than lagMax pictures: fail closed");
    Check(h.Check(12, p1) == kNoProof, "level 1 still wants a hit seen");
    h.OnResult(11, 5, 1);  // a hit at frame 11
    Check(h.hits.load() == 1 && h.Check(12, p1) == kHold, "a hit: blocked for the hold");
    Check(h.Check(11 + 12, p1) == kHold && h.Check(11 + 13, p1) == kNoFlow, "hold 12 ends exactly after 12 pictures (then the flow check)");
    h.OnResult(20, 0, 1);
    Check(h.Check(11 + 13, p1) == kAllowed, "level 1 after the hold, results flowing, proof seen: allowed");
    // minPx: 3 samples are not a hit at minPx 4
    History g;
    g.OnResult(5, 3, 4);
    Check(g.hits.load() == 0 && g.lastHit.load() == 0, "samples below minPx are no hit");
    g.OnResult(6, 4, 4);
    Check(g.hits.load() == 1 && g.lastHit.load() == 7, "samples at minPx are a hit");
    // out-of-order results: lastHit / lastSeen only ever move forward
    g.OnResult(3, 100, 1);
    Check(g.lastHit.load() == 7 && g.lastSeen.load() == 7, "an older result never moves the newest marks back");
    // NEG: a controller that forgets the flow check allows after silence
    History s;
    Check(s.Check(1000, p2) != kAllowed, "NEG: silence must not allow");
}

static void TestCensusMatrix() {
    History h;
    h.NoteFrame(50, 1, false);  // game: cannot show
    h.NoteGated(50);
    h.OnResult(50, 7, 1);       // but the query saw him
    h.NoteFrame(51, 2, false);  // may show
    h.OnResult(51, 9, 1);
    h.NoteFrame(52, 1, true);   // downgraded by the gate
    h.OnResult(52, 0, 1);
    h.OnResult(53, 0, 1);       // no note: unmatched
    Check(h.byHint[1][1].load() == 1 && h.byHint[2][1].load() == 1 && h.byHint[1][0].load() == 1, "matrix by raw hint and hit");
    Check(h.gatedResults.load() == 1 && h.gatedShowed.load() == 1, "a gated picture that showed him is counted as a miss");
    Check(h.unmatched.load() == 1, "a result with no note is unmatched");
    // a stale ring entry (same slot, another frame) must not be matched
    History k;
    k.NoteFrame(5, 1, false);
    k.OnResult(5 + kRing, 1, 1);
    Check(k.unmatched.load() == 1 && k.byHint[1][1].load() == 0, "ring slot of another frame is not matched");
    k.NoteFrame(7, 1, false);
    k.NoteGated(7 + kRing);  // NoteGated for a frame whose slot holds another tag does nothing
    k.OnResult(7, 0, 1);
    Check(k.gatedResults.load() == 0, "NoteGated of a frame that is not in its slot is ignored");
}

// ---- the whole loop: a simulated game with a result latency; counts the pictures where he was hidden and the silhouette was missing ----
struct Sim {
    History h;
    Params p;
    int latency;
    int misses = 0, gatedPics = 0, shown = 0;
    std::vector<std::pair<int, uint32_t>> inflight;  // (arrival present, frame) results to deliver
    std::vector<uint32_t> inflightSamples;
    explicit Sim(int lvl, int lat) : p(ParamsFor(lvl)), latency(lat) {}
    // hidden(f) says whether Mario's silhouette has pixels in picture f; rays(f): the game's hint is 2 (may show) when a collision occluder exists
    template <class Hidden, class Rays>
    void Run(int from, int to, Hidden hidden, Rays rays) {
        for (int f = from; f < to; ++f) {
            const int raw = rays(f) ? 2 : 1;
            bool gate = false;
            if (raw == 1) gate = h.Allows(f, p);
            h.NoteFrame(f, raw, raw == 1 && !gate);
            if (gate) h.NoteGated(f);
            if (gate) { ++gatedPics; if (hidden(f)) ++misses; } else ++shown;
            // the query of this picture is delivered `latency` pictures later
            inflight.push_back({f + latency, static_cast<uint32_t>(f)});
            inflightSamples.push_back(hidden(f) ? 25u : 0u);
            for (size_t i = 0; i < inflight.size();) {
                if (inflight[i].first <= f) { h.OnResult(inflight[i].second, inflightSamples[i], 1); inflight.erase(inflight.begin() + i); inflightSamples.erase(inflightSamples.begin() + i); }
                else ++i;
            }
        }
    }
};

static void TestSimulation() {
    // 1. open ground 0..199, then he walks behind a collision-free object for 300..339, then open again
    auto hid = [](int f) { return f >= 300 && f < 340; };
    auto noRays = [](int) { return false; };
    {
        Sim s(2, 2);
        s.Run(0, 500, hid, noRays);
        Check(s.gatedPics > 300, "level 2: most open-ground pictures are gated (the saving)");
        Check(s.misses == 3, "level 2, latency 2: the silhouette is missing for exactly latency+1 pictures at the onset");
        // after the hit the hold keeps it on: no gated picture between 303 and 339 + hold
        int gatedInside = 0;
        Sim t(2, 2);
        t.Run(0, 303, hid, noRays);
        const int g0 = t.gatedPics;
        t.Run(303, 340 + t.p.hold, hid, noRays);
        gatedInside = t.gatedPics - g0;
        Check(gatedInside == 0, "level 2: no picture gated while he is hidden and for the hold after");
        Check(t.h.gatedShowed.load() == 3, "the census counts the 3 misses");
    }
    {
        // level 1: nothing is gated until a hit was seen once (proof), then the same
        Sim s(1, 2);
        s.Run(0, 299, hid, noRays);
        Check(s.gatedPics == 0, "level 1: no hit seen yet, nothing gated (the counter is not trusted by silence)");
        s.Run(299, 600, hid, noRays);
        Check(s.gatedPics > 150, "level 1: after the hit was seen, gating resumes");
    }
    {
        // collision occluder: the game's rays say "may show" at once: no miss at all, whatever the latency
        auto rays = [](int f) { return f >= 300 && f < 340; };
        Sim s(2, 3);
        s.Run(0, 500, hid, rays);
        Check(s.misses == 0, "a collision occluder is covered instantly by the game's rays (no query delay)");
    }
    {
        // NEG: with a very slow query (latency 20 > lagMax) the gate fails closed everywhere
        Sim s(2, 20);
        s.Run(0, 400, hid, noRays);
        Check(s.gatedPics < 40, "NEG: results too late: the gate does not run on stale news");
    }
}

static DrawRec MakeDraw(uint8_t occ) { DrawRec d; d.occ = occ; d.idxCount = 6; return d; }

static void TestGlue() {
    mock::qs.assign(1, mock::Q());
    mock::open = -1; mock::presents = 0; mock::latency = 3; mock::begins = mock::ends = mock::begunWhileOpen = 0; mock::plantQueue.clear(); mock::plantedResult = 0;
    Glue g;
    g.Enable(true, 1);
    History h;
    CmdBuf buf;
    buf.frame = 10;
    // picture: normal draw, 3 flagged draws with a shader command between two of them, a normal draw, present
    buf.draws.push_back(MakeDraw(0));  // 0
    buf.draws.push_back(MakeDraw(1));  // 1
    buf.draws.push_back(MakeDraw(1));  // 2
    buf.draws.push_back(MakeDraw(1));  // 3
    buf.draws.push_back(MakeDraw(0));  // 4
    buf.cmds = {{CMD_DRAW, 0}, {CMD_DRAW, 1}, {CMD_SHADER, 0}, {CMD_DRAW, 2}, {CMD_PROGRAM, 0}, {CMD_DRAW, 3}, {CMD_DRAW, 4}, {CMD_PRESENT, 0}};
    mock::plantedResult = 42;
    for (const Cmd& c : buf.cmds) {
        g.Step(buf, c);
        if (c.type == CMD_PRESENT) { ++mock::presents; g.Present(h); }
    }
    Check(mock::begins == 1 && mock::ends == 1 && mock::begunWhileOpen == 0, "one query for the run of flagged draws, shader commands inside do not split it");
    Check(g.open < 0, "the normal draw after the run closed the query");
    Check(h.results.load() == 0, "not available yet: nothing published");
    // two presents later the result is there
    buf.frame = 11;
    buf.cmds = {{CMD_PRESENT, 0}};
    ++mock::presents; g.Step(buf, buf.cmds[0]); g.Present(h);
    Check(h.results.load() == 0, "one picture later: still not available");
    ++mock::presents; g.Step(buf, buf.cmds[0]); g.Present(h);
    Check(h.results.load() == 1 && h.samplesSum.load() == 42 && h.lastHit.load() == 11, "two pictures later: published with the picture it measured (frame 10)");
    // a run left open at the present is closed by it
    buf.frame = 12;
    buf.cmds = {{CMD_DRAW, 1}, {CMD_PRESENT, 0}};
    g.Step(buf, buf.cmds[0]);
    Check(g.open >= 0, "run open");
    g.Present(h);
    Check(g.open < 0 && g.forced == 1, "present closes an open run");
    // slot exhaustion: more pending queries than slots -> the extra runs are not measured, nothing breaks
    mock::latency = 1000;
    Glue g2;
    g2.Enable(true, 1);
    History h2;
    for (int f = 20; f < 40; ++f) {
        buf.frame = f;
        buf.cmds = {{CMD_DRAW, 1}, {CMD_DRAW, 0}, {CMD_PRESENT, 0}};
        for (const Cmd& c : buf.cmds) { g2.Step(buf, c); if (c.type == CMD_PRESENT) { ++mock::presents; g2.Present(h2); } }
    }
    Check(g2.began == static_cast<uint64_t>(Glue::kSlots) && g2.noSlot >= 14, "six slots, then no free slot: the run is skipped");
    Check(h2.results.load() == 0, "nothing available: nothing published, no wait");
    // off: Step does nothing, and Enable(false) closes an open run
    Glue g3;
    g3.Enable(true, 1);
    buf.frame = 50;
    g3.Step(buf, Cmd{CMD_DRAW, 1});
    Check(g3.open >= 0, "g3 open");
    g3.Enable(false, 1);
    Check(g3.open < 0, "switching off closes the run");
    // a flagged draw with no indices does not open a run
    DrawRec e = MakeDraw(1);
    e.idxCount = 0;
    buf.draws.push_back(e);
    Glue g4;
    g4.Enable(true, 1);
    g4.Step(buf, Cmd{CMD_DRAW, static_cast<uint32_t>(buf.draws.size() - 1)});
    Check(g4.open < 0, "empty flagged draw: no query");
}

static void TestAcross() {
    using namespace port::across;
    Check(kMemberCount == 7, "seven members (scenes26 added FIRSTCLEAR, CLEARQ, TAILDEPTH at level 2)");
    float v = -1;
    Check(!TableValue(0, "GXV_SILH_OCC", &v), "level 0 adds nothing");
    Check(TableValue(1, "GXV_SILH_OCC", &v) && v == 1 && TableValue(2, "GXV_SILH_OCC", &v) && v == 2, "OCC 1 / 2");
    Check(TableValue(1, "GXV_SILH_GATE", &v) && v == 1 && TableValue(2, "GXV_SILH_GATE", &v) && v == 1, "gate on at both levels (OCC makes it safe)");
    Check(TableValue(1, "GXV_SHADOW_STENCIL", &v) && v == 0 && TableValue(2, "GXV_SHADOW_STENCIL", &v) && v == 1, "stencil only at level 2");
    Check(!TableValue(3, "GXV_SILH_OCC", &v) && !TableValue(1, "GXV_NOPE", &v), "level 3 and unknown names are refused");
    Check(ClampLevel(-4) == 0 && ClampLevel(9) == 2 && ClampLevel(1) == 1, "clamp");
    // fan-out: a member the environment sets is left alone
    std::vector<std::string> wrote;
    int n = FanOut(2, [](const char* name) { return std::strcmp(name, "GXV_SILH_OCC") == 0; }, [&](const char* name, float) { wrote.push_back(name); });
    Check(n == 6 && wrote.size() == 6, "fan-out skips the member the environment set");
    Check(TableValue(2, "GXV_TAILDEPTH", &v) && v == 1 && TableValue(1, "GXV_TAILDEPTH", &v) && v == 0 && TableValue(2, "GXV_FIRSTCLEAR", &v) && v == 1, "scenes26 members: level 2 only");
    for (const std::string& w : wrote) Check(w != "GXV_SILH_OCC", "the set member was not written");
    int none = FanOut(0, [](const char*) { return false; }, [&](const char*, float) { wrote.push_back("x"); });
    Check(none == 0, "level 0 writes nothing");
    // expand (preset): the line names silh_gate itself -> kept
    std::vector<std::pair<std::string, float>> out;
    Expand(1, [](const char* name) { return std::strcmp(name, "GXV_SILH_GATE") == 0; }, [&](const char* name, float val) { out.push_back({name, val}); });
    bool hasGate = false;
    for (auto& o : out) if (o.first == "GXV_SILH_GATE") hasGate = true;
    Check(!hasGate && out.size() == 2, "preset expansion keeps a named member and writes no member that is 0 at the level");
}

int main() {
    TestParamsAndCheck();
    TestCensusMatrix();
    TestSimulation();
    TestGlue();
    TestAcross();
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
