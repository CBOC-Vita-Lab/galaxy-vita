// hrw_gpucost_gl: host unit + real GLES2 FBO pixel-oracle tests for 04-hrw-gpucost.diff:
//   1. Pure logic tests for port::hrwgpu::RectTracker, ConservativeDrawRect, PlanOwnQuads,
//      SceneSwapPlainEligible, and SnapSwapEligible (including GXV_DD_DYN display-texture class guards).
//   2. Real GLES2 FBO pixel-oracle for GXV_HRW_SCENE_SWAP_PLAIN + GXV_HRW_SNAP_SWAP across 3 consecutive
//      pairs (verifying 0/255 max pixel delta on both scene-without-HUD and finished Picture A, plus Clear isolation).
//   3. Real GLES2 RGBA8+stencil FBO pixel-oracle for GXV_HRW_OWN_SCISSOR across clean alpha (Q1 skipped),
//      localized alpha writer (Q1 & Q2 scissored), and ownDirty fallback (full Q2), verifying 0/255 max RGBA
//      delta AND zero residual kOwnBit across the entire stencil plane.
#include "port/hrw_gpucost.h"
#include "vgx_cmd.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
namespace {

void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

GLuint Compile(GLenum type, const char* text) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "COMPILE FAIL:\n%s\n%s\n", log, text);
    }
    Require(ok != 0, "shader compiles");
    return s;
}

GLuint Link(const char* vsSrc, const char* fsSrc) {
    GLuint vs = Compile(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, fsSrc);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    Require(ok != 0, "program links");
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

constexpr int kW = 128, kH = 96;
constexpr GLuint kOwnBit = 0x80u;

GLuint MakeRgbaTex(int w, int h) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return t;
}

void DrawRectColor(GLuint progColor, GLint uCol, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    const float nx0 = x0 * 2.0f / kW - 1.0f, ny0 = y0 * 2.0f / kH - 1.0f;
    const float nx1 = x1 * 2.0f / kW - 1.0f, ny1 = y1 * 2.0f / kH - 1.0f;
    const float pos[8] = {nx0, ny0, nx1, ny0, nx0, ny1, nx1, ny1};
    glUseProgram(progColor);
    glUniform4f(uCol, r, g, b, a);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
}

void DrawFullTex(GLuint progTex, GLint uSrc, GLuint tex) {
    const float pos[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    glUseProgram(progTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(uSrc, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
}

std::vector<uint8_t> ReadTex(GLuint fbo, GLuint tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "readback FBO complete");
    std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

int MaxDelta(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t& diffPx) {
    Require(a.size() == b.size(), "equal pixel buffers");
    int maxd = 0;
    diffPx = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        bool d = false;
        for (int c = 0; c < 4; ++c) {
            const int dl = std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]));
            if (dl > maxd) maxd = dl;
            if (dl != 0) d = true;
        }
        if (d) ++diffPx;
    }
    return maxd;
}

void TestPureLogic() {
    using namespace port::hrwgpu;

    // 1. RectTracker + PlanOwnQuads
    RectTracker alpha, mark;
    alpha.MarkFull(960, 544);
    Require(alpha.IsFull(960, 544), "starts full");
    alpha.ResetEmpty();
    Require(alpha.empty && alpha.Area() == 0, "reset empty after whole-EFB alpha-0 clear");
    mark.Reset(false, 960, 544);
    mark.AddRect(300, 200, 420, 340, 960, 544);
    OwnQuadPlan p = PlanOwnQuads(true, true, false, 960, 544, alpha, mark);
    Require(p.q1 == kQuadSkip, "clean alpha skips Quad 1 completely");
    Require(p.q2 == kQuadScissor && p.q2Rect[0] == 300 && p.q2Rect[1] == 200 && p.q2Rect[2] == 120 && p.q2Rect[3] == 140,
            "Quad 2 scissored to Mario's exact bounding box");

    alpha.AddRect(50, 40, 150, 120, 960, 544);
    p = PlanOwnQuads(true, true, false, 960, 544, alpha, mark);
    Require(p.q1 == kQuadScissor && p.q1Rect[0] == 50 && p.q1Rect[2] == 100, "localized alpha writer scissors Quad 1");

    // Stale ownDirty forces full-screen Quad 2 even when mark box is small.
    p = PlanOwnQuads(true, true, true, 960, 544, alpha, mark);
    Require(p.q2 == kQuadFull, "ownDirty forces full-screen Quad 2");

    // Disabled switch forces full-screen quads.
    p = PlanOwnQuads(false, true, false, 960, 544, alpha, mark);
    Require(p.q1 == kQuadFull && p.q2 == kQuadFull, "off state uses full-screen quads");

    // ConservativeDrawRect intersects scissor, extent, and ab with safety pad.
    int x0, y0, x1, y1;
    const int sc[4] = {100, 50, 400, 300};
    ConservativeDrawRect(960, 544, 1.5f, 1.0f, true, sc, 0, 0, 640, 544, true, 120, 80, 60, 100, 2, x0, y0, x1, y1);
    Require(x0 == 178 && y0 == 78 && x1 == 272 && y1 == 182, "ConservativeDrawRect scales + pads + intersects");

    // 2. SceneSwapPlainEligible guards
    SceneSwapPlainIn sp;
    sp.enabled = true; sp.tgtW = sp.rtW = 960; sp.tgtH = sp.rtH = 544; sp.efbColor = 10; sp.texScene = 11;
    Require(SceneSwapPlainEligible(sp), "plain swap eligible when both plain and matching size");
    sp.efbIsDisplay = true; Require(!SceneSwapPlainEligible(sp), "plain swap rejects display EFB texture");
    sp.efbIsDisplay = false; sp.sceneIsDisplay = true; Require(!SceneSwapPlainEligible(sp), "plain swap rejects display scene texture");
    sp.sceneIsDisplay = false; sp.keepRt = true; Require(!SceneSwapPlainEligible(sp), "plain swap rejects KeepRenderTargets");
    sp.keepRt = false; sp.ovX = 16; Require(!SceneSwapPlainEligible(sp), "plain swap rejects overscan");
    sp.ovX = 0; sp.efbSamples = 4; Require(!SceneSwapPlainEligible(sp), "plain swap rejects MSAA");

    // 3. SnapSwapEligible guards (plain <-> plain with whole color+alpha clear only; display and non-clearing CopyDisp rejected)
    SnapSwapIn ss;
    ss.enabled = true; ss.snapW = ss.rtW = 960; ss.snapH = ss.rtH = 544; ss.efbColor = 10; ss.nextTex = 12; ss.clearWholeColorAlpha = true;
    Require(SnapSwapEligible(ss), "snap swap eligible plain <-> plain with whole clear");
    ss.clearWholeColorAlpha = false;
    Require(!SnapSwapEligible(ss), "snap swap rejects non-clearing CopyDisp");
    ss.clearWholeColorAlpha = true; ss.efbIsDisplay = true; ss.nextIsDisplay = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects display EFB (directd owns scanout buffers)");
    ss.efbIsDisplay = false; ss.nextIsDisplay = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects display nextTex");
    ss.nextIsDisplay = false; ss.hasPrevKeep = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects PrevKeep (c.keepTex != 0)");
    ss.hasPrevKeep = false; ss.maskAtCopy = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects maskAtCopy");
    ss.maskAtCopy = false; ss.captureActive = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects captureActive");
    ss.captureActive = false; ss.flipActive = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects flipActive");
    ss.flipActive = false; ss.dumpActive = true;
    Require(!SnapSwapEligible(ss), "snap swap rejects dumpActive");

    std::printf("unit logic: RectTracker, ConservativeDrawRect, PlanOwnQuads, SceneSwapPlainEligible, SnapSwapEligible OK\n");
}

void TestSwapPixelOracle(GLuint progColor, GLint uCol, GLuint progTex, GLint uSrc) {
    GLuint efbFbo = 0, copyFbo = 0, readFbo = 0;
    glGenFramebuffers(1, &efbFbo);
    glGenFramebuffers(1, &copyFbo);
    glGenFramebuffers(1, &readFbo);

    // Reference buffers (copy path) vs Swap buffers (SceneSwapPlain + SnapSwap 3-buffer rotation).
    GLuint refEfb = MakeRgbaTex(kW, kH), refScene = MakeRgbaTex(kW, kH), refSnap = MakeRgbaTex(kW, kH);
    GLuint swEfb = MakeRgbaTex(kW, kH), swScene = MakeRgbaTex(kW, kH), swSnap = MakeRgbaTex(kW, kH);

    auto drawScene3D = [&](int pairIdx) {
        const float shift = static_cast<float>(pairIdx * 7);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.08f + 0.05f * pairIdx, 0.14f, 0.22f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        DrawRectColor(progColor, uCol, 10 + shift, 12, 85 + shift, 74, 0.78f, 0.34f, 0.19f, 0.0f);
        DrawRectColor(progColor, uCol, 44 + shift, 28, 76 + shift, 66, 0.21f, 0.69f, 0.88f, 1.0f); // Mario's own-pixel alpha=1
    };
    auto drawHudTail = [&](int pairIdx) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        DrawRectColor(progColor, uCol, 6, 4, 46, 20, 0.95f, 0.85f, 0.10f, 0.80f - 0.1f * pairIdx);
        DrawRectColor(progColor, uCol, 88, 74, 122, 92, 0.10f, 0.90f, 0.45f, 0.75f);
        glDisable(GL_BLEND);
    };

    for (int pair = 0; pair < 3; ++pair) {
        // ---- Reference path: SceneSnap copies EFB -> refScene, Snapshot copies EFB -> refSnap, then Clear EFB.
        glBindFramebuffer(GL_FRAMEBUFFER, efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refEfb, 0);
        glViewport(0, 0, kW, kH);
        drawScene3D(pair);

        // Reference SceneSnap copy pass
        glBindFramebuffer(GL_FRAMEBUFFER, copyFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refScene, 0);
        glViewport(0, 0, kW, kH);
        DrawFullTex(progTex, uSrc, refEfb);

        // Reference HUD tail on EFB
        glBindFramebuffer(GL_FRAMEBUFFER, efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refEfb, 0);
        glViewport(0, 0, kW, kH);
        drawHudTail(pair);

        // Reference Snapshot copy pass
        glBindFramebuffer(GL_FRAMEBUFFER, copyFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refSnap, 0);
        glViewport(0, 0, kW, kH);
        DrawFullTex(progTex, uSrc, refEfb);

        // Reference present readback & Clear
        const std::vector<uint8_t> refPresented = ReadTex(readFbo, refEfb);
        glBindFramebuffer(GL_FRAMEBUFFER, efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refEfb, 0);
        glClearColor(0.9f, 0.1f, 0.9f, 0.5f);
        glClear(GL_COLOR_BUFFER_BIT);

        // ---- Swap path: SceneSwapPlain swaps swEfb <-> swScene + 1:1 draw-back; Snapshot skips copy;
        //      Present reads swEfb; SnapSwapCommit swaps swEfb <-> swSnap right before Clear!
        glBindFramebuffer(GL_FRAMEBUFFER, efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swEfb, 0);
        glViewport(0, 0, kW, kH);
        drawScene3D(pair);

        // SceneSwapPlain: old=swEfb -> swScene; fresh=swScene -> swEfb; 1:1 DrawQuad(old) into swEfb
        std::swap(swEfb, swScene);
        glBindFramebuffer(GL_FRAMEBUFFER, efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swEfb, 0);
        glViewport(0, 0, kW, kH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        DrawFullTex(progTex, uSrc, swScene);

        // HUD tail directly on swEfb
        drawHudTail(pair);

        // Present reads swEfb (ZERO snapshot copy pass!)
        const std::vector<uint8_t> swPresented = ReadTex(readFbo, swEfb);

        // SnapSwapCommit before Clear: finishedA=swEfb -> swSnap; fresh=swSnap -> swEfb
        std::swap(swEfb, swSnap);
        glBindFramebuffer(GL_FRAMEBUFFER, efbFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swEfb, 0);
        glClearColor(0.9f, 0.1f, 0.9f, 0.5f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Verify ALL 3 textures are distinct and ALL outputs match 0/255!
        Require(swEfb != swScene && swEfb != swSnap && swScene != swSnap, "3-buffer rotation keeps all textures distinct");
        const std::vector<uint8_t> rScenePx = ReadTex(readFbo, refScene), sScenePx = ReadTex(readFbo, swScene);
        const std::vector<uint8_t> rSnapPx = ReadTex(readFbo, refSnap), sSnapPx = ReadTex(readFbo, swSnap);
        size_t dPres = 0, dScene = 0, dSnap = 0;
        const int mdPres = MaxDelta(refPresented, swPresented, dPres);
        const int mdScene = MaxDelta(rScenePx, sScenePx, dScene);
        const int mdSnap = MaxDelta(rSnapPx, sSnapPx, dSnap);
        std::printf("swap oracle pair %d: present diffPx %zu (max %d/255), texScene diffPx %zu (max %d/255), W.tex diffPx %zu (max %d/255)\n",
                    pair, dPres, mdPres, dScene, mdScene, dSnap, mdSnap);
        Require(mdPres == 0 && mdScene == 0 && mdSnap == 0, "SceneSwapPlain + SnapSwap pixel-identical");
    }

    glDeleteTextures(1, &refEfb); glDeleteTextures(1, &refScene); glDeleteTextures(1, &refSnap);
    glDeleteTextures(1, &swEfb); glDeleteTextures(1, &swScene); glDeleteTextures(1, &swSnap);
    glDeleteFramebuffers(1, &efbFbo); glDeleteFramebuffers(1, &copyFbo); glDeleteFramebuffers(1, &readFbo);
}

void TestOwnScissorPixelOracle(GLuint progColor, GLint uCol) {
    GLuint fbo = 0, tex = 0, stRb = 0;
    glGenFramebuffers(1, &fbo);
    tex = MakeRgbaTex(kW, kH);
    glGenRenderbuffers(1, &stRb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, stRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kW, kH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, stRb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stRb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, kW, kH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stRb);
    }
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8+stencil FBO complete");

    auto runCase = [&](const char* name, bool localizedAlpha, bool ownDirty) {
        auto render = [&](bool scissorOn, std::vector<uint8_t>& outRgba, size_t& residualOwnPx) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, kW, kH);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilMask(0xFF);
            glClearColor(0.20f, 0.30f, 0.40f, 0.0f);
            glClearStencil(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            port::hrwgpu::RectTracker alphaBox, markBox;
            alphaBox.ResetEmpty(); // whole-EFB clear alpha=0
            markBox.Reset(ownDirty, kW, kH);

            if (ownDirty) {
                // Stale kOwnBit mark in top-right corner outside Mario's box
                glEnable(GL_STENCIL_TEST);
                glStencilMask(kOwnBit);
                glStencilFunc(GL_ALWAYS, kOwnBit, kOwnBit);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                DrawRectColor(progColor, uCol, 102, 72, 120, 90, 0, 0, 0, 0);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDisable(GL_STENCIL_TEST);
            }

            // Opaque world geometry with alphaWrite masked off (does NOT dirty alphaBox)
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            DrawRectColor(progColor, uCol, 4, 4, 124, 92, 0.45f, 0.55f, 0.65f, 0.99f);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            if (localizedAlpha) {
                // Localized non-Mario draw writing non-zero alpha in [10..34, 12..44]
                DrawRectColor(progColor, uCol, 10, 12, 34, 44, 0.90f, 0.25f, 0.15f, 0.75f);
                alphaBox.AddRect(8, 10, 36, 46, kW, kH);
            }

            // Mario draw marking kOwnBit in [48..84, 26..66]
            glEnable(GL_STENCIL_TEST);
            glStencilMask(kOwnBit);
            glStencilFunc(GL_ALWAYS, kOwnBit, kOwnBit);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            DrawRectColor(progColor, uCol, 48, 26, 84, 66, 0.85f, 0.12f, 0.18f, 0.35f);
            markBox.AddRect(46, 24, 86, 68, kW, kH);

            // OwnMarkToAlpha pass (colour kept via blend ZERO/ONE, alpha overwritten via ONE/ZERO)
            const port::hrwgpu::OwnQuadPlan qp = port::hrwgpu::PlanOwnQuads(scissorOn, true, ownDirty, kW, kH, alphaBox, markBox);
            if (scissorOn && !localizedAlpha) Require(qp.q1 == port::hrwgpu::kQuadSkip, "clean alpha skips Q1");
            if (scissorOn && localizedAlpha) Require(qp.q1 == port::hrwgpu::kQuadScissor, "localized alpha scissors Q1");
            if (scissorOn && !ownDirty) Require(qp.q2 == port::hrwgpu::kQuadScissor, "clean ownMark scissors Q2");
            if (scissorOn && ownDirty) Require(qp.q2 == port::hrwgpu::kQuadFull, "ownDirty uses full Q2");

            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
            glStencilMask(kOwnBit);
            if (qp.q1 != port::hrwgpu::kQuadSkip) {
                if (qp.q1 == port::hrwgpu::kQuadScissor) { glEnable(GL_SCISSOR_TEST); glScissor(qp.q1Rect[0], qp.q1Rect[1], qp.q1Rect[2], qp.q1Rect[3]); }
                else glDisable(GL_SCISSOR_TEST);
                glStencilFunc(GL_NOTEQUAL, kOwnBit, kOwnBit);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                DrawRectColor(progColor, uCol, 0, 0, kW, kH, 0.0f, 0.0f, 0.0f, 0.0f);
            }
            if (qp.q2 != port::hrwgpu::kQuadSkip) {
                if (qp.q2 == port::hrwgpu::kQuadScissor) { glEnable(GL_SCISSOR_TEST); glScissor(qp.q2Rect[0], qp.q2Rect[1], qp.q2Rect[2], qp.q2Rect[3]); }
                else glDisable(GL_SCISSOR_TEST);
                glStencilFunc(GL_EQUAL, kOwnBit, kOwnBit);
                glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                DrawRectColor(progColor, uCol, 0, 0, kW, kH, 0.0f, 0.0f, 0.0f, 1.0f);
            }
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);

            outRgba.resize(static_cast<size_t>(kW) * kH * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, outRgba.data());

            // Stencil probe: paint magenta anywhere kOwnBit is STILL set (must touch 0 pixels!)
            glStencilMask(0);
            glStencilFunc(GL_EQUAL, kOwnBit, kOwnBit);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            DrawRectColor(progColor, uCol, 0, 0, kW, kH, 1.0f, 0.0f, 1.0f, 1.0f);
            glDisable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            std::vector<uint8_t> afterProbe(static_cast<size_t>(kW) * kH * 4);
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, afterProbe.data());
            residualOwnPx = 0;
            MaxDelta(outRgba, afterProbe, residualOwnPx);
        };

        std::vector<uint8_t> refPx, optPx;
        size_t refRes = 0, optRes = 0;
        render(false, refPx, refRes);
        render(true, optPx, optRes);
        size_t diffPx = 0;
        const int maxd = MaxDelta(refPx, optPx, diffPx);
        std::printf("own scissor %-26s: diffPx %zu (max %d/255), residual kOwnBit px %zu\n", name, diffPx, maxd, optRes);
        Require(maxd == 0 && diffPx == 0 && refRes == 0 && optRes == 0, "OwnMarkToAlpha scissored/skipped output is bit-exact with 0 residual stencil bits");
    };

    runCase("clean-alpha (Q1 skip)", false, false);
    runCase("localized-alpha (Q1+Q2)", true, false);
    runCase("ownDirty fallback", true, true);

    glDeleteRenderbuffers(1, &stRb);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
}

} // namespace

int main() {
    TestPureLogic();
    Require(vgx::host::CreateContext(), "GL context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    glDisable(GL_DITHER);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    const char* kVs =
        "attribute vec2 a_pos;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    const char* kFsColor =
        "precision highp float;\n"
        "uniform vec4 u_col;\n"
        "void main() { gl_FragColor = u_col; }\n";
    const char* kFsTex =
        "precision highp float;\n"
        "uniform sampler2D u_src;\n"
        "varying vec2 v_uv;\n"
        "void main() { gl_FragColor = texture2D(u_src, v_uv); }\n";

    GLuint progColor = Link(kVs, kFsColor);
    GLuint progTex = Link(kVs, kFsTex);
    GLint uCol = glGetUniformLocation(progColor, "u_col");
    GLint uSrc = glGetUniformLocation(progTex, "u_src");

    TestSwapPixelOracle(progColor, uCol, progTex, uSrc);
    TestOwnScissorPixelOracle(progColor, uCol);

    glDeleteProgram(progColor);
    glDeleteProgram(progTex);
    std::printf("PASS\n");
    return 0;
}
