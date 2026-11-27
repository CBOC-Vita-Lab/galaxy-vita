// GXV_TAILFOLD (tail37), day 1: the stretch scan, the refusals and the census line (vgx_tailfold.h). No GL.
#include "vgx_tailfold.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vgx::tf;

static Item Res() { return Item{}; }
static Item Leave() { Item i; i.kind = kLeave; return i; }
static Item Disp() { Item i; i.kind = kDisp; return i; }
static Item Dr() { Item i; i.kind = kDraw; return i; }
static Item PutBack() { Item i = Dr(); i.d.cover = true; i.d.textured = true; i.d.sourceWhole = true; i.d.sourceHandover = true; return i; }
static Inputs Fine() { Inputs i; i.clearWhole = true; return i; }
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static Reason Run(std::vector<Item> v, const Inputs& in, unsigned* stretch = nullptr) {
    const Result r = Scan(v.data(), v.size(), v.size() - 1, in);
    if (stretch) *stretch = r.stretch;
    return r.why;
}
int main() {
    int fails = 0;
    unsigned st = 0;
    // the bloom picture: scene draws, the glow's capture (leaves the EFB), the put-back, the glow add, a fade quad, 2D draws, the display copy
    { std::vector<Item> v = {Dr(), Dr(), Leave(), Res(), PutBack(), Dr(), Dr(), Disp()};
      CHECK(Run(v, Fine(), &st) == kFoldable && st == 3); }
    // no bloom: the scene's draws only: the first draw is not a covering textured quad
    { std::vector<Item> v = {Leave(), Dr(), Dr(), Disp()}; CHECK(Run(v, Fine()) == kFirstQuad); }
    // nothing after the last leave
    { std::vector<Item> v = {Dr(), Leave(), Res(), Disp()}; CHECK(Run(v, Fine()) == kNoTail); }
    // a put-back that lies before a later capture (DoF after the bloom): the stretch after the capture does not hold it
    { std::vector<Item> v = {Dr(), PutBack(), Dr(), Leave(), Dr(), Disp()}; CHECK(Run(v, Fine()) == kCopyInside); }
    // the put-back's source is not a handed-over buffer (a partial copy, or no swap)
    { Item p = PutBack(); p.d.sourceHandover = false; std::vector<Item> v = {Leave(), p, Dr(), Disp()}; CHECK(Run(v, Fine()) == kNotHandover); }
    { Item p = PutBack(); p.d.blend = true; std::vector<Item> v = {Leave(), p, Dr(), Disp()}; CHECK(Run(v, Fine()) != kFoldable); }
    // draws of the stretch
    { Item d = Dr(); d.d.depthTest = true; std::vector<Item> v = {Leave(), PutBack(), d, Disp()}; CHECK(Run(v, Fine()) == kDepth); }
    { Item d = Dr(); d.d.depthWrite = true; std::vector<Item> v = {Leave(), PutBack(), d, Disp()}; CHECK(Run(v, Fine()) == kDepth); }
    { Item d = Dr(); d.d.dstAlpha = true; std::vector<Item> v = {Leave(), PutBack(), d, Disp()}; CHECK(Run(v, Fine()) == kDstAlpha); }
    { Item d = Dr(); d.d.stencil = true; std::vector<Item> v = {Leave(), PutBack(), d, Disp()}; CHECK(Run(v, Fine()) == kStencil); }
    { Item d = Dr(); d.d.effect = true; std::vector<Item> v = {Leave(), PutBack(), d, Disp()}; CHECK(Run(v, Fine()) == kEffectTarget); }
    // the picture-wide refusals, each alone
    std::vector<Item> ok = {Leave(), PutBack(), Dr(), Disp()};
    CHECK(Run(ok, Fine()) == kFoldable);
    { Inputs i = Fine(); i.hrw = 5; CHECK(Run(ok, i) == kHrw); }
    { Inputs i = Fine(); i.capture = true; CHECK(Run(ok, i) == kCapture); }
    { Inputs i = Fine(); i.msaa = true; CHECK(Run(ok, i) == kMsaa); }
    { Inputs i = Fine(); i.quality = true; CHECK(Run(ok, i) == kQuality); }
    { Inputs i = Fine(); i.movie = true; CHECK(Run(ok, i) == kMovie); }
    { Inputs i = Fine(); i.edge = true; CHECK(Run(ok, i) == kEdge); }
    { Inputs i = Fine(); i.rung1to1 = false; CHECK(Run(ok, i) == kRung); }
    { Inputs i = Fine(); i.clearWhole = false; CHECK(Run(ok, i) == kColourLive); }
    // no display copy at the given index: not a picture
    { std::vector<Item> v = {Dr(), Dr()}; const Result r = Scan(v.data(), v.size(), 1, Fine()); CHECK(r.why == kNoPicture); }
    // a leave inside the stretch's run ends it: the stretch is only what follows the LAST leave
    { std::vector<Item> v = {Leave(), PutBack(), Dr(), Leave(), PutBack(), Disp()}; CHECK(Run(v, Fine(), &st) == kFoldable && st == 1); }
    // tailfold39 (GXV_TAILFOLD=3): the refusals the apply mode adds
    { Item d = Dr(); d.d.noProj = true; std::vector<Item> v = {Leave(), PutBack(), d, Disp()}; CHECK(Run(v, Fine()) == kNoProj); }
    { Inputs i = Fine(); i.presentPass = true; CHECK(Run(ok, i) == kPresentPass); }
    { Inputs i = Fine(); i.presentPass = true; i.edge = true; CHECK(Run(ok, i) == kEdge); } // (the older reasons come first)
    { Inputs i = Fine(); i.presentPass = true; i.clearWhole = false; CHECK(Run(ok, i) == kPresentPass); }
    // the census
    Census c;
    Result a; a.why = kFoldable; a.stretch = 4;
    Result b; b.why = kEdge; b.stretch = 3;
    Result n; n.why = kNoPicture; n.stretch = 9;
    c.Note(a); c.Note(a); c.Note(b); c.Note(n);
    CHECK(c.pictures == 3 && c.foldable == 2 && c.reason[kEdge] == 1 && c.stretchDraws == 11 && c.foldableDraws == 8);
    char line[800];
    CHECK(c.Line(line, sizeof line, 600, 2) == 0);
    CHECK(c.Line(line, sizeof line, 3, 2) > 0);
    std::printf("%s\n", line);
    CHECK(std::strstr(line, "3 pictures, foldable 2 (67%)") && std::strstr(line, "edge pass 1") && std::strstr(line, "4.0 in the foldable"));
    // mode 3: the folds are counted when the picture ends, the line says "applied"; the 10 s window line
    c.folded = 2; c.foldedDraws = 7; c.reason[kLate] = 1; c.lastLog = 0;
    CHECK(c.Line(line, sizeof line, 3, 3) > 0);
    CHECK(std::strstr(line, "GXV_TAILFOLD=3 (applied") && std::strstr(line, "folded 2 (67% of pictures), 3.5 draws") && std::strstr(line, "declined at run time 1"));
    Window w; w.pictures = 100; w.armed = 40; w.folded = 38; w.late = 2; w.foldedDraws = 380; w.sceneSumFolded = 76; w.sceneNFolded = 38; w.sceneSumOther = 300; w.sceneNOther = 60; w.refused[kEdge] = 5;
    CHECK(w.Line(line, sizeof line, 10.0) > 0);
    std::printf("%s\n", line);
    CHECK(std::strstr(line, "10.0 s: 100 pictures, armed 40, folded 38 (38%; 10.0 tail draws each)") && std::strstr(line, "EFB scenes saved 38") && std::strstr(line, "folded 2.00 over 38, others 5.00 over 60") &&
          std::strstr(line, "edge pass 5"));
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("PASS: vgx_tailfold_test\n");
    return 0;
}
