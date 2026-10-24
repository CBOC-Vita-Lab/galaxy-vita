// Headless stand-in for the OpenXR frame loop: drives vr_game.cpp with two
// simulated eyes and writes what the swapchains would receive, side by side,
// to a PNG.  Used by tools/run_headless.sh (PETARI_XRSIM=1) to check the
// headset presentation without wearing the headset.
#include <GLES3/gl32.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "port/port.h"
#include "../gx/gl_renderer.h"
#include "vr_renderer.h"

extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h);

namespace {

bool sInit = false;
GLuint sTex[2], sFbo[2];
const int kRecW = 1680, kRecH = 1760;  // Quest 3 recommended eye size
int kW = kRecW, kH = kRecH;            // swapchain stand-ins (vr::swapchainScale())
double sTime = 1.0;
// PETARI_XRSIM_MV=1: SpaceWarp's motion vectors and depth too, at a quarter
// of the recommended eye size; the left eye's are saved next to the shot as
// <shot>_mv.png (red/green: horizontal/vertical motion around grey, blue:
// 0 world, 128 fixed in the room, 255 player).
bool sMotion = false;
const int kMvW = kRecW / 4, kMvH = kRecH / 4;
GLuint sMvTex[2], sMvDepth[2];
vr::Extent sUsed{0, 0};  // part of the eye images rendered (Super Resolution: the render size)
// PETARI_XRSIM_LAYERS=1: the panels go out as UI layers, as in the headset;
// their images are composited into the eyes here in the compositor's stead.
bool sLayers = false;
GLuint sLayerTex[vr::kUiLayerCount], sLayerFbo[vr::kUiLayerCount];

xm::Quat axisAngle(xm::Vec3 axis, float deg) {
    float h = deg * 0.5f * 3.14159265f / 180.0f;
    return {axis.x * sinf(h), axis.y * sinf(h), axis.z * sinf(h), cosf(h)};
}

xm::Quat mul(xm::Quat a, xm::Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

}  // namespace

// Renders `frames` simulated display frames (72 Hz) and saves the last to
// `path`; without a path (continuous rendering for profiling) each frame is
// finished on the GPU before returning, like the compositor would pace it.
extern "C" void port_headless_xrsim(const char* path, int frames, float yawDeg, float pitchDeg) {
    if (!sInit) {
        sInit = true;
        // PETARI_VRINI=<path>: the settings file the VR app reads (and the
        // settings panel writes).
        if (const char* ini = getenv("PETARI_VRINI")) {
            vr::loadSettings(ini);
        }
        // The passthrough setting works here too: the room shows as a flat
        // grey-green in the pictures saved (see below).
        vr::setPassthroughAvailable(true);
        vr::init();
        // PETARI_XRSIM_SWAPSCALE=<s>: swapchain images at that size instead
        // (relative to the recommended one), e.g. larger than the render
        // size so the composite scales.
        float swapScale = getenv("PETARI_XRSIM_SWAPSCALE") ? (float)atof(getenv("PETARI_XRSIM_SWAPSCALE")) : vr::swapchainScale();
        kW = (int)lroundf(kRecW * swapScale);
        kH = (int)lroundf(kRecH * swapScale);
        glGenTextures(2, sTex);
        glGenFramebuffers(2, sFbo);
        for (int e = 0; e < 2; e++) {
            glBindTexture(GL_TEXTURE_2D, sTex[e]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, kW, kH);  // like the swapchain
            glBindFramebuffer(GL_FRAMEBUFFER, sFbo[e]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sTex[e], 0);
        }
        sLayers = getenv("PETARI_XRSIM_LAYERS") != nullptr;
        if (sLayers) {
            glGenTextures(vr::kUiLayerCount, sLayerTex);
            glGenFramebuffers(vr::kUiLayerCount, sLayerFbo);
            for (int k = 0; k < vr::kUiLayerCount; k++) {
                int w, h;
                vr::uiLayerSize(k, &w, &h);
                glBindTexture(GL_TEXTURE_2D, sLayerTex[k]);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, w, h);  // like the swapchain
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, sLayerFbo[k]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sLayerTex[k], 0);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            vr::setUiLayers(true);
        }
        sMotion = getenv("PETARI_XRSIM_MV") != nullptr;
        if (sMotion) {
            glGenTextures(2, sMvTex);
            glGenTextures(2, sMvDepth);
            for (int e = 0; e < 2; e++) {
                glBindTexture(GL_TEXTURE_2D, sMvTex[e]);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, kMvW, kMvH);
                glBindTexture(GL_TEXTURE_2D, sMvDepth[e]);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, kMvW, kMvH);
            }
            vr::setMotionSize(kMvW, kMvH);
        }
    }
    vr::FrameInfo fi{};
    xm::Quat q = mul(axisAngle({0, 1, 0}, yawDeg), axisAngle({1, 0, 0}, pitchDeg));
    const float halfIpd = 0.032f;
    for (int e = 0; e < 2; e++) {
        vr::EyeInfo& eye = fi.eyes[e];
        xm::Vec3 offset = xm::rotate(q, {e == 0 ? -halfIpd : halfIpd, 0, 0});
        eye.position = offset;
        eye.orientation = q;
        eye.view = xm::inversePose(q, offset);
        const float r = 48.0f * 3.14159265f / 180.0f;
        eye.proj = xm::projectionFov(-r, r, r, -r, vr::kNearZ, vr::kFarZ);
        eye.width = kW;
        eye.height = kH;
    }
    fi.cullTanX = fi.cullTanY = tanf(48.0f * 3.14159265f / 180.0f);
    fi.motion = sMotion;
    // PETARI_XRSIM_BUDGET=<ms>: GPU time allowed per eye, to exercise the
    // dynamic resolution (none: the render scale stays at 1).
    static const float sBudget = getenv("PETARI_XRSIM_BUDGET") ? (float)atof(getenv("PETARI_XRSIM_BUDGET")) : 0.0f;
    fi.eyeBudgetMs = sBudget;
    // Right controller held low and to the right, aiming just past the
    // diorama's anchor.  PETARI_XRSIM_AIM="t0-t1:x,y,z[:A];..." aims it at
    // stage point x,y,z instead from t0 to t1 s after boot (holding A with
    // :A), e.g. at the settings panel beside the pause menu.
    xm::Vec3 aimFrom{0.22f, -0.35f, -0.25f};
    xm::Vec3 aimAt{0.15f, -0.8f, -1.55f};
    bool aDown = false;
    if (const char* spec = getenv("PETARI_XRSIM_AIM")) {
        const char* t0env = getenv("PETARI_T0_MS");
        double now = (port_host_time_ns() / 1000000 - (t0env ? atoll(t0env) : 0)) / 1000.0;
        for (const char* p = spec; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr) {
            float t0, t1, x, y, z;
            char a = 0;
            int n = sscanf(p, "%f-%f:%f,%f,%f:%c", &t0, &t1, &x, &y, &z, &a);
            if (n >= 5 && now >= t0 && now < t1) {
                aimAt = {x, y, z};
                aDown = n == 6 && a == 'A';
            }
        }
    }
    // PETARI_XRSIM_TURN="t:dir;...": snap turns of the diorama (the right
    // stick in the headset) t s after boot, dir 1 right, -1 left.
    if (const char* turns = getenv("PETARI_XRSIM_TURN")) {
        static int sTurnsDone = 0;
        const char* t0env = getenv("PETARI_T0_MS");
        double now = (port_host_time_ns() / 1000000 - (t0env ? atoll(t0env) : 0)) / 1000.0;
        int index = 0;
        for (const char* p = turns; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr, index++) {
            float t;
            int dir;
            if (sscanf(p, "%f:%d", &t, &dir) != 2 || index < sTurnsDone) continue;
            if (now < t) break;
            vr::snapTurn(dir);
            sTurnsDone = index + 1;
        }
    }
    for (int i = 0; i < frames; i++) {
        sTime += 1.0 / 72.0;
        fi.time = sTime;
        float px, py;
        xm::Vec3 aimDir = xm::normalize(aimAt - aimFrom);  // unit, like the controller's
        vr::pointerFromRay(aimFrom, aimDir, &px, &py);
        float onPanel = vr::settingsPointer(aimFrom, aimDir, aDown);
        if (onPanel > 0.0f) {
            vr::setAimLength(onPanel);
        }
        vr::beginFrame(fi);
        for (int e = 0; e < 2; e++) {
            sUsed = vr::renderEye(e, fi, sFbo[e], kW, kH);
        }
        if (sLayers) {
            for (int k : vr::kUiLayerOrder) {
                vr::UiLayer l = vr::uiLayer(k);
                if (!l.visible) continue;
                if (l.changed) vr::drawUiLayer(k, sLayerFbo[k]);
                for (int e = 0; e < 2; e++) {
                    if (l.eye != vr::kBothEyes && l.eye != (e == 0 ? vr::kLeftEye : vr::kRightEye)) continue;
                    glBindFramebuffer(GL_FRAMEBUFFER, sFbo[e]);
                    glViewport(0, 0, sUsed.width, sUsed.height);
                    vr::compositeUiLayer(k, sLayerTex[k], fi.eyes[e].proj * fi.eyes[e].view);
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        if (sMotion) {
            for (int e = 0; e < 2; e++) {
                vr::renderMotion(e, fi, sMvTex[e], sMvDepth[e]);
            }
            xm::Quat dq;
            xm::Vec3 dp;
            bool cut = vr::finishMotion(&dq, &dp);
            static int sLogged = 0;
            if (cut || sLogged++ % 120 == 0) {
                port_log("xrsim: motion: %s, room moved %.4f %.4f %.4f m, turned %.4f rad", cut ? "not to extrapolate" : "extrapolate", dp.x, dp.y, dp.z,
                         2.0f * acosf(fminf(1.0f, fabsf(dq.w))));
            }
        }
    }
    if (!path) {
        int64_t t0 = port_host_time_ns();
        glFinish();
        port_perf_gpu_wait(port_host_time_ns() - t0);
        return;
    }
    int rw = sUsed.width, rh = sUsed.height;
    std::vector<unsigned char> px((size_t)rw * 2 * rh * 4), eyePx((size_t)rw * rh * 4);
    // PETARI_XRSIM_SMALL: the left eye only, at half size (quick to save,
    // for sequences).
    static const bool sSmall = getenv("PETARI_XRSIM_SMALL") != nullptr;
    for (int e = 0; e < (sSmall ? 1 : 2); e++) {
        glBindFramebuffer(GL_FRAMEBUFFER, sFbo[e]);
        glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, eyePx.data());
        if (sSmall) {
            for (int y = 0; y < rh / 2; y++) {
                for (int x = 0; x < rw / 2; x++) {
                    for (int c = 0; c < 4; c++) {
                        const unsigned char* s = &eyePx[((size_t)(y * 2) * rw + x * 2) * 4 + c];
                        px[((size_t)y * (rw / 2) + x) * 4 + c] = (unsigned char)((s[0] + s[4] + s[rw * 4] + s[rw * 4 + 4] + 2) / 4);
                    }
                }
            }
            continue;
        }
        for (int y = 0; y < rh; y++) {
            memcpy(&px[((size_t)y * rw * 2 + (size_t)e * rw) * 4], &eyePx[(size_t)y * rw * 4], (size_t)rw * 4);
        }
    }
    // Where the eye images are see-through (the passthrough setting) the
    // compositor shows the room: a flat colour stands for it here.
    const unsigned char kRoom[3] = {96, 132, 108};
    for (size_t i = 0; i < px.size(); i += 4) {
        unsigned a = px[i + 3];
        for (int c = 0; c < 3 && a < 255; c++) {
            unsigned v = px[i + c] + (255 - a) * kRoom[c] / 255;
            px[i + c] = (unsigned char)(v > 255 ? 255 : v);
        }
        px[i + 3] = 255;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (sSmall) {
        port_headless_write_png(path, px.data(), rw / 2, rh / 2);
    } else {
        port_headless_write_png(path, px.data(), rw * 2, rh);
    }
    port_log("xrsim: %d frames -> %s (eyes %dx%d of %dx%d)", frames, path, rw, rh, kW, kH);
    if (sMotion) {
        std::vector<unsigned char> img;
        std::string summary;
        vr::motionDebugImage(sMvTex[0], &img, &summary);
        std::string mvPath = path;
        if (mvPath.size() > 4 && mvPath.compare(mvPath.size() - 4, 4, ".png") == 0) mvPath.resize(mvPath.size() - 4);
        mvPath += "_mv.png";
        port_headless_write_png(mvPath.c_str(), img.data(), kMvW, kMvH);
        port_log("xrsim: motion vectors -> %s: %s", mvPath.c_str(), summary.c_str());
    }
    // The aim passes 0.15 m right of the anchor: in the world, its ray should
    // miss the watched point by about 0.15 m / scale.
    float o[3], d[3];
    const gpu::CameraInfo& cam = gpu::renderer().camera();
    if (port_vr_pointer_ray(o, d) && cam.valid) {
        xm::Vec3 rel{cam.watch[0] - o[0], cam.watch[1] - o[1], cam.watch[2] - o[2]};
        xm::Vec3 dir{d[0], d[1], d[2]};
        xm::Vec3 off = rel - dir * xm::dot(rel, dir);
        port_log("xrsim: aim ray passes %.0f units from the watched point, %.0f units along", xm::length(off), xm::dot(rel, dir));
    }
}
