// openstall26: host test of GXV_SMOOTHSTART's pure parts (platform/include/port/smoothstart.h) and of the stage tally's SelectTop (GXV_STAGEBUILD_FAST) against the plain loop.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "port/smoothstart.h"
#include "../platform/vgx/src/vgx_stage_tally.h"
#include "../platform/vgx/src/vgx_texwarm.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---- 1. the umbrella ---------------------------------------------------------------------------------------------------------------------------
static void TestUmbrella() {
    using namespace port::smoothstart;
    CHECK(ClampLevel(-3) == 0 && ClampLevel(0) == 0 && ClampLevel(1) == 1 && ClampLevel(2) == 2 && ClampLevel(9) == 2, "clamp");
    std::unordered_map<std::string, std::string> out;
    int n = FanOut(0, [](const char*) { return false; }, [&](const char* k, const char* v) { out[k] = v; });
    CHECK(n == 0 && out.empty(), "level 0 writes nothing");
    n = FanOut(1, [](const char*) { return false; }, [&](const char* k, const char* v) { out[k] = v; });
    CHECK(n == kMemberCount && out["GXV_PREWARM_RES"] == "2" && out["GXV_TEXWARM"] == "1" && out["GXV_ENDWAIT_LOG"] == "1" && out["GXV_SMOOTHSTART_MS"] == "400", "level 1");
    out.clear();
    n = FanOut(2, [](const char* k) { return std::strcmp(k, "GXV_TEXWARM") == 0; }, [&](const char* k, const char* v) { out[k] = v; });
    CHECK(n == kMemberCount - 1 && !out.count("GXV_TEXWARM") && out["GXV_SMOOTHSTART_MS"] == "600", "level 2 leaves a member the environment set");
    // the closed-time cap of level 1 is the owner's 400 ms
    for (int i = 0; i < kMemberCount; ++i)
        if (!std::strcmp(kMembers[i].name, "GXV_SMOOTHSTART_MS")) CHECK(std::atoi(kMembers[i].v[1]) <= 400, "level 1 cap");
}
static void TestGovernor() {
    using namespace port::smoothstart;
    Governor g;
    g.Reset(400.0);
    CHECK(!g.Exhausted() && g.msPerKiB == kPriorMsPerKiB, "fresh");
    CHECK(g.KiBLeft() == static_cast<uint32_t>(400.0 / kPriorMsPerKiB), "prior allows %u KiB", g.KiBLeft());
    // no observation until two looks; slow back end (c80 tours): 0.23 ms/KiB
    g.Observe(1000, 100.0);
    g.Observe(1000 + 63, 100.0 + 14.0);
    CHECK(g.msPerKiB == kPriorMsPerKiB, "under 64 KiB: no change");
    g.Observe(1000 + 600, 100.0 + 138.0);
    CHECK(g.msPerKiB > 0.2 && g.msPerKiB < 0.26, "follows 0.23: %.3f", g.msPerKiB);
    // fast back end (c79: 0.015)
    Governor f;
    f.Reset(400.0);
    f.Observe(0, 0.0);
    for (int i = 1; i <= 12; ++i) f.Observe(static_cast<uint64_t>(i) * 1000, i * 15.0);
    CHECK(f.msPerKiB < 0.03 && f.msPerKiB >= 0.005, "fast follows 0.015: %.4f", f.msPerKiB);
    CHECK(f.KiBLeft() == 4096, "clamped at 4096 KiB");
    // a counter that went back (a new session / reset) is not a negative rate
    f.Observe(10, 1.0);
    CHECK(f.msPerKiB >= 0.005, "reset counters");
    // spending runs the cap out; one item may always go while not exhausted
    Governor s;
    s.Reset(100.0);
    s.msPerKiB = 0.25;
    CHECK(s.KiBLeft() == 400, "100 ms at 0.25 = 400 KiB: %u", s.KiBLeft());
    s.Spend(300, 5.0);  // 75 + 5
    CHECK(!s.Exhausted() && s.KiBLeft() == 80, "left %u", s.KiBLeft());
    s.Spend(100, 0.0);  // +25 -> 105
    CHECK(s.Exhausted() && s.KiBLeft() == 0, "exhausted");
    Governor t;
    t.Reset(1.0);
    t.msPerKiB = 1.0;
    t.Spend(0, 0.5);
    CHECK(t.KiBLeft() == 1, "a sliver left still lets one item go");
    Governor z;
    z.Reset(0.0);
    CHECK(z.Exhausted() && z.KiBLeft() == 0, "cap 0 = nothing");
    Governor off;
    off.Reset(1e12);
    CHECK(!off.Exhausted() && off.KiBLeft() == 4096, "off = unlimited");
}
static void TestHold() {
    using namespace port::smoothstart;
    CHECK(HoldWanted(3, 0.0, 400.0, 0.0), "items left, fresh");
    CHECK(!HoldWanted(0, 0.0, 400.0, 0.0), "nothing left");
    CHECK(!HoldWanted(3, 400.0, 400.0, 0.0), "cap reached");
    CHECK(HoldWanted(3, 399.0, 400.0, 199.0), "just inside");
    CHECK(!HoldWanted(3, 100.0, 400.0, kStallMs), "stalled");
    CHECK(!HoldWanted(3, 0.0, 0.0, 0.0), "cap 0");
    // a simulated hold: the replay does 2 items every picture, the cap is never passed, the total time is bounded by the cap
    unsigned left = 37;
    double held = 0, moved = 0;
    int pics = 0;
    while (HoldWanted(left, held, 400.0, held - moved)) { held += 16.7; left -= std::min(left, 2u); moved = held; ++pics; }
    CHECK(left == 0 && held < 400.0 && pics == 19, "hold ends with the plan: %d pictures, %.0f ms", pics, held);
    // a plan that never moves ends by the stall rule, not the cap
    left = 5; held = 0; moved = 0;
    while (HoldWanted(left, held, 400.0, held - moved)) held += 16.7;
    CHECK(held >= kStallMs && held < kStallMs + 17.0, "stall ends it at %.0f ms", held);
}
static void TestFormat() {
    using namespace port::smoothstart;
    Census c;
    c.stage = "PhantomGalaxy";
    c.level = 1;
    c.capMs = 400;
    c.closedMs = 1007;
    c.closedPics = 8;
    c.holdMs = 213;
    c.holdPlan = 41;
    c.holdEnd = "plan done";
    c.texClosed = 38; c.texClosedKiB = 1011; c.texClosedMs = 22; c.texPlanned = 41; c.texMiss = 3; c.texGaveUp = 0;
    c.tgtMade = 2; c.tgtMs = 74;
    c.texOpenReplay = 0; c.texFirst = 9; c.texFirstKiB = 200; c.texFirstMs = 4; c.tgtTaken = 2; c.tgtPlay = 0;
    c.worstMs = 61; c.worstAtS = 3.5; c.worstCause = "texture uploads"; c.msPerKiB = 0.23;
    char line[900];
    const size_t n = Format(line, sizeof line, c);
    CHECK(n == std::strlen(line) && n > 200 && n < 899, "length %zu", n);
    CHECK(std::strstr(line, "smoothstart: stage PhantomGalaxy: GXV_SMOOTHSTART=1: closed screen 1007 ms, 8 pictures (the hold added 213 ms of at most 400; 41 plan items left at its start; ended: plan done)"), "head: %s", line);
    CHECK(std::strstr(line, "created closed: textures 38 (1011 KiB, 22 ms front end; plan 41, missed 3, given up 0), targets 2 (74 ms)"), "closed: %s", line);
    CHECK(std::strstr(line, "worst open-screen picture 61 ms at +3.5 s (texture uploads)"), "worst: %s", line);
    char tiny[40];
    const size_t m = Format(tiny, sizeof tiny, c);
    CHECK(m < sizeof tiny && std::strlen(tiny) == m, "tiny buffer");
    CHECK(std::strstr(line, "%") == nullptr, "no unexpanded format: %s", line);
}

// the replay's Due(): what the hold waits for
static void TestDue() {
    using namespace vgx::texwarm;
    std::vector<Item> items(5);
    for (unsigned i = 0; i < items.size(); ++i) { items[i].addr = 0x1000 + i * 0x100; items[i].size = 4096; items[i].w = items[i].h = 32; items[i].hash = 1 + i; }
    Replay r;
    r.Begin(&items);
    CHECK(r.Due(1) == 5 && r.Left() == 5, "all due at the start");
    size_t idx = 0;
    uint64_t pic = 10;
    while (r.Next(pic, idx)) r.Result(idx, idx < 2 ? kHit : kMissMemory, pic);  // two made, three missed (the memory is not there)
    CHECK(r.Left() == 3 && r.Due(pic + 1) == 0, "misses wait kRetryGap pictures: nothing to hold for (%zu)", r.Due(pic + 1));
    CHECK(r.Due(pic + kRetryGap) == 3, "due again after the gap");
    for (unsigned t = 0; t < kMaxMisses; ++t) { pic += kRetryGap; while (r.Next(pic, idx)) r.Result(idx, kMissMemory, pic); }
    CHECK(r.Left() == 0 && r.GaveUp() == 3 && r.Due(pic + 1000) == 0, "given up after %u misses", kMaxMisses);
}

// ---- 2. SelectTop against the plain loop ------------------------------------------------------------------------------------------------------
struct Rec { uint64_t vs, fs; uint32_t lb; };
struct Set { std::vector<Rec> recs; };
using Sets = std::unordered_map<uint32_t, Set>;
using Ids = std::unordered_map<uint64_t, uint16_t>;
static void TestSelectTop() {
    using namespace vgx::stagetally;
    std::mt19937 rng(2610);
    const uint32_t kPlayer = 0x504C5952u, kMenu = 0x4D454E55u;
    unsigned compared = 0, nonEmpty = 0, truncated = 0, mergedFromTarget = 0;
    for (int round = 0; round < 120; ++round) {
        Ids vs, fs;
        const unsigned nv = 12 + rng() % 70, nf = 20 + rng() % 130;
        for (unsigned i = 0; i < nv; ++i) vs[0x1000 + i] = static_cast<uint16_t>(i);
        for (unsigned i = 0; i < nf; ++i) fs[0x2000 + i] = static_cast<uint16_t>(i);
        Sets sets;
        const unsigned nstages = 3 + rng() % 16;
        std::vector<uint32_t> keys;
        size_t recs = 0;
        for (unsigned s = 0; s < nstages; ++s) {
            const uint32_t key = s == 0 ? kMenu : s == 1 ? kPlayer : 0x10000000u + rng() % 100000;
            Set& set = sets[key];
            const unsigned nr = rng() % 400;
            for (unsigned r = 0; r < nr; ++r)
                set.recs.push_back(Rec{0x1000 + (rng() % (nv + 3)), 0x2000 + (rng() % (nf + 3)), static_cast<uint32_t>(rng() % 50000)});
            recs += nr;
            keys.push_back(key);
        }
        const unsigned minN = 1 + rng() % 4;
        std::unordered_set<uint32_t> held;
        for (int q = 0; q < 1 + static_cast<int>(rng() % 40); ++q) held.insert((static_cast<uint32_t>(rng() % nv) << 16) | (rng() % nf));
        const auto skip = [&](uint32_t k) { return held.count(k) != 0; };
        Cache cache;
        // half of the rounds the tally is made in slices (and its order prepared in the background), half whole at the first call
        if (round & 1) {
            double clk = 0;
            cache.BeginRefresh(sets, recs, 7u, kPlayer);
            while (!cache.StepRefresh(sets, vs, fs, 3.0, [&] { return clk += 1.0; })) {}
            cache.PrepareOrder(minN);
            CHECK(cache.OrderReady(minN), "prepared");
        }
        for (const uint32_t target : keys) {
            if (target == kPlayer || target == kMenu) continue;
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, target, kPlayer, vs, fs, ref);
            PlainOrder(ref, minN, skip, refOrder);
            const size_t maxOut = (rng() % 3 == 0) ? 1000000 : 1 + rng() % 200;
            bool rebuilt = false;
            cache.SelectTop(sets, recs, 7u, target, kPlayer, vs, fs, minN, skip, maxOut, got, gotOrder, &rebuilt);
            Order want = refOrder;
            if (want.size() > maxOut) { want.resize(maxOut); ++truncated; }
            CHECK(gotOrder == want, "round %d target %08x: order differs (%zu vs %zu entries)", round, target, gotOrder.size(), want.size());
            bool same = gotOrder == want;
            for (size_t i = 0; same && i < gotOrder.size(); ++i) {
                const Cnt& a = got.at(gotOrder[i].second);
                const Cnt& b = ref.at(want[i].second);
                same = a.n == b.n && a.lb == b.lb && a.vs == b.vs && a.fs == b.fs;
            }
            CHECK(same, "round %d target %08x: counts / layout words differ", round, target);
            // the full Select agrees too (it is what runs without the switch)
            CntMap got2;
            Order order2;
            cache.Select(sets, recs, 7u, target, kPlayer, vs, fs, minN, skip, got2, order2);
            CHECK(order2 == refOrder, "round %d target %08x: Select unchanged", round, target);
            // how often the target's own (lowered) keys were in the answer: the merge is exercised
            const auto ti = sets.find(target);
            if (ti != sets.end()) {
                std::unordered_set<uint32_t> mine;
                for (const Rec& r : ti->second.recs) { const auto v = vs.find(r.vs), f = fs.find(r.fs); if (v != vs.end() && f != fs.end()) mine.insert((uint32_t(v->second) << 16) | f->second); }
                for (const auto& o : gotOrder) if (mine.count(o.second)) { ++mergedFromTarget; break; }
            }
            ++compared;
            nonEmpty += !gotOrder.empty();
        }
        // the sets grow (records arrive in play): the next call makes the whole tally and the sorted list again, and still agrees
        const uint32_t k = keys[rng() % keys.size()];
        for (int a = 0; a < 9; ++a) { sets[k].recs.push_back(Rec{0x1000 + (rng() % nv), 0x2000 + (rng() % nf), static_cast<uint32_t>(rng() % 50000)}); ++recs; }
        for (const uint32_t target : keys) {
            if (target == kPlayer || target == kMenu) continue;
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, target, kPlayer, vs, fs, ref);
            PlainOrder(ref, minN, skip, refOrder);
            bool rebuilt = false;
            cache.SelectTop(sets, recs, 7u, target, kPlayer, vs, fs, minN, skip, 160, got, gotOrder, &rebuilt);
            if (refOrder.size() > 160) refOrder.resize(160);
            CHECK(rebuilt, "round %d: stale tally made whole", round);
            CHECK(gotOrder == refOrder, "round %d: after records arrived", round);
            break;
        }
        // another minN against the same tally: the sorted list is made again
        {
            const uint32_t target = keys.back();
            if (target != kPlayer && target != kMenu) {
                const unsigned m2 = minN + 1;
                CntMap ref, got;
                Order refOrder, gotOrder;
                PlainTally(sets, target, kPlayer, vs, fs, ref);
                PlainOrder(ref, m2, skip, refOrder);
                cache.SelectTop(sets, recs, 7u, target, kPlayer, vs, fs, m2, skip, 1000000, got, gotOrder);
                CHECK(gotOrder == refOrder, "round %d: other minN", round);
            }
        }
    }
    CHECK(compared > 400 && nonEmpty > 250 && truncated > 100 && mergedFromTarget > 100, "exercised: %u compared (%u non-empty, %u truncated, %u with the target's own keys in the answer)", compared, nonEmpty, truncated, mergedFromTarget);
    std::printf("selecttop: %u builds compared with the plain loop (%u non-empty, %u cut to maxOut, %u with the target's own lowered keys in the answer)\n", compared, nonEmpty, truncated, mergedFromTarget);
}
// a console-sized set (41 stages, ~7.7k records, ~2k shared programs): the work each form does, on this host (information only)
static void TestSize() {
    using namespace vgx::stagetally;
    std::mt19937 rng(41);
    Ids vs, fs;
    for (unsigned i = 0; i < 1300; ++i) vs[0x1000 + i] = static_cast<uint16_t>(i);
    for (unsigned i = 0; i < 800; ++i) fs[0x2000 + i] = static_cast<uint16_t>(i);
    Sets sets;
    size_t recs = 0;
    std::vector<uint32_t> keys;
    for (unsigned s = 0; s < 41; ++s) {
        const uint32_t key = 0x10000000u + s;
        keys.push_back(key);
        for (unsigned r = 0; r < 190; ++r) {  // (a skewed draw from 2.4k programs: the head is shared by many stages, as on the console)
            const double u = (rng() % 100000) / 100000.0;
            const unsigned p = static_cast<unsigned>(2400.0 * u * u * u);
            sets[key].recs.push_back(Rec{0x1000 + p % 1300, 0x2000 + (p * 7) % 800, static_cast<uint32_t>(rng())});
            ++recs;
        }
    }
    std::unordered_set<uint32_t> held;
    const auto skip = [&](uint32_t k) { return held.count(k) != 0; };
    Cache cache;
    double clk = 0;
    cache.BeginRefresh(sets, recs, 1u, 0x504C5952u);
    while (!cache.StepRefresh(sets, vs, fs, 5.0, [&] { return clk += 1.0; })) {}
    cache.PrepareOrder(3);
    auto t0 = std::chrono::steady_clock::now();
    unsigned sink = 0;
    for (int i = 0; i < 20; ++i) { CntMap c; Order o; cache.Select(sets, recs, 1u, keys[i], 0x504C5952u, vs, fs, 3, skip, c, o); sink += static_cast<unsigned>(o.size()); }
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) { CntMap c; Order o; cache.SelectTop(sets, recs, 1u, keys[i], 0x504C5952u, vs, fs, 3, skip, 160, c, o); sink += static_cast<unsigned>(o.size()); }
    auto t2 = std::chrono::steady_clock::now();
    const double a = std::chrono::duration<double, std::milli>(t1 - t0).count() / 20, b = std::chrono::duration<double, std::milli>(t2 - t1).count() / 20;
    std::printf("size: Select %.2f ms, SelectTop %.2f ms a build on this host (%zu keys in the tally; checksum %u)\n", a, b, cache.Keys(), sink);
    CHECK(b <= a, "SelectTop is not dearer (%.2f vs %.2f)", b, a);
}

int main() {
    TestUmbrella();
    TestGovernor();
    TestHold();
    TestFormat();
    TestDue();
    TestSelectTop();
    TestSize();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("openstall26_model: all passed\n");
    return 0;
}
