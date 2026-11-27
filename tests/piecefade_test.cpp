// q30fin26: host test of port/piecefade.h (GXV_THINFADE): the shrink / grow ramp of the items the cuts thin out, the stagger, the snap-back and the census.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "port/piecefade.h"
#include "port/lockfade.h"

using namespace port;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static piecefade::Table* NewTable() { static piecefade::Table t; t.Clear(); t.ResetCounts(); t.Config(24, 4); return &t; }

static void TestRamp() {
    piecefade::Table* t = NewTable();
    const uintptr_t key = 0x1000;
    uint32_t turn = 100;
    bool created = false, hidden = false;
    // a cut wants it hidden: it shrinks monotonic over 24 turns and only then is "hidden" (the stamp)
    float prev = 2.0f;
    int firstHidden = -1;
    for (int i = 0; i < 40; ++i, ++turn) {
        hidden = t->Judge(key, true, true, turn, &created);
        float sc[3] = {1, 1, 1};
        bool erased = false;
        t->Apply(key, sc, &erased);
        CHECK(sc[0] <= prev + 1e-6f);
        CHECK(sc[0] >= piecefade::kMinScale - 1e-6f);
        if (i > 0 && prev - sc[0] > 0.12f) CHECK(false);   // no step bigger than 12 % of the size in a turn
        prev = sc[0];
        if (hidden && firstHidden < 0) firstHidden = i;
    }
    CHECK(created == false);   // (created only on the first judgement)
    CHECK(firstHidden >= 23 && firstHidden <= 25);
    CHECK(t->Wants(key));
    // grow back when no longer wanted, at a candidate's pace
    prev = 0.0f;
    for (int i = 0; i < 40; ++i, ++turn) {
        hidden = t->Judge(key, false, true, turn);
        CHECK(!hidden);
        float sc[3] = {1, 1, 1};
        bool erased = false;
        if (t->Find(key)) { t->Apply(key, sc, &erased); CHECK(sc[0] >= prev - 1e-6f); prev = sc[0]; }
    }
    CHECK(t->Live() == 0);   // (the entry is gone once back at full size, and the base scale restored)
    const piecefade::Census c = t->counts();
    CHECK(c.begun == 1 && c.completedOut == 1 && c.completedIn == 1);
}
static void TestSnap() {
    piecefade::Table* t = NewTable();
    uint32_t turn = 10;
    const uintptr_t key = 0x2000;
    for (int i = 0; i < 30; ++i, ++turn) t->Judge(key, true, true, turn);
    int n = 0;
    while (t->Find(key) && n < 50) {   // near the player now (not a candidate): 4x faster, never held small
        t->Judge(key, false, false, turn++);
        float sc[3] = {1, 1, 1};
        bool erased;
        t->Apply(key, sc, &erased);
        ++n;
    }
    CHECK(n <= 8);
    CHECK(t->counts().snapped >= 1);
}
static void TestStagger() {
    // 4000 items, the cut wants them hidden at hashed turns (the Fader): at most ~1.2 % start in one turn, none pops
    piecefade::Table* t = NewTable();
    const int N = 600;   // (the table's live limit)
    std::vector<int> startTurn(N);
    for (int i = 0; i < N; ++i) startTurn[i] = 100 + (i * 37) % 120;
    int maxStarts = 0;
    float maxStep = 0;
    std::vector<float> last(N, 1.0f);
    for (uint32_t turn = 100; turn < 400; ++turn) {
        int starts = 0;
        for (int i = 0; i < N; ++i) {
            const uintptr_t key = 0x10000 + static_cast<uintptr_t>(i) * 64;
            const bool want = static_cast<int>(turn) >= startTurn[i];
            if (!want && !t->Find(key)) continue;
            bool created = false;
            t->Judge(key, want, true, turn, &created);
            if (created) ++starts;
            float sc[3] = {1, 1, 1};
            bool erased;
            if (t->Find(key)) { t->Apply(key, sc, &erased); const float d = last[i] - sc[0]; if (d > maxStep) maxStep = d; last[i] = sc[0]; }
        }
        if (starts > maxStarts) maxStarts = starts;
    }
    CHECK(maxStep < 0.12f);          // not one item loses more than 12 % of its size in a turn (no pop)
    CHECK(maxStarts <= 8);           // at most 8 of 600 begin in a turn
    const piecefade::Census c = t->counts();
    CHECK(c.begun == static_cast<uint64_t>(N));
    CHECK(c.maxLive <= static_cast<uint32_t>(piecefade::kMaxLive));
    std::printf("piecefade stagger: max starts a turn %d, max step %.3f, begun %llu, hidden at end %u\n", maxStarts, static_cast<double>(maxStep), static_cast<unsigned long long>(c.begun), 0u);
    piecefade::Census g;
    t->Sample(g);
    CHECK(g.hiddenNow == static_cast<uint32_t>(N));
}
static void TestOverflow() {
    piecefade::Table* t = NewTable();
    uint32_t hiddenAtOnce = 0;
    for (int i = 0; i < piecefade::kMaxLive + 50; ++i) {
        bool created;
        if (t->Judge(0x50000 + static_cast<uintptr_t>(i) * 16, true, true, 5, &created)) ++hiddenAtOnce;   // (table full: the old cut takes over for that item)
    }
    CHECK(hiddenAtOnce == 50);
    CHECK(t->counts().overflow == 50);
}
int main() {
    TestRamp();
    TestSnap();
    TestStagger();
    TestOverflow();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
