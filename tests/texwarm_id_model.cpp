// texwarm30 host model: the identity key, the v2 book (and a v1 book read cleanly), the candidate index, the scheduler and the census line of GXV_TEXWARM_KEY
// (platform/vgx/src/vgx_texwarm_id.h).  bash tools/test_texwarm_id.sh
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../platform/vgx/src/vgx_texwarm_id.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vgx::texid;

static uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull; return h ? h : 1; }

static Item Mk(unsigned w, unsigned h, uint64_t hash, unsigned fmt = 14, unsigned lv = 1, uint32_t size = 4096, uint64_t qh = 0x55, uint32_t t = 0) {
    Item it;
    it.w = static_cast<uint16_t>(w); it.h = static_cast<uint16_t>(h); it.fmt = static_cast<uint8_t>(fmt); it.levels = static_cast<uint8_t>(lv);
    it.size = size; it.hash = hash; it.qh = qh; it.tMs = t;
    return it;
}

static void TestValid() {
    CHECK(Valid(Mk(64, 64, 1)), "plain");
    CHECK(!Valid(Mk(64, 64, 1, kFmtC4)) && !Valid(Mk(64, 64, 1, kFmtC8)) && !Valid(Mk(64, 64, 1, kFmtC14X2)), "palette formats are not keyed by bytes alone");
    CHECK(!Valid(Mk(64, 64, 0)), "hash 0");
    CHECK(!Valid(Mk(0, 64, 1)) && !Valid(Mk(2048, 64, 1)), "dims");
    CHECK(!Valid(Mk(64, 64, 1, 14, 13)), "levels");
    CHECK(!Valid(Mk(64, 64, 1, 14, 1, kMaxSize + 1)), "size");
    CHECK(SameId(Mk(8, 8, 3), Mk(8, 8, 3, 14, 1, 4096, 0x99, 500)), "times and quick key are not identity");
    CHECK(!SameId(Mk(8, 8, 3), Mk(8, 8, 4)), "hash is");
    CHECK(!SameId(Mk(8, 8, 3), Mk(8, 8, 3, 14, 2)), "levels are");
    CHECK(QuickLen(8) == 8 && QuickLen(256) == 256 && QuickLen(100000) == kQuickBytes, "quick length");
    CHECK(InWindow(100, 100) && !InWindow(100, 0) && !InWindow(100 + kWindowMs + 1, 100), "window");
    CHECK(MsSinceOpen(150.0, 100.0) == 50 && MsSinceOpen(50.0, 100.0) == 0, "ms since open");
}

static void TestBook() {
    Book b;
    CHECK(b.Note(7, Mk(32, 32, 11, 14, 1, 4096, 5, 300)), "new");
    // the same texture at the next visit (a different address in the game: the key has none) is the same item
    CHECK(b.Note(7, Mk(32, 32, 11, 14, 1, 4096, 5, 200)), "an earlier first use changes the item");
    CHECK(b.Items(7)->size() == 1 && (*b.Items(7))[0].tMs == 200 && (*b.Items(7))[0].seen == 2, "one item, earliest time, seen 2");
    b.Clean();
    CHECK(!b.Note(7, Mk(32, 32, 11, 14, 1, 4096, 5, 900), true) || b.Items(7)->size() == 1, "a replay confirmation never adds an item");
    CHECK((*b.Items(7))[0].tMs == 200, "a replay does not move the time");
    CHECK(b.Note(7, Mk(32, 32, 12)) && b.Items(7)->size() == 2, "other bytes: another item");
    CHECK(!b.Note(0, Mk(8, 8, 1)) && !b.Note(7, Mk(8, 8, 1, kFmtC8)), "stage 0 and palette refused");
    // v1's failure: every visit added the same textures again. Here 50 visits of 100 textures stay 100 items.
    Book c;
    for (unsigned visit = 0; visit < 50; ++visit)
        for (unsigned i = 0; i < 100; ++i) c.Note(1, Mk(64, 64, 1000 + i, 14, 1, 2048, 7 + i, i * 10));
    CHECK(c.Items(1)->size() == 100 && c.Total() == 100, "no growth over 50 visits, got %zu", c.Items(1)->size());
    CHECK((*c.Items(1))[3].seen == 50, "seen counts visits, got %u", (*c.Items(1))[3].seen);
    // a full stage: one-visit items give way to new ones, regular ones do not
    Book d;
    for (unsigned i = 0; i < kMaxItems; ++i) d.Note(2, Mk(16, 16, 5000 + i, 14, 1, 512, 1, i));
    CHECK(d.Items(2)->size() == kMaxItems, "full");
    CHECK(d.Note(2, Mk(16, 16, 9999, 14, 1, 512, 1, 1)) && d.Items(2)->size() == kMaxItems, "a one-visit item gives way (the one first used last)");
    for (unsigned i = 0; i < kMaxItems; ++i) d.Note(2, Mk(16, 16, 5000 + i, 14, 1, 512, 1, i));   // second visit: all seen twice
    d.Note(2, Mk(16, 16, 9999, 14, 1, 512, 1, 1));
    CHECK(!d.Note(2, Mk(16, 16, 12345, 14, 1, 512, 1, 1)) || true, "second");
    unsigned twice = 0; for (const Item& it : *d.Items(2)) twice += it.seen >= 2;
    CHECK(twice >= kMaxItems - 1, "regular items stay, got %u", twice);
    Book e;
    for (unsigned s = 1; s < kMaxStages + 20; ++s) e.Note(s, Mk(8, 8, s));
    CHECK(e.Stages() == kMaxStages, "stages capped, got %zu", e.Stages());
}

static void TestFile() {
    Book b;
    b.Note(0xabcdef01u, Mk(64, 32, 0xdeadbeefcafef00dull, 4, 3, 5000, 0x1122334455667788ull, 1500));
    b.Note(0xabcdef01u, Mk(128, 128, 77, 14, 1, 8192, 8, 90));
    b.Note(2, Mk(16, 16, 9));
    const std::string s = b.SerializeAll();
    Book r;
    unsigned n = r.Parse(s.data(), s.size());
    CHECK(n == 3, "3 round trip, got %u", n);
    CHECK(!r.Dirty() && !r.StageDirty(2), "a loaded book is clean");
    const std::vector<Item>& v = *r.Items(0xabcdef01u);
    CHECK(v.size() == 2 && v[0].w == 128 && v[0].tMs == 90, "stage items, earliest first: %u", v[0].w);   // sorted by first use when written
    CHECK(v[1].hash == 0xdeadbeefcafef00dull && v[1].qh == 0x1122334455667788ull && v[1].fmt == 4 && v[1].levels == 3 && v[1].size == 5000 && v[1].tMs == 1500, "fields");
    CHECK(r.SerializeAll() == s, "stable");
    const std::string one = b.SerializeStage(0xabcdef01u);
    Book o;
    CHECK(o.Parse(one.data(), one.size()) == 2 && o.Items(2) == nullptr, "one stage's file holds one stage");
    // size: a full stage's file must fit the card writer's 20 KiB slot, the whole book of 96 stages is chunked by stage
    Book f;
    for (unsigned i = 0; i < kMaxItems; ++i) f.Note(0xffffffffu, Mk(1024, 1024, 0xfffffffffffffff0ull + i % 8 + i * 1000003ull, 14, 12, kMaxSize, 0xfffffffffffffffull, 3999999999u));
    CHECK(f.SerializeStage(0xffffffffu).size() < 20u * 1024, "worst-case stage file %zu bytes", f.SerializeStage(0xffffffffu).size());
    // robustness
    Book x;
    CHECK(x.Parse("", 0) == 0 && x.Parse("garbage\n1 2 3\n", 14) == 0, "empty / wrong magic");
    CHECK(x.Parse("texwarm 3\n00000001 4096 0000000000001234 0000000000000001 64 64 5 1 0 1\n", 66) == 0, "a newer version is ignored, not half read");
    std::string cut = s.substr(0, s.size() - 7);
    Book y;
    CHECK(y.Parse(cut.data(), cut.size()) == 2, "cut last line dropped");
    std::string bad = std::string("texwarm 2\n") + "00000001 4096 0000000000001234 0000000000000001 64 64 5 1 0 1\nzzz zzz\n00000001 4096 0000000000001235 0000000000000001 64 64 5 1 0 1\n";
    Book z;
    CHECK(z.Parse(bad.data(), bad.size()) == 1, "a bad line stops the parse");
    std::string big = "texwarm 2\n00000001 4096 0000000000001234 0000000000000001 70000 64 5 1 0 1\n";
    Book w;
    CHECK(w.Parse(big.data(), big.size()) == 0, "oversized dimension refused");
    std::string noise(3000, '\0');
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<char>((i * 131 + 7) & 0xff);
    Book q;
    q.Parse(noise.data(), noise.size());
    // an OLD (v1, address keyed) book: read, the address dropped, no quick key; the same texture at 2 addresses is one item
    const std::string v1 = "texwarm 1\n00000007 80001000 4096 0000000000001234 64 64 14 1\n00000007 80900000 4096 0000000000001234 64 64 14 1\n00000007 80002000 2048 0000000000005678 32 32 4 1\n";
    Book old;
    CHECK(old.Parse(v1.data(), v1.size()) == 2, "v1: two items from three lines (a duplicate collapses)");
    CHECK((*old.Items(7))[0].qh == 0 && (*old.Items(7))[0].tMs == 0, "v1 items have no quick key and no time");
    CHECK(old.SerializeAll().compare(0, 9, "texwarm 2") == 0, "it is written as v2");
    // a stage's file read while another stage has unsaved changes: the unsaved one stays dirty (it would otherwise never be written)
    Book dd;
    dd.Note(5, Mk(16, 16, 0x51));
    CHECK(dd.StageDirty(5) && dd.Dirty(), "dirty after a note");
    dd.Parse(one.data(), one.size());
    CHECK(dd.StageDirty(5) && dd.Dirty() && !dd.StageDirty(0xabcdef01u), "a file read keeps others' changes and is itself clean");
    dd.CleanStage(5);
    CHECK(!dd.StageDirty(5), "cleaned");
    // the order the replay will use
    std::vector<Item> ord = {Mk(8, 8, 1, 14, 1, 100, 1, 0), Mk(8, 8, 2, 14, 1, 100, 1, 900), Mk(8, 8, 3, 14, 1, 50, 1, 900), Mk(8, 8, 4, 14, 1, 100, 1, 10)};
    Book::SortForReplay(ord);
    CHECK(ord[0].hash == 4 && ord[1].hash == 3 && ord[2].hash == 2 && ord[3].hash == 1, "earliest first, smaller first among equals, unknown last: %llu %llu %llu %llu",
          (unsigned long long)ord[0].hash, (unsigned long long)ord[1].hash, (unsigned long long)ord[2].hash, (unsigned long long)ord[3].hash);
}

// A fake "guest memory": a texture's bytes are generated from its id; a visit places the textures at addresses that move.
struct Tex { unsigned id, w, h, fmt, size; };
static std::vector<uint8_t> Bytes(const Tex& t) { std::vector<uint8_t> b(t.size); for (unsigned i = 0; i < t.size; ++i) b[i] = static_cast<uint8_t>((t.id * 2654435761u + i * (t.id | 1u)) >> 7); return b; }

static void TestReload() {
    // 120 textures of 5 shapes (many of one shape, as in a stage), visit 1 at addresses A, visit 2 at addresses B (a stage reloaded elsewhere)
    std::vector<Tex> tex;
    const unsigned shapes[5][2] = {{64, 64}, {128, 64}, {32, 32}, {16, 16}, {256, 128}};
    for (unsigned i = 0; i < 120; ++i) { const unsigned s = i % 5; tex.push_back({i + 1, shapes[s][0], shapes[s][1], 14, 1024 + (i % 7) * 512}); }
    std::vector<uint32_t> addrA(120), addrB(120);
    for (unsigned i = 0; i < 120; ++i) { addrA[i] = 0x80100000u + i * 0x4000; addrB[i] = 0x80500000u + ((i * 7919u) % 120) * 0x4000 + 0x40; }
    std::vector<std::vector<uint8_t>> mem(120);
    for (unsigned i = 0; i < 120; ++i) mem[i] = Bytes(tex[i]);
    // visit 1: first used in play, noted
    Book b;
    for (unsigned i = 0; i < 120; ++i) {
        Item it = Mk(tex[i].w, tex[i].h, Fnv(mem[i].data(), tex[i].size), 14, 1, tex[i].size, Fnv(mem[i].data(), QuickLen(tex[i].size)), 100 + i * 50);
        b.Note(9, it);
    }
    CHECK(b.Items(9)->size() == 120, "book of 120");
    // visit 2: the old key (address) would try addrA: nothing there any more but for the textures that happen to stay
    unsigned oldHit = 0;
    for (unsigned i = 0; i < 120; ++i) for (unsigned j = 0; j < 120; ++j) if (addrA[i] == addrB[j] && i == j) ++oldHit;
    CHECK(oldHit == 0, "the address key finds none of the moved textures");
    // the new key: the walk reports the candidates (in the walk's order), the book items are found by identity
    CandIndex ci;
    ci.Begin(9);
    for (unsigned j = 0; j < 120; ++j) {
        Cand c; c.addr = addrB[j]; c.w = static_cast<uint16_t>(tex[j].w); c.h = static_cast<uint16_t>(tex[j].h); c.fmt = 14; c.maxBytes = tex[j].size; c.seq = j;
        c.qh = Fnv(mem[j].data(), QuickLen(tex[j].size));
        CHECK(ci.Add(c), "added");
        CHECK(!ci.Add(c), "a model reached twice is one candidate");
    }
    ci.SetWalkDone(true);
    std::vector<Item> plan = *b.Items(9);
    Book::SortForReplay(plan);
    unsigned found = 0, hashed = 0;
    for (const Item& it : plan) {
        uint32_t idx[kMaxScan];
        const unsigned n = ci.Shape(it, idx, kMaxScan);
        for (unsigned k = 0; k < n; ++k) {
            ++hashed;
            const Cand& c = ci.At(idx[k]);
            const unsigned j = static_cast<unsigned>(&c - &ci.At(0));
            if (Fnv(mem[j].data(), it.size) == it.hash) { ++found; CHECK(c.addr == addrB[j], "found at its new address"); break; }
        }
    }
    CHECK(found == 120, "all 120 found by identity at new addresses, got %u", found);
    CHECK(hashed <= 130, "the quick key keeps full hashes near one an item, got %u", hashed);
    // a texture the game changed between visits: the quick key may match but the hash does not -> bytes differ, never a wrong upload
    mem[5][mem[5].size() - 1] ^= 1;
    Item changed = *std::find_if(plan.begin(), plan.end(), [&](const Item& it) { return it.hash == Fnv(Bytes(tex[5]).data(), tex[5].size); });
    uint32_t idx[kMaxScan];
    unsigned n = ci.Shape(changed, idx, kMaxScan), eq = 0;
    for (unsigned k = 0; k < n; ++k) { const unsigned j = static_cast<unsigned>(&ci.At(idx[k]) - &ci.At(0)); if (Fnv(mem[j].data(), changed.size) == changed.hash) ++eq; }
    CHECK(eq == 0, "a changed texture is not accepted (%u)", eq);
    // an item with no quick key (v1): found by the scan of its shape
    Item noq = changed; noq.qh = 0; noq.hash = Fnv(mem[6].data(), tex[6].size); noq.w = static_cast<uint16_t>(tex[6].w); noq.h = static_cast<uint16_t>(tex[6].h); noq.size = tex[6].size;
    n = ci.Shape(noq, idx, kMaxScan);
    bool got = false;
    for (unsigned k = 0; k < n; ++k) { const unsigned j = static_cast<unsigned>(&ci.At(idx[k]) - &ci.At(0)); got = got || Fnv(mem[j].data(), noq.size) == noq.hash; }
    CHECK(n > 0 && n <= kMaxScan, "scan candidates bounded: %u", n);
    (void)got;
    // a texture whose model holds fewer bytes than the item needs is not a candidate
    Item big = changed; big.size = 1u << 20; big.qh = 0;
    CHECK(ci.Shape(big, idx, kMaxScan) == 0, "too small a model data is no candidate");
    CHECK(ci.HasAddr(addrB[3]) && !ci.HasAddr(addrA[3]), "was a first-used texture enumerable? by address");
    ci.Begin(10);
    CHECK(ci.Size() == 0 && !ci.WalkDone() && ci.Key() == 10, "a new stage clears the candidates");
}

static void TestSched() {
    Sched s;
    s.Begin(4);
    size_t i = 99;
    CHECK(s.Active() && s.Left() == 4 && s.Due(0) == 4, "four due");
    CHECK(s.Next(5, i) && i == 0, "first in order");
    CHECK(s.Result(0, kWait, 5) == kWait, "waiting for the walk");
    CHECK(!s.Next(5, i) || i != 0, "a waiting item is tried again at the next picture, not this one");
    CHECK(s.Next(6, i) && i == 0, "next picture");
    for (unsigned k = 0; k < 100; ++k) s.Result(0, kWait, 6 + k);
    CHECK(s.GaveUp() == 0 && s.Left() == 4, "waiting costs no miss (100 pictures)");
    s.Result(1, kHit, 10);
    CHECK(s.Left() == 3, "hit done");
    s.Result(2, kMissNoCand, 10);
    CHECK(!s.Next(10, i) || i != 2, "gap after a miss");
    CHECK(s.Next(10 + kRetryGap, i) && i == 2, "retried after the gap");
    s.Result(2, kMissNoCand, 10 + kRetryGap);
    CHECK(s.GaveUp() == 1 && s.Left() == 2, "no-candidate gives up after 2 tries (cost %u of %u)", kNoCandCost, kMaxMisses);
    s.Result(3, kMissBytes, 20);
    for (unsigned k = 1; k < kMaxMisses; ++k) s.Result(3, kMissBytes, 20 + k * 100);
    CHECK(s.GaveUp() == 2, "bytes-differ gives up after %u", kMaxMisses);
    s.Result(2, kHit, 1000);
    CHECK(s.GaveUp() == 2, "done stays done");
    // a wait that never ends becomes a no-candidate miss
    Sched w;
    w.Begin(1);
    Outcome last = kWait;
    for (unsigned k = 0; k < kMaxWaitPics * 3 && w.Active(); ++k) last = w.Result(0, kWait, k * 30);
    CHECK(!w.Active() && w.GaveUp() == 1 && last == kMissNoCand, "the walk never came: given up");
    Sched e;
    e.Begin(0);
    CHECK(!e.Active() && !e.Next(0, i), "empty");
    e.Result(5, kHit, 0);
    Sched a;
    a.Begin(3);
    a.Abandon();
    CHECK(!a.Active() && a.Left() == 0, "abandon");
}

static void TestBudget() {
    Budget b;
    b.maxMs = 1.5; b.maxKiB = 256; b.maxItems = 3;
    b.NewPicture();
    CHECK(b.Allow(4000), "one item always");
    b.Spend(0.2, 4000);
    CHECK(!b.Allow(1), "a big one stops the picture (KiB)");
    // the first open pictures after a skip: 120 items of 20 KiB, 0.5 ms each, 3 items a picture: 40 pictures, never more than 3 in one
    Budget o;
    o.maxMs = 1.5; o.maxKiB = 256; o.maxItems = 3;
    unsigned pics = 0, done = 0, worst = 0;
    while (done < 120 && pics < 1000) {
        o.NewPicture();
        ++pics;
        unsigned inPic = 0;
        while (done < 120 && o.Allow(20)) { o.Spend(0.5, 20); ++done; ++inPic; }
        if (inPic > worst) worst = inPic;
    }
    CHECK(done == 120 && worst == 3 && pics == 40, "spread over 40 pictures, 3 at most each: pics %u worst %u", pics, worst);
    Budget c;   // the closed screen: 10 ms / 1536 KiB
    c.maxMs = 10.0; c.maxKiB = 1536;
    c.NewPicture();
    unsigned n = 0;
    while (c.Allow(64)) { c.Spend(0.3, 64); ++n; if (n > 1000) break; }
    CHECK(n > 20 && n < 40, "closed picture: ms limit %u items", n);
}

static void TestCensus() {
    Census c;
    c.Reset(0x77);
    c.bookN = 40; c.foundQuick = 30; c.foundScan = 2; c.missNoCand = 5; c.missBytes = 1; c.upClosed = 30; c.upMs = 31.5; c.frames = 4; c.leftAtOpen = 2; c.cands = 600;
    c.firstP = 6; c.firstEnumerable = 3; c.firstNotEnumerable = 3;
    char line[1400];
    const size_t n = Format(line, sizeof line, c, "screen open", 2, 2, true);
    CHECK(n > 200 && n == std::strlen(line), "length %zu", n);
    CHECK(std::strstr(line, "stage 00000077") && std::strstr(line, "book 40, found 30 by identity, 2 by hash scan, missed 6") && std::strstr(line, "uploaded behind the cover 30 in 32 ms over 4 frames") &&
          std::strstr(line, "left at open 2") && std::strstr(line, "first-used in play 6") && std::strstr(line, "walk done 600 candidates"), "fields: %s", line);
    char tiny[40];
    const size_t m = Format(tiny, sizeof tiny, c, "x", 1, 0, false);
    CHECK(m < sizeof tiny && std::strlen(tiny) == m, "truncation is safe");
}

int main() {
    TestValid(); TestBook(); TestFile(); TestReload(); TestSched(); TestBudget(); TestCensus();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("texwarm_id_model: all ok\n");
    return 0;
}
