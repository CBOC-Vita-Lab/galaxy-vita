// sampler30: host test of the sampler's pure part (platform/include/port/sampler_ring.h): settings parsing, the fixed ring (wrap, dropped count),
// the in-place sort, the totals, the report file and the log summary, with fake samples. Writes the report to argv[1] if given (tools/test_sampler.sh
// feeds it to tools/sampler/symbolize.py).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "port/sampler_ring.h"

using namespace port::sampler;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void TestSettings() {
    CHECK(ParsePeriod(nullptr) == 1000 && ParsePeriod("") == 1000 && ParsePeriod("abc") == 1000, "defaults");
    CHECK(ParsePeriod("1") == 250 && ParsePeriod("249") == 250 && ParsePeriod("250") == 250, "low clamp");
    CHECK(ParsePeriod("2000") == 2000 && ParsePeriod("10000") == 10000 && ParsePeriod("10001") == 10000 && ParsePeriod("99999999") == 10000, "high clamp");
    CHECK(ParsePeriod("-5") == 250, "negative clamps low");
    CHECK(ParseSeconds(nullptr) == 0 && ParseSeconds("0") == 0 && ParseSeconds("-3") == 0 && ParseSeconds("x") == 0 && ParseSeconds("30") == 30 && ParseSeconds("99999") == 3600, "seconds");
    CHECK(ParseDumpSecs(nullptr) == 20 && ParseDumpSecs("") == 20 && ParseDumpSecs("x") == 20 && ParseDumpSecs("0") == 0 && ParseDumpSecs("-3") == 0 && ParseDumpSecs("7") == 7 && ParseDumpSecs("99999") == 3600, "dump seconds (0 = off)");
    ThreadSel t;
    ParseThreads(nullptr, t); CHECK(t.game && !t.render && !t.helper && !t.guest && t.nExtra == 0, "default is game");
    ParseThreads("", t); CHECK(t.game, "empty is game");
    ParseThreads("game,render,helper", t); CHECK(t.game && t.render && t.helper && !t.guest, "three groups");
    ParseThreads(" render , guest ", t); CHECK(!t.game && t.render && t.guest, "spaces trimmed, game not implied");
    ParseThreads("vgx-player,smg_par,x,y,z", t); CHECK(!t.game && t.nExtra == 4 && !strcmp(t.extra[0], "vgx-player") && !strcmp(t.extra[1], "smg_par"), "names kept, at most 4");
    ParseThreads(",,", t); CHECK(t.game, "only separators is game");
    CHECK(StageMatches("", "AnyGalaxy") && StageMatches(nullptr, "x"), "empty filter matches");
    CHECK(StageMatches("honey", "HoneyBeeKingdomGalaxy:0") && StageMatches("GALAXY:0", "HoneyBeeKingdomGalaxy:0") && !StageMatches("beach", "HoneyBeeKingdomGalaxy") && !StageMatches("x", nullptr), "stage filter");
    CHECK(!StageMatches("longer than the name", "short"), "filter longer than the stage");
}

static void TestRing() {
    std::vector<Rec> store(8);
    Ring r; r.Init(store.data(), 8);
    CHECK(r.Kept() == 0 && r.Dropped() == 0, "empty");
    for (uint32_t i = 0; i < 5; ++i) r.Push(0x1000 + i, 0x2000, MakeTag(0, kRun, 0));
    CHECK(r.Kept() == 5 && r.Dropped() == 0 && r.Pushed() == 5, "below capacity");
    for (uint32_t i = 5; i < 21; ++i) r.Push(0x1000 + i, 0x2000, MakeTag(0, kRun, 0));
    CHECK(r.Kept() == 8 && r.Dropped() == 13 && r.Pushed() == 21, "wrapped: kept %u dropped %llu", r.Kept(), (unsigned long long)r.Dropped());
    // the newest 8 pcs (0x100D..0x1014) must be what is kept
    std::vector<uint32_t> pcs; for (uint32_t i = 0; i < r.Kept(); ++i) pcs.push_back(r.Data()[i].pc);
    std::sort(pcs.begin(), pcs.end());
    CHECK(pcs.front() == 0x100D && pcs.back() == 0x1014, "newest kept: %x..%x", pcs.front(), pcs.back());
    r.Clear(); CHECK(r.Kept() == 0 && r.Pushed() == 0, "clear");
    Ring z; z.Push(1, 2, 3); CHECK(z.Kept() == 0, "uninitialised ring ignores pushes");
    // the tag packs and unpacks
    const uint32_t tag = MakeTag(7, kWait, 0x41);
    CHECK(TagSlot(tag) == 7 && TagKind(tag) == kWait && TagWait(tag) == 0x41, "tag round trip");
}

static void TestSort() {
    std::mt19937 rng(7);
    for (uint32_t n : {0u, 1u, 2u, 3u, 17u, 1000u, 5000u}) {
        std::vector<Rec> a(n);
        for (auto& x : a) x = {uint32_t(rng() % 50), uint32_t(rng() % 5), MakeTag(uint32_t(rng() % 3), (rng() & 1) ? kRun : kWait, uint32_t(rng() % 4))};
        std::vector<Rec> b = a;
        SortRecs(a.data(), n);
        std::sort(b.begin(), b.end(), RecLess);
        bool same = true;
        for (uint32_t i = 0; i < n; ++i) same = same && a[i].pc == b[i].pc && a[i].lr == b[i].lr && a[i].tag == b[i].tag;
        CHECK(same, "sort of %u records", n);
    }
}

static std::string ReadAll(FILE* f) {
    std::string s; char buf[512]; size_t n;
    std::rewind(f);
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    return s;
}

static Header MakeHeader() {
    Header h;
    h.periodUs = 1000; h.windowMs = 2500; strcpy(h.reason, "seconds"); strcpy(h.stage, "HoneyBeeKingdomGalaxy:0");
    h.pcWord = 15; h.lrWord = 14; strcpy(h.anchorName, "port_sampler_boot"); h.anchorAddr = 0x81234560;
    h.slots[0].used = true; h.slots[0].uid = 0x40010123; strcpy(h.slots[0].name, "game"); h.slots[0].fails = 2;
    h.slots[2].used = true; h.slots[2].uid = 0x40010456; strcpy(h.slots[2].name, "render");
    return h;
}

static void TestReport(const char* outPath) {
    std::vector<Rec> store(64);
    Ring r; r.Init(store.data(), 64);
    // game (slot 0): 10 samples in A<-B, 6 in A<-C, 4 in D<-B, 5 waiting on a semaphore (type 2), 3 delaying (type 65)
    for (int i = 0; i < 10; ++i) r.Push(0x81001000, 0x81002000, MakeTag(0, kRun, 0));
    for (int i = 0; i < 6; ++i) r.Push(0x81001000, 0x81003000, MakeTag(0, kRun, 0));
    for (int i = 0; i < 4; ++i) r.Push(0x81004000, 0x81002000, MakeTag(0, kRun, 0));
    for (int i = 0; i < 5; ++i) r.Push(0x0000AAAA, 0, MakeTag(0, kWait, 2));
    for (int i = 0; i < 3; ++i) r.Push(0x0000BBBB, 0, MakeTag(0, kWait, 65));
    // render (slot 2): 7 running, 1 waiting
    for (int i = 0; i < 7; ++i) r.Push(0x81005000, 0x81002000, MakeTag(2, kRun, 0));
    r.Push(1, 0, MakeTag(2, kWait, 3));
    SlotTotals tot[kMaxSlots];
    Totals(r, tot);
    CHECK(tot[0].run == 20 && tot[0].wait == 8 && tot[0].nWaitTypes == 2, "game totals %u/%u/%d", tot[0].run, tot[0].wait, tot[0].nWaitTypes);
    CHECK(tot[2].run == 7 && tot[2].wait == 1 && tot[1].run == 0, "render totals");
    Header h = MakeHeader();
    char line[600];
    FormatSummary(line, sizeof line, h, r, "ux0:data/smg/sampler-x.txt");
    std::printf("%s\n", line);
    CHECK(strstr(line, "game run 20 wait 8 fail 2;") && strstr(line, "render run 7 wait 1 fail 0;") && strstr(line, "2500 ms window (seconds)") &&
          strstr(line, "36 samples kept (0 dropped)") && strstr(line, "file ux0:data/smg/sampler-x.txt"), "summary line: %s", line);
    char tiny[40];
    const int n = FormatSummary(tiny, sizeof tiny, h, r, "f");
    (void)n; CHECK(strlen(tiny) < sizeof tiny, "summary never overruns a small buffer");
    FILE* f = std::tmpfile();
    const int lines = WriteReport(f, h, r);
    const std::string text = ReadAll(f);
    std::fclose(f);
    CHECK(text.rfind("SMGSAMPLER 1\n", 0) == 0, "magic");
    CHECK(text.find("kept 36\n") != std::string::npos && text.find("dropped 0\n") != std::string::npos && text.find("anchor port_sampler_boot 0x81234560\n") != std::string::npos, "header fields");
    CHECK(text.find("thread 0 40010123 game 2\n") != std::string::npos && text.find("thread 2 40010456 render 0\n") != std::string::npos, "thread lines");
    CHECK(text.find("run 0 20\n") != std::string::npos && text.find("wait 0 2 5\n") != std::string::npos && text.find("wait 0 65 3\n") != std::string::npos && text.find("wait 2 3 1\n") != std::string::npos, "run and wait lines");
    CHECK(text.find("pc 0 0x81001000 0x81002000 10\n") != std::string::npos && text.find("pc 0 0x81001000 0x81003000 6\n") != std::string::npos &&
          text.find("pc 0 0x81004000 0x81002000 4\n") != std::string::npos && text.find("pc 2 0x81005000 0x81002000 7\n") != std::string::npos, "pc lines");
    int count = 0; for (char c : text) count += c == '\n';
    CHECK(count == lines, "returned line count %d vs %d", lines, count);
    if (outPath) { FILE* o = std::fopen(outPath, "w"); if (o) { std::fputs(text.c_str(), o); std::fclose(o); } }

    // a wrapped ring reports only what it kept, and says what it dropped
    std::vector<Rec> small(4);
    Ring w; w.Init(small.data(), 4);
    for (int i = 0; i < 10; ++i) w.Push(0x81001000 + 4u * (i % 2), 0x81002000, MakeTag(0, kRun, 0));
    FILE* g = std::tmpfile();
    WriteReport(g, h, w);
    const std::string t2 = ReadAll(g); std::fclose(g);
    CHECK(t2.find("pushed 10\n") != std::string::npos && t2.find("kept 4\n") != std::string::npos && t2.find("dropped 6\n") != std::string::npos && t2.find("run 0 4\n") != std::string::npos, "wrapped report");
    // an empty ring writes a valid report
    std::vector<Rec> e(4);
    Ring er; er.Init(e.data(), 4);
    FILE* k = std::tmpfile(); WriteReport(k, h, er); const std::string t3 = ReadAll(k); std::fclose(k);
    CHECK(t3.find("kept 0\n") != std::string::npos && t3.find("run 0 0\n") != std::string::npos && t3.find("\npc ") == std::string::npos, "empty report");
}

int main(int argc, char** argv) {
    TestSettings(); TestRing(); TestSort(); TestReport(argc > 1 ? argv[1] : nullptr);
    if (g_fail) { std::printf("sampler_ring_test: %d FAILED\n", g_fail); return 1; }
    std::printf("sampler_ring_test: ok\n");
    return 0;
}
