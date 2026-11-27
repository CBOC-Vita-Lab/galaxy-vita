// hitch25 host model: the per-stage line's pure parts (port/hitch25.h) and the sliced stage tally (platform/vgx/src/vgx_stage_tally.h GXV_STAGEBUILD_SLICE)
// against the whole-tally form.  bash tools/test_hitch25.sh
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "port/hitch25.h"
#include "../platform/vgx/src/vgx_stage_tally.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---- 1. the stage line ---------------------------------------------------------------------------------------------------------------------------
static void TestLine() {
    using namespace port::hitch25;
    CHECK(!IsHold(33.4) && !IsHold(41.0) && IsHold(41.8) && IsHold(50) && IsHold(542), "IsHold: 3 refreshes means 2.5 or more at 60 Hz");
    Stage s;
    s.Begin("FactoryGalaxy", 0xB3969649u, 114.8);
    CHECK(!s.any(), "nothing seen yet");
    // the star select opens: the stage queue's build (the picture of 127 ms), the scene key is not the stage's yet
    s.Seen(1000, false, true, 1000000000);
    s.Hold(1000, 127, port::hitch24::kJob);
    // the scene starts, 6 pictures behind the closed wipe, then the screen opens
    s.Seen(9000, true, false, 9000000000ll);
    for (int i = 0; i < 5; ++i) s.Seen(9100 + i * 20, true, false, 9100000000ll + i * 20000000ll);
    s.Seen(9500, true, true, 9500000000ll);
    CHECK(s.startMs == 9000 && s.openMs == 9500 && s.beforeOpenPictures == 6 && s.openNs == 9500000000ll, "start %.0f open %.0f before %u", s.startMs, s.openMs, s.beforeOpenPictures);
    s.Hold(9600, 33.4, port::hitch24::kBackOther);          // not a hold
    s.Hold(10000, 48, port::hitch24::kUpstream);            // first 5 s
    s.Hold(14400, 107, port::hitch24::kGame);               // first 5 s (4.9 s after the reveal)
    s.Hold(14500, 542, port::hitch24::kGame);               // later (5.0 s)
    s.Hold(20000, 45, port::hitch24::kUpstream);
    CHECK(s.pre.n == 1 && s.pre.maxMs == 127, "before the reveal: %u", s.pre.n);
    CHECK(s.first.n == 2 && s.first.maxMs == 107 && s.later.n == 2 && s.later.maxMs == 542, "windows %u/%u", s.first.n, s.later.n);
    port::hitch25::Turn t[4];
    t[0].endNs = 8900000000ll; t[0].gapUs = 251000; t[0].state = kMenu; t[0].part = kPartEndFrame;
    t[1].endNs = 9300000000ll; t[1].gapUs = 625000; t[1].state = kClosed; t[1].part = kPartEndFrame;
    t[2].endNs = 17000000000ll; t[2].gapUs = 385000; t[2].state = kPlay; t[2].part = kPartDraw;
    char tt[320];
    TurnText(t, 3, s.openNs, tt, sizeof tt);
    CHECK(std::strstr(tt, "before the reveal 2 (max 625 ms: end frame") && std::strstr(tt, "1 at the menu / star select, 1 behind the wipe") && std::strstr(tt, "after it 1 (max 385 ms: draw)"), "%s", tt);
    char line[900];
    s.Format(line, sizeof line, tt);
    std::printf("%s\n", line);
    CHECK(std::strstr(line, "hitch25: stage FactoryGalaxy: first visible picture at 500 ms after the scene start (6 pictures behind the closed screen)"), "%s", line);
    CHECK(std::strstr(line, "before the reveal: 1 holds >=3 refreshes (max 127 ms)") && std::strstr(line, "first 5 s 2 (max 107 ms), later 2 (max 542 ms)"), "%s", line);
    CHECK(std::strstr(line, "causes: game thread 2 (649 ms), no picture arrived") && std::strstr(line, "queue build 115 ms; game turns >=100 ms"), "%s", line);
    // a stage whose screen never opened
    Stage u;
    u.Begin("Never", 1, 0);
    u.Seen(5, true, false, 5000000);
    s.Format(line, sizeof line, "");
    u.Format(line, sizeof line, "");
    CHECK(std::strstr(line, "no visible picture") && !std::strstr(line, "queue build"), "%s", line);
    // a truncated buffer stays terminated
    char tiny[40];
    const size_t n = s.Format(tiny, sizeof tiny, tt);
    CHECK(n < sizeof tiny && std::strlen(tiny) == n, "tiny buffer: %zu", n);
    // no turn list: everything is "before"
    TurnText(t, 0, 0, tt, sizeof tt);
    CHECK(std::strstr(tt, "before the reveal 0 (max 0 ms: not timed") && std::strstr(tt, "after it 0"), "%s", tt);
    TurnText(t, 3, 0, tt, sizeof tt);
    CHECK(std::strstr(tt, "before the reveal 3") && std::strstr(tt, "after it 0"), "%s", tt);
}

// ---- 2. the sliced tally against the whole one ---------------------------------------------------------------------------------------------------
struct Rec { uint64_t vs, fs; uint32_t lb; };
struct Set { std::vector<Rec> recs; };
using Sets = std::unordered_map<uint32_t, Set>;
using Ids = std::unordered_map<uint64_t, uint16_t>;
static bool Same(const vgx::stagetally::CntMap& a, const vgx::stagetally::Order& oa, const vgx::stagetally::CntMap& b, const vgx::stagetally::Order& ob) {
    if (oa != ob) return false;
    for (size_t i = 0; i < oa.size(); ++i) {
        const auto& x = a.at(oa[i].second);
        const auto& y = b.at(ob[i].second);
        if (x.n != y.n || x.lb != y.lb || x.vs != y.vs || x.fs != y.fs) return false;
    }
    return true;
}
static void TestSlice() {
    using namespace vgx::stagetally;
    std::mt19937 rng(777);
    const uint32_t kPlayer = 0x504C5952u, kMenu = 0x4D454E55u;
    unsigned compared = 0, slices = 0, nonEmpty = 0;
    for (int round = 0; round < 80; ++round) {
        Ids vs, fs;
        const unsigned nv = 20 + rng() % 60, nf = 30 + rng() % 120;
        for (unsigned i = 0; i < nv; ++i) vs[0x1000 + i] = static_cast<uint16_t>(i);
        for (unsigned i = 0; i < nf; ++i) fs[0x2000 + i] = static_cast<uint16_t>(i);
        Sets sets;
        const unsigned nstages = 3 + rng() % 14;
        std::vector<uint32_t> keys;
        size_t recs = 0;
        for (unsigned s = 0; s < nstages; ++s) {
            const uint32_t key = s == 0 ? kMenu : s == 1 ? kPlayer : 0x10000000u + rng() % 100000;
            Set& set = sets[key];
            const unsigned nr = rng() % 300;
            for (unsigned r = 0; r < nr; ++r)
                set.recs.push_back(Rec{0x1000 + (rng() % (nv + 3)), 0x2000 + (rng() % (nf + 3)), static_cast<uint32_t>(rng() % 50000)});
            recs += nr;
            keys.push_back(key);
        }
        const unsigned minN = 1 + rng() % 4;
        std::unordered_set<uint32_t> held;
        for (int q = 0; q < 12; ++q) held.insert((static_cast<uint32_t>(rng() % nv) << 16) | (rng() % nf));
        const auto skip = [&](uint32_t k) { return held.count(k) != 0; };
        Cache cache;
        CHECK(!cache.Fresh(recs, sets.size(), 7u) && !cache.Building(), "a new cache is not fresh");
        // the sliced refresh with a clock that advances 1 ms every read and a budget of a few ms
        double clk = 0;
        const auto now = [&] { return clk += 1.0; };
        cache.BeginRefresh(sets, recs, 7u, kPlayer);
        CHECK(cache.Building() && !cache.Fresh(recs, sets.size(), 7u), "building");
        unsigned guard = 0;
        const double budget = 1.0 + rng() % 5;
        while (!cache.StepRefresh(sets, vs, fs, budget, now)) { ++slices; CHECK(++guard < 100000, "the refresh ends"); if (guard >= 100000) break; }
        CHECK(!cache.Building() && cache.Fresh(recs, sets.size(), 7u), "fresh after the refresh");
        for (const uint32_t target : keys) {
            if (target == kPlayer || target == kMenu) continue;
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, target, kPlayer, vs, fs, ref);
            PlainOrder(ref, minN, skip, refOrder);
            bool rebuilt = true;
            cache.Select(sets, recs, 7u, target, kPlayer, vs, fs, minN, skip, got, gotOrder, &rebuilt);
            CHECK(!rebuilt, "the sliced tally is found fresh (no whole rebuild)");
            CHECK(Same(ref, refOrder, got, gotOrder), "round %d target %08x", round, target);
            ++compared;
            nonEmpty += !refOrder.empty();
        }
        // another minN against the same tally (the candidate list is made again)
        {
            const unsigned m2 = minN + 1;
            const uint32_t target = keys.back();
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, target, kPlayer, vs, fs, ref);
            PlainOrder(ref, m2, skip, refOrder);
            cache.Select(sets, recs, 7u, target, kPlayer, vs, fs, m2, skip, got, gotOrder);
            CHECK(Same(ref, refOrder, got, gotOrder), "round %d other minN", round);
        }
        // records arrive while a refresh is half done: Select (the state key changed) makes the whole tally and ends the refresh; the result is still the loop's
        cache.BeginRefresh(sets, recs, 7u, kPlayer);
        cache.StepRefresh(sets, vs, fs, 2.0, now);
        const uint32_t k = keys[rng() % keys.size()];
        for (int a = 0; a < 7; ++a) { sets[k].recs.push_back(Rec{0x1000 + (rng() % nv), 0x2000 + (rng() % nf), static_cast<uint32_t>(rng() % 50000)}); ++recs; }
        for (const uint32_t target : keys) {
            if (target == kPlayer || target == kMenu) continue;
            CntMap ref, got;
            Order refOrder, gotOrder;
            PlainTally(sets, target, kPlayer, vs, fs, ref);
            PlainOrder(ref, minN, skip, refOrder);
            bool rebuilt = false;
            cache.Select(sets, recs, 7u, target, kPlayer, vs, fs, minN, skip, got, gotOrder, &rebuilt);
            CHECK(rebuilt && !cache.Building(), "stale: made whole, refresh ended");
            CHECK(Same(ref, refOrder, got, gotOrder), "round %d after records arrived", round);
            break;
        }
        // a refresh whose sets grow while it runs ends with the old key: the state is stale again and the next Select makes the whole tally
        cache.BeginRefresh(sets, recs, 7u, kPlayer);
        cache.StepRefresh(sets, vs, fs, 1.0, now);
        sets[0x7777u].recs.push_back(Rec{0x1000, 0x2000, 1});  // (a new stage: the sets may rehash between slices)
        for (int a = 0; a < 50; ++a) sets[0x20000000u + a].recs.push_back(Rec{0x1000 + (rng() % nv), 0x2000 + (rng() % nf), 5});
        while (!cache.StepRefresh(sets, vs, fs, 3.0, now)) {}
        CHECK(!cache.Fresh(recs + 1, sets.size(), 7u), "a refresh made for the old state is not fresh for the new one");
    }
    CHECK(compared > 300 && nonEmpty > 150 && slices > 100, "exercised: %u builds compared (%u non-empty), %u unfinished slices", compared, nonEmpty, slices);
    std::printf("slice: %u target builds compared with the plain loop (%u non-empty), %u slices that stopped for the budget\n", compared, nonEmpty, slices);
}

int main() {
    TestLine();
    TestSlice();
    if (g_fail) { std::printf("hitch25_model: %d FAILED\n", g_fail); return 1; }
    std::printf("hitch25_model: ok\n");
    return 0;
}
