// GXV_BLOOM_BRACKET (bloom37): the decision, the owner tags, the census and the log line (vgx_bloombracket.h). No GL.
#include "vgx_bloombracket.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace vgx::bb;

static Inputs Good() {
    Inputs i;
    i.mode = 1;
    i.dead = true;
    i.markersOk = true;
    i.copy.present = true;
    i.copy.gxFmt = 0x06;
    i.copy.w = 640; i.copy.h = 456; i.copy.dstW = 640; i.copy.dstH = 456;
    i.copy.identity = true;
    for (DrawFacts* d : {&i.fill, &i.put}) {
        d->present = true; d->cover = true; d->colorWrite = true; d->alphaWrite = true;
        d->w = 640; d->h = 456;
    }
    return i;
}
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    int fails = 0;
    CHECK(Decide(Good()) == kDropped);
    { Inputs i = Good(); i.dead = false; CHECK(Decide(i) == kLive); }
    { Inputs i = Good(); i.hrw = 5; CHECK(Decide(i) == kHrw); }
    { Inputs i = Good(); i.host = kHostMsaa; CHECK(Decide(i) == kMsaa); }
    { Inputs i = Good(); i.host = kHostFrame; CHECK(Decide(i) == kHost); }
    { Inputs i = Good(); i.markersOk = false; CHECK(Decide(i) == kMarkers); }
    { Inputs i = Good(); i.fill.present = false; CHECK(Decide(i) == kMarkers); }
    { Inputs i = Good(); i.foreignBetween = true; CHECK(Decide(i) == kForeign); }
    // the capture
    { Inputs i = Good(); i.copy.gxFmt = 0x04; CHECK(Decide(i) == kCopyShape); }          // RGB565 quantises: not an identity
    { Inputs i = Good(); i.copy.identity = false; CHECK(Decide(i) == kCopyShape); }
    { Inputs i = Good(); i.copy.dstW = 320; CHECK(Decide(i) == kCopyShape); }             // box-halved
    { Inputs i = Good(); i.copy.x = 8; CHECK(Decide(i) == kCopyShape); }
    { Inputs i = Good(); i.copy.clearEnable = true; CHECK(Decide(i) == kCopyShape); }    // the copy's own clear would be lost
    { Inputs i = Good(); i.copy.depth = true; CHECK(Decide(i) == kCopyShape); }
    { Inputs i = Good(); i.copy.fromPass = true; CHECK(Decide(i) == kCopyShape); }
    { Inputs i = Good(); i.copy.skip = true; CHECK(Decide(i) == kCopyShape); }
    // the two draws
    { Inputs i = Good(); i.fill.cover = false; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.put.blend = true; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.put.depthWrite = true; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.fill.depthTest = true; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.put.dstAlphaZero = 1; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.fill.stOp = 1; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.fill.colorWrite = false; CHECK(Decide(i) == kDrawShape); }
    { Inputs i = Good(); i.fill.w = 639; CHECK(Decide(i) == kDrawShape); }               // the fill's black would peek out
    { Inputs i = Good(); i.put.h = 528; CHECK(Decide(i) == kDrawShape); }                // the put-back reaches beyond the capture
    // planes: the fill wrote alpha, the put-back does not: refused; the other way round and neither: fine; no alpha in the EFB: fine
    { Inputs i = Good(); i.put.alphaWrite = false; CHECK(Decide(i) == kPlanes); }
    { Inputs i = Good(); i.put.alphaWrite = false; i.efbAlpha = false; CHECK(Decide(i) == kDropped); }
    { Inputs i = Good(); i.fill.alphaWrite = false; CHECK(Decide(i) == kDropped); }
    { Inputs i = Good(); i.fill.alphaWrite = false; i.put.alphaWrite = false; CHECK(Decide(i) == kDropped); }
    { Inputs i = Good(); i.sharedReader = true; CHECK(Decide(i) == kSharedRead); }
    // the order of the tests: a live bracket is live whatever else (never a refusal); HRW before the shape tests
    { Inputs i = Good(); i.dead = false; i.hrw = 1; i.copy.gxFmt = 4; CHECK(Decide(i) == kLive); }
    { Inputs i = Good(); i.hrw = 1; i.copy.gxFmt = 4; CHECK(Decide(i) == kHrw); }
    // owner tags: round trip, never an HRW key (id << 8 | phase with small ids, 0xFE for the sky)
    for (unsigned id = 1; id < 64; ++id)
        for (unsigned role = 1; role <= 2; ++role) {
            const uint16_t t = OwnerTag(id, role);
            CHECK(IsOwnerTag(t) && OwnerId(t) == id && OwnerRole(t) == role);
        }
    CHECK(!IsOwnerTag(0) && !IsOwnerTag(static_cast<uint16_t>((1u << 8) | 2u)) && !IsOwnerTag(static_cast<uint16_t>(0xFEu << 8 | 5u)));
    CHECK(OwnerTag(1, 1) != OwnerTag(1, 2) && OwnerTag(1, 1) != OwnerTag(2, 1));
    // the census and its line
    Census c;
    c.Picture(true);
    c.Note(kLive); c.Note(kLive); c.Note(kDropped); c.Note(kDropped); c.Note(kForeign); c.Note(kMsaa);
    CHECK(c.brackets == 6 && c.dead == 4 && c.dropped == 2 && c.reason[kForeign] == 1 && c.reason[kMsaa] == 1 && c.reason[kLive] == 0);
    char line[600];
    CHECK(c.Line(line, sizeof line, 300, 1) == 0);   // not due
    CHECK(c.Line(line, sizeof line, 6, 1) > 0);
    std::printf("%s\n", line);
    CHECK(std::strstr(line, "dead 4 (67%)") && std::strstr(line, "dropped 2") && std::strstr(line, "foreign command 1") && std::strstr(line, "msaa 1"));
    Census c2;
    c2.Note(kDropped);
    CHECK(c2.Line(line, sizeof line, 1, 2) > 0 && std::strstr(line, "would drop 1") && std::strstr(line, "refused: none"));
    CHECK(HostBlock().load() == 0);
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("PASS: vgx_bloombracket_test\n");
    return 0;
}
