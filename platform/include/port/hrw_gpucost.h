#pragma once
// hrw_gpucost: exact helpers for reducing 60 Assist Picture A GPU overhead
// (GXV_HRW_SCENE_SWAP_PLAIN, GXV_HRW_SNAP_SWAP, GXV_HRW_OWN_SCISSOR).
// Pure header shared by vgx_backend_gl.cpp and tests/hrw_gpucost_gl.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace port::hrwgpu {

// Conservative pixel-space bounding rectangle [x0, y0, x1, y1) inside [0, rtW) x [0, rtH).
struct RectTracker {
    bool empty = true;
    bool full = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    void ResetEmpty() {
        empty = true;
        full = false;
        x0 = y0 = x1 = y1 = 0;
    }

    void MarkFull(int rtW, int rtH) {
        if (rtW <= 0 || rtH <= 0) {
            ResetEmpty();
            return;
        }
        empty = false;
        full = true;
        x0 = 0;
        y0 = 0;
        x1 = rtW;
        y1 = rtH;
    }

    void Reset(bool startFull, int rtW, int rtH) {
        if (startFull) MarkFull(rtW, rtH);
        else ResetEmpty();
    }

    void AddRect(int ax0, int ay0, int ax1, int ay1, int rtW, int rtH) {
        if (full || rtW <= 0 || rtH <= 0) return;
        const int cx0 = std::clamp(ax0, 0, rtW);
        const int cy0 = std::clamp(ay0, 0, rtH);
        const int cx1 = std::clamp(ax1, 0, rtW);
        const int cy1 = std::clamp(ay1, 0, rtH);
        if (cx1 <= cx0 || cy1 <= cy0) return;
        if (empty) {
            empty = false;
            x0 = cx0; y0 = cy0; x1 = cx1; y1 = cy1;
        } else {
            x0 = std::min(x0, cx0);
            y0 = std::min(y0, cy0);
            x1 = std::max(x1, cx1);
            y1 = std::max(y1, cy1);
        }
        if (x0 <= 0 && y0 <= 0 && x1 >= rtW && y1 >= rtH) {
            full = true;
            x0 = 0; y0 = 0; x1 = rtW; y1 = rtH;
        }
    }

    bool IsFull(int rtW, int rtH) const {
        return !empty && (full || (x0 <= 0 && y0 <= 0 && x1 >= rtW && y1 >= rtH));
    }

    uint64_t Area() const {
        if (empty || x1 <= x0 || y1 <= y0) return 0;
        return static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
    }
};

// Conservative target-pixel bounds for a draw from its active GL scissor (if enabled),
// EFB extent (exX/Y/W/H), and optional projected vertex bounds (abX/Y/W/H), padded by `padPx` pixels.
inline void ConservativeDrawRect(int rtW, int rtH, float sx, float sy,
                                 bool scOn, const int sc[4],
                                 int exX, int exY, int exW, int exH,
                                 bool hasAb, int abX, int abY, int abW, int abH,
                                 int padPx,
                                 int& outX0, int& outY0, int& outX1, int& outY1) {
    int x0 = 0, y0 = 0, x1 = rtW, y1 = rtH;
    if (scOn && sc) {
        x0 = std::max(x0, sc[0]);
        y0 = std::max(y0, sc[1]);
        x1 = std::min(x1, sc[0] + sc[2]);
        y1 = std::min(y1, sc[1] + sc[3]);
    }
    auto intersectEfb = [&](int bx, int by, int bw, int bh) {
        if (bw <= 0 || bh <= 0) return;
        const int px0 = static_cast<int>(std::floor(static_cast<float>(bx) * sx)) - padPx;
        const int py0 = static_cast<int>(std::floor(static_cast<float>(by) * sy)) - padPx;
        const int px1 = static_cast<int>(std::ceil(static_cast<float>(bx + bw) * sx)) + padPx;
        const int py1 = static_cast<int>(std::ceil(static_cast<float>(by + bh) * sy)) + padPx;
        x0 = std::max(x0, px0);
        y0 = std::max(y0, py0);
        x1 = std::min(x1, px1);
        y1 = std::min(y1, py1);
    };
    intersectEfb(exX, exY, exW, exH);
    if (hasAb) intersectEfb(abX, abY, abW, abH);
    outX0 = std::clamp(x0, 0, rtW);
    outY0 = std::clamp(y0, 0, rtH);
    outX1 = std::clamp(x1, 0, rtW);
    outY1 = std::clamp(y1, 0, rtH);
}

// Decision for OwnMarkToAlpha's two quads under GXV_HRW_OWN_SCISSOR=1.
enum QuadAction : uint8_t {
    kQuadFull = 0,     // full-screen quad (no scissor)
    kQuadScissor = 1,  // scissored to [x0, y0, x1 - x0, y1 - y0]
    kQuadSkip = 2,     // provably a no-op (0 pixels)
};

struct OwnQuadPlan {
    QuadAction q1 = kQuadFull;
    int q1Rect[4] = {0, 0, 0, 0};
    QuadAction q2 = kQuadFull;
    int q2Rect[4] = {0, 0, 0, 0};
};

inline OwnQuadPlan PlanOwnQuads(bool scissorOn, bool live, bool ownDirty, int rtW, int rtH,
                                const RectTracker& alphaBox, const RectTracker& markBox) {
    OwnQuadPlan p;
    p.q1Rect[2] = rtW; p.q1Rect[3] = rtH;
    p.q2Rect[2] = rtW; p.q2Rect[3] = rtH;
    if (!live) {
        p.q1 = kQuadSkip;
    } else if (scissorOn) {
        if (alphaBox.empty) {
            p.q1 = kQuadSkip;
        } else if (!alphaBox.IsFull(rtW, rtH)) {
            p.q1 = kQuadScissor;
            p.q1Rect[0] = alphaBox.x0;
            p.q1Rect[1] = alphaBox.y0;
            p.q1Rect[2] = alphaBox.x1 - alphaBox.x0;
            p.q1Rect[3] = alphaBox.y1 - alphaBox.y0;
        }
    }
    if (scissorOn && !ownDirty) {
        if (markBox.empty) {
            p.q2 = kQuadSkip;
        } else if (!markBox.IsFull(rtW, rtH)) {
            p.q2 = kQuadScissor;
            p.q2Rect[0] = markBox.x0;
            p.q2Rect[1] = markBox.y0;
            p.q2Rect[2] = markBox.x1 - markBox.x0;
            p.q2Rect[3] = markBox.y1 - markBox.y0;
        }
    }
    return p;
}

// Eligibility for GXV_HRW_SCENE_SWAP_PLAIN=1 (plain RGBA8 hand-over when directd is not active).
struct SceneSwapPlainIn {
    bool enabled = false;
    int efbSamples = 1;
    int ovX = 0, ovY = 0;
    int tgtW = 0, tgtH = 0, rtW = 0, rtH = 0;
    uint32_t efbColor = 0, texScene = 0;
    bool efbIsDisplay = false, sceneIsDisplay = false;
    bool sceneBuf = false;
    bool keepRt = false;
};

inline bool SceneSwapPlainEligible(const SceneSwapPlainIn& in) {
    if (!in.enabled || in.keepRt) return false;
    if (in.efbSamples > 1 || in.ovX || in.ovY) return false;
    if (in.rtW <= 0 || in.rtH <= 0 || in.tgtW != in.rtW || in.tgtH != in.rtH) return false;
    if (!in.efbColor || !in.texScene || in.efbColor == in.texScene) return false;
    if (in.sceneBuf || in.efbIsDisplay || in.sceneIsDisplay) return false;
    return true;
}

// Eligibility for GXV_HRW_SNAP_SWAP=1 (deferred zero-copy hand-over after present, before clear).
struct SnapSwapIn {
    bool enabled = false;
    bool keepRt = false;
    bool dumpActive = false;
    bool flipActive = false;
    bool captureActive = false;
    bool hudNativeEngaged = false;
    bool wantsSnapDepth = false;
    bool maskAtCopy = false;
    bool hasPrevKeep = false;
    int efbSamples = 1;
    int ovX = 0, ovY = 0;
    int snapW = 0, snapH = 0, rtW = 0, rtH = 0;
    uint32_t efbColor = 0, nextTex = 0;
    bool efbIsDisplay = false, nextIsDisplay = false;
    bool clearWholeColorAlpha = false;
};

inline bool SnapSwapEligible(const SnapSwapIn& in) {
    if (!in.enabled || in.keepRt) return false;
    if (in.dumpActive || in.flipActive || in.captureActive || in.hudNativeEngaged) return false;
    if (in.wantsSnapDepth || in.maskAtCopy || in.hasPrevKeep) return false;
    if (in.efbSamples > 1 || in.ovX || in.ovY) return false;
    if (in.rtW <= 0 || in.rtH <= 0 || in.snapW != in.rtW || in.snapH != in.rtH) return false;
    if (!in.efbColor || !in.nextTex || in.efbColor == in.nextTex) return false;
    // Direct-display scanout ownership is managed by directd::Commit(); only plain EFB textures swap here,
    // and CopyDisp must clear the whole colour+alpha plane right after SnapSwapCommit so recycled nextTex never leaks.
    if (in.efbIsDisplay || in.nextIsDisplay || !in.clearWholeColorAlpha) return false;
    return true;
}

} // namespace port::hrwgpu
