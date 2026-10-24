// GL-level test of the characters' edge smoothing at the present (GXV_CHARAA; backend::PresentCharaAA in
// vgx_backend_gl.cpp), on the desktop's GL (Mesa llvmpipe, surfaceless EGL): the real back end executes synthetic command
// buffers. The EFB (rung 2: 640x456, the console's common case) holds a white triangle over its lower left half on grey,
// so a 45-degree staircase runs across the whole picture. The display copy carries one character rectangle over the middle
// of that edge. It is captured as the console presents it (GXV_CAP_PRESENT=960x544, the sharpened upscale to the screen)
// twice, without and with the smoothing (GXV_CAP_AB=1: one picture, the two stacked). Checked:
//   - outside the rectangle (on the screen, 2 px of slack) the two are identical, bit for bit;
//   - inside it, where the picture is flat, they differ by at most 1/255 (the same sums in another shader);
//   - on the edge inside it, the staircase is smoothed: the steepest step across the edge per row is lower;
//   - the frame begins no more scenes than without a rectangle (the pass is drawn into the display's own scene);
//   - a display copy without rectangles draws nothing (counters unchanged, the two pictures identical);
//   - with the EFB multisampled (rungs 7 / 8, where the platform has them) nothing is drawn.
//   tools/test_vgx_charaa_gl.sh [output-dir]  (also writes before/after crops as PPM there)
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
void Present(CmdBuf& b, bool clearGrey, const EfbRect* box) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clearGrey;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    if (box) { d.chara[0] = *box; d.charaN = 1; }
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT, 0);
}
struct Shot {
    std::vector<uint8_t> px; // top row first, RGBA
    int w = 0, h = 0;        // h: both pictures
    unsigned scenes = 0;
    uint64_t presents = 0, boxes = 0, pixels = 0, skips = 0;
};
// One frame: grey EFB, the white triangle, a display copy (with `box` or none) captured.
Shot Frame(uint64_t frame, const EfbRect* box) {
    {
        CmdBuf a;
        a.frame = frame;
        Present(a, true, nullptr); // (the EFB grey)
        backend::Execute(a);
    }
    const float white[4] = {1, 1, 1, 1};
    backend::DebugDrawEfbTriangle(white);
    CmdBuf b;
    b.frame = frame + 1;
    Present(b, false, box);
    uint64_t p0, b0, x0, s0;
    backend::CharaAACounters(p0, b0, x0, s0);
    backend::RequestCapture(true);
    backend::Execute(b);
    backend::RequestCapture(false);
    Shot s;
    s.px = backend::LastDisplayCopy(s.w, s.h);
    s.scenes = backend::DebugLastFrameScenes();
    uint64_t p1, b1, x1, s1;
    backend::CharaAACounters(p1, b1, x1, s1);
    s.presents = p1 - p0; s.boxes = b1 - b0; s.pixels = x1 - x0; s.skips = s1 - s0;
    return s;
}
inline int Lum(const uint8_t* p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }
void WritePpm(const std::string& path, const Shot& s, int half, int x0, int y0, int w, int h, int zoom) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w * zoom, h * zoom);
    const int ph = s.h / 2;
    for (int y = 0; y < h * zoom; ++y)
        for (int x = 0; x < w * zoom; ++x) {
            const uint8_t* p = &s.px[(static_cast<size_t>(half * ph + y0 + y / zoom) * s.w + x0 + x / zoom) * 4];
            std::fwrite(p, 1, 3, f);
        }
    std::fclose(f);
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
        Present(b, true, nullptr);
        backend::Execute(b);
    }
    backend::ForceQualityRung(2);
    const EfbRect box{200, 100, 200, 200};
    // the rectangle on the screen (960x544 from 640x456), with 2 px of slack
    const int bx0 = 200 * 960 / 640 - 2, bx1 = 400 * 960 / 640 + 2, by0 = 100 * 544 / 456 - 2, by1 = (300 * 544 + 455) / 456 + 2;

    std::printf("one character rectangle, rung 2 (640x456 presented at 960x544):\n");
    const Shot a = Frame(100, &box);
    const Shot n = Frame(200, nullptr);
    if (a.w != 960 || a.h != 1088 || n.w != 960 || n.h != 1088) {
        std::printf("  [FAIL] captures are %dx%d / %dx%d, expected 960x1088 (GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1)\n", a.w, a.h, n.w, n.h);
        return 1;
    }
    const int W = a.w, H = a.h / 2;
    auto at = [&](const Shot& s, int half, int x, int y) { return &s.px[(static_cast<size_t>(half * H + y) * W + x) * 4]; };
    uint64_t outsideDiff = 0, flatPx = 0, flatBad = 0, changed = 0;
    int flatWorst = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t *p = at(a, 0, x, y), *q = at(a, 1, x, y);
            const bool diff = std::memcmp(p, q, 3) != 0;
            const bool inside = x >= bx0 && x < bx1 && y >= by0 && y < by1;
            if (!inside) { outsideDiff += diff; continue; }
            changed += diff;
            // flat: the before picture's 5x5 neighbourhood holds one colour
            bool flat = x >= 2 && y >= 2 && x < W - 2 && y < H - 2;
            for (int dy = -2; dy <= 2 && flat; ++dy)
                for (int dx = -2; dx <= 2 && flat; ++dx) flat = std::memcmp(at(a, 0, x + dx, y + dy), p, 3) == 0;
            if (flat) {
                ++flatPx;
                const int d = std::max({std::abs(p[0] - q[0]), std::abs(p[1] - q[1]), std::abs(p[2] - q[2])});
                flatWorst = std::max(flatWorst, d);
                flatBad += d > 1;
            }
        }
    char line[400];
    std::snprintf(line, sizeof line, "outside the rectangle: %llu pixels differ (expected 0)", static_cast<unsigned long long>(outsideDiff));
    Check(outsideDiff == 0, line);
    std::snprintf(line, sizeof line, "flat inside: %llu pixels, %llu differ by more than 1 (worst %d)", static_cast<unsigned long long>(flatPx),
                  static_cast<unsigned long long>(flatBad), flatWorst);
    Check(flatPx > 1000 && flatBad == 0, line);
    // The edge, row by row inside the rectangle (5 px in from its sides): the steepest luma step between neighbours.
    int rows = 0, smoother = 0;
    double stepBefore = 0, stepAfter = 0;
    for (int y = by0 + 5; y < by1 - 5; ++y) {
        int mb = 0, ma = 0;
        for (int x = bx0 + 5; x < bx1 - 6; ++x) {
            mb = std::max(mb, std::abs(Lum(at(a, 0, x + 1, y)) - Lum(at(a, 0, x, y))));
            ma = std::max(ma, std::abs(Lum(at(a, 1, x + 1, y)) - Lum(at(a, 1, x, y))));
        }
        if (mb < 20) continue; // (no edge on this row)
        ++rows;
        smoother += ma < mb;
        stepBefore += mb;
        stepAfter += ma;
    }
    std::snprintf(line, sizeof line, "edge: %d rows, steepest step %.1f -> %.1f (luma levels, mean), lower on %d rows; %llu pixels changed", rows,
                  rows ? stepBefore / rows : 0.0, rows ? stepAfter / rows : 0.0, smoother, static_cast<unsigned long long>(changed));
    Check(rows > 100 && smoother * 10 >= rows * 8 && stepAfter < stepBefore * 0.85 && changed > 200, line);
    std::snprintf(line, sizeof line, "counters: %llu present, %llu rectangle, %llu screen pixels (expected 1, 1, %d)", static_cast<unsigned long long>(a.presents),
                  static_cast<unsigned long long>(a.boxes), static_cast<unsigned long long>(a.pixels), (bx1 - bx0 - 4) * (by1 - by0 - 4));
    Check(a.presents == 1 && a.boxes == 1 && a.pixels > 0 && a.pixels <= static_cast<uint64_t>((bx1 - bx0) * (by1 - by0)), line);
    std::snprintf(line, sizeof line, "scenes of the frame: %u with the rectangle, %u without (no scene added)", a.scenes, n.scenes);
    Check(a.scenes == n.scenes && a.scenes > 0, line);
    uint64_t noBoxDiff = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) noBoxDiff += std::memcmp(at(n, 0, x, y), at(n, 1, x, y), 3) != 0;
    std::snprintf(line, sizeof line, "a display copy without rectangles: %llu presents, %llu pixels differ (expected 0, 0)", static_cast<unsigned long long>(n.presents),
                  static_cast<unsigned long long>(noBoxDiff));
    Check(n.presents == 0 && noBoxDiff == 0, line);
    // the same frame's before / after, picture-for-picture, apart from the rectangle (the no-box frame's lower half)
    uint64_t sameAsNone = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) sameAsNone += std::memcmp(at(a, 0, x, y), at(n, 0, x, y), 3) != 0;
    std::snprintf(line, sizeof line, "the sharpened present itself is unchanged by the rectangle: %llu pixels differ (expected 0)", static_cast<unsigned long long>(sameAsNone));
    Check(sameAsNone == 0, line);
    // crops for the eye (4x): the rectangle's upper middle, where the edge crosses
    int cx = 0;
    {
        int best = -1;
        for (int x = bx0 + 5; x < bx1 - 6; ++x) {
            const int d = std::abs(Lum(at(a, 0, x + 1, (by0 + by1) / 2)) - Lum(at(a, 0, x, (by0 + by1) / 2)));
            if (d > best) { best = d; cx = x; }
        }
    }
    const int cw = 96, ch = 64, cy = (by0 + by1) / 2 - ch / 2;
    cx = std::clamp(cx - cw / 2, 0, W - cw);
    WritePpm(out + "/charaa_edge_before.ppm", a, 0, cx, cy, cw, ch, 4);
    WritePpm(out + "/charaa_edge_after.ppm", a, 1, cx, cy, cw, ch, 4);

    if (backend::QualityTopRung() >= 7) {
        std::printf("multisampled EFB (rung 7):\n");
        backend::ForceQualityRung(7);
        const Shot m = Frame(300, &box);
        uint64_t d = 0;
        for (int y = 0; y < H && m.h == 2 * H; ++y)
            for (int x = 0; x < W; ++x) d += std::memcmp(&m.px[(static_cast<size_t>(y) * W + x) * 4], &m.px[(static_cast<size_t>(H + y) * W + x) * 4], 3) != 0;
        std::snprintf(line, sizeof line, "%d samples: %llu presents drawn, %llu skipped, %llu pixels differ (expected 0, 1, 0)", backend::EfbSamples(),
                      static_cast<unsigned long long>(m.presents), static_cast<unsigned long long>(m.skips), static_cast<unsigned long long>(d));
        Check(backend::EfbSamples() > 1 && m.presents == 0 && m.skips == 1 && d == 0 && m.h == 2 * H, line);
    } else {
        std::printf("multisampled EFB: not available on this GL (QualityTopRung %d), not checked\n", backend::QualityTopRung());
    }
    std::printf("%s (%d failed)\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
