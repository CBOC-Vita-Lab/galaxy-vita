// endwait26 host model: GXV_TEXWARM's record / replay / budget / census logic (platform/vgx/src/vgx_texwarm.h).  bash tools/test_texwarm.sh
#include <cstdio>
#include <cstring>
#include <string>

#include "../platform/vgx/src/vgx_texwarm.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vgx::texwarm;

static Item Mk(uint32_t addr, unsigned w, unsigned h, unsigned fmt = 5, unsigned lv = 1, uint32_t size = 4096, uint64_t hash = 0x1234) {
    Item it;
    it.addr = addr; it.w = static_cast<uint16_t>(w); it.h = static_cast<uint16_t>(h); it.fmt = static_cast<uint8_t>(fmt); it.levels = static_cast<uint8_t>(lv);
    it.size = size; it.hash = hash;
    return it;
}

static void TestValid() {
    CHECK(Valid(Mk(0x80001000, 64, 64)), "plain item valid");
    CHECK(!Valid(Mk(0x80001000, 64, 64, kFmtC4)), "CI4 excluded");
    CHECK(!Valid(Mk(0x80001000, 64, 64, kFmtC8)), "CI8 excluded");
    CHECK(!Valid(Mk(0x80001000, 64, 64, kFmtC14X2)), "CI14X2 excluded");
    CHECK(!Valid(Mk(0, 64, 64)), "address 0");
    CHECK(!Valid(Mk(0x1000, 0, 64)), "zero width");
    CHECK(!Valid(Mk(0x1000, 2048, 64)), "too wide");
    CHECK(!Valid(Mk(0x1000, 64, 64, 5, 1, kMaxSize + 1)), "too big");
    CHECK(!Valid(Mk(0x1000, 64, 64, 5, 1, 4096, 0)), "hash 0");
    CHECK(!Valid(Mk(0x1000, 64, 64, 5, 13)), "13 levels");
}

static void TestWindow() {
    CHECK(!InWindow(100, 0), "screen never opened");
    CHECK(InWindow(100, 100), "at open");
    CHECK(InWindow(100 + kWindowMs, 100), "at the end of the window");
    CHECK(!InWindow(100 + kWindowMs + 1, 100), "after the window");
    CHECK(!InWindow(50, 100), "before open");
}

static void TestBook() {
    Book b;
    CHECK(b.Note(7, Mk(0x1000, 32, 32)), "first note is new");
    CHECK(!b.Note(7, Mk(0x1000, 32, 32)), "duplicate is not");
    CHECK(b.Note(7, Mk(0x1000, 32, 32, 5, 1, 4096, 0x99)), "same place with other bytes updates");
    CHECK(b.Items(7)->size() == 1 && (*b.Items(7))[0].hash == 0x99, "updated in place");
    CHECK(!b.Note(0, Mk(0x2000, 32, 32)), "stage 0 refused");
    CHECK(!b.Note(7, Mk(0x2000, 32, 32, kFmtC8)), "CI refused");
    for (unsigned i = 0; i < kMaxItems + 10; ++i) b.Note(8, Mk(0x10000 + i * 64, 16, 16));
    CHECK(b.Items(8)->size() == kMaxItems, "items per stage capped at %u, got %zu", kMaxItems, b.Items(8)->size());
    Book c;
    for (unsigned s = 1; s < kMaxStages + 20; ++s) c.Note(s, Mk(0x1000, 8, 8));
    CHECK(c.Stages() == kMaxStages, "stages capped, got %zu", c.Stages());
    CHECK(b.KiB(8) == kMaxItems * 4096 / 1024, "KiB sum, got %llu", static_cast<unsigned long long>(b.KiB(8)));
    CHECK(b.Items(99) == nullptr, "unknown stage");
}

static void TestFile() {
    Book b;
    b.Note(0xabcdef01u, Mk(0x80123000, 64, 32, 4, 3, 5000, 0xdeadbeefcafef00dull));
    b.Note(0xabcdef01u, Mk(0x80124000, 128, 128, 14, 1, 8192));
    b.Note(2, Mk(0x80125000, 16, 16));
    const std::string s = b.Serialize();
    Book r;
    unsigned n = r.Parse(s.data(), s.size());
    CHECK(n == 3, "3 items round-trip, got %u", n);
    CHECK(!r.Dirty(), "a loaded book is clean");
    CHECK(r.Items(0xabcdef01u) && r.Items(0xabcdef01u)->size() == 2, "stage items");
    const Item& it = (*r.Items(0xabcdef01u))[0];
    CHECK(it.addr == 0x80123000 && it.w == 64 && it.h == 32 && it.fmt == 4 && it.levels == 3 && it.size == 5000 && it.hash == 0xdeadbeefcafef00dull, "fields");
    CHECK(r.Serialize() == s, "serialize is stable");
    // robustness
    Book x;
    CHECK(x.Parse("", 0) == 0, "empty");
    CHECK(x.Parse("garbage\n1 2 3\n", 14) == 0, "wrong magic");
    std::string cut = s.substr(0, s.size() - 7);   // last line cut short
    Book y;
    n = y.Parse(cut.data(), cut.size());
    CHECK(n == 2, "a cut last line is dropped, got %u", n);
    std::string bad = std::string("texwarm 1\n") + "00000001 80001000 4096 0000000000001234 64 64 5 1\nzzz zzz\n00000001 80002000 4096 0000000000001234 64 64 5 1\n";
    Book z;
    n = z.Parse(bad.data(), bad.size());
    CHECK(n == 1, "a bad line stops the parse, got %u", n);
    std::string big = "texwarm 1\n00000001 80001000 4096 0000000000001234 70000 64 5 1\n";
    Book w;
    CHECK(w.Parse(big.data(), big.size()) == 0, "oversized dimension refused");
    // binary noise must not crash
    std::string noise(2000, '\0');
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<char>((i * 131 + 7) & 0xff);
    Book v;
    v.Parse(noise.data(), noise.size());
}

static void TestBudget() {
    Budget b;
    b.maxMs = 3.0; b.maxKiB = 512;
    b.NewPicture();
    CHECK(b.Allow(4000), "one item is always allowed, however big");
    b.Spend(0.5, 4000);
    CHECK(!b.Allow(1), "after a big one the KiB limit stops more");
    b.NewPicture();
    b.Spend(1.0, 100);
    CHECK(b.Allow(100), "room left");
    b.Spend(2.5, 100);
    CHECK(!b.Allow(1), "ms limit reached");
    b.NewPicture();
    CHECK(b.Allow(1), "new picture, new budget");
    // spreading: 40 items of 64 KiB under 512 KiB a picture take >= 5 pictures
    Budget c;
    unsigned pics = 0, done = 0;
    while (done < 40) {
        c.NewPicture();
        ++pics;
        while (done < 40 && c.Allow(64)) { c.Spend(0.1, 64); ++done; }
        if (pics > 100) break;
    }
    CHECK(pics == 5, "40 x 64 KiB at 512 KiB a picture = 5 pictures, got %u", pics);
}

static void TestReplay() {
    std::vector<Item> v = {Mk(0x1000, 8, 8), Mk(0x2000, 8, 8), Mk(0x3000, 8, 8)};
    Replay r;
    r.Begin(&v);
    CHECK(r.Active() && r.Left() == 3, "three to do");
    size_t i = 99;
    CHECK(r.Next(10, i) && i == 0, "first in order");
    r.Result(0, kHit, 10);
    CHECK(r.Next(10, i) && i == 1, "next");
    r.Result(1, kMissMemory, 10);
    CHECK(r.Next(10, i) && i == 2, "a missed one is skipped for now");
    r.Result(2, kAlready, 10);
    CHECK(!r.Next(10, i), "nothing due until the retry gap");
    CHECK(!r.Next(10 + kRetryGap - 1, i), "not yet");
    CHECK(r.Next(10 + kRetryGap, i) && i == 1, "retried after the gap");
    for (unsigned k = 0; k < kMaxMisses; ++k) { r.Result(1, kMissBytes, 100 + k); }
    CHECK(!r.Active(), "given up after %u misses", kMaxMisses);
    CHECK(r.GaveUp() == 1, "one given up, got %u", r.GaveUp());
    r.Result(1, kHit, 500);  // after done: ignored
    CHECK(r.Left() == 0, "done stays done");
    Replay e;
    e.Begin(nullptr);
    CHECK(!e.Active() && !e.Next(0, i), "empty replay");
    std::vector<Item> none;
    e.Begin(&none);
    CHECK(!e.Active(), "no items");
    e.Result(5, kHit, 0);   // out of range: ignored
}

static void TestCensus() {
    Census c;
    c.Reset(0x77);
    bool over = false;
    c.FirstUse(100, 1.0, false, 0.0, 3.0, over);
    CHECK(!over && c.deferN == 0, "under budget");
    c.FirstUse(900, 2.5, false, 1.0, 3.0, over);
    CHECK(over && c.picsOver == 1 && c.deferN == 1 && c.deferKiB == 900, "first to cross the line counts the picture and would be deferred");
    c.FirstUse(50, 1.0, false, 3.5, 3.0, over);
    CHECK(!over && c.picsOver == 1 && c.deferN == 2, "later ones in the same picture defer without a new picture");
    c.FirstUse(300, 4.0, true, 4.5, 3.0, over);
    CHECK(c.deferN == 2 && c.firstUseN == 4, "Mario's is never counted as deferred");
    CHECK(c.worstPicMs > 8.4 && c.worstPicMs < 8.6, "worst picture, got %.2f", c.worstPicMs);
    char line[1200];
    const size_t n = Format(line, sizeof line, c, "screen open", 2, 12);
    CHECK(n > 100 && n == std::strlen(line), "format length %zu", n);
    CHECK(std::strstr(line, "stage 00000077") && std::strstr(line, "GXV_TEXWARM=2") && std::strstr(line, "book 12 items"), "format fields: %s", line);
    char tiny[40];
    const size_t m = Format(tiny, sizeof tiny, c, "x", 1, 0);
    CHECK(m < sizeof tiny && std::strlen(tiny) == m, "truncation is safe");
}

int main() {
    TestValid(); TestWindow(); TestBook(); TestFile(); TestBudget(); TestReplay(); TestCensus();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("texwarm_model: all ok\n");
    return 0;
}
