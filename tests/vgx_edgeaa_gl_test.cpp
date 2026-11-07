// GL-level test of the whole-picture edge smoothing at the present (GXV_EDGE_AA; backend::PresentEdgeAA in vgx_backend_gl.cpp), on the
// desktop's GL (Mesa llvmpipe, surfaceless EGL): the real back end executes synthetic command buffers, as tests/vgx_charaa_gl_test.cpp.
// The EFB holds a white triangle over its lower left half on grey (a 45-degree staircase across the whole picture) and one HUD
// rectangle on the display copy. Captured as the console presents it (GXV_CAP_PRESENT=960x544), without and with the pass
// (GXV_CAP_AB=1: the two stacked). Checked, at rung 6 (960x544, shown 1:1: Locked 30's picture):
//   - flat pixels (5x5 of one colour) are the plain present's, bit for bit (amount 0: the centre read alone);
//   - the HUD rectangle (and 1 px around it on the screen) is the plain present's, bit for bit;
//   - the edge outside it is smoothed (the steepest luma step per row is lower);
//   - no scene is added; a copy not marked for smoothing draws nothing (counters unchanged, pictures identical);
// at rung 2 (640x456 upscaled to 960x544, sharpened): flat pixels within 1 level of the sharpened present, the edge smoothed, the HUD
// rectangle bit for bit; GXV_EDGE_AA=3 (the second run): the right half of the screen bit for bit, the left half smoothed;
// with the EFB multisampled (rung 7, where the platform has it): nothing drawn, the skip counted.
//   tools/test_vgx_edgeaa_gl.sh [output-dir]   (also writes before / after crops as PPM there)
#include "vgx_autoq.h"
#include "vgx_cmd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return std::getenv("GXV_LOG") && std::atoi(std::getenv("GXV_LOG")) != 0; }
} // namespace vgx

using namespace vgx;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void Push(CmdBuf& b, CmdType t, uint32_t arg) { b.cmds.push_back(Cmd{t, arg}); }
const EfbRect kHud{300, 150, 80, 60};
void Present(CmdBuf& b, bool clearGrey, bool edge) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clearGrey;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    if (edge) { d.edge = true; d.hud[0] = kHud; d.hudN = 1; }
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT, 0);
}
struct Shot {
    std::vector<uint8_t> px; // top row first, RGBA
    int w = 0, h = 0;        // h: both pictures
    unsigned scenes = 0;
    uint64_t presents = 0, hud = 0, msaa = 0, other = 0;
};
Shot Frame(uint64_t frame, bool edge) {
    {
        CmdBuf a;
        a.frame = frame;
        Present(a, true, false); // (the EFB grey)
        backend::Execute(a);
    }
    const float white[4] = {1, 1, 1, 1};
    backend::DebugDrawEfbTriangle(white);
    CmdBuf b;
    b.frame = frame + 1;
    Present(b, false, edge);
    uint64_t p0, h0, m0, o0;
    backend::EdgeAACounters(p0, h0, m0, o0);
    backend::RequestCapture(true);
    backend::Execute(b);
    backend::RequestCapture(false);
    Shot s;
    s.px = backend::LastDisplayCopy(s.w, s.h);
    s.scenes = backend::DebugLastFrameScenes();
    uint64_t p1, h1, m1, o1;
    backend::EdgeAACounters(p1, h1, m1, o1);
    s.presents = p1 - p0; s.hud = h1 - h0; s.msaa = m1 - m0; s.other = o1 - o0;
    return s;
}
inline int Lum(const uint8_t* p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }
void WritePpm(const std::string& path, const Shot& s, int half, int x0, int y0, int w, int h, int zoom) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w * zoom, h * zoom);
    const int ph = s.h / 2;
    for (int y = 0; y < h * zoom; ++y)
        for (int x = 0; x < w * zoom; ++x) std::fwrite(&s.px[(static_cast<size_t>(half * ph + y0 + y / zoom) * s.w + x0 + x / zoom) * 4], 1, 3, f);
    std::fclose(f);
}
// One rung: the checks above. hudX0.. are the HUD rectangle on the screen, 1 px of slack outwards.
void CheckRung(int rung, const std::string& out, bool split) {
    backend::ForceQualityRung(rung);
    const Shot a = Frame(100 + rung * 10, true);
    int rw = 0, rh = 0, rs = 0;
    backend::QualityRungSize(rw, rh, rs); // (after a frame: the rung is applied at the start of the next one executed)
    const Shot n = Frame(200 + rung * 10, false);
    char line[400];
    if (a.w != 960 || a.h != 1088) { std::printf("  [FAIL] capture is %dx%d\n", a.w, a.h); ++g_fail; return; }
    const int W = a.w, H = a.h / 2;
    auto at = [&](const Shot& s, int half, int x, int y) { return &s.px[(static_cast<size_t>(half * H + y) * W + x) * 4]; };
    std::printf("rung %d (%dx%d, %d samples, presented at 960x544)%s:\n", rung, rw, rh, rs, split ? ", GXV_EDGE_AA=3" : "");
    if (rs > 1) {
        uint64_t d = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) d += std::memcmp(at(a, 0, x, y), at(a, 1, x, y), 3) != 0;
        std::snprintf(line, sizeof line, "multisampled EFB: %llu presents smoothed, %llu skipped, %llu pixels differ (expected 0, 1, 0)",
                      (unsigned long long)a.presents, (unsigned long long)a.msaa, (unsigned long long)d);
        Check(a.presents == 0 && a.msaa == 1 && d == 0, line);
        return;
    }
    const int hx0 = kHud.x * 960 / 640 - 1, hx1 = (kHud.x + kHud.w) * 960 / 640 + 1;
    const int hy0 = kHud.y * 544 / 456 - 1, hy1 = ((kHud.y + kHud.h) * 544 + 455) / 456 + 1;
    const bool oneToOne = rw == 960 && rh == 544;
    uint64_t hudDiff = 0, flatPx = 0, flatBad = 0, changed = 0, rightDiff = 0;
    int flatWorst = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t *p = at(a, 0, x, y), *q = at(a, 1, x, y);
            const bool diff = std::memcmp(p, q, 3) != 0;
            const bool inHud = x >= hx0 + 2 && x < hx1 - 2 && y >= hy0 + 2 && y < hy1 - 2; // (inside, 1 px of rounding slack)
            if (inHud) { hudDiff += diff; continue; }
            if (split && x >= W / 2 + 1) { rightDiff += diff; continue; }
            changed += diff;
            bool flat = x >= 2 && y >= 2 && x < W - 2 && y < H - 2;
            for (int dy = -2; dy <= 2 && flat; ++dy)
                for (int dx = -2; dx <= 2 && flat; ++dx) flat = std::memcmp(at(a, 0, x + dx, y + dy), p, 3) == 0;
            if (flat) {
                ++flatPx;
                const int d = std::max({std::abs(p[0] - q[0]), std::abs(p[1] - q[1]), std::abs(p[2] - q[2])});
                flatWorst = std::max(flatWorst, d);
                flatBad += d > (oneToOne ? 0 : 1);
            }
        }
    std::snprintf(line, sizeof line, "flat pixels: %llu, %llu differ by more than %d (worst %d)", (unsigned long long)flatPx, (unsigned long long)flatBad,
                  oneToOne ? 0 : 1, flatWorst);
    Check(flatPx > 100000 && flatBad == 0, line);
    std::snprintf(line, sizeof line, "HUD rectangle: %llu pixels differ (expected 0)", (unsigned long long)hudDiff);
    Check(hudDiff == 0, line);
    if (split) {
        std::snprintf(line, sizeof line, "right half (not smoothed): %llu pixels differ (expected 0)", (unsigned long long)rightDiff);
        Check(rightDiff == 0, line);
    }
    // The edge, row by row (left of the HUD rectangle, and left half with =3): the steepest luma step between neighbours.
    int rows = 0, smoother = 0;
    double sb = 0, sa = 0;
    for (int y = 20; y < H - 20; ++y) {
        if (y >= hy0 - 8 && y < hy1 + 8) continue;
        int mb = 0, ma = 0;
        for (int x = 4; x < (split ? W / 2 - 8 : W - 5); ++x) {
            mb = std::max(mb, std::abs(Lum(at(a, 0, x + 1, y)) - Lum(at(a, 0, x, y))));
            ma = std::max(ma, std::abs(Lum(at(a, 1, x + 1, y)) - Lum(at(a, 1, x, y))));
        }
        if (mb < 20) continue;
        ++rows;
        smoother += ma < mb;
        sb += mb;
        sa += ma;
    }
    std::snprintf(line, sizeof line, "edge: %d rows, steepest step %.1f -> %.1f (luma levels, mean), lower on %d rows; %llu pixels changed", rows,
                  rows ? sb / rows : 0.0, rows ? sa / rows : 0.0, smoother, (unsigned long long)changed);
    // (GXV_PRESENT_FAST: its edge term leaves a diagonal edge of an upscaled picture as the plain present shows it, by design: the
    // bilinear upscale has smoothed it already, see vgx_backend_gl.cpp "the fast present" and tests/vgx_present_fast_gl_test.cpp)
    const bool fastUp = std::getenv("GXV_PRESENT_FAST") && std::atoi(std::getenv("GXV_PRESENT_FAST")) != 0 && !oneToOne;
    if (fastUp) std::printf("  [--] %s (not checked: GXV_PRESENT_FAST leaves upscaled diagonals alone)\n", line);
    else Check(rows > 100 && smoother * 10 >= rows * 8 && sa < sb * 0.85 && changed > 200, line);
    std::snprintf(line, sizeof line, "counters: %llu present smoothed, %llu HUD rectangle (expected 1, 1)", (unsigned long long)a.presents, (unsigned long long)a.hud);
    Check(a.presents == 1 && a.hud == 1, line);
    std::snprintf(line, sizeof line, "scenes of the frame: %u smoothed, %u not (no scene added)", a.scenes, n.scenes);
    Check(a.scenes == n.scenes && a.scenes > 0, line);
    uint64_t nd = 0, same = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            nd += std::memcmp(at(n, 0, x, y), at(n, 1, x, y), 3) != 0;
            same += std::memcmp(at(a, 0, x, y), at(n, 0, x, y), 3) != 0;
        }
    std::snprintf(line, sizeof line, "a copy not marked: %llu presents, %llu pixels differ; the plain present unchanged by the mark: %llu differ (expected 0, 0, 0)",
                  (unsigned long long)n.presents, (unsigned long long)nd, (unsigned long long)same);
    Check(n.presents == 0 && nd == 0 && same == 0, line);
    if (!split) {
        // crops for the eye (4x) where the edge crosses the middle row
        int cx = 0, best = -1;
        for (int x = 4; x < W - 5; ++x) {
            const int d = std::abs(Lum(at(a, 0, x + 1, H / 2 + 60)) - Lum(at(a, 0, x, H / 2 + 60)));
            if (d > best) { best = d; cx = x; }
        }
        const int cw = 96, ch = 64;
        cx = std::clamp(cx - cw / 2, 0, W - cw);
        const std::string tag = "edgeaa_rung" + std::to_string(rung);
        WritePpm(out + "/" + tag + "_before.ppm", a, 0, cx, H / 2 + 60 - ch / 2, cw, ch, 4);
        WritePpm(out + "/" + tag + "_after.ppm", a, 1, cx, H / 2 + 60 - ch / 2, cw, ch, 4);
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
    if (!backend::Init(0, 0)) { std::printf("back end Init failed\n"); return 2; }
    {
        CmdBuf b;
        b.frame = 1;
        b.targets.push_back(SetTargetCmd{640, 456});
        Push(b, CMD_SET_TARGET, 0);
        Present(b, true, false);
        backend::Execute(b);
    }
    const bool split = std::getenv("GXV_EDGE_AA") && std::atoi(std::getenv("GXV_EDGE_AA")) == 3;
    CheckRung(6, out, split);
    CheckRung(2, out, split);
    {
        // Performance mode's edge step (GXV_AUTOQ_EDGESTEP, set by the controller under GPU load): the marked copy is presented as without
        // the switch (counted as a skip), the same pixels as an unmarked copy; off again, it is smoothed again.
        backend::DebugSetEdgeStep(true);
        (void)Frame(400, false);  // (the step latches at the start of a frame)
        const Shot off = Frame(410, true), plain = Frame(420, false);
        backend::DebugSetEdgeStep(false);
        (void)Frame(430, false);
        const Shot on = Frame(440, true);
        const bool same = off.w == plain.w && off.h == plain.h && off.px == plain.px;
        char line[200];
        std::snprintf(line, sizeof line, "edge step on: %llu presents smoothed, %llu skipped, the picture %s the unmarked one; off again: %llu smoothed",
                      (unsigned long long)off.presents, (unsigned long long)off.other, same ? "equals" : "DIFFERS FROM", (unsigned long long)on.presents);
        Check(off.presents == 0 && off.other == 1 && same && on.presents == 1, line);
    }
    if (backend::QualityTopRung() >= 7) CheckRung(7, out, split);
    else std::printf("multisampled EFB: not available on this GL (QualityTopRung %d), not checked\n", backend::QualityTopRung());
    std::printf("%s (%d failed)\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
