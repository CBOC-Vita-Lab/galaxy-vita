// GL-level test of a change of render scale between frames (ApplyRung in vgx_backend_gl.cpp), on the desktop's GL
// (Mesa llvmpipe, surfaceless EGL): the real back end executes synthetic command buffers.
//   tools/test_vgx_autoq_gl.sh       (runs it with GXV_KEEP_RT=0, the console's behaviour, and =1, the emulator's)
// Frame 1 at rung 2 (640x456) makes: two plain copy textures (red, green, by fill copies), an atlas with both pictures
// in cells (copies out of a texture into atlas cells), a second atlas whose cell is a pass cleared to blue (a pass drawn
// straight into an atlas cell), and leaves the EFB cleared to grey by the display copy. Then the rung changes; the next
// frame copies the atlas cells out again and takes a display copy. Checked: every render texture has the size its EFB
// size gives at the new scale (the size the next copy into it will make), the pictures kept across the change are still
// there (atlas cells, plain textures, the EFB), the display copy has the new size. Then once more to the lowest rung.
// Rungs 7 and 8 (the EFB multisampled, 2x and 4x; here a multisampled renderbuffer resolved into the EFB's texture, on the
// Vita vitaGL's per-framebuffer multisampling) take part in the stress runs, and a triangle's edge is checked: hard at rung 6,
// smoothed (partly covered pixels between the two colours) at 7 and 8, in the display copy, an ordinary EFB copy and a
// GXV_EFBSWAP copy (the EFB's own texture handed over: it must hold the resolved picture).
#include "vgx_autoq.h"
#include "vgx_cmd.h"

#include <GLES2/gl2.h>

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

void Identity(float m[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = r == c ? 1.0f : 0.0f;
}
void Push(CmdBuf& b, CmdType t, uint32_t arg) { b.cmds.push_back(Cmd{t, arg}); }

CopyTexCmd Fill(uint32_t tex, float r, float g, float bl) {
    CopyTexCmd c;
    c.texId = tex;
    c.src = EfbRect{0, 0, 64, 32};
    c.dstW = 64;
    c.dstH = 32;
    c.fill = true;
    c.fillRgba[0] = r; c.fillRgba[1] = g; c.fillRgba[2] = bl; c.fillRgba[3] = 1;
    Identity(c.mat);
    return c;
}
// A copy out of a rectangle of texture `src` (sw x sh, EFB units) into texture `dst`, whole or a cell of an atlas.
CopyTexCmd FromTex(uint32_t src, uint16_t sw, uint16_t sh, EfbRect rect, uint32_t dst, uint16_t dtw = 0, uint16_t dth = 0, int16_t dx = 0, int16_t dy = 0) {
    CopyTexCmd c;
    c.texId = dst;
    c.srcTex = src;
    c.srcTexW = sw;
    c.srcTexH = sh;
    c.src = rect;
    c.dstW = static_cast<uint16_t>(rect.w);
    c.dstH = static_cast<uint16_t>(rect.h);
    c.dstTexW = dtw;
    c.dstTexH = dth;
    c.dstX = dx;
    c.dstY = dy;
    Identity(c.mat);
    return c;
}
void Present(CmdBuf& b, bool clearGrey) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clearGrey;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT, 0);
}

// The centre pixel of a texture; the texture's size.
bool Centre(uint32_t tex, int& w, int& h, uint8_t px[4]) {
    std::vector<uint8_t> rgba;
    if (!backend::DebugReadTexture(tex, rgba, w, h)) return false;
    std::memcpy(px, &rgba[(static_cast<size_t>(h / 2) * w + w / 2) * 4], 4);
    return true;
}
bool Near(const uint8_t px[4], int r, int g, int b) {
    return std::abs(px[0] - r) <= 3 && std::abs(px[1] - g) <= 3 && std::abs(px[2] - b) <= 3;
}
int Scaled(int efb, int rungSize, int base) { return static_cast<int>(std::floor(efb * (static_cast<float>(rungSize) / base) + 0.5f)); }

void CheckTex(uint32_t tex, int ew, int eh, int rung, int r, int g, int b, const char* what) {
    int w = 0, h = 0;
    uint8_t px[4] = {};
    const bool there = Centre(tex, w, h, px);
    const int xw = Scaled(ew, autoq::RungAt(rung).w, autoq::kBaseW), xh = Scaled(eh, autoq::RungAt(rung).h, autoq::kBaseH);
    char line[300];
    std::snprintf(line, sizeof line, "%s: %dx%d (expected %dx%d), centre %02X%02X%02X (expected %02X%02X%02X)", what, w, h, xw, xh, px[0], px[1], px[2], r, g, b);
    Check(there && w == xw && h == xh && (r < 0 || Near(px, r, g, b)), line); // (r < 0: the size only)
}

// The four pixels 2 in from the middle of each edge must still hold the picture's colour (no bleed from outside an
// atlas cell after the resample: the cell boundaries moved with the scale).
void CheckEdges(uint32_t tex, int r, int g, int b, const char* what) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool ok = backend::DebugReadTexture(tex, rgba, w, h) && w > 4 && h > 4;
    const int pts[4][2] = {{w / 2, 2}, {w / 2, h - 3}, {2, h / 2}, {w - 3, h / 2}};
    char worst[64] = "";
    for (const auto& q : pts) {
        if (!ok) break;
        const uint8_t* px = &rgba[(static_cast<size_t>(q[1]) * w + q[0]) * 4];
        if (!Near(px, r, g, b)) {
            ok = false;
            std::snprintf(worst, sizeof worst, " (%d,%d) is %02X%02X%02X", q[0], q[1], px[0], px[1], px[2]);
        }
    }
    char line[200];
    std::snprintf(line, sizeof line, "%s: edges 2 px in hold the colour%s", what, worst);
    Check(ok, line);
}

// Frame 2 and 3: the cells copied out of the atlases into textures base, base+1, base+2.
void CopyCellsOut(CmdBuf& b, uint32_t base) {
    b.copyTex.push_back(FromTex(20, 256, 128, EfbRect{8, 8, 64, 32}, base));
    b.copyTex.push_back(FromTex(20, 256, 128, EfbRect{8, 72, 64, 32}, base + 1));
    b.copyTex.push_back(FromTex(21, 128, 64, EfbRect{4, 4, 40, 20}, base + 2));
    for (int i = 3; i > 0; --i) Push(b, CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - i));
}
} // namespace

int main() {
    if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
    if (!backend::Init(0, 0)) { std::printf("back end Init failed\n"); return 2; }
    std::printf("GXV_KEEP_RT=%s\n", std::getenv("GXV_KEEP_RT") ? std::getenv("GXV_KEEP_RT") : "0");

    // ---- frame 1, rung 2 (640x456)
    {
        CmdBuf b;
        b.frame = 1;
        b.targets.push_back(SetTargetCmd{640, 456});
        Push(b, CMD_SET_TARGET, 0);
        b.copyTex.push_back(Fill(10, 1, 0, 0));
        b.copyTex.push_back(Fill(11, 0, 1, 0));
        b.copyTex.push_back(FromTex(10, 64, 32, EfbRect{0, 0, 64, 32}, 20, 256, 128, 8, 8));
        b.copyTex.push_back(FromTex(11, 64, 32, EfbRect{0, 0, 64, 32}, 20, 256, 128, 8, 72));
        for (uint32_t i = 0; i < 4; ++i) Push(b, CMD_COPY_TEX, i);
        // a pass drawn straight into a cell of atlas 21 (picture 22), cleared to blue with its gutter; no draws
        PassCmd p;
        p.rect = EfbRect{100, 100, 40, 20};
        p.rgba[0] = 0; p.rgba[1] = 0; p.rgba[2] = 1; p.rgba[3] = 1;
        p.directTex = 22;
        p.atlasTex = 21;
        p.atlasW = 128; p.atlasH = 64; p.atlasX = 4; p.atlasY = 4;
        p.atlasGutter = 2;
        b.passes.push_back(p);
        Push(b, CMD_PASS_BEGIN, 0);
        CopyTexCmd end;
        end.texId = 22;
        end.src = p.rect;
        end.dstW = 40;
        end.dstH = 20;
        end.fromPass = true;
        Identity(end.mat);
        b.copyTex.push_back(end);
        Push(b, CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1));
        Present(b, true);
        backend::Execute(b);
        std::printf("frame 1 at rung 2:\n");
        CheckTex(10, 64, 32, 2, 255, 0, 0, "red texture");
        CheckTex(20, 256, 128, 2, 0, 0, 0, "atlas 20 (centre outside the cells)");
        CheckTex(21, 128, 64, 2, 0, 0, 0, "atlas 21 (centre outside the cell)");
    }
    // ---- frame 2, rung 6 (960x544)
    {
        backend::ForceQualityRung(6);
        CmdBuf b;
        b.frame = 2;
        CopyCellsOut(b, 30);
        Present(b, false);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        std::printf("frame 2 at rung 6:\n");
        CheckTex(10, 64, 32, 6, 255, 0, 0, "red texture, resampled");
        CheckTex(11, 64, 32, 6, 0, 255, 0, "green texture, resampled");
        CheckTex(30, 64, 32, 6, 255, 0, 0, "atlas 20 cell 1 (red) copied out after the change");
        CheckTex(31, 64, 32, 6, 0, 255, 0, "atlas 20 cell 2 (green) copied out after the change");
        CheckTex(32, 40, 20, 6, 0, 0, 255, "atlas 21 pass cell (blue) copied out after the change");
        CheckEdges(30, 255, 0, 0, "atlas 20 cell 1");
        CheckEdges(31, 0, 255, 0, "atlas 20 cell 2");
        CheckEdges(32, 0, 0, 255, "atlas 21 pass cell");
        int w = 0, h = 0;
        const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
        const uint8_t* c = px.size() >= static_cast<size_t>(w) * h * 4 && w > 0 ? &px[(static_cast<size_t>(h / 2) * w + w / 2) * 4] : nullptr;
        char line[200];
        std::snprintf(line, sizeof line, "display copy %dx%d (expected 960x544), centre %02X%02X%02X (the EFB's grey kept through the change)", w, h, c ? c[0] : 0,
                      c ? c[1] : 0, c ? c[2] : 0);
        Check(w == 960 && h == 544 && c && Near(c, 128, 128, 128), line);
    }
    // ---- frame 3, rung 0 (544x388)
    {
        backend::ForceQualityRung(0);
        CmdBuf b;
        b.frame = 3;
        CopyCellsOut(b, 40);
        Present(b, false);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        std::printf("frame 3 at rung 0:\n");
        CheckTex(20, 256, 128, 0, 0, 0, 0, "atlas 20 size");
        CheckTex(40, 64, 32, 0, 255, 0, 0, "atlas 20 cell 1 (red) after a second change");
        CheckTex(41, 64, 32, 0, 0, 255, 0, "atlas 20 cell 2 (green) after a second change");
        CheckTex(42, 40, 20, 0, 0, 0, 255, "atlas 21 pass cell (blue) after a second change");
        CheckEdges(40, 255, 0, 0, "atlas 20 cell 1");
        CheckEdges(41, 0, 255, 0, "atlas 20 cell 2");
        CheckEdges(42, 0, 0, 255, "atlas 21 pass cell");
        int w = 0, h = 0;
        backend::LastDisplayCopy(w, h);
        char line[100];
        std::snprintf(line, sizeof line, "display copy %dx%d (expected 544x388)", w, h);
        Check(w == 544 && h == 388, line);
    }
    // ---- stress: the rung changes before every frame, 28 frames, all rungs up and down (the Strikers port found dark
    // frames with a test that resized every 7 frames). Every frame copies atlas 20's red cell out again and takes a display
    // copy; checked: the display copy shows the EFB's grey (no dark or lost picture) and the red picture survives.
    {
        const bool target = std::getenv("GXV_AUTOQ_TARGET") && std::atoi(std::getenv("GXV_AUTOQ_TARGET")) == 30;
        const int rung = target ? -1 : 0;
        backend::ForceQualityRung(-1);
        CmdBuf b;
        b.frame = 4;
        CopyCellsOut(b, 43);
        Present(b, false);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        const int expectedW = target ? 480 : 544, expectedH = target ? 340 : 388;
        int w = 0, h = 0;
        const auto& pixels = backend::LastDisplayCopy(w, h);
        Check(w == expectedW && h == expectedH,
              target ? "target 30 enables the 480x340 emergency rung" : "target off preserves the 544x388 floor");
        const uint8_t* centre = w > 0 && h > 0 && pixels.size() == static_cast<size_t>(w) * h * 4 ?
            &pixels[(static_cast<size_t>(h / 2) * w + w / 2) * 4] : nullptr;
        Check(centre && Near(centre, 128, 128, 128), "emergency-floor request preserves the EFB picture");
        CheckTex(43, 64, 32, rung, 255, 0, 0, "red atlas cell at the requested floor");
        CheckTex(44, 64, 32, rung, 0, 255, 0, "green atlas cell at the requested floor");
        CheckTex(45, 40, 20, rung, 0, 0, 255, "blue pass cell at the requested floor");
        CheckEdges(43, 255, 0, 0, "red atlas cell at the requested floor");
        CheckEdges(44, 0, 255, 0, "green atlas cell at the requested floor");
        CheckEdges(45, 0, 0, 255, "blue pass cell at the requested floor");
    }
    {
        std::printf("stress: a new rung before each of 28 frames:\n");
        int dark = 0, lost = 0, wrongSize = 0;
        const int order[] = {6, 0, 3, 8, 5, 1, 7, 4, 2, 8, 6, 1, 5, 7, 0, 2, 4, 3};
        const int top = backend::QualityTopRung();
        for (int i = 0; i < 28; ++i) {
            const int rung = std::min(order[i % 18], top);
            backend::ForceQualityRung(rung);
            CmdBuf b;
            b.frame = 10 + static_cast<uint64_t>(i);
            b.copyTex.push_back(FromTex(20, 256, 128, EfbRect{8, 8, 64, 32}, 50));
            Push(b, CMD_COPY_TEX, 0);
            Present(b, false);
            backend::RequestCapture(true);
            backend::Execute(b);
            backend::RequestCapture(false);
            int w = 0, h = 0;
            const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
            const int xw = Scaled(640, autoq::kLadder[rung].w, autoq::kBaseW), xh = Scaled(456, autoq::kLadder[rung].h, autoq::kBaseH);
            if (w != xw || h != xh) ++wrongSize;
            for (const auto& q : {std::pair<int, int>{w / 2, h / 2}, {3, 3}, {w - 4, h - 4}}) {
                if (w <= 0 || h <= 0) break;
                const uint8_t* c = &px[(static_cast<size_t>(q.second) * w + q.first) * 4];
                if (!Near(c, 128, 128, 128)) ++dark;
            }
            int tw = 0, th = 0;
            uint8_t c[4] = {};
            if (!Centre(50, tw, th, c) || !Near(c, 255, 0, 0)) ++lost;
        }
        char line[160];
        std::snprintf(line, sizeof line, "28 frames: %d display-copy pixels not grey, %d red pictures lost, %d display copies of the wrong size", dark, lost, wrongSize);
        Check(dark == 0 && lost == 0 && wrongSize == 0, line);
    }
    // ---- long stress (the Strikers port's first console test of its dynamic resolution ran out of video memory: its pool kept
    // every size it had used): 3000 frames, a new rung every 3 frames (all rungs), each frame a few copies of different sizes
    // and an offscreen pass; the first third also uses three sizes that the rest never use again (another scene). Checked: the
    // pool and the render targets' memory stay bounded (the last third needs no more than the first). Then 200 frames at one
    // rung: 20 with the first scene's sizes, 180 without; the objects only those sizes used are let go as idle (GXV_POOL_IDLE,
    // 60 presents). Under GXV_KEEP_RT (the emulator's way) textures are never deleted, so every change leaks its resampled
    // textures on purpose (llvmpipe's memory runs out after a few hundred changes): 300 frames there, and the pool cap (48)
    // is what is checked.
    const bool keepRt = std::getenv("GXV_KEEP_RT") && std::atoi(std::getenv("GXV_KEEP_RT")) != 0;
    auto stressFrame = [&](int i, bool firstScene) {
            CmdBuf b;
            b.frame = 1000 + static_cast<uint64_t>(i);
            const uint16_t sizes[4][2] = {{64, 32}, {160, 160}, {320, 228}, {40, 20}};
            for (uint32_t k = 0; k < 4; ++k) {
                const uint16_t* sz = sizes[k];
                CopyTexCmd c = Fill(60 + k, 0.2f, 0.4f, 0.6f);
                c.src = EfbRect{0, 0, static_cast<int16_t>(sz[0]), static_cast<int16_t>(sz[1])};
                c.dstW = sz[0];
                c.dstH = sz[1];
                b.copyTex.push_back(c);
            }
            if (firstScene) { // the first scene's own sizes
                const uint16_t extra[3][2] = {{200, 100}, {96, 96}, {480, 300}};
                for (int k = 0; k < 3; ++k) {
                    CopyTexCmd c = Fill(70 + static_cast<uint32_t>(k), 0.9f, 0.9f, 0.1f);
                    c.src = EfbRect{0, 0, static_cast<int16_t>(extra[k][0]), static_cast<int16_t>(extra[k][1])};
                    c.dstW = extra[k][0];
                    c.dstH = extra[k][1];
                    b.copyTex.push_back(c);
                }
            }
            for (uint32_t k = 0; k < b.copyTex.size(); ++k) Push(b, CMD_COPY_TEX, k);
            PassCmd ps; // an offscreen pass of its own size, ended by its copy
            ps.rect = EfbRect{10, 10, 120, 90};
            ps.rgba[2] = 1;
            b.passes.push_back(ps);
            Push(b, CMD_PASS_BEGIN, 0);
            CopyTexCmd end;
            end.texId = 80;
            end.src = ps.rect;
            end.dstW = 120;
            end.dstH = 90;
            end.fromPass = true;
            Identity(end.mat);
            b.copyTex.push_back(end);
            Push(b, CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1));
            Present(b, false);
            backend::Execute(b);
    };
    {
        // (GXV_TEST_STRESS_FRAMES=<n>: another length, for measuring)
        const int frames = std::getenv("GXV_TEST_STRESS_FRAMES") ? std::atoi(std::getenv("GXV_TEST_STRESS_FRAMES")) : (keepRt ? 300 : 3000), third = frames / 3;
        std::printf("long stress: %d frames, a new rung every 3 frames:\n", frames);
        unsigned maxPoolA = 0, maxPoolC = 0;
        uint64_t maxBytesA = 0, maxBytesC = 0, evicted = 0;
        for (int i = 0; i < frames; ++i) {
            if (i % 3 == 0) backend::ForceQualityRung((i / 3) % (backend::QualityTopRung() + 1));
            stressFrame(i, i < third);
            unsigned pooled = 0;
            uint64_t bytes = 0;
            backend::DebugPoolStats(pooled, bytes, evicted);
            if (i < third) { maxPoolA = std::max(maxPoolA, pooled); maxBytesA = std::max(maxBytesA, bytes); }
            if (i >= frames - third) { maxPoolC = std::max(maxPoolC, pooled); maxBytesC = std::max(maxBytesC, bytes); }
        }
        char line[300];
        std::snprintf(line, sizeof line, "first third: at most %u pooled objects, %.1f MiB of render targets; last third: at most %u, %.1f MiB",
                      maxPoolA, maxBytesA / 1048576.0, maxPoolC, maxBytesC / 1048576.0);
        // (Under GXV_KEEP_RT the pool is cut back when it is over 48 at a present or a change, so one frame's new objects
        // can take it a little past: 56 allowed. There what the pooled objects hold depends on which sizes happen to be
        // among the 48, so only the object count is bounded; the memory bound is checked in the console's way, KEEP_RT=0.)
        Check(maxPoolA <= 56 && maxPoolC <= 56 && (keepRt || (maxPoolC <= maxPoolA && maxBytesC <= maxBytesA)), line);
        // At one rung: the first scene's sizes for 20 frames, then 180 frames without them.
        backend::ForceQualityRung(autoq::kToday);
        unsigned withScene = 0, after = 0;
        uint64_t bytesWith = 0, bytesAfter = 0, evictedBefore = 0, evictedAfter = 0;
        for (int i = 0; i < 200; ++i) {
            stressFrame(5000 + i, i < 20);
            unsigned pooled = 0;
            uint64_t bytes = 0;
            backend::DebugPoolStats(pooled, bytes, i < 20 ? evictedBefore : evictedAfter);
            if (i == 19) { withScene = pooled; bytesWith = bytes; }
            if (i == 199) { after = pooled; bytesAfter = bytes; }
        }
        std::snprintf(line, sizeof line, "one rung: %u pooled objects (%.1f MiB) with the first scene's sizes, %u (%.1f MiB) 180 frames later; %llu let go as idle",
                      withScene, bytesWith / 1048576.0, after, bytesAfter / 1048576.0, static_cast<unsigned long long>(evictedAfter - evictedBefore));
        if (keepRt)
            Check(after <= 56, line);
        else
            Check(after < withScene && bytesAfter < bytesWith && evictedAfter > evictedBefore, line);
    }
    // ---- the video-memory check of a step up (RungFits). The script runs the GXV_KEEP_RT=0 pass with GXV_AUTOQ_VRAM_KIB
    // (a simulated free figure): each rung above the current one needs its projected render targets (the pixels' ratio; the
    // EFB's depth with the rung's samples: rungs 7 and 8 add one and three EFB depth buffers of 960x544) plus the margin
    // (16 MiB), at least 16 MiB up to 640x456 and 32 MiB above it, and fits exactly when that much is free; with 24 MiB
    // simulated, from rung 0 the steps to 1 and 2 fit and those above 640x456 do not. Without the switch (the other pass,
    // the desktop) every step fits.
    {
        std::printf("video-memory check of a step up:\n");
        backend::ForceQualityRung(0);
        stressFrame(9000, false);
        const char* sim = std::getenv("GXV_AUTOQ_VRAM_KIB");
        unsigned pooled = 0;
        uint64_t bytes = 0, evicted = 0;
        backend::DebugPoolStats(pooled, bytes, evicted);
        int current = 0, fitting = 0, refused = 0;
        bool consistent = true;
        double prevNeed = 0, proj[autoq::kRungs] = {};
        std::string rows;
        const int top = backend::QualityTopRung();
        for (int r = 0; r <= top; ++r) {
            double need = 0, freeMiB = 0;
            const bool fits = backend::DebugRungFits(r, need, freeMiB, current, proj[r]);
            if (r <= current) continue;
            const double pix = static_cast<double>(autoq::kLadder[r].w) * autoq::kLadder[r].h / (static_cast<double>(autoq::kLadder[current].w) * autoq::kLadder[current].h);
            if (autoq::kLadder[r].samples == 1) // (single-sampled: the pixels' ratio, give or take the depth buffers' 32x32 tiles)
                consistent &= std::fabs(proj[r] - bytes / 1048576.0 * pix) <= 0.05 * bytes / 1048576.0 * pix + 0.1;
            const double expect = std::max(proj[r] + 16.0, r > autoq::kToday ? 32.0 : 16.0);
            if (sim) consistent &= std::fabs(need - expect) < 0.01 && fits == (freeMiB >= need) && need > prevNeed &&
                                   std::fabs(freeMiB - std::atof(sim) / 1024.0) < 0.01 && (r > autoq::kToday ? !fits : true);
            else consistent &= fits && need == 0;
            prevNeed = need - 0.001; // (the floors make neighbours equal)
            (fits ? fitting : refused)++;
            char cell[80];
            std::snprintf(cell, sizeof cell, " %d:%s(%.1f, targets %.1f)", r, fits ? "fits" : "no", need, proj[r]);
            rows += cell;
        }
        char line[700];
        std::snprintf(line, sizeof line, "from rung %d, render targets %.1f MiB, %s free:%s", current, bytes / 1048576.0, sim ? sim : "no figure", rows.c_str());
        Check(consistent && (sim ? (fitting > 0 && refused > 0) : refused == 0), line);
        if (top >= 7) {
            // One EFB depth buffer of 960x544 at 4 bytes a sample: 1.99 MiB more for 2x, 5.98 MiB for 4x.
            const double one = 960.0 * 544.0 * 4.0 / 1048576.0;
            const double d7 = proj[7] - proj[6], d8 = top >= 8 ? proj[8] - proj[6] : 3 * one;
            std::snprintf(line, sizeof line, "multisampling rungs: render targets +%.2f MiB at 2x, +%.2f MiB at 4x over 960x544 (EFB depth samples)", d7, d8);
            Check(std::fabs(d7 - one) < 0.05 && std::fabs(d8 - 3 * one) < 0.1, line);
        }
    }
    // ---- no memory for a new size (as the Strikers port met on the console, 160c3f9; vitaGL gives no error, the back end
    // asks the texture's data pointer): simulated by DebugFailAllocation(k), the k-th allocation check from now fails. In a
    // change of size the checks come in this order: 1 the EFB picture's copy at the new size, 2 the new EFB target, then
    // one for each render texture's second copy. Checked from rung 2 (640x456, the EFB grey) to rung 5:
    //   2 fails: the change is refused, the target stays 640x456 with its picture, the rung stays 2;
    //   1 fails: the change is made, the new target starts cleared (no picture kept);
    //   3 fails: the change is made, one render texture is resized without its picture (plain resize), the others resampled.
    {
        std::printf("no memory for a new size (simulated):\n");
        auto toRung = [&](int rung, int failAt, uint64_t frame) {
            backend::ForceQualityRung(2);
            CmdBuf a;
            a.frame = frame;
            Present(a, true); // (the EFB grey at rung 2)
            backend::Execute(a);
            backend::DebugFailAllocation(failAt);
            backend::ForceQualityRung(rung);
            CmdBuf b;
            b.frame = frame + 1;
            Present(b, false);
            backend::RequestCapture(true);
            backend::Execute(b);
            backend::RequestCapture(false);
            backend::DebugFailAllocation(0);
        };
        int w = 0, h = 0;
        toRung(5, 2, 6000);
        {
            const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
            const bool grey = w > 0 && h > 0 && Near(&px[(static_cast<size_t>(h / 2) * w + w / 2) * 4], 128, 128, 128);
            char line[200];
            std::snprintf(line, sizeof line, "the new target fails: rung %d, display copy %dx%d (expected rung 2, 640x456), picture %s", backend::QualityRung(), w, h,
                          grey ? "kept" : "LOST");
            Check(backend::QualityRung() == 2 && w == 640 && h == 456 && grey, line);
        }
        toRung(5, 1, 6010);
        {
            const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
            const uint8_t* c = w > 0 && h > 0 ? &px[(static_cast<size_t>(h / 2) * w + w / 2) * 4] : nullptr;
            char line[200];
            std::snprintf(line, sizeof line, "the picture's copy fails: rung %d, display copy %dx%d (expected rung 5, 880x520), centre %02X (cleared)", backend::QualityRung(), w, h,
                          c ? c[0] : 0xFF);
            Check(backend::QualityRung() == 5 && w == 880 && h == 520 && c && c[0] < 8, line);
        }
        toRung(5, 3, 6020);
        {
            unsigned resampled = 0, plain = 0;
            backend::DebugLastChange(resampled, plain);
            backend::LastDisplayCopy(w, h);
            char line[200];
            std::snprintf(line, sizeof line, "a render texture's second copy fails: rung %d, %u resampled, %u resized without the picture", backend::QualityRung(), resampled, plain);
            Check(backend::QualityRung() == 5 && w == 880 && plain == 1 && resampled >= 1, line);
            // every texture of the test still has the size the new rung gives
            CheckTex(10, 64, 32, 5, -1, -1, -1, "red texture after the change (size)");
        }
    }
    // ---- multisampling (rungs 7 and 8): a white triangle over the lower left half of the grey EFB, then a GXV_EFBSWAP copy
    // of the whole EFB (texture 95: the EFB's own texture handed over), an ordinary copy of its lower left quarter (texture
    // 96) and a display copy. Counted in each: pixels between the two colours (partly covered by the triangle) and their
    // distinct levels. At rung 6 the edge is hard (none); at 7 and 8 every picture has them (resolved samples: in the EFB's
    // texture that the swap hands over too, never an unresolved target), 4x with more levels than 2x. The flat parts keep
    // their colours, and a change back to rung 6 keeps the picture (the EFB's texture is the resolved picture).
    if (backend::QualityTopRung() >= 7) {
        std::printf("multisampling:\n");
        const int top = backend::QualityTopRung();
        int prevLevels = -1;
        for (int rung : {6, 7, top}) {
            if (rung == 7 && top == 7 && prevLevels > 0) break;
            backend::ForceQualityRung(rung);
            {
                CmdBuf b;
                b.frame = 7000 + static_cast<uint64_t>(rung);
                Present(b, true); // (the EFB grey)
                backend::Execute(b);
            }
            const float white[4] = {1, 1, 1, 1};
            backend::DebugDrawEfbTriangle(white);
            CmdBuf b;
            b.frame = 7100 + static_cast<uint64_t>(rung);
            CopyTexCmd whole;
            whole.texId = 95;
            whole.src = EfbRect{0, 0, 640, 456};
            whole.dstW = 640;
            whole.dstH = 456;
            Identity(whole.mat);
            b.copyTex.push_back(whole);
            CopyTexCmd quarter = whole;
            quarter.texId = 96;
            quarter.src = EfbRect{0, 228, 320, 228}; // (EFB rows top down: its lower left quarter)
            quarter.dstW = 320;
            quarter.dstH = 228;
            b.copyTex.push_back(quarter);
            Push(b, CMD_COPY_TEX, 0);
            Push(b, CMD_COPY_TEX, 1);
            Present(b, false);
            const unsigned swaps0 = backend::DebugEfbSwaps();
            backend::RequestCapture(true);
            backend::Execute(b);
            backend::RequestCapture(false);
            const unsigned swaps = backend::DebugEfbSwaps() - swaps0;
            struct Pic { const char* name; std::vector<uint8_t> px; int w = 0, h = 0; };
            Pic pics[3];
            pics[0].name = "display copy";
            {
                int w = 0, h = 0;
                pics[0].px = backend::LastDisplayCopy(w, h);
                pics[0].w = w;
                pics[0].h = h;
            }
            pics[1].name = "whole-EFB copy (GXV_EFBSWAP)";
            backend::DebugReadTexture(95, pics[1].px, pics[1].w, pics[1].h);
            pics[2].name = "quarter copy";
            backend::DebugReadTexture(96, pics[2].px, pics[2].w, pics[2].h);
            const int samples = backend::EfbSamples();
            bool ok = samples == std::min<int>(autoq::kLadder[rung].samples, 4);
            int levelsMin = 1 << 30;
            std::string detail;
            for (Pic& p : pics) {
                if (p.w <= 0 || p.h <= 0 || p.px.size() < static_cast<size_t>(p.w) * p.h * 4) { ok = false; detail += std::string(" ") + p.name + ": none"; continue; }
                unsigned edge = 0;
                bool seen[256] = {};
                int levels = 0;
                for (int y = 0; y < p.h; ++y)
                    for (int x = 0; x < p.w; ++x) {
                        const uint8_t v = p.px[(static_cast<size_t>(y) * p.w + x) * 4];
                        if (v > 128 + 6 && v < 255 - 6) {
                            ++edge;
                            if (!seen[v / 8]) { seen[v / 8] = true; ++levels; }
                        }
                    }
                // Flat parts: the triangle's (lower left: the first rows of a GL read-back) white, the opposite corner grey.
                const uint8_t* in = &p.px[(static_cast<size_t>(p.h / 8) * p.w + p.w / 8) * 4];
                const uint8_t* out = &p.px[(static_cast<size_t>(p.h * 7 / 8) * p.w + p.w * 7 / 8) * 4];
                // (the quarter copy is the triangle's own corner: inside it the triangle's edge runs from its top left to its
                // bottom right, so its first rows are white and its top right is grey as well)
                const bool flat = Near(in, 255, 255, 255) && Near(out, 128, 128, 128);
                ok &= flat;
                if (rung == 6) ok &= edge <= static_cast<unsigned>(p.h) / 50;
                else ok &= edge >= static_cast<unsigned>(p.h) / 2;
                levelsMin = std::min(levelsMin, levels);
                char part[160];
                std::snprintf(part, sizeof part, " %s %dx%d: %u edge pixels, %d levels%s;", p.name, p.w, p.h, edge, levels, flat ? "" : " (FLAT AREAS WRONG)");
                detail += part;
            }
            // (4x has at least the levels of 2x; more where the GL really has 2x: llvmpipe makes every multisampled buffer 4x)
            if (rung != 6 && prevLevels > 0 && autoq::kLadder[rung].samples > 2) ok &= levelsMin >= prevLevels;
            if (rung != 6) prevLevels = levelsMin;
            ok &= swaps == 1;
            char line[900];
            std::snprintf(line, sizeof line, "rung %d, %d samples, %u swap copies:%s", rung, samples, swaps, detail.c_str());
            Check(ok, line);
        }
        // Back to 960x544 without multisampling: the picture is kept (the triangle's edge stays as it was resolved).
        backend::ForceQualityRung(6);
        CmdBuf b;
        b.frame = 7200;
        Present(b, false);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        int w = 0, h = 0;
        const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
        bool kept = w == 960 && h == 544 && px.size() >= static_cast<size_t>(w) * h * 4 && backend::EfbSamples() == 1;
        unsigned edge = 0;
        if (kept) {
            kept = Near(&px[(static_cast<size_t>(h / 8) * w + w / 8) * 4], 255, 255, 255) && Near(&px[(static_cast<size_t>(h * 7 / 8) * w + w * 7 / 8) * 4], 128, 128, 128);
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                if (px[i * 4] > 128 + 6 && px[i * 4] < 255 - 6) ++edge;
        }
        char line[200];
        std::snprintf(line, sizeof line, "back to rung 6: display copy %dx%d keeps the picture, %u smoothed edge pixels", w, h, edge);
        Check(kept && edge >= static_cast<unsigned>(h) / 2, line);
        CmdBuf g; // (the EFB grey again for what follows)
        g.frame = 7201;
        Present(g, true);
        backend::Execute(g);
    } else {
        std::printf("multisampling: not available here (no checks)\n");
    }
    // ---- the sharpened present (PresentSharpened): the EFB's left half red, right half grey (display copies' clears of
    // those rectangles), drawn into a 960x544 target at amount 0.5. Flat areas must keep their colour (an unsharp mask
    // changes only edges), and the edge must be sharper than plain bilinear (a red overshoot next to it).
    {
        std::printf("sharpened present:\n");
        backend::ForceQualityRung(2);
        CmdBuf b;
        b.frame = 100;
        CopyDispCmd d;
        d.src = EfbRect{0, 0, 320, 456};
        d.clear.enable = true;
        d.clear.rgba[0] = 0.8f; d.clear.rgba[1] = 0.1f; d.clear.rgba[2] = 0.1f;
        b.copyDisp.push_back(d);
        Push(b, CMD_COPY_DISP, 0);
        Push(b, CMD_PRESENT, 0);
        backend::Execute(b);
        GLuint fbo = 0, tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        std::vector<uint8_t> sharp(960 * 544 * 4), plain(960 * 544 * 4);
        const bool ok1 = backend::PresentSharpened(fbo, 0, 0, 640, 456, 0, 0, 960, 544, 0.5f, true);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, sharp.data());
        const bool ok2 = backend::PresentSharpened(fbo, 0, 0, 640, 456, 0, 0, 960, 544, 0.0f, true);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, plain.data());
        auto at = [](const std::vector<uint8_t>& v, int x, int y) { return &v[(static_cast<size_t>(y) * 960 + x) * 4]; };
        const int y = 272;
        const bool flat = Near(at(sharp, 100, y), 204, 26, 26) && Near(at(sharp, 860, y), 128, 128, 128);
        // the edge is at x = 480 (EFB x 320); 2-3 pixels left of it, the sharpened red must exceed the plain one
        int overshoot = 0;
        for (int x = 470; x < 480; ++x) overshoot = std::max(overshoot, at(sharp, x, y)[0] - at(plain, x, y)[0]);
        char line[200];
        std::snprintf(line, sizeof line, "shader built %d/%d; flat areas keep their colour (%02X%02X%02X, %02X%02X%02X); red overshoot at the edge %d levels", ok1, ok2,
                      at(sharp, 100, y)[0], at(sharp, 100, y)[1], at(sharp, 100, y)[2], at(sharp, 860, y)[0], at(sharp, 860, y)[1], at(sharp, 860, y)[2], overshoot);
        Check(ok1 && ok2 && flat && overshoot >= 4, line);
        char amounts[160];
        std::snprintf(amounts, sizeof amounts, "auto amounts: 640x456 -> %.2f, 960x544 -> %.2f, 544x388 -> %.2f (to a 960x544 screen)", backend::SharpenAmount(640, 456, 960, 544),
                      backend::SharpenAmount(960, 544, 960, 544), backend::SharpenAmount(544, 388, 960, 544));
        Check(std::fabs(backend::SharpenAmount(640, 456, 960, 544) - 0.5f) < 0.01f && backend::SharpenAmount(960, 544, 960, 544) < 0.01f, amounts);
    }
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
