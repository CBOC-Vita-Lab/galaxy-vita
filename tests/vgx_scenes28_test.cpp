// scenes28: host test of platform/vgx/src/vgx_scenes28.h (the corner pass's plan, the exact resample map, the HUD grid, the A/B phase). tools/test_vgx_scenes28.sh
#include "../platform/vgx/src/vgx_scenes28.h"
#include <cmath>
#include <cstdio>

using namespace vgx::s28;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static Item Draw(int x, int y, int w, int h, uint8_t ak) { Item d; d.kind = IK_DRAW; d.r = {x, y, w, h}; d.alphaKind = ak; return d; }
static Item Copy(uint32_t dst, bool cand, bool zero) { Item c; c.kind = IK_COPY; c.r = {448, 0, 160, 160}; c.dstTex = dst; c.cornerCand = cand; c.zeroBefore = zero; return c; }

int main() {
    // a picture: clear quad (kind 1, whole), world draws, the corner draws (kind 2), the circle, the copy, the clear again
    std::vector<Item> it;
    it.push_back(Draw(0, 0, 640, 456, 1));
    it.push_back(Draw(100, 100, 50, 50, 0));
    Item sil = Copy(7, false, false); sil.r = {0, 0, 640, 456}; it.push_back(sil);                 // another copy (the silhouette's)
    it.push_back(Draw(449, 1, 158, 158, 2));
    it.push_back(Draw(449, 1, 158, 158, 2));
    it.push_back(Draw(449, 1, 158, 158, 2));
    it.push_back(Copy(42, true, true));                                                           // item 6
    it.push_back(Draw(448, 0, 160, 160, 1));
    Plan p = PlanCorner(it, 6);
    CHECK(p.why == W_PLANNED && p.ins == 0 && p.replay.size() == 3 && p.replay[0] == 3 && p.replay[2] == 5);
    // not known to be 0
    it[6].zeroBefore = false;
    CHECK(PlanCorner(it, 6).why == W_NOTZERO);
    it[6].zeroBefore = true;
    // a replayed draw tests depth
    it[4].depthRead = true;
    CHECK(PlanCorner(it, 6).why == W_DEPTH);
    it[4].depthRead = false;
    it[4].unsafeReplay = true;
    CHECK(PlanCorner(it, 6).why == W_UNSAFE);
    it[4].unsafeReplay = false;
    // a replayed draw samples a texture that a copy between rewrote
    it[3].nTex = 1; it[3].tex[0] = 7;
    CHECK(PlanCorner(it, 6).why == W_TEXWRITTEN);
    it[3].tex[0] = 8;
    CHECK(PlanCorner(it, 6).why == W_PLANNED);
    // a draw before the copy reads the copy's own texture
    it[1].nTex = 1; it[1].tex[0] = 42;
    CHECK(PlanCorner(it, 6).why == W_DEST);
    { // (ss24) with the ping-pong texture the plan is made, and says so; a replayed draw that reads it too is fine (it sees the texture as it was)
        const Plan q = PlanCorner(it, 6, true);
        CHECK(q.why == W_PLANNED && q.alt && q.replay.size() == 3);
        it[4].nTex = 1; it[4].tex[0] = 42;
        CHECK(PlanCorner(it, 6, true).why == W_PLANNED);
        it[4].nTex = 0;
        CHECK(!PlanCorner(it, 6, true).replay.empty() && !PlanCorner(it, 6).alt);
    }
    it[1].nTex = 0;
    { // (ss24) another copy into / out of the texture, or an upload of it, still refuses, under their own names
        Item cp; cp.kind = IK_COPY; cp.dstTex = 42; cp.r = Rect{0, 0, 10, 10};
        it.insert(it.begin() + 2, cp);
        CHECK(PlanCorner(it, 7, true).why == W_DESTCOPY && PlanCorner(it, 7).why == W_DESTCOPY);
        it.erase(it.begin() + 2);
        Item rs; rs.kind = IK_RES; rs.resTex = 42;
        it.insert(it.begin() + 2, rs);
        CHECK(PlanCorner(it, 7, true).why == W_DESTRES && PlanCorner(it, 7).why == W_DESTRES);
        it.erase(it.begin() + 2);
    }
    // a shader command between the first draw and the copy
    Item res; res.kind = IK_RES; res.resShader = true;
    it.insert(it.begin() + 4, res);
    CHECK(PlanCorner(it, 7).why == W_RES);
    it.erase(it.begin() + 4);
    // an upload of a buffer a replayed draw reads
    it[5].vbuf = 9;
    Item up; up.kind = IK_RES; up.resBuf = 9;
    it.insert(it.begin() + 3, up);
    CHECK(PlanCorner(it, 7).why == W_RES);
    it.erase(it.begin() + 3);
    it[5].vbuf = 0;
    // a constant-0 writer after the first alpha writer is replayed too; one before it is not
    it.insert(it.begin() + 3, Draw(440, 0, 200, 200, 1));       // before the first kind-2 draw touching the corner
    it.insert(it.begin() + 6, Draw(440, 0, 200, 200, 1));       // between kind-2 draws
    p = PlanCorner(it, 8);
    CHECK(p.why == W_PLANNED && p.replay.size() == 4 && p.replay[0] == 4 && p.replay[1] == 5 && p.replay[2] == 6 && p.replay[3] == 7);
    // no alpha writer touches the corner
    std::vector<Item> e;
    e.push_back(Draw(0, 0, 640, 456, 1));
    e.push_back(Draw(10, 10, 20, 20, 2));
    e.push_back(Copy(42, true, true));
    CHECK(PlanCorner(e, 2).why == W_EMPTY);
    // no draw before
    std::vector<Item> n; n.push_back(Copy(42, true, true));
    CHECK(PlanCorner(n, 0).why == W_NODRAW);
    // a render-size change
    it.clear();
    it.push_back(Draw(0, 0, 640, 456, 1)); Item tg; tg.kind = IK_TARGET; it.push_back(tg); it.push_back(Draw(449, 1, 100, 100, 2)); it.push_back(Copy(42, true, true));
    CHECK(PlanCorner(it, 3).why == W_TARGET);
    // too many
    it.clear();
    it.push_back(Draw(0, 0, 640, 456, 1));
    for (int i = 0; i < 60; ++i) it.push_back(Draw(449, 1, 100, 100, 2));
    it.push_back(Copy(42, true, true));
    CHECK(PlanCorner(it, 61).why == W_TOOMANY);

    // the resample map: the sample positions (render pixels, in the EFB's grid) of every texel of the destination, EFB path against pass path
    const int efbW = 640, efbH = 456;
    const int rtW[] = {960, 848, 720, 608, 960}, rtH[] = {544, 480, 404, 346, 456};
    double worst = 0;
    for (int s = 0; s < 5; ++s) {
        const float sx = static_cast<float>(rtW[s]) / efbW, sy = static_cast<float>(rtH[s]) / efbH;
        const int sxI = 448, syI = 0, sw = 160, sh = 160;
        auto Rnd = [](float v) { return static_cast<int>(std::floor(v + 0.5f)); };
        const int mg = kCornerMargin;                                       // the pass covers the copy's rectangle and a margin (BeginPass rounds it like any pass)
        const int ex0 = sxI - mg, ey0 = syI - mg < 0 ? 0 : syI - mg, ex1 = sxI + sw + mg, ey1 = syI + sh + mg;
        const int x0 = Rnd(ex0 * sx), y0 = Rnd(ey0 * sy), x1 = Rnd(ex1 * sx), y1 = Rnd(ey1 * sy);
        const int pw = x1 - x0, ph = y1 - y0, dw = Rnd(sw * sx), dh = Rnd(sh * sy);
        const MapUV m = ResampleMap(sxI, syI, sw, sh, sx, sy, x0, y0, pw, ph);
        // EFB path: texture coordinates of the quad (normalised to the render-size EFB texture), per destination texel centre
        const float eu0 = static_cast<float>(sxI) / efbW, ev0 = static_cast<float>(syI) / efbH, euw = static_cast<float>(sw) / efbW, evh = static_cast<float>(sh) / efbH;
        for (int ty = 0; ty < dh; ++ty)
            for (int tx = 0; tx < dw; tx += 7) {
                const double fx = (tx + 0.5) / dw, fy = (ty + 0.5) / dh;
                const double ex = (eu0 + fx * euw) * rtW[s], ey = (ev0 + fy * evh) * rtH[s];            // EFB render pixel position
                const double px = (m.u0 + fx * m.uw) * pw + x0, py = (m.v0 + fy * m.vh) * ph + y0;      // pass position moved back to the EFB's grid
                worst = std::fmax(worst, std::fmax(std::fabs(ex - px), std::fabs(ey - py)));
            }
        // the footprint stays inside the pass target (bilinear reads one pixel either side of the position)
        // (one pixel more either side for the bilinear read: the margin gives at least that at every scale down to 0.95)
        CHECK((m.u0 * pw) >= 1.0 - 1e-3 && (m.v0 * ph) >= (syI == 0 ? 0.0 : 1.0) - 1e-3 && (m.u0 + m.uw) * pw <= pw - 1.0 + 1e-3 && (m.v0 + m.vh) * ph <= ph - 1.0 + 1e-3);
    }
    std::printf("resample positions: worst difference %.2e render pixels\n", worst);
    CHECK(worst < 1e-3);

    // HUD grid
    HudGrid g; g.Reset(640, 456);
    CHECK(!g.Touches({0, 0, 640, 456}, 0));
    g.Mark({8, 8, 100, 40});
    CHECK(g.Marked() > 0);
    CHECK(g.Touches({20, 20, 10, 10}, 0));
    CHECK(!g.Touches({300, 200, 50, 50}, 16));
    CHECK(g.Touches({110, 30, 10, 10}, 40));
    g.Mark({0, 0, 640, 456});
    CHECK(g.Marked() == HudGrid::kCols * HudGrid::kRows && g.Touches({320, 228, 1, 1}, 0));
    g.Reset(640, 456);
    g.Mark({600, 400, 40, 56});
    CHECK(g.Touches({610, 410, 4, 4}, 0) && !g.Touches({0, 0, 100, 100}, 16));
    // A/B phase
    CHECK(!AbExactPhase(5, 0) && !AbExactPhase(0, 4) && !AbExactPhase(3, 4) && AbExactPhase(4, 4) && AbExactPhase(7, 4) && !AbExactPhase(8, 4));

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
