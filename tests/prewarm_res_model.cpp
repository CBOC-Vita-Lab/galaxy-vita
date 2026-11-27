// handover26 host model: GXV_PREWARM_RES's record / replay logic (platform/vgx/src/vgx_prewarm_res.h).  bash tools/test_prewarm_res.sh
#include <cstdio>
#include <string>

#include "../platform/vgx/src/vgx_prewarm_res.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vgx::prewarmres;

static Item Mk(unsigned w, unsigned h, unsigned pw, unsigned ph, uint8_t kind = kCopyTex, uint8_t depth = 0) {
    Item it;
    it.w = static_cast<uint16_t>(w); it.h = static_cast<uint16_t>(h); it.picW = static_cast<uint16_t>(pw); it.picH = static_cast<uint16_t>(ph);
    it.kind = kind; it.depth = depth;
    return it;
}

static void TestSeed() { // stall27
    Item sd[8];
    const uint32_t beach = Fnv("HeavenlyBeachGalaxy");
    CHECK(beach == 0x4aa29ec8u, "the stage key is the FNV of the name (%08x)", beach);
    unsigned n = Seeded(beach, 960, 544, sd, 8);
    CHECK(n == 3 && sd[0].w == 240 && sd[0].h == 136 && sd[1].w == 480 && sd[1].h == 272 && sd[2].w == 120 && sd[2].h == 68 && sd[2].kind == kPassObj && sd[2].depth == 1, "Beach Bowl chain at 960x544");
    n = Seeded(Fnv("KoopaBattleVs2Galaxy"), 848, 480, sd, 8);
    CHECK(n == 2 && sd[0].w == 424 && sd[0].h == 240 && sd[1].w == 212 && sd[1].h == 120, "Vs2 at 848x480: 424x240, 212x120");
    CHECK(Seeded(0x1234u, 960, 544, sd, 8) == 0, "no seed for other stages");
    Plan p;
    BuildPlan(nullptr, beach, 1, 960, 544, 8, 64ull << 20, p, 640, 456, true);
    CHECK(p.n == 5 && p.seeded == 3 && p.generic == 2, "plan = 2 generic + 3 seeded (%u %u %u)", p.n, p.generic, p.seeded);
    BuildPlan(nullptr, beach, 1, 960, 544, 8, 64ull << 20, p, 640, 456, false);
    CHECK(p.n == 2 && p.seeded == 0, "seed off");
}
static void TestGeneric() {
    Item g[4];
    unsigned n = Generic(960, 544, g, 4);
    CHECK(n == 2, "two generic items at 960x544, got %u", n);
    CHECK(g[0].w == 240 && g[0].h == 191 && g[0].kind == kCopyTex, "the starter's target is 240x191, got %ux%u", g[0].w, g[0].h);
    CHECK(g[1].w == 960 && g[1].h == 544, "the full-size copy target, got %ux%u", g[1].w, g[1].h);
    n = Generic(848, 480, g, 4);
    CHECK(n == 2 && g[0].w == 212 && g[0].h == 168 && g[1].w == 848 && g[1].h == 480, "848x480 scales: %ux%u and %ux%u", g[0].w, g[0].h, g[1].w, g[1].h);
    CHECK(Generic(0, 544, g, 4) == 0 && Generic(960, 544, g, 1) == 0 && Generic(9000, 544, g, 4) == 0, "bad sizes give none");
}

static void TestBook() {
    Book b;
    const uint32_t F = 0xB3969649u, E = 0x099E9A42u;
    CHECK(b.Note(F, Mk(240, 191, 960, 544)), "first note is new");
    CHECK(!b.Note(F, Mk(240, 191, 960, 544)), "the same item again is not new");
    CHECK(b.Note(F, Mk(240, 191, 848, 480)), "another picture size is another item");
    CHECK(b.Note(F, Mk(960, 544, 960, 544, kPassObj, 0)), "a pass object of the same size is another item");
    CHECK(b.Note(F, Mk(960, 544, 960, 544, kPassObj, 1)), "a pass object gaining depth is news");
    CHECK(!b.Note(F, Mk(960, 544, 960, 544, kPassObj, 1)), "... once");
    CHECK(!b.Note(0, Mk(1, 1, 960, 544)) && !b.Note(F, Mk(0, 5, 960, 544)) && !b.Note(F, Mk(5000, 5, 960, 544)), "stage 0 and bad sizes are refused");
    CHECK(b.Items(F) && b.Items(F)->size() == 3, "three items (the depth merged into the pass object)");
    for (unsigned i = 0; i < 20; ++i) b.Note(E, Mk(10 + i, 10 + i, 960, 544));
    CHECK(b.Items(E)->size() == kMaxItems, "a stage keeps at most %u items, got %u", kMaxItems, static_cast<unsigned>(b.Items(E)->size()));
    CHECK(b.Dirty(), "dirty after notes");

    const std::string s = b.Serialize();
    Book c;
    const unsigned got = c.Parse(s.data(), s.size());
    CHECK(got == b.Total(), "parse takes every item: %u of %u", got, static_cast<unsigned>(b.Total()));
    CHECK(!c.Dirty(), "a parsed book is clean");
    CHECK(c.Serialize() == s, "serialize . parse is the identity");
    CHECK(c.Items(F)->size() == 3 && (*c.Items(F))[2].depth == 1, "depth survives");

    // cut short / bad
    Book d;
    CHECK(d.Parse(s.data(), s.size() - 5) == b.Total() - 1, "a cut-short last line is left out");
    Book e;
    CHECK(e.Parse("nonsense\n00000001 1 1 1 1 0 0\n", 29) == 0 && e.Total() == 0, "wrong magic reads nothing");
    Book f;
    const std::string bad = std::string(Book::Magic()) + "\n00000001 10 10 960 544 0 0\ngarbage line\n00000002 10 10 960 544 0 0\n";
    CHECK(f.Parse(bad.data(), bad.size()) == 1 && f.Total() == 1, "a bad line stops the parse");
    const std::string extra = std::string(Book::Magic()) + "\n00000001 10 10 960 544 0 0 7\n";
    Book g;
    CHECK(g.Parse(extra.data(), extra.size()) == 0, "a line with too many fields is bad");
    Book h;
    CHECK(h.Parse("", 0) == 0, "empty text");
    // stage count cap
    Book many;
    for (uint32_t k = 1; k <= kMaxStages + 10; ++k) many.Note(k, Mk(8, 8, 960, 544));
    CHECK(many.Stages() == kMaxStages, "stage cap %u, got %u", kMaxStages, static_cast<unsigned>(many.Stages()));
}

static void TestPlan() {
    Book b;
    const uint32_t F = 0xB3969649u;
    b.Note(F, Mk(240, 191, 960, 544));            // also generic: must be made once
    b.Note(F, Mk(480, 272, 960, 544, kPassObj, 1));
    b.Note(F, Mk(100, 100, 848, 480));            // another picture size: not this one's
    Plan p;
    BuildPlan(&b, F, 2, 960, 544, 8, 64ull << 20, p);
    CHECK(p.n == 3 && p.recorded == 2 && p.generic == 1, "mode 2: n=%u recorded=%u generic=%u", p.n, p.recorded, p.generic);
    CHECK(p.item[0].w == 240 && p.item[1].w == 480 && p.item[2].w == 960, "recorded first in order, then the generic rest");
    CHECK(p.item[1].kind == kPassObj && p.item[1].depth == 1, "kind and depth kept");
    BuildPlan(&b, F, 1, 960, 544, 8, 64ull << 20, p);
    CHECK(p.n == 2 && p.recorded == 0 && p.generic == 2, "mode 1: generic only, n=%u", p.n);
    BuildPlan(&b, F, 0, 960, 544, 8, 64ull << 20, p);
    CHECK(p.n == 0, "mode 0: nothing");
    BuildPlan(&b, 0x1234u, 2, 960, 544, 8, 64ull << 20, p);
    CHECK(p.n == 2 && p.recorded == 0, "a stage with no record gets the generic list");
    BuildPlan(nullptr, F, 2, 960, 544, 8, 64ull << 20, p);
    CHECK(p.n == 2, "no book: generic");
    BuildPlan(&b, F, 2, 960, 544, 2, 64ull << 20, p);
    CHECK(p.n == 2 && p.recorded == 2, "item cap: recorded first");
    // byte cap: 240x191x4 = 183 KiB; 960x544x4 = 2.0 MiB
    BuildPlan(nullptr, F, 1, 960, 544, 8, 1ull << 20, p);
    CHECK(p.n == 1 && p.droppedBytes == 1 && p.item[0].w == 240, "byte cap keeps the small one, n=%u dropped=%u", p.n, p.droppedBytes);
    BuildPlan(&b, F, 2, 848, 480, 8, 64ull << 20, p);
    CHECK(p.n == 3 && p.recorded == 1 && p.item[0].w == 100, "a picture size of its own: recorded for it first, then 2 generic, n=%u", p.n);
    // depth is merged into a generic duplicate
    Book d;
    d.Note(F, Mk(960, 544, 960, 544, kCopyTex, 1));
    BuildPlan(&d, F, 2, 960, 544, 8, 64ull << 20, p);
    CHECK(p.n == 2 && p.item[0].w == 960 && p.item[0].depth == 1, "same item twice is made once (depth kept)");
}


// texwarm30: the picture size the stage steps UP to (console tours: 848x480 at the opening, 960x544 at +7.7 s; "4 -> 6 ... premade textures taken 0")
static void TestAlt() {
    const uint32_t F = 0xB3969649u;
    Book b;
    b.Note(F, Mk(212, 168, 848, 480));
    b.Note(F, Mk(240, 191, 960, 544));            // made in play after the step: recorded for the 960x544 picture
    b.Note(F, Mk(480, 272, 960, 544, kPassObj, 1));
    Plan p;
    BuildPlan(&b, F, 2, 848, 480, 6, 64ull << 20, p, 640, 456, false, 960, 544);
    CHECK(p.recorded == 1 && p.alt == 3 && p.n == 5, "848 plan + 3 for 960: n=%u recorded=%u generic=%u alt=%u", p.n, p.recorded, p.generic, p.alt);
    CHECK(p.item[p.n - 3].w == 240 && p.item[p.n - 3].picW == 960 && p.item[p.n - 2].w == 480, "the step-up picture's recorded items come first among the alt ones");
    BuildPlan(&b, F, 2, 848, 480, 6, 64ull << 20, p);
    CHECK(p.alt == 0 && p.n == 2, "no step-up size: the plan is as before (n=%u)", p.n);
    BuildPlan(&b, F, 2, 960, 544, 6, 64ull << 20, p, 640, 456, false, 960, 544);
    CHECK(p.alt == 0, "the same size is not a step up");
    BuildPlan(nullptr, F, 1, 848, 480, 6, 64ull << 20, p, 640, 456, false, 960, 544);   // first visit, no book: the generic copy target and the full-size texture of 960x544
    CHECK(p.alt == 2 && p.item[p.n - 2].w == 240 && p.item[p.n - 1].w == 960 && p.item[p.n - 1].h == 544, "first visit: generic for the step-up size, n=%u alt=%u", p.n, p.alt);
    BuildPlan(nullptr, F, 1, 848, 480, 6, 3ull << 20, p, 640, 456, false, 960, 544);   // the byte cap holds
    CHECK(p.n >= 1 && p.droppedBytes >= 1, "byte cap applies to the alt items too (n=%u dropped=%u)", p.n, p.droppedBytes);
    BuildPlan(&b, F, 2, 848, 480, 8, 64ull << 20, p, 640, 456, false, 960, 544);
    CHECK(p.n <= kMaxItems + kMaxAlt && p.n <= sizeof p.item / sizeof p.item[0], "array bound (n=%u)", p.n);
}

static void TestWindow() {
    CHECK(Recordable(1000, 500) && Recordable(15500, 500) && !Recordable(15600, 500) && !Recordable(400, 500) && !Recordable(1000, 0), "window rule");
    Worst w;
    w.Note(100, 8000, "game thread");
    w.Note(520, 8300, "render targets made");
    w.Note(90, 9000, "gpu");
    w.Note(900, 25000, "late");   // after the census window
    w.Note(800, -5, "closed");    // before the screen opened
    CHECK(w.ms == 520 && w.atMs == 8300 && std::string(w.cause) == "render targets made", "worst stall %.0f at %.0f (%s)", w.ms, w.atMs, w.cause);
}

int main() {
    TestGeneric();
    TestSeed();
    TestBook();
    TestPlan(); TestAlt();
    TestWindow();
    if (g_fail) { std::printf("prewarm_res_model: %d FAILED\n", g_fail); return 1; }
    std::printf("prewarm_res_model: ok\n");
    return 0;
}
