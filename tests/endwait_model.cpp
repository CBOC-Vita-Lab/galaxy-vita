// endwait26 host model: GXV_ENDWAIT_LOG's record and line (platform/vgx/src/vgx_endwait.h).  bash tools/test_endwait.sh
#include <cstdio>
#include <cstring>

#include "../platform/vgx/src/vgx_endwait.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vgx::endwait;

static Pic Mk(uint64_t frame, int64_t s, int64_t e) {
    Pic p;
    p.frame = frame; p.startNs = s; p.endNs = e;
    return p;
}

static void TestPic() {
    Pic p = Mk(1, 1000000, 101000000);  // 100 ms
    p.idleUs = 20000; p.decodeUs = 10000; p.dlUs = 5000; p.joinUs = 8000; p.passUs = 7000; p.gateUs = 0;
    CHECK(p.busyUs() == 80000, "busy %u", p.busyUs());
    CHECK(p.parseUs() == 50000, "parse %u", p.parseUs());
    p.idleUs = 500000;
    CHECK(p.busyUs() == 0 && p.parseUs() == 0, "idle longer than the picture never goes negative");
    Pic q;
    CHECK(!q.valid(), "empty is invalid");
}

static void TestRing() {
    Ring r;
    CHECK(r.Count() == 0, "empty ring");
    Pic out[4];
    CHECK(r.Overlapping(0, 1000, out, 4) == 0, "nothing in an empty ring");
    for (unsigned i = 0; i < 40; ++i) r.Push(Mk(i + 1, i * 10000000ll, i * 10000000ll + 9000000));
    CHECK(r.Count() == kRing && r.Total() == 40, "ring keeps %u", kRing);
    CHECK(r.Back(0).frame == 40 && r.Back(kRing - 1).frame == 40 - kRing + 1, "newest and oldest");
    // pictures 38..40 (index 37..39: 370..399 ms) overlap [375, 395] ms
    unsigned n = r.Overlapping(375000000, 385000000, out, 4);
    CHECK(n == 2 && out[0].frame == 38 && out[1].frame == 39, "overlap got %u (%llu..)", n, n ? static_cast<unsigned long long>(out[0].frame) : 0ull);
    n = r.Overlapping(0, 400000000, out, 3);
    CHECK(n == 3 && out[2].frame == 40, "cap keeps the newest, got %u", n);
    CHECK(r.Overlapping(1, 2, out, 4) == 0, "older than the ring");
    CHECK(r.Overlapping(900000000, 990000000, out, 4) == 0, "newer than the ring");
}

static void TestStage() {
    StageClock c;
    c.Seen(5, 1, 100);
    CHECK(c.key == 5 && c.startNs == 100 && c.openNs == 0, "started closed");
    c.Seen(5, 0, 200);
    CHECK(c.openNs == 200, "opened");
    c.Seen(5, 0, 300);
    CHECK(c.openNs == 200, "open time kept");
    unsigned ok = 0;
    for (unsigned i = 0; i < 100; ++i) ok += c.LineAllowed() ? 1 : 0;
    CHECK(ok == kLinesPerStage, "lines capped at %u, got %u", kLinesPerStage, ok);
    c.Seen(6, 0, 400);
    CHECK(c.startNs == 400 && c.openNs == 400 && c.LineAllowed(), "a new stage resets");
    StageClock z;
    z.Seen(0, 0, 10);
    CHECK(z.openNs == 0, "no key, never open");
}

static void TestFormat() {
    Inputs in;
    in.stageKey = 0x2f7c8bbc; in.sinceStartS = 8.4; in.sinceOpenS = 8.3; in.waitMs = 363;
    in.waitStartNs = 1000000000;
    char line[4096];
    size_t n = Format(line, sizeof line, in);
    CHECK(n == std::strlen(line) && std::strstr(line, "no picture record overlaps"), "no pictures: %s", line);
    in.nPics = 1;
    in.pics[0] = Mk(28777, 1000000000, 1363000000);
    in.pics[0].decodeUs = 20000; in.pics[0].decodeN = 3; in.pics[0].decodeKiB = 900; in.pics[0].dlUs = 250000; in.pics[0].dlN = 40; in.pics[0].idleUs = 5000;
    in.be.stage = 0; in.be.sinceMs = 395; in.be.jobs = 0; in.be.limit = 2; in.be.freeBufs = 2;
    in.haveBeDone = true; in.done.pictures = 2; in.done.targetMs = 80; in.done.targetsMade = 1;
    n = Format(line, sizeof line, in);
    CHECK(n == std::strlen(line), "length");
    CHECK(std::strstr(line, "stage 2f7c8bbc +8.4 s") && std::strstr(line, "screen open +8.3 s") && std::strstr(line, "waited 363 ms"), "head: %s", line);
    CHECK(std::strstr(line, "picture 28777") && std::strstr(line, "(3 tex, 900 KiB)"), "picture: %s", line);
    CHECK(std::strstr(line, "verdict: display-list builds"), "the biggest part is named: %s", line);
    CHECK(std::strstr(line, "target create 80 ms for 1 made"), "back end part: %s", line);
    in.sinceOpenS = -1; in.where = 2; in.stream.full = 3; in.stream.fullMs = 12;
    Format(line, sizeof line, in);
    CHECK(std::strstr(line, "screen closed") && std::strstr(line, "deferred draw-done") && std::strstr(line, "3 full-segment waits"), "variants: %s", line);
    char tiny[64];
    n = Format(tiny, sizeof tiny, in);
    CHECK(n < sizeof tiny && std::strlen(tiny) == n, "truncation safe (%zu)", n);
    BeDone d; d.targetMs = 100; d.targetsMade = 1; d.texMs = 3; d.texN = 4; d.texKiB = 64; d.finishMs = 2;
    n = FormatBe(line, sizeof line, 28777, 140, d);
    CHECK(n == std::strlen(line) && std::strstr(line, "frame 28777") && std::strstr(line, "target create 100 ms (1 made"), "be line: %s", line);
}

int main() {
    TestPic(); TestRing(); TestStage(); TestFormat();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("endwait_model: all ok\n");
    return 0;
}
