// hrw28 host test: the significance set's pure logic (port/hrw_set.h) and the merge with several owners (vgx_hrw.h Compose / Uncompose).
#include "../platform/vgx/src/vgx_hrw.h"
#include "../platform/include/port/hrw_set.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace port::hrwset;
namespace hrwv = vgx::hrw;
using vgx::CmdBuf; using vgx::DrawRec; using vgx::Cmd;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// a fake clock: each draw advances it by the unit's cost
int RunComposeTests();
static double gNow = 0;
static double NowFake() { return gNow; }

static void TestBudget() {
    int order[6] = {0, 1, 2, 3, 4, 5};
    const double cost[6] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    std::vector<int> drawn;
    gNow = 0;
    double spent = 0; int first = 0;
    int n = RunBudgeted(order, 6, 2.5, NowFake, [&](int u) { drawn.push_back(u); gNow += cost[u]; return 1; }, &spent, &first);
    CHECK(n == 3);                 // 1, 2, then at 2.0 < 2.5 a third; at 3.0 the loop stops
    CHECK(first == 3);
    CHECK(spent <= 2.5 + 1.0 + 1e-9);   // overrun by at most one unit
    CHECK(drawn[0] == 0 && drawn[2] == 2);
    // a budget of 0 still draws the most significant one
    drawn.clear(); gNow = 0;
    n = RunBudgeted(order, 6, 0.0, NowFake, [&](int u) { drawn.push_back(u); gNow += 1; return 1; }, &spent, &first);
    CHECK(n == 1 && drawn.size() == 1 && first == 1);
    // a unit that cannot be drawn (-1) is skipped and not counted
    drawn.clear(); gNow = 0;
    n = RunBudgeted(order, 4, 10.0, NowFake, [&](int u) { if (u == 1) return -1; drawn.push_back(u); gNow += 0.1; return 1; }, &spent, &first);
    CHECK(n == 3 && first == 4);
}

static void TestSelect() {
    Params p; p.budgetMs = 1.0; p.maxN = 16;
    std::vector<Cand> c(10);
    for (int i = 0; i < 10; ++i) { c[i].id = i; c[i].eye[2] = -300.0f; c[i].radius = 60.0f; c[i].nearDist = 300; c[i].costMs = 0.3f; }
    int out[16]; double used = 0; int worth = 0;
    int m = Select(c.data(), 10, p, out, 16, &used, &worth);
    CHECK(m == 3);                  // 0.3 each: three fit in 1.0
    CHECK(used <= 1.0 + 1e-9);
    CHECK(worth == 10);
    // most significant first: a big near one beats the rest
    c[7].radius = 400.0f; c[7].eye[2] = -150.0f;
    m = Select(c.data(), 10, p, out, 16, &used, &worth);
    CHECK(m >= 1 && out[0] == 7);
    // protected first
    c[2].flags = kProtected;
    m = Select(c.data(), 10, p, out, 16, &used, &worth);
    CHECK(out[0] == 2);
}

// stagger: every eligible unit is refreshed at least once in N pairs when the budget holds only a fraction of them
static void TestStagger() {
    Params p; p.budgetMs = 1.0; p.maxN = 16;
    const int U = 12;
    Book book;
    std::vector<int> lastDrawn(U, 0);
    int worstGap = 0;
    for (uint32_t pair = 1; pair <= 200; ++pair) {
        std::vector<Cand> c(U);
        for (int i = 0; i < U; ++i) {
            c[i].id = i; c[i].eye[2] = -300.0f - 10.0f * i; c[i].radius = 100.0f; c[i].nearDist = 200;
            Book::Rec& r = book.Touch(1000 + i * 16, pair);
            c[i].costMs = r.costMs; c[i].stale = r.stale;
        }
        int out[16]; double used; int worth;
        const int m = Select(c.data(), U, p, out, 16, &used, &worth);
        bool drawn[U] = {};
        for (int j = 0; j < m; ++j) { drawn[out[j]] = true; Book::Drawn(book.Touch(1000 + out[j] * 16, pair), pair, 0.3); }
        for (int i = 0; i < U; ++i) {
            if (drawn[i]) { lastDrawn[i] = pair; }
            else { Book::Waited(book.Touch(1000 + i * 16, pair)); if (pair - lastDrawn[i] > static_cast<uint32_t>(worstGap)) worstGap = pair - lastDrawn[i]; }
        }
        book.Sweep(pair);
    }
    printf("stagger: worst wait %d pairs for 12 units with room for 3 per pair\n", worstGap);
    CHECK(worstGap <= 12);          // no eligible unit starves beyond N = 12 pairs
}

static void TestBook() {
    Book b;
    Book::Rec& r = b.Touch(0x1230, 1);
    r.costMs = 0.5f;
    CHECK(b.Find(0x1230) != nullptr && b.Find(0x1230)->costMs == 0.5f);
    CHECK(b.Find(0x9990) == nullptr);
    Book::Waited(b.Touch(0x1230, 2)); Book::Waited(b.Touch(0x1230, 3));
    CHECK(b.Find(0x1230)->stale == 2);
    Book::Drawn(b.Touch(0x1230, 4), 4, 1.5);
    CHECK(b.Find(0x1230)->stale == 0 && b.Find(0x1230)->freshPair == 4);
    CHECK(b.Find(0x1230)->costMs > 0.5f && b.Find(0x1230)->costMs < 1.5f);
    b.Sweep(40);                    // not seen for > 16 pairs: forgotten
    CHECK(b.Find(0x1230) == nullptr);
    // the table is never overrun: 2000 keys over 512 slots
    for (uint64_t k = 1; k <= 2000; ++k) b.Touch(k * 16, static_cast<uint32_t>(k));
    CHECK(b.Find(2000 * 16) != nullptr);
}

static void TestStats() {
    PairStats s;
    NoteLag(s, 0.5); NoteLag(s, 3.0); NoteLag(s, 20.0);
    CHECK(s.lagBins[0] == 1 && s.lagBins[2] == 1 && s.lagBins[4] == 1 && s.lagMax == 20.0);
    CHECK(PhaseOf(0x10, false) == 0x30 && PhaseOf(0x10, true) == 0x70);
    CHECK(PhaseValid(0x28) && !PhaseValid(0x40));
    s.Clear(); CHECK(s.pairs == 0 && s.lagMax == 0);
    CHECK(ReasonName(kRsnPartPass)[0] == 'p');
}

static void persp(float p[16], float scale) {
    std::memset(p, 0, 16 * sizeof(float));
    p[0] = 1.2f * scale; p[5] = -1.7f * scale; p[10] = -1.01f; p[11] = -1.0f; p[14] = -2.0f;
}
// one draw: its own projection block and model-view block (appended to uni), `n` vertices of 16 bytes, 3 indices
static uint32_t PushBlock(CmdBuf& b, const float* v, size_t n) {
    const uint32_t off = static_cast<uint32_t>((b.uni.size() + 3) / 4 * 4);
    b.uni.resize(off + n);
    std::memcpy(b.uni.data() + off, v, n * sizeof(float));
    return off;
}
static void AddDraw(CmdBuf& b, uint16_t owner, float tag, bool chunked = true, uint32_t tex = 0) {
    float P[16]; persp(P, 1.0f);
    float mv[24] = {1, 0, 0, tag, 0, 1, 0, 0, 0, 0, 1, -10};
    DrawRec d;
    d.owner = owner;
    d.ublock[vgx::UB_PROJ] = PushBlock(b, P, 16);
    d.ublock[vgx::UB_MV] = PushBlock(b, mv, 24);
    d.vbuf = 0;
    if (chunked && b.vtxChunks.empty()) b.vtxChunks.push_back(0);
    d.vchunk = 0;
    d.vtxFirst = static_cast<uint32_t>(b.vtx.size() / 16);
    d.vtxCount = 4;
    d.idxOffset = static_cast<uint32_t>(b.idx.size());
    d.idxCount = 3;
    d.tex[0].texId = tex;
    const size_t v0 = b.vtx.size();
    b.vtx.resize(v0 + 64);
    for (size_t i = 0; i < 64; ++i) b.vtx[v0 + i] = static_cast<uint8_t>(tag + i);
    for (uint16_t i = 0; i < 3; ++i) b.idx.push_back(static_cast<uint16_t>(d.vtxFirst + i));
    b.draws.push_back(d);
    b.cmds.push_back(Cmd{vgx::CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
}
static CmdBuf* MakeBuf() {
    CmdBuf* b = new CmdBuf();
    b->cmds.reserve(256); b->draws.reserve(64); b->vtx.reserve(1 << 16); b->idx.reserve(1 << 14); b->uni.reserve(1 << 14);
    return b;
}
static float TagOf(const CmdBuf& b, const Cmd& c) { return b.uni[b.draws[c.arg].ublock[vgx::UB_MV] + 3]; }


static void TestPairBudgets() {
    hrwv::Costs c; c.feBMs = 1.0; c.gameUndrawnMs = 8.7; c.partialMs = 3.0;
    hrwv::PairBudgetsOut b = hrwv::PairBudgets(c);
    CHECK(b.feMs > 31.0 && b.feMs < 32.4);                 // 33.3 * 0.97 - 1
    CHECK(b.gameDrawnMs > 20.0 && b.gameDrawnMs < 21.0);   // 32.3 - 8.7 - 3.0
    c.gameUndrawnMs = 40;                                  // an impossible target is floored, not followed
    CHECK(hrwv::PairBudgets(c).gameDrawnMs == 10.0);
    CHECK(std::strcmp(hrwv::GovStatusName(hrwv::kGovNoFit), "declined (the pair does not fit)") == 0);
    hrwv::Governor g;
    CHECK(g.Status() == hrwv::kGovOff);
}

int main() {
    TestPairBudgets();
    TestBudget(); TestSelect(); TestStagger(); TestBook(); TestStats();
    fails += RunComposeTests();
    printf(fails ? "hrw28_test: %d FAILED\n" : "hrw28_test: all passed\n", fails);
    return fails != 0;
}

// several owners: units 16, 17, 18 in the opaque and translucent passes of buffer types, interleaved with world draws; fresh draws for two of them (and Mario)
int RunComposeTests() {
    const int f0 = fails;
    const uint16_t oA = hrwv::OwnerKey(16, PhaseOf(0x10, false)), oB = hrwv::OwnerKey(17, PhaseOf(0x12, false)), oBx = hrwv::OwnerKey(17, PhaseOf(0x12, true)), oC = hrwv::OwnerKey(18, PhaseOf(0x0D, false));
    const uint16_t oM = hrwv::OwnerKey(hrwv::kOwnMario, hrwv::kPhPlayerDraw);
    CmdBuf* A = MakeBuf();
    AddDraw(*A, 0, 1);      // world
    AddDraw(*A, oA, 2);     // unit 16 (anchor)
    AddDraw(*A, 0, 3);
    AddDraw(*A, oB, 4);     // unit 17 opaque (anchor)
    AddDraw(*A, oC, 5);     // unit 18: not refreshed this pair: stays
    AddDraw(*A, oM, 6);     // Mario
    AddDraw(*A, oBx, 7);    // unit 17 translucent (anchor of its own key)
    AddDraw(*A, oA, 8);     // unit 16 again: removed, no insertion
    AddDraw(*A, 0, 9);
    CmdBuf* F = MakeBuf();  // the partial draw: Mario first, then the units in significance order (17 before 16), 17's translucent half last
    AddDraw(*F, oM, 106);
    AddDraw(*F, oB, 104);
    AddDraw(*F, oA, 102);
    AddDraw(*F, oBx, 107);
    const uint64_t h0 = hrwv::HashBuf(*A);
    hrwv::Undo u; hrwv::ComposeStats st; hrwv::Scratch sc;
    CHECK(hrwv::Compose(*A, *F, u, st, sc) == hrwv::kComposeOk);
    CHECK(st.freshKeys == 4 && st.matchedKeys == 4 && st.orphanKeys == 0);
    // each owner's fresh draws sit where ITS first retained draw was (A's order is the blending order), not where F made them
    const float want[] = {1, 102, 3, 104, 5, 106, 107, 9};
    size_t i = 0;
    for (const Cmd& c : A->cmds) { CHECK(i < 8 && TagOf(*A, c) == want[i]); ++i; }
    CHECK(i == 8);
    hrwv::Uncompose(*A, u);
    CHECK(hrwv::HashBuf(*A) == h0);                 // undo exactness
    for (int k = 0; k < 10; ++k) { CHECK(hrwv::Compose(*A, *F, u, st, sc) == hrwv::kComposeOk); hrwv::Uncompose(*A, u); CHECK(hrwv::HashBuf(*A) == h0); }
    // the same with only one unit fresh (budget spent before the other): the other stays exactly as A drew it
    CmdBuf* G = MakeBuf();
    AddDraw(*G, oA, 202);
    CHECK(hrwv::Compose(*A, *G, u, st, sc) == hrwv::kComposeOk);
    const float want2[] = {1, 202, 3, 4, 5, 6, 7, 9};
    i = 0;
    for (const Cmd& c : A->cmds) { CHECK(i < 8 && TagOf(*A, c) == want2[i]); ++i; }
    hrwv::Uncompose(*A, u);
    CHECK(hrwv::HashBuf(*A) == h0);
    delete A; delete F; delete G;
    return fails - f0;
}
