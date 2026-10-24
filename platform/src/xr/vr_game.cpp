// Presents the game in the headset.
//
// Frames with a 3D camera (gameplay) are rendered per eye through the VR rig
// (vr_rig.h): the world becomes a diorama in front of the player, and the
// HUD (the draws between the HUD markers) is rendered once into its own
// target and shown on a panel in front of the player.  Frames without a
// camera (logos, menus, movies) are shown on a virtual screen.
#include <GLES3/gl32.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>

#include "../gx/gl_renderer.h"
#include "../gx/gpu.h"
#include "port/port.h"
#include "vr_renderer.h"
#include "vr_rig.h"

std::atomic<uint64_t> gPresentedFrames{0};

namespace {

// sRGB-encoded game colors -> linear, so the sRGB swapchain stores them as is.
const char* kCommon = R"(
vec3 toLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
)";

const char* kBlitVs = R"(#version 320 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The composite of the diorama into the eye image.  Its first %s is the
// common code, the second defines sourceColor(): the diorama's colour at the
// pixel (kBlitPlain, kBlitCas1, kBlitCasScaled).
const char* kBlitFs = R"(#version 320 es
precision mediump float;
uniform sampler2D uTex;
uniform float uBrightness;
uniform float uVignette;  // 0 = off .. 1 = strong
uniform vec4 uWipe;       // closed (0..1), kind (0 fade, 1 ring), ring centre uv
uniform vec4 uWipeColor;  // linear colour, w = width / height of the eye
in vec2 vUv;
out vec4 oColor;
%s
%s
void main() {
    vec2 d = vUv * 2.0 - 1.0;
    float edge = smoothstep(0.35, 1.05, length(d));
    float v = 1.0 - uVignette * edge;
    vec3 c = toLinear(sourceColor()) * v;
    if (uWipe.y < 0.5) {
        c = mix(c, uWipeColor.rgb, uWipe.x);
    } else {
        // A ring closing on its centre (radius in units of the eye's height;
        // 1.6 clears the view from any centre on it).
        float r = (1.0 - uWipe.x) * 1.6;
        float inside = 1.0 - smoothstep(r - 0.015, r, length((vUv - uWipe.zw) * vec2(uWipeColor.w, 1.0)));
        c = mix(uWipeColor.rgb, c, inside);
    }
    oColor = vec4(c * uBrightness, 1.0);
}
)";

const char* kBlitPlain = R"(
vec3 sourceColor() { return texture(uTex, vUv).rgb; }
)";

// AMD FidelityFX CAS (contrast adaptive sharpening), after CasFilter in
// ffx_cas.h (https://github.com/GPUOpen-Effects/FidelityFX-CAS):
//   Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions: The above copyright notice and this
//   permission notice shall be included in all copies or substantial
//   portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//   WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH
//   THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// A cross-shaped sharpening filter whose strength follows the local
// contrast, so flat areas and strong edges are sharpened less (no
// ringing); the weights come from the green channel, as ffx_cas.h's fast
// path uses them for all three.  Runs on the game's sRGB-encoded colours.
//   uCasPeak: -1 / mix(8, 5, sharpness)
//   uCasScale: diorama pixels per eye image pixel (the scaled variant)
// kBlitCas1: diorama and eye image the same size (Super Resolution).
const char* kBlitCas1 = R"(
uniform float uCasPeak;
vec3 casLoad(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0).rgb; }
vec3 sourceColor() {
    ivec2 sp = ivec2(gl_FragCoord.xy);
    vec3 a = casLoad(sp + ivec2(-1, -1));
    vec3 b = casLoad(sp + ivec2(0, -1));
    vec3 c = casLoad(sp + ivec2(1, -1));
    vec3 d = casLoad(sp + ivec2(-1, 0));
    vec3 e = casLoad(sp);
    vec3 f = casLoad(sp + ivec2(1, 0));
    vec3 g = casLoad(sp + ivec2(-1, 1));
    vec3 h = casLoad(sp + ivec2(0, 1));
    vec3 i = casLoad(sp + ivec2(1, 1));
    // Soft minimum and maximum: the cross plus the whole 3x3 (twice the size).
    float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);
    float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);
    mn += min(min(min(mn, a.g), min(c.g, g.g)), i.g);
    mx += max(max(max(mx, a.g), max(c.g, g.g)), i.g);
    // Distance to the signal's limits over the maximum, shaped.
    float amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    float w = amp * uCasPeak;
    return clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}
)";

// The scaled variant: the sharpened results at the four diorama pixels
// around the eye pixel, blended bilinearly with less weight where the local
// contrast is high (keeps edges thin).
const char* kBlitCasScaled = R"(
uniform float uCasPeak;
uniform highp vec2 uCasScale;
vec3 casLoad(ivec2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0).rgb; }
vec3 sourceColor() {
    highp vec2 pp = gl_FragCoord.xy * uCasScale - 0.5;
    highp vec2 fp = floor(pp);
    pp -= fp;
    ivec2 sp = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = casLoad(sp + ivec2(0, -1));
    vec3 c = casLoad(sp + ivec2(1, -1));
    vec3 e = casLoad(sp + ivec2(-1, 0));
    vec3 f = casLoad(sp);
    vec3 g = casLoad(sp + ivec2(1, 0));
    vec3 h = casLoad(sp + ivec2(2, 0));
    vec3 i = casLoad(sp + ivec2(-1, 1));
    vec3 j = casLoad(sp + ivec2(0, 1));
    vec3 k = casLoad(sp + ivec2(1, 1));
    vec3 l = casLoad(sp + ivec2(2, 1));
    vec3 n = casLoad(sp + ivec2(0, 2));
    vec3 o = casLoad(sp + ivec2(1, 2));
    float mnf = min(min(min(b.g, e.g), min(f.g, g.g)), j.g);
    float mxf = max(max(max(b.g, e.g), max(f.g, g.g)), j.g);
    float mng = min(min(min(c.g, f.g), min(g.g, h.g)), k.g);
    float mxg = max(max(max(c.g, f.g), max(g.g, h.g)), k.g);
    float mnj = min(min(min(f.g, i.g), min(j.g, k.g)), n.g);
    float mxj = max(max(max(f.g, i.g), max(j.g, k.g)), n.g);
    float mnk = min(min(min(g.g, j.g), min(k.g, l.g)), o.g);
    float mxk = max(max(max(g.g, j.g), max(k.g, l.g)), o.g);
    float wf = sqrt(clamp(min(mnf, 1.0 - mxf) / max(mxf, 1e-4), 0.0, 1.0)) * uCasPeak;
    float wg = sqrt(clamp(min(mng, 1.0 - mxg) / max(mxg, 1e-4), 0.0, 1.0)) * uCasPeak;
    float wj = sqrt(clamp(min(mnj, 1.0 - mxj) / max(mxj, 1e-4), 0.0, 1.0)) * uCasPeak;
    float wk = sqrt(clamp(min(mnk, 1.0 - mxk) / max(mxk, 1e-4), 0.0, 1.0)) * uCasPeak;
    float s = (1.0 - pp.x) * (1.0 - pp.y) / (1.0 / 32.0 + mxf - mnf);
    float t = pp.x * (1.0 - pp.y) / (1.0 / 32.0 + mxg - mng);
    float u = (1.0 - pp.x) * pp.y / (1.0 / 32.0 + mxj - mnj);
    float v = pp.x * pp.y / (1.0 / 32.0 + mxk - mnk);
    float qbe = wf * s, qch = wg * t, qin = wj * u, qlo = wk * v;
    float qf = wg * t + wj * u + s;
    float qg = wf * s + wk * v + t;
    float qj = wf * s + wk * v + u;
    float qk = wg * t + wj * u + v;
    vec3 sum = (b + e) * qbe + (c + h) * qch + (i + n) * qin + (l + o) * qlo + f * qf + g * qg + j * qj + k * qk;
    return clamp(sum / (2.0 * (qbe + qch + qin + qlo) + qf + qg + qj + qk), 0.0, 1.0);
}
)";

const char* kQuadVs = R"(#version 320 es
uniform mat4 uMvp;
out vec2 vUv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUv = vec2(p.x, p.y);
    gl_Position = uMvp * vec4(p - 0.5, 0.0, 1.0);
}
)";

// Premultiplied texture: color is scaled by coverage, so decode after
// un-premultiplying.
const char* kQuadFs = R"(#version 320 es
precision mediump float;
uniform sampler2D uTex;
uniform float uOpaque;
uniform float uBrightness;
uniform float uAlpha;
in vec2 vUv;
out vec4 oColor;
%s
void main() {
    vec4 c = texture(uTex, vUv);
    float a = mix(c.a, 1.0, uOpaque);
    vec3 straight = a > 0.0 ? c.rgb / max(a, 1e-4) : vec3(0.0);
    a *= uAlpha;
    oColor = vec4(toLinear(clamp(straight, 0.0, 1.0)) * a * uBrightness, a);
}
)";

// A UI layer's image (sRGB texture: sampling gives linear, premultiplied
// colour) composited the way the runtime does, for the headless simulator.
const char* kLayerFs = R"(#version 320 es
precision mediump float;
uniform sampler2D uTex;
uniform vec2 uUvScale;  // the part of the image used
in vec2 vUv;
out vec4 oColor;
%s
void main() { oColor = texture(uTex, vUv * uUvScale); }
)";

const char* kLaserVs = R"(#version 320 es
uniform mat4 uMvp;
uniform vec3 uCorners[4];
out vec2 vUv;
void main() {
    vUv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = uMvp * vec4(uCorners[gl_VertexID], 1.0);
}
)";

// uShape 0: beam (fades along its length), 1: round reticle, 2: the skip
// indicator (uRing: fill 0..1, spinner head in turns or < 0 for none, flash
// 0..1).
const char* kLaserFs = R"(#version 320 es
precision mediump float;
uniform vec4 uColor;
uniform int uShape;
uniform vec3 uRing;
in vec2 vUv;
out vec4 oColor;
%s
// Right-pointing triangle: back edge at x = -w, apex at x = w, half-height h.
float triangle(vec2 p, float w, float h) {
    vec2 n = vec2(h, 2.0 * w) / length(vec2(h, 2.0 * w));
    return max(-w - p.x, dot(vec2(p.x - w, abs(p.y)), n));
}
void main() {
    if (uShape == 2) {
        // Premultiplied layers: a dark disc, the ring (a dim track, filled
        // clockwise from the top), a fast-forward glyph.
        vec2 q = vUv * 2.0 - 1.0;
        float r = length(q);
        float px = fwidth(r);
        vec4 c = vec4(0.0, 0.0, 0.0, 0.5 * (1.0 - smoothstep(1.0 - px, 1.0, r)));
        float band = smoothstep(0.7 - px, 0.7, r) * (1.0 - smoothstep(0.9 - px, 0.9, r));
        float turn = fract(atan(q.x, q.y) / 6.2831853 + 1.0);
        float lit;
        if (uRing.y < 0.0) {
            lit = uRing.x >= 1.0 ? 1.0 : uRing.x <= 0.0 ? 0.0 : smoothstep(turn - 0.008, turn + 0.008, uRing.x);
        } else {
            lit = max(0.0, 1.0 - fract(uRing.y - turn) / 0.45);  // a head and its fading tail
        }
        vec3 ringColor = mix(mix(vec3(0.85), vec3(1.0, 0.82, 0.25), lit), vec3(1.0), uRing.z);
        float ringA = band * mix(0.3, 1.0, max(lit, uRing.z));
        c = vec4(ringColor * ringA, ringA) + c * (1.0 - ringA);
        float g = min(triangle(q - vec2(-0.16, 0.0), 0.17, 0.32), triangle(q - vec2(0.16, 0.0), 0.17, 0.32));
        float glyphA = 0.95 * (1.0 - smoothstep(-px, px, g));
        c = vec4(vec3(glyphA), glyphA) + c * (1.0 - glyphA);
        c *= uColor.a;
        oColor = vec4(toLinear(c.rgb / max(c.a, 1e-4)) * c.a, c.a);
        return;
    }
    float a;
    if (uShape == 0) {
        a = (1.0 - abs(vUv.x * 2.0 - 1.0)) * mix(0.9, 0.15, vUv.y);
    } else {
        float d = length(vUv * 2.0 - 1.0);
        a = smoothstep(1.0, 0.7, d) * (0.55 + 0.45 * smoothstep(0.5, 0.8, d));
    }
    a *= uColor.a;
    oColor = vec4(toLinear(uColor.rgb) * a, a);
}
)";

// SpaceWarp motion vectors (see renderMotion): this frame's normalized
// device coordinates minus the last frame's, for the point seen at each
// pixel.  Full-screen: the point comes from the scene's depth (or a fixed
// depth); w carries a tag for debug views (0 world, 1 player, 0.5 fixed in
// the room).
const char* kMotionFs = R"(#version 320 es
precision highp float;
uniform highp sampler2D uDepth;
uniform mat4 uCurToPrev;       // this frame's NDC -> the last frame's clip space
uniform float uDepthOverride;  // >= 0: this depth everywhere
uniform float uMaxDepth;
uniform float uTag;
in vec2 vUv;
layout(location = 0) out vec4 oMotion;
void main() {
    float d = uDepthOverride >= 0.0 ? uDepthOverride : texture(uDepth, vUv).r;
    d = min(d, uMaxDepth);
    vec4 cur = vec4(vUv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 prev = uCurToPrev * cur;
    oMotion = vec4(prev.w > 1e-6 ? cur.xyz - prev.xyz / prev.w : vec3(0.0), uTag);
    gl_FragDepth = d;
}
)";

// A panel's motion vectors (with kQuadVs), where its texture is opaque
// enough (uAlphaMin < 0: everywhere); depth from the panel itself.
const char* kMotionQuadFs = R"(#version 320 es
precision highp float;
uniform sampler2D uTex;
uniform float uAlphaMin;
uniform mat4 uCurToPrev;
uniform vec2 uInvSize;  // 1 / motion image size
uniform float uTag;
in vec2 vUv;
layout(location = 0) out vec4 oMotion;
void main() {
    if (uAlphaMin >= 0.0 && texture(uTex, vUv).a < uAlphaMin) discard;
    vec4 cur = vec4(gl_FragCoord.xy * uInvSize * 2.0 - 1.0, gl_FragCoord.z * 2.0 - 1.0, 1.0);
    vec4 prev = uCurToPrev * cur;
    oMotion = vec4(prev.w > 1e-6 ? cur.xyz - prev.xyz / prev.w : vec3(0.0), uTag);
}
)";

GLuint sQuadProgram, sLaserProgram, sLayerProgram, sVao;
GLint sLayerTex, sLayerMvp, sLayerUvScale;
// Composite programs: plain, CAS at 1:1, CAS scaling.
struct BlitProgram {
    GLuint program = 0;
    GLint tex, brightness, vignette, wipe, wipeColor, casPeak, casScale;
};
enum { kBlitPlainProgram, kBlitCas1Program, kBlitCasScaledProgram };
BlitProgram sBlit[3];
GLint sLaserMvp, sLaserCorners, sLaserColor, sLaserShape, sLaserRing;
GLuint sMotionProgram, sMotionQuadProgram, sMotionFbo;
GLint sMotionDepth, sMotionMatrix, sMotionDepthOverride, sMotionMaxDepth, sMotionTag;
GLint sMotionQuadTex, sMotionQuadMvp, sMotionQuadAlphaMin, sMotionQuadMatrix, sMotionQuadInvSize, sMotionQuadTag;

// Pointer ray from the last input update (stage space).
std::atomic<int> sPointerInWorld{0};
bool sAimValid = false;
bool sAimOnPanel = false;  // the ray is on one of the VR layer's own panels (settings, setup)
xm::Vec3 sAimOrigin{0, 0, 0}, sAimDir{0, 0, -1};
// Screen wipe reported by the game (see port_vr_wipe).
std::mutex sWipeLock;
int sWipeKind = 0;
float sWipeClosed = 0.0f;
unsigned int sWipeRgb = 0;
uint64_t sWipeFrame = 0;  // game frame that drew the wipe
std::atomic<int> sDioramaShown{0};
float sWipeDim = 0.0f;  // how far the current eye's wipe has closed (panel, laser)

// The wipe on the frame being shown: it holds while no newer frame arrives
// (the game draws nothing while it loads behind a closed wipe).
void currentWipe(int* kind, float* closed, unsigned int* rgb) {
    uint64_t shown = gpu::renderer().frameNumber();
    std::lock_guard<std::mutex> lock(sWipeLock);
    bool fresh = shown <= sWipeFrame;
    *kind = sWipeKind;
    *closed = fresh ? sWipeClosed : 0.0f;
    *rgb = sWipeRgb;
}

// Pointer touches reported by the game (see port_vr_pointer_touched).
std::atomic<int64_t> sTouchPulseAt{0};  // start of the latest pulse
std::atomic<int64_t> sTouchTaken{0};    // pulse last handed to takePointerTouch
std::atomic<uint64_t> sLastTouchId{0};
std::atomic<int64_t> sLastTouchSeen{0};
float sAimLength = 1.0f;
// How far the pointer reaches into the world (game units), reported by the
// game each frame (see port_vr_pointer_reach).
std::atomic<float> sReachWorld{0.0f};
std::atomic<int64_t> sReachAt{0};
GLint sQuadTex, sQuadMvp, sQuadOpaque, sQuadBrightness, sQuadAlpha;

// Fade through black when the presentation changes (virtual screen <->
// diorama) or the rig jumps, so the world never switches under the player.
const float kFadeHalf = 0.2f;  // seconds out, then the same back in
float sFade = 0.0f;            // 0 = fully visible, 1 = black
float sVignette = 0.0f;        // current comfort vignette strength
bool sFadingOut = false;
bool sShownVrMode = false;     // presentation currently on screen

// Hold-A-to-skip indicator (cutscene_skip.cpp): a ring at the lower right of
// the view that fills while A is held, spins while a cutscene is
// fast-forwarded (the view dimmed to black meanwhile), and pops once the
// skip is done.
float sSkipFill = 0.0f;   // shown fill: drains back when A is let go early
float sSkipAlpha = 0.0f;
float sSkipPop = -1.0f;   // 0..1 while popping after a skip, < 0 otherwise
float sSkipSpin = 0.0f;   // spinner head, turns
float sSkipDim = 0.0f;    // view darkened while fast-forwarding
bool sSkipForwarding = false;
unsigned sSkipsSeen = 0;

// Brightness of what is drawn over the view (panels, laser).
float overlayBrightness() { return (1.0f - sFade) * (1.0f - sWipeDim) * (1.0f - sSkipDim); }

gpu::EfbTarget sEye[2], sHud, sFlat;
bool sVrMode = false;
vr::RigState sRig;
vr::RigParams sRigParams;
// PETARI_CUTTEST: a much wider cutaway, to see it in captures.
bool sCutawayTest = false;
// Comfort settings (see vr::loadSettings).
bool sCutawayOn = true;
float sVignetteStrength = 1.0f;
float sResolution = 1.6f;     // largest eye size, relative to the headset's recommended one
float sRefreshRate = 120.0f;  // requested display refresh rate, Hz
bool sHighClocks = true;      // ask for Meta's SustainedHigh CPU/GPU levels
bool sSpaceWarp = true;       // Application SpaceWarp at 120 Hz
bool sSuperRes = true;        // Meta Quest Super Resolution (eye at render size)
bool sSharpen = false;        // AMD FidelityFX CAS in the composite
float sSharpness = 0.5f;      // its strength, 0..1
float sMinScale = 0.8f;       // lowest render scale (min_resolution)
bool sGiantScreen = true;     // gameplay on the giant virtual screen (giant_screen; the default at first start)
bool sStereoScreen = false;   // the giant screen's picture as a stereo pair (stereo_screen)
float sStereoDepth = 1.75f;   // how far before the farthest things Mario is drawn, in eye distances (stereo_depth)
float sStereoFar = 0.95f;     // the farthest things' distance apart on the screen, in eye distances (stereo_far)
float sStereoSize = 0.8f;     // each picture's size relative to the screen's single one (stereo_resolution)
float sSkipHold = 1.0f;       // seconds A is held to skip a cutscene (skip_hold)
bool sInvertCamera = true;    // the right stick's turns swapped (invert_camera)
// The room around the giant screen instead of the dark (passthrough): the
// eye images are see-through where nothing is drawn, over the headset's
// passthrough layer (xr_app.cpp).
bool sPassthrough = false;       // the setting
bool sPassAvailable = false;     // the headset can show its passthrough
float sPassAmount = 0.0f;        // how far the room is faded in, 0..1
float sSwapchainScale = 0.0f;    // the eye swapchains' size, fixed when they are made
float sRefreshRates[8];          // the display's refresh rates (xr_app.cpp)
int sRefreshRateCount = 0;
bool sFlatFresh = false;      // the virtual screen's picture was drawn since its layer last took it
bool sUiLayers = false;       // the panels with text go out as compositor layers (vr::setUiLayers)
bool sHudFresh = false;       // eye 0 drew the HUD since the HUD layer last took it
int sHudDraws = 0;            // draws in that HUD
float sCutAmount = 0.0f;  // cutaway grown in (1) while the player is hidden
xm::Mat4 sStageFromView = xm::Mat4::identity();
uint64_t sLastFrame = 0;
std::string sSettingsPath;  // petari_vr.ini

// Panels in stage space (the app's space: its origin is the head's position
// when the session started, facing where it faced; see xr_app.cpp).
// The virtual screen (menus, cutscenes): 3.2 m wide 2.6 m away.  The giant
// screen (gameplay from the game's camera): 5.33 m wide (3 m tall),
// screen_distance away; at the default 4.5 m it spans 61 deg, a big TV seen
// from the couch (at 66 deg and 4.6 m the picture filled the view, and
// every swing of the game camera moved most of what the player saw).
float sScreenDistance = 4.5f;
std::string sGamePath;  // game_path
xm::Vec3 screenCenter() { return sGiantScreen ? xm::Vec3{0.0f, 0.0f, -sScreenDistance} : xm::Vec3{0.0f, 0.0f, -2.6f}; }
float screenWidth() { return sGiantScreen ? 5.33f : 3.2f; }
const xm::Vec3 kHudCenter{0.0f, -0.08f, -1.0f};
const float kHudWidth = 1.45f;

// ---------------------------------------------------------------------------
// The giant screen in stereoscopic 3D (stereo_screen).  The game frame is
// replayed twice from the game's own camera, once for each eye, with the 3D
// draws shifted sideways by their depth (gpu::EyeView::flatStereo): as from
// two cameras a little apart whose frustums meet at the "convergence" depth.
// What is that far from the camera looks to be on the screen itself, what is
// farther lies behind it as through a window, and what is nearer comes out
// in front.  Each eye is shown its own picture (two layers, or the eye
// images' own panels).
//
// Depth: how far apart the two pictures draw a point, in distances between
// the player's eyes (taken from the two eye poses), is
//     stereo_far - stereo_depth * (Mario's depth / the point's depth)
// positive: behind the screen, negative: before it.  The farthest things
// (the sky) get stereo_far, 0.95: the lines of sight to them are almost
// parallel, as to things at infinity, and more would make the eyes turn
// outwards.  That is as far behind the screen as anything can look, and it
// is little: under a degree between the screen 4.5 m away and infinity.
// With the whole scene behind the screen (stereo_depth about 0.5) the 3D was
// barely there.  So the scene comes out of the screen instead: stereo_depth
// (1.75, the settings panel's "3D depth") is how far before the farthest
// things Mario is drawn.  At 1.75 he stands two metres before the screen and
// the ground before him nearer still: a model of the world reaching out of
// the screen, several times the depth.  The convergence that gives is stereo_depth / stereo_far times
// Mario's depth (or that of what the camera looks at, if nearer); it follows
// the camera smoothly, and jumps when the camera does.  Nothing is drawn more
// than kStereoNear eye distances apart before the screen (2.5: 1.3 m from the
// player with the screen 4.5 m away), whatever comes right up to the camera.
// The HUD (the draws between the HUD markers: counters, text, menus, the
// pointer's cursor) would look wrong on the screen itself, behind the scene
// it covers.  It floats before the screen as a whole, where the ground
// half as far as Mario is drawn (stereo_far - 2 * stereo_depth, when that is
// before the screen).  Everything else drawn flat stays on the screen.
// The pointer's cursor is part of the HUD, but floating there it stopped
// short of the scene it points into (the eyes on what it pointed at saw it
// double).  While it points into the scene it is drawn as far apart as the
// point under it (port_vr_pointer_depth: the game's own ray through the
// cursor against the map), so it lies on what it points at; on menus it
// stays with the HUD.
// Cost: the GPU's time for a frame goes mostly with the pixels drawn, so the
// two pictures are smaller than the single one (stereo_resolution, 0.8:
// 1638x922 for 2048x1152, still about a texel for each display pixel with
// the screen 4.5 m away).  They go into the lower left part of the layers'
// images as they are, and the compositor does the one scaling to the display.
// ---------------------------------------------------------------------------
const float kStereoNear = 2.5f;  // the nearest things' distance apart before the screen, in eye distances
gpu::EfbTarget sFlatPair[2];    // the left and the right eye's picture
bool sFlatRightFresh = false;   // the right one was drawn since its layer last took it
bool sStereoFrame = false;      // the game frame on show is rendered as a stereo pair
bool sStereoShown = false;      // the picture last finished for the screen is a stereo pair
// The screen's 3D pictures (the giant screen's gameplay, a flight on the
// virtual screen) follow the GPU's load as the diorama's eyes do: they are
// drawn smaller while it cannot keep up (the screen's render scale, see
// updateScale).  A smaller single picture goes into sFlatPair[0] and is
// shown through the layer's image rectangle, as a pair's pictures are; menus
// and movies keep the full size (sFlat).
bool sScaledFrame = false;      // the game frame on show is drawn as one smaller picture
bool sScaledShown = false;      // the picture last finished for the screen is one
int sPictureW = 0, sPictureH = 0;  // the size of the pictures last finished in sFlatPair
float sConvergence = 0.0f;      // game units, following the camera
float sStereoShift = 0.0f;      // the farthest things' shift for one eye, in half screen widths
float sStereoHudShift = 0.0f;   // the HUD's, the other way
float sPointerParallax = -1.0f; // the pointer cursor's shift on the scene, in shifts of the farthest things
bool sPointerOnHud = false;     // the pointer is on menus: its cursor goes with the HUD
// The depth of the point under the game's pointer cursor (see
// port_vr_pointer_depth).
std::atomic<float> sPointerDepth{0.0f};
std::atomic<int64_t> sPointerDepthAt{0};

// Once per frame of a stereo pair: the convergence for the frame's camera,
// and the shifts for the distance between the player's eyes.
void updateStereo(const vr::FrameInfo& frame, const gpu::CameraInfo& cam, float dt) {
    auto depth = [&cam](const float* p) { return -(cam.view[8] * p[0] + cam.view[9] * p[1] + cam.view[10] * p[2] + cam.view[11]); };
    float d = depth(cam.watch);
    if (cam.flags & PORT_GX_CAMERA_PLAYER) {
        float player = depth(cam.player);
        if (player > 50.0f) d = d > 50.0f ? fminf(d, player) : player;
    }
    if (!(d > 50.0f)) d = 1500.0f;  // nothing in front of the camera to go by
    float target = fminf(6000.0f, fmaxf(150.0f, d * sStereoDepth / sStereoFar));
    if (!sStereoShown || sConvergence <= 0.0f || target > 2.0f * sConvergence || target < 0.5f * sConvergence) {
        sConvergence = target;  // the first frame, or a cut
    } else {
        sConvergence += (target - sConvergence) * (1.0f - powf(0.5f, dt / 0.4f));
    }
    float ipd = xm::length(frame.eyes[1].position - frame.eyes[0].position);
    ipd = ipd > 0.045f && ipd < 0.08f ? ipd : 0.063f;
    sStereoShift = sStereoFar * ipd / screenWidth();
    sStereoHudShift = fminf(kStereoNear, fmaxf(0.0f, 2.0f * sStereoDepth - sStereoFar)) * ipd / screenWidth();
    // The cursor follows the depth under it quickly, but not in jumps as it
    // crosses an edge.
    float under = port_host_time_ns() - sPointerDepthAt.load() < 250000000 ? sPointerDepth.load() : 0.0f;
    float parallax = under > 1.0f ? fminf(kStereoNear / sStereoFar, fmaxf(-1.0f, sConvergence / under - 1.0f)) : -1.0f;
    sPointerParallax += (parallax - sPointerParallax) * (1.0f - powf(0.5f, dt / 0.05f));
    sPointerOnHud = (cam.flags & PORT_GX_CAMERA_POINTER_UI) != 0;
}

GLuint compile(GLenum type, const char* fmt, bool withCommon) {
    char src[8192];
    if (withCommon) {
        snprintf(src, sizeof(src), fmt, kCommon);
    } else {
        snprintf(src, sizeof(src), "%s", fmt);
    }
    GLuint s = glCreateShader(type);
    const char* p = src;
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        port_fatal("vr shader compile failed: %s", log);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs, false));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs, true));
    glLinkProgram(p);
    return p;
}

// kBlitFs with `source` as its sourceColor() (the common code goes in when
// compile() formats it).
BlitProgram linkBlit(const char* source) {
    char fs[8192];
    snprintf(fs, sizeof(fs), kBlitFs, "%s", source);
    BlitProgram b;
    b.program = link(kBlitVs, fs);
    b.tex = glGetUniformLocation(b.program, "uTex");
    b.brightness = glGetUniformLocation(b.program, "uBrightness");
    b.vignette = glGetUniformLocation(b.program, "uVignette");
    b.wipe = glGetUniformLocation(b.program, "uWipe");
    b.wipeColor = glGetUniformLocation(b.program, "uWipeColor");
    b.casPeak = glGetUniformLocation(b.program, "uCasPeak");
    b.casScale = glGetUniformLocation(b.program, "uCasScale");
    return b;
}

void ensureTarget(gpu::EfbTarget& t, int w, int h) {
    if (t.width != w || t.height != h) {
        if (t.fbo) gpu::renderer().destroyTarget(t);
        t = gpu::renderer().createTarget(w, h);
    }
}

// A panel of the given width (height from the texture aspect) at `center`,
// facing +Z.
xm::Mat4 panelModel(xm::Vec3 center, float width, float aspect) {
    xm::Mat4 m = xm::translation(center);
    xm::Mat4 s = xm::Mat4::identity();
    s.at(0, 0) = width;
    s.at(1, 1) = width / aspect;
    return m * s;
}

void drawQuad(GLuint texture, const xm::Mat4& mvp, bool opaque, float alpha) {
    glUseProgram(sQuadProgram);
    glUniformMatrix4fv(sQuadMvp, 1, GL_FALSE, mvp.m);
    glUniform1f(sQuadOpaque, opaque ? 1.0f : 0.0f);
    glUniform1f(sQuadBrightness, overlayBrightness());
    glUniform1f(sQuadAlpha, alpha);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, 0);
    glUniform1i(sQuadTex, 0);
    if (opaque) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    glBindVertexArray(sVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void drawPanel(const gpu::EfbTarget& tex, const xm::Mat4& mvp, bool opaque) { drawQuad(tex.color, mvp, opaque, 1.0f); }

// Distance along the ray to the plane of a panel facing +Z (1 m if parallel
// or behind).
float panelDistance(xm::Vec3 o, xm::Vec3 d, xm::Vec3 center) {
    if (fabsf(d.z) < 1e-4f) return 1.0f;
    float t = (center.z - o.z) / d.z;
    return t > 0.0f ? t : 1.0f;
}

// Beam from the right controller to where it points, plus a reticle there.
void drawLaser(const vr::EyeInfo& eye, const xm::Mat4& viewProj) {
    if (!sAimValid || overlayBrightness() <= 0.0f) return;
    // On the giant screen the game's own cursor shows where the controller
    // points, as on a TV: no beam from the controller to it.  The VR layer's
    // own panels have no cursor, so the beam stays for those.
    if (sGiantScreen && !sVrMode && !sAimOnPanel && !vr::setupActive()) return;
    xm::Vec3 p0 = sAimOrigin, p1 = sAimOrigin + sAimDir * sAimLength;
    xm::Vec3 toEye = xm::normalize(eye.position - (p0 + p1) * 0.5f);
    xm::Vec3 across = xm::normalize(xm::cross(sAimDir, toEye));
    // A little wider at the far end, so a beam reaching metres into the
    // world stays visible there.
    xm::Vec3 side = across * 0.0012f, side1 = across * (0.0012f + 0.0008f * sAimLength);
    glUseProgram(sLaserProgram);
    glUniformMatrix4fv(sLaserMvp, 1, GL_FALSE, viewProj.m);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(sVao);
    // Beam: corners ordered for a strip (x across, y along).
    float beam[12] = {p0.x - side.x, p0.y - side.y, p0.z - side.z, p0.x + side.x, p0.y + side.y, p0.z + side.z,
                      p1.x - side1.x, p1.y - side1.y, p1.z - side1.z, p1.x + side1.x, p1.y + side1.y, p1.z + side1.z};
    glUniform3fv(sLaserCorners, 4, beam);
    glUniform4f(sLaserColor, 0.75f, 0.9f, 1.0f, 0.8f * overlayBrightness());
    glUniform1i(sLaserShape, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Reticle facing the eye, sized to stay readable with distance; it swells
    // and brightens for a moment when the pointer touches a new target.
    float pulse = 1.0f - (float)(port_host_time_ns() - sTouchPulseAt.load()) / 250000000.0f;
    pulse = pulse > 0.0f ? pulse : 0.0f;
    float r = (0.008f + 0.01f * sAimLength) * (1.0f + 0.6f * pulse);
    xm::Vec3 n = xm::normalize(eye.position - p1);
    xm::Vec3 u = xm::normalize(xm::cross(fabsf(n.y) < 0.9f ? xm::Vec3{0, 1, 0} : xm::Vec3{1, 0, 0}, n));
    xm::Vec3 v = xm::cross(n, u);
    u = u * r;
    v = v * r;
    float ret[12] = {p1.x - u.x - v.x, p1.y - u.y - v.y, p1.z - u.z - v.z, p1.x + u.x - v.x, p1.y + u.y - v.y, p1.z + u.z - v.z,
                     p1.x - u.x + v.x, p1.y - u.y + v.y, p1.z - u.z + v.z, p1.x + u.x + v.x, p1.y + u.y + v.y, p1.z + u.z + v.z};
    glUniform3fv(sLaserCorners, 4, ret);
    glUniform4f(sLaserColor, 1.0f, 0.95f + 0.05f * pulse, 0.55f + 0.45f * pulse, 0.95f * overlayBrightness());
    glUniform1i(sLaserShape, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void updateSkipIndicator(float dt) {
    float progress;
    int forwarding;
    unsigned skips;
    port_skip_indicator(&progress, &forwarding, &skips);
    bool skipped = skips != sSkipsSeen;
    sSkipsSeen = skips;
    if (forwarding) {
        sSkipFill = 1.0f;
        sSkipAlpha = 1.0f;
        sSkipPop = -1.0f;
        sSkipSpin = fmodf(sSkipSpin + dt * 1.2f, 1.0f);
    } else if (sSkipForwarding || skipped) {
        sSkipPop = 0.0f;  // done: the full ring pops away
        sSkipFill = 0.0f;
        sSkipAlpha = 0.0f;
    } else {
        // Shown from a fifth of the hold on, so a tap (advancing a text box)
        // does not flash it; full when the skip happens.
        float fill = fminf(1.0f, fmaxf(0.0f, (progress - 0.2f) / 0.8f));
        sSkipFill = fill >= sSkipFill ? fill : fmaxf(fill, sSkipFill - dt * 2.5f);
        float alpha = progress > 0.2f ? 1.0f : 0.0f;
        sSkipAlpha = alpha > sSkipAlpha ? fminf(alpha, sSkipAlpha + dt / 0.12f) : fmaxf(alpha, sSkipAlpha - dt / 0.25f);
    }
    sSkipForwarding = forwarding;
    if (sSkipPop >= 0.0f) {
        sSkipPop += dt / 0.35f;
        if (sSkipPop >= 1.0f) sSkipPop = -1.0f;
    }
    sSkipDim = forwarding ? fminf(1.0f, sSkipDim + dt / 0.25f) : fmaxf(0.0f, sSkipDim - dt / 0.4f);
}

// Head-locked: 0.9 m ahead, a little right of and below the centre of view.
void drawSkipIndicator(const vr::FrameInfo& frame, const xm::Mat4& viewProj) {
    bool popping = sSkipPop >= 0.0f;
    float alpha = popping ? 1.0f - sSkipPop : sSkipAlpha;
    if (alpha <= 0.0f) return;
    xm::Quat q = frame.eyes[0].orientation;
    xm::Vec3 head = (frame.eyes[0].position + frame.eyes[1].position) * 0.5f;
    xm::Vec3 c = head + xm::rotate(q, xm::Vec3{0.22f, -0.18f, -0.9f});
    float half = 0.5f * 0.085f * (popping ? 1.0f + 0.4f * sSkipPop : 0.8f + 0.2f * sSkipAlpha);
    xm::Vec3 u = xm::rotate(q, xm::Vec3{half, 0.0f, 0.0f});
    xm::Vec3 v = xm::rotate(q, xm::Vec3{0.0f, half, 0.0f});
    float quad[12] = {c.x - u.x - v.x, c.y - u.y - v.y, c.z - u.z - v.z, c.x + u.x - v.x, c.y + u.y - v.y, c.z + u.z - v.z,
                      c.x - u.x + v.x, c.y - u.y + v.y, c.z - u.z + v.z, c.x + u.x + v.x, c.y + u.y + v.y, c.z + u.z + v.z};
    glUseProgram(sLaserProgram);
    glUniformMatrix4fv(sLaserMvp, 1, GL_FALSE, viewProj.m);
    glUniform3fv(sLaserCorners, 4, quad);
    glUniform4f(sLaserColor, 1.0f, 1.0f, 1.0f, alpha);
    glUniform1i(sLaserShape, 2);
    glUniform3f(sLaserRing, popping ? 1.0f : sSkipFill, sSkipForwarding ? sSkipSpin : -1.0f, popping ? 1.0f - sSkipPop : 0.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(sVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

// Ray (stage space) against a panel; returns the Wii pointer position
// (-1..1, x right, y down) if it hits.
bool rayPanel(xm::Vec3 o, xm::Vec3 d, xm::Vec3 center, float width, float aspect, float* px, float* py) {
    if (fabsf(d.z) < 1e-4f) return false;
    float t = (center.z - o.z) / d.z;
    if (t <= 0.0f) return false;
    xm::Vec3 hit = o + d * t;
    float height = width / aspect;
    *px = (hit.x - center.x) / (width * 0.5f);
    *py = -(hit.y - center.y) / (height * 0.5f);
    return *px > -1.3f && *px < 1.3f && *py > -1.3f && *py < 1.3f;
}


// Headset view for the game's object culling (see port_vr_cull_view) and the
// diorama's axes for Mario's movement (port_vr_move_axes), in world space.
std::mutex sCullLock;
bool sCullValid = false;
float sCullMtx[12], sCullTanX = 1.0f, sCullTanY = 1.0f;
xm::Vec3 sMoveRight{1, 0, 0}, sMoveUp{0, 1, 0}, sMoveForward{0, 0, -1};

void publishCullView(const vr::FrameInfo& frame) {
    // Centre eye in stage space.
    const vr::EyeInfo& e0 = frame.eyes[0];
    const vr::EyeInfo& e1 = frame.eyes[1];
    xm::Vec3 pos = (e0.position + e1.position) * 0.5f;
    xm::Quat q = e0.orientation;
    xm::Vec3 left = vr::stageDirToWorld(sRig, xm::rotate(q, {-1, 0, 0}));
    xm::Vec3 up = vr::stageDirToWorld(sRig, xm::rotate(q, {0, 1, 0}));
    xm::Vec3 fwd = vr::stageDirToWorld(sRig, xm::rotate(q, {0, 0, -1}));
    xm::Vec3 p = vr::stageToWorld(sRig, sRigParams, pos);
    std::lock_guard<std::mutex> lock(sCullLock);
    float m[12] = {left.x, up.x, fwd.x, p.x, left.y, up.y, fwd.y, p.y, left.z, up.z, fwd.z, p.z};
    memcpy(sCullMtx, m, sizeof(m));
    sMoveRight = vr::stageDirToWorld(sRig, {1, 0, 0});
    sMoveUp = vr::stageDirToWorld(sRig, {0, 1, 0});
    sMoveForward = vr::stageDirToWorld(sRig, {0, 0, -1});
    // Both eyes' field of view plus a margin for head motion before the next
    // game frame is shown.
    sCullTanX = frame.cullTanX * 1.35f;
    sCullTanY = frame.cullTanY * 1.35f;
    sCullValid = true;
}

extern "C" uint64_t port_gx_recording_frame(void);

// Aim ray in game world space (see port_vr_pointer_ray).
std::mutex sRayLock;
bool sRayValid = false;
float sRayWorld[6];

}  // namespace

extern "C" int port_vr_pointer_in_world(void) { return sPointerInWorld.load(); }

extern "C" void port_vr_wipe(int kind, float closed, unsigned int rgb) {
    std::lock_guard<std::mutex> lock(sWipeLock);
    closed = closed < 0.0f ? 0.0f : closed > 1.0f ? 1.0f : closed;
    uint64_t frame = port_gx_recording_frame();
    // Two in one frame (a fade over the cinema frame's black): the more
    // closed one shows.
    if (frame == sWipeFrame && closed < sWipeClosed) {
        return;
    }
    sWipeKind = kind;
    sWipeClosed = closed;
    sWipeRgb = rgb;
    sWipeFrame = frame;
}

extern "C" int port_vr_diorama(void) { return sDioramaShown.load(); }

namespace {
std::atomic<const void*> sTransitHost{nullptr};
}  // namespace

extern "C" void port_vr_transit_begin(const void* host) { sTransitHost.store(host); }

extern "C" void port_vr_transit_end(const void* host) {
    const void* expected = host;
    sTransitHost.compare_exchange_strong(expected, nullptr);
}

extern "C" const void* port_vr_transit_host(void) { return sTransitHost.load(); }

extern "C" void port_vr_pointer_touched(unsigned long long id) {
    int64_t now = port_host_time_ns();
    if (id != sLastTouchId.load() || now - sLastTouchSeen.load() > 200000000) {
        sTouchPulseAt.store(now);  // a new target, or back after a gap
    }
    sLastTouchId.store(id);
    sLastTouchSeen.store(now);
}

extern "C" void port_vr_pointer_depth(float depth) {
    sPointerDepth.store(depth);
    sPointerDepthAt.store(port_host_time_ns());
}

extern "C" void port_vr_pointer_reach(float distance) {
    sReachWorld.store(distance);
    sReachAt.store(port_host_time_ns());
}

extern "C" int port_vr_pointer_ray(float* origin, float* dir) {
    std::lock_guard<std::mutex> lock(sRayLock);
    if (!sRayValid || !sPointerInWorld.load()) return 0;
    memcpy(origin, sRayWorld, 3 * sizeof(float));
    memcpy(dir, sRayWorld + 3, 3 * sizeof(float));
    return 1;
}

extern "C" int port_vr_cull_view(float* cameraMtx, float* tanHalfX, float* tanHalfY) {
    std::lock_guard<std::mutex> lock(sCullLock);
    if (!sCullValid) return 0;
    memcpy(cameraMtx, sCullMtx, sizeof(sCullMtx));
    *tanHalfX = sCullTanX;
    *tanHalfY = sCullTanY;
    return 1;
}

extern "C" int port_vr_move_axes(float* right, float* up, float* forward) {
    std::lock_guard<std::mutex> lock(sCullLock);
    if (!sCullValid) return 0;
    const xm::Vec3* v[3] = {&sMoveRight, &sMoveUp, &sMoveForward};
    float* out[3] = {right, up, forward};
    for (int i = 0; i < 3; i++) {
        out[i][0] = v[i]->x;
        out[i][1] = v[i]->y;
        out[i][2] = v[i]->z;
    }
    return 1;
}


namespace {

// Every numeric setting of petari_vr.ini (vr::Setting): the file's loader
// and the settings panel both go through this table.  The defaults are the
// values the variables start with (taken before the file is read).
#define FLAG(v) [] { return v ? 1.0f : 0.0f; }, [](float x) { v = x != 0.0f; }
#define NUMBER(v) [] { return v; }, [](float x) { v = x; }
vr::Setting sSettings[] = {
    {"diorama_scale", 0.05f, 20.0f, 0.0f, [] { return sRigParams.scale * 500.0f; }, [](float x) { sRigParams.scale = x / 500.0f; }},
    {"diorama_height", -3.0f, 3.0f, 0.0f, [] { return -sRigParams.anchor.y; }, [](float x) { sRigParams.anchor.y = -x; }},
    {"diorama_distance", 0.2f, 20.0f, 0.0f, [] { return -sRigParams.anchor.z; }, [](float x) { sRigParams.anchor.z = -x; }},
    {"follow_smoothing", 0.0f, 10.0f, 0.0f, NUMBER(sRigParams.posHalfLife)},
    {"turn_smoothing", 0.0f, 10.0f, 0.0f, NUMBER(sRigParams.rotHalfLife)},
    {"vignette", 0.0f, 1.0f, 0.0f, NUMBER(sVignetteStrength)},
    {"cutaway", 0.0f, 1.0f, 0.0f, FLAG(sCutawayOn)},
    {"turn_with_camera", 0.0f, 1.0f, 0.0f, FLAG(sRigParams.turnWithCamera)},
    {"skip_hold", 0.0f, 10.0f, 0.0f, [] { return sSkipHold; },
     [](float x) {
         sSkipHold = x;
         port_skip_set_hold_seconds(x);
     }},
    {"invert_camera", 0.0f, 1.0f, 0.0f, [] { return sInvertCamera ? 1.0f : 0.0f; },
     [](float x) {
         sInvertCamera = x != 0.0f;
         port_input_set_camera_inverted(sInvertCamera);
     }},
    {"resolution", 0.5f, 2.0f, 0.0f, NUMBER(sResolution)},
    {"min_resolution", 0.5f, 1.25f, 0.0f, NUMBER(sMinScale)},
    {"refresh_rate", 60.0f, 144.0f, 0.0f, NUMBER(sRefreshRate)},
    {"high_clocks", 0.0f, 1.0f, 0.0f, FLAG(sHighClocks)},
    {"space_warp", 0.0f, 1.0f, 0.0f, FLAG(sSpaceWarp)},
    {"super_resolution", 0.0f, 1.0f, 0.0f, FLAG(sSuperRes)},
    {"sharpening", 0.0f, 1.0f, 0.0f, FLAG(sSharpen)},
    {"sharpening_strength", 0.0f, 1.0f, 0.0f, NUMBER(sSharpness)},
    {"giant_screen", 0.0f, 1.0f, 0.0f, FLAG(sGiantScreen)},
    {"screen_distance", 1.0f, 20.0f, 0.0f, NUMBER(sScreenDistance)},
    {"passthrough", 0.0f, 1.0f, 0.0f, FLAG(sPassthrough)},
    {"stereo_screen", 0.0f, 1.0f, 0.0f, FLAG(sStereoScreen)},
    {"stereo_depth", 0.25f, 3.0f, 0.0f, NUMBER(sStereoDepth)},
    {"stereo_far", 0.5f, 1.0f, 0.0f, NUMBER(sStereoFar)},
    {"stereo_resolution", 0.5f, 1.0f, 0.0f, NUMBER(sStereoSize)},
};
#undef FLAG
#undef NUMBER

void takeDefaults() {
    static bool sTaken = false;
    if (sTaken) return;
    sTaken = true;
    for (vr::Setting& setting : sSettings) {
        setting.def = setting.get();
    }
}

}  // namespace

namespace vr {

const Setting* findSetting(const char* key) {
    takeDefaults();
    for (const Setting& setting : sSettings) {
        if (!strcmp(setting.key, key)) return &setting;
    }
    return nullptr;
}

void loadSettings(const char* path) {
    takeDefaults();
    sSettingsPath = path;
    FILE* f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1280];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float value;
        char text[1024];
        if (sscanf(line, " game_path = %1023[^\r\n]", text) == 1) {
            sGamePath = text;
            while (!sGamePath.empty() && sGamePath.back() == ' ') sGamePath.pop_back();
            port_log("%s: game_path = %s", path, sGamePath.c_str());
            continue;
        }
        if (sscanf(line, " language = %31[A-Za-z]", text) == 1) {
            port_language_set(text);
            port_log("%s: language = %s", path, text);
            continue;
        }
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %f", key, &value) != 2) {
            continue;
        }
        const Setting* setting = findSetting(key);
        if (!setting) {
            port_log("%s: unknown setting %s", path, key);
            continue;
        }
        float taken = fminf(setting->max, fmaxf(setting->min, value));
        setting->set(taken);
        if (taken != value) {
            port_log("%s: %s = %g (%g is outside %g to %g)", path, key, taken, value, setting->min, setting->max);
        } else {
            port_log("%s: %s = %g", path, key, value);
        }
    }
    fclose(f);
}

const std::string& gamePath() { return sGamePath; }
void setAimLength(float metres) {
    sAimLength = metres;
    sAimOnPanel = true;
}
void drawOverlayQuad(GLuint texture, const xm::Mat4& mvp, float alpha) { drawQuad(texture, mvp, false, alpha); }
// Fixed once the swapchains are made from it (the resolution setting may
// change in play; the dynamic resolution's range follows it at once).
float swapchainScale() {
    if (sSwapchainScale <= 0.0f) sSwapchainScale = fminf(sResolution, 1.25f);
    return sSwapchainScale;
}
float refreshRate() { return sRefreshRate; }
void setRefreshRates(const float* rates, int count) {
    sRefreshRateCount = count < 8 ? count : 8;
    for (int i = 0; i < sRefreshRateCount; i++) sRefreshRates[i] = rates[i];
}
int refreshRates(float* rates, int max) {
    int n = sRefreshRateCount < max ? sRefreshRateCount : max;
    for (int i = 0; i < n; i++) rates[i] = sRefreshRates[i];
    return n;
}
bool invertCamera() { return sInvertCamera; }
bool passthroughWanted() { return sPassthrough && sGiantScreen; }
float passthroughShown() { return sPassAmount; }
bool passthroughAvailable() { return sPassAvailable; }
void setPassthroughAvailable(bool available) { sPassAvailable = available; }
bool highClocks() { return sHighClocks; }
bool spaceWarp() { return sSpaceWarp; }
bool superResolution() { return sSuperRes; }
bool snapTurn(int dir) {
    if (!sVrMode || !sRig.valid || sRigParams.turnWithCamera || dir == 0) return false;
    sRig.pendingTurn += (dir > 0 ? 1.0f : -1.0f) * 0.78539816f;
    port_log("vr: snap turn %s", dir > 0 ? "right" : "left");
    return true;
}

// ---------------------------------------------------------------------------
// Dynamic resolution.  The game renders each eye into a target of the
// current scale (reallocated when the scale changes, which is rare), and the
// composite scales it to the swapchain, which has a fixed size (the
// resolution setting, at most 1.25: about the display's own resolution in
// the middle of the view).  A target exactly the size rendered matters on
// this tiled GPU: every render pass loads and stores its whole target.
// The scale goes down as soon as the frame loop misses refreshes in the
// diorama (the ground truth: the runtime holds the loop back when a
// refresh's GPU work, the compositor's included, overruns the refresh), or
// when an eye's GPU time, measured with timer queries, comes close to the
// refresh.  It goes back up a step at a time once the eyes have had
// headroom for a while, but not for 20 s to a scale that missed refreshes.
// The timer misses part of a frame's work (the last resolve), so its limits
// are fractions of the refresh.
// ---------------------------------------------------------------------------
namespace {

const float kScaleStep = 0.05f;
float sScale = 1.0f;  // current scale, relative to the recommended eye size
// The same for the screen's 3D picture, relative to its full size
// (2048x1152; each picture of a stereo pair is stereo_resolution times that).
float sScreenScale = 1.0f;
// Which of the two the frame on show is drawn at, and so which one the
// GPU's load moves: the diorama's eyes, the screen's 3D picture, or neither
// (menus, movies).
enum { kNoScale, kEyeScale, kScreenScale };
int sScaled = kNoScale;
float& activeScale() { return sScaled == kScreenScale ? sScreenScale : sScale; }
// Their limits.  The eyes: min_resolution to resolution.  The screen's
// picture: 1 is all there is to show, and its lowest is three quarters of
// min_resolution (0.6 at the default 0.8: 1229 pixels across, where the
// display has about 1500 for the giant screen 4.5 m away).
float scaleMin() { return sScaled == kScreenScale ? fminf(1.0f, fmaxf(0.5f, 0.75f * sMinScale)) : sMinScale; }
float scaleMax() { return sScaled == kScreenScale ? 1.0f : sResolution; }

const GLenum kTimeElapsed = 0x88BF;  // GL_TIME_ELAPSED_EXT
const GLenum kGpuDisjoint = 0x8FBB;  // GL_GPU_DISJOINT_EXT
struct EyeTimer {
    GLuint query = 0;
    bool pending = false;
    bool diorama = false;  // timed an eye of the diorama (the scale applies there)
    float scale = 0.0f;
};
EyeTimer sTimers[8];
int sTimerNext = 0;
int sTimerActive = -1;
bool sTimersOk = false;

// Results at the current scale since it was last changed (or last judged
// too slow): how many, and how many went over the limit.
int sTimedEyes = 0, sSlowEyes = 0;
float sWorstMs = 0.0f, sSecondMs = 0.0f;  // the two slowest
double sChangedAt = 0.0, sLastBusyAt = 0.0, sNow = 0.0;
// Frames that missed refreshes in the diorama (noteMissedRefreshes), counted
// while they come less than a second apart: one hitch (a shader compile) may
// miss two refreshes at once, an overloaded GPU keeps missing them.
int sMissed = 0;
double sLastMissAt = -1e9;
float sCeiling = 9.0f;  // lowest scale that recently missed refreshes, minus a step
double sCeilingUntil = 0.0;
// For the log.
float sLogWorstMs = 0.0f, sLogSumMs = 0.0f;
int sLogCount = 0, sLogChanges = 0, sLogMissed = 0;
float sLogMinScale = 9.0f, sLogMaxScale = 0.0f;
float sLogScreenMin = 9.0f, sLogScreenMax = 0.0f;
double sLogAt = 0.0;
float sBudgetMs = 0.0f;

void initTimers() {
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    sTimersOk = ext && strstr(ext, "GL_EXT_disjoint_timer_query");
    if (sTimersOk) {
        for (EyeTimer& t : sTimers) glGenQueries(1, &t.query);
    }
    port_log("vr: dynamic resolution %.2f-%.2f%s", sMinScale, sResolution, sTimersOk ? "" : " unavailable (no GPU timer queries): fixed 1.0");
}

// Moves to scale `s` (within the limits).  The results so far are judged
// either way, so the minimum scale does not keep deciding to go lower.
void setScale(float s) {
    s = fminf(scaleMax(), fmaxf(scaleMin(), s));
    sTimedEyes = 0;
    sSlowEyes = 0;
    sWorstMs = sSecondMs = 0.0f;
    sMissed = 0;
    sChangedAt = sNow;
    float& scale = activeScale();
    if (fabsf(s - scale) < 0.001f) return;
    scale = s;
    sLogChanges++;
}

// Limits, as fractions of an eye's budget: going over `kSlow` (on two eyes
// since the last judgement: a single hitch such as a texture upload does not
// count) lowers the scale towards `kTarget`; staying under `kBusy` for 1.5 s
// raises it a step.  The timer queries miss part of an eye's work, hence
// the low fractions; the runtime's counters (kSlowM...) measure all of it.
const float kSlow = 0.8f, kTarget = 0.68f, kBusy = 0.58f;
const float kSlowM = 0.92f, kTargetM = 0.82f, kBusyM = 0.72f;

// The runtime's counters (vr::notePerformance): when the last new sample
// came, the last app GPU time (a repeat is no new sample) and an eye's
// budget with the compositor's share of each refresh taken off.
double sMetricsAt = -1.0;
float sLastAppGpuMs = -1.0f, sMetricBudgetMs = 0.0f;
float sLogAppGpuSum = 0.0f, sLogAppGpuMax = 0.0f, sLogCompGpuSum = 0.0f, sLogUtilSum = 0.0f, sLogUtilMax = 0.0f;
int sLogAppGpuCount = 0, sLogCompGpuCount = 0, sLogUtilCount = 0;

// The runtime refreshes its counters about once a second.
bool metricsActive() { return sMetricsAt >= 0.0 && sNow - sMetricsAt < 2.5; }

void takeEyeTime(float ms, const EyeTimer& t) {
    if (!t.diorama || fabsf(t.scale - sScale) > 0.001f) return;  // other presentation, or rendered before the last change
    if (metricsActive()) {
        // Judged by the runtime's counters instead; the timer only logs.
        sLogWorstMs = fmaxf(sLogWorstMs, ms);
        sLogSumMs += ms;
        sLogCount++;
        return;
    }
    sTimedEyes++;
    if (ms > sWorstMs) {
        sSecondMs = sWorstMs;
        sWorstMs = ms;
    } else if (ms > sSecondMs) {
        sSecondMs = ms;
    }
    if (ms > kSlow * sBudgetMs) sSlowEyes++;
    if (ms >= kBusy * sBudgetMs) sLastBusyAt = sNow;
    sLogWorstMs = fmaxf(sLogWorstMs, ms);
    sLogSumMs += ms;
    sLogCount++;
}

void pollTimers() {
    GLint disjoint = 0;
    glGetIntegerv(kGpuDisjoint, &disjoint);  // reading it also clears it
    for (EyeTimer& t : sTimers) {
        if (!t.pending) continue;
        GLuint available = 0;
        glGetQueryObjectuiv(t.query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) continue;
        GLuint ns = 0;
        glGetQueryObjectuiv(t.query, GL_QUERY_RESULT, &ns);
        t.pending = false;
        if (!disjoint) takeEyeTime(ns / 1e6f, t);
    }
}

void beginEyeTimer(bool diorama) {
    if (!sTimersOk) return;
    EyeTimer& t = sTimers[sTimerNext];
    if (t.pending) {
        t.pending = false;  // never read: 8 eyes old, the GPU is far behind or the result was lost
    }
    glBeginQuery(kTimeElapsed, t.query);
    t.diorama = diorama;
    t.scale = sScale;
    sTimerActive = sTimerNext;
    sTimerNext = (sTimerNext + 1) % 8;
}

void endEyeTimer() {
    if (sTimerActive < 0) return;
    glEndQuery(kTimeElapsed);
    sTimers[sTimerActive].pending = true;
    sTimerActive = -1;
}

// Debug: PETARI_FIXED_SCALE="<t>:<scale>;..." holds the render scale from t
// seconds after boot (PETARI_T0_MS) on, to time the eyes at each scale; 0
// when not set or not yet due.
float fixedScale() {
    static const char* spec = gpu::debugEnv("PETARI_FIXED_SCALE");
    static const char* t0 = gpu::debugEnv("PETARI_T0_MS");
    if (!spec) return 0.0f;
    double now = (port_host_time_ns() / 1000000 - (t0 ? atoll(t0) : 0)) / 1000.0;
    float scale = 0.0f;
    for (const char* p = spec; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr) {
        float t, v;
        if (sscanf(p, "%f:%f", &t, &v) == 2 && now >= t) scale = fminf(2.0f, fmaxf(0.5f, v));
    }
    return scale;
}

// Once per game frame, before it is rendered; `scaled` is what the frame is
// drawn at (kEyeScale, kScreenScale or kNoScale).
void updateScale(const vr::FrameInfo& frame, int scaled) {
    sNow = frame.time;
    sBudgetMs = frame.eyeBudgetMs;
    if (scaled != sScaled) {
        // Another presentation: it has its own scale, judged afresh.
        sScaled = scaled;
        sTimedEyes = sSlowEyes = sMissed = 0;
        sWorstMs = sSecondMs = 0.0f;
        sChangedAt = sNow;
        sCeiling = 9.0f;
    }
    float& scale = activeScale();
    scale = fminf(scale, scaleMax());
    float fixed = fminf(fixedScale(), scaleMax());
    if (sScaled == kNoScale) {
        if (sTimersOk) pollTimers();
    } else if (fixed > 0.0f) {
        if (fabsf(fixed - scale) > 0.001f) {
            scale = fixed;
            sChangedAt = sNow;
            sLogChanges++;
            port_log("vr: %s held at %.2f (PETARI_FIXED_SCALE)", sScaled == kScreenScale ? "screen picture scale" : "render scale", scale);
        }
        if (sTimersOk) pollTimers();
    } else if (!metricsActive() && (sScaled == kScreenScale || !sTimersOk || sBudgetMs <= 0.0f)) {
        // Nothing to go by (the screen's picture is judged by the runtime's
        // counters only: the timers time an eye, and one eye draws it all).
        scale = fminf(fmaxf(1.0f, scaleMin()), scaleMax());
    } else {
        if (sTimersOk) pollTimers();
        float target = metricsActive() ? kTargetM * sMetricBudgetMs : kTarget * sBudgetMs;
        if (scale < scaleMin() - 0.001f) {
            setScale(scaleMin());  // the minimum was raised
        } else if (sMissed >= 2) {
            // The loop fell behind at this scale: down two steps, and leave
            // this scale alone for a while.
            sCeiling = scale - kScaleStep;
            sCeilingUntil = sNow + 20.0;
            setScale(scale - 2.0f * kScaleStep);
        } else if (sSlowEyes >= 2) {
            // Down at once, by about the pixel share the eyes are over (the
            // second slowest: the slowest may be a one-off).
            float s = floorf(scale * sqrtf(target / sSecondMs) / kScaleStep + 0.001f) * kScaleStep;
            setScale(fminf(s, scale - kScaleStep));
        } else if (sTimedEyes >= 24 && sNow - fmax(sChangedAt, fmax(sLastBusyAt, sLastMissAt)) > 1.5 &&
                   (scale + kScaleStep <= sCeiling + 0.001f || sNow > sCeilingUntil)) {
            setScale(scale + kScaleStep);  // headroom for a while: up a step
        }
    }
    sLogMinScale = fminf(sLogMinScale, sScale);
    sLogMaxScale = fmaxf(sLogMaxScale, sScale);
    if (sScaled == kScreenScale) {
        sLogScreenMin = fminf(sLogScreenMin, sScreenScale);
        sLogScreenMax = fmaxf(sLogScreenMax, sScreenScale);
    }
    if (sNow - sLogAt >= 10.0) {
        if (sLogAt > 0.0 && sLogCount > 0) {
            port_log("vr: render scale %.2f-%.2f (now %.2f, %d changes); eye GPU %.1f ms avg %.1f max of %.1f; %d refreshes missed", sLogMinScale,
                     sLogMaxScale, sScale, sLogChanges, sLogSumMs / sLogCount, sLogWorstMs, sBudgetMs, sLogMissed);
        }
        if (sLogAt > 0.0 && sLogScreenMax > 0.0f) {
            port_log("vr: screen picture scale %.2f-%.2f (now %.2f, %d changes; lowest %.2f); %d refreshes missed", sLogScreenMin, sLogScreenMax,
                     sScreenScale, sLogChanges, fminf(1.0f, fmaxf(0.5f, 0.75f * sMinScale)), sLogMissed);
        }
        sLogScreenMin = 9.0f;
        sLogScreenMax = 0.0f;
        if (sLogAt > 0.0 && (sLogAppGpuCount > 0 || sLogUtilCount > 0)) {
            port_log("vr: runtime counters: app GPU %.1f ms a frame avg %.1f max (%d samples; %.1f ms an eye allowed), compositor GPU %.1f ms a refresh, "
                     "GPU %.0f%% busy avg %.0f%% max",
                     sLogAppGpuCount ? sLogAppGpuSum / sLogAppGpuCount : -1.0f, sLogAppGpuMax, sLogAppGpuCount, sMetricBudgetMs,
                     sLogCompGpuCount ? sLogCompGpuSum / sLogCompGpuCount : -1.0f, sLogUtilCount ? sLogUtilSum / sLogUtilCount : -1.0f, sLogUtilMax);
        }
        sLogAppGpuSum = sLogAppGpuMax = sLogCompGpuSum = sLogUtilSum = sLogUtilMax = 0.0f;
        sLogAppGpuCount = sLogCompGpuCount = sLogUtilCount = 0;
        sLogAt = sNow;
        sLogWorstMs = sLogSumMs = 0.0f;
        sLogCount = sLogChanges = sLogMissed = 0;
        sLogMinScale = 9.0f;
        sLogMaxScale = 0.0f;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// SpaceWarp (Meta's Application SpaceWarp).  At 120 Hz the game's 60 frames
// a second go to the compositor with motion vectors and depth, and it
// synthesizes the refresh in between from them, instead of showing each
// frame twice: that doubled the image of everything moving across the view,
// in the diorama the whole world while it follows Mario.  A motion vector
// is how far the point seen at a pixel moved in normalized device
// coordinates since the last frame, the head's motion included.  They are
// worked out from the scene's depth, kept at the scene depth marker at the
// motion vector size: each point is taken to stay where it is in the world
// while the rig and the head move, except where the player's draws tagged
// the stencil buffer, which moves with Mario's centre.  The HUD panel and
// the virtual screen stay where they are in the room.  Objects moving on
// their own (enemies, platforms) still move at 60 frames a second.
// ---------------------------------------------------------------------------
namespace {

struct MotionFrame {
    bool valid = false;
    double time = 0.0;     // display time, seconds
    bool vr = false;       // the diorama (otherwise the virtual screen)
    bool snapped = false;  // the rig jumped
    xm::Mat4 eyeView[2];   // eye-from-stage
    xm::Mat4 stageFromWorld = xm::Mat4::identity();  // the rig
    bool player = false;
    xm::Vec3 playerStage{0, 0, 0};  // Mario's centre in the room
};
MotionFrame sMotionPrev, sMotionCur;
bool sMotionOn = false;  // this frame's motion vectors are rendered
int sMotionW = 0, sMotionH = 0;
gpu::EfbTarget sSnap[2];  // the scene's depth and player tags, per eye
bool sSnapTaken[2] = {false, false};
bool sPlayerJumped = false;  // Mario moved too far since the last frame for motion vectors
// Depth written for the far background (the sky, the virtual screen's
// surroundings): just short of the far plane, so the compositor takes the
// motion vectors given there rather than making up its own for empty pixels.
const float kFarDepth = 0.9999f;
// For the log.
int sLogMotionFrames = 0, sLogMotionCuts = 0, sLogNoSnapshot = 0, sLogJumps = 0;
double sLogMotionAt = 0.0;

// Stage points of the world seen this frame -> where they were in the last
// frame's stage (the rig's motion), and the same for points on Mario.
void motionDeltas(xm::Mat4* world, xm::Mat4* player) {
    *world = xm::Mat4::identity();
    *player = xm::Mat4::identity();
    sPlayerJumped = false;
    const MotionFrame& p = sMotionPrev;
    const MotionFrame& c = sMotionCur;
    if (!p.valid || !p.vr || !c.vr) return;
    *world = p.stageFromWorld * xm::inverse(c.stageFromWorld);
    *player = *world;
    if (p.player && c.player) {
        xm::Mat4 turn = *world;
        turn.at(0, 3) = turn.at(1, 3) = turn.at(2, 3) = 0.0f;
        // How far Mario moved in the world, in the room's metres: a warp
        // or a respawn is no motion to extrapolate.
        float moved = xm::length(p.playerStage - xm::transformPoint(*world, c.playerStage));
        if (moved < 0.3f) {
            *player = xm::translation(p.playerStage) * turn * xm::translation(c.playerStage * -1.0f);
        } else {
            sPlayerJumped = true;
        }
    }
}

void motionPass(const xm::Mat4& curToPrev, float depthOverride, float tag) {
    glUniformMatrix4fv(sMotionMatrix, 1, GL_FALSE, curToPrev.m);
    glUniform1f(sMotionDepthOverride, depthOverride);
    glUniform1f(sMotionTag, tag);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// A panel's motion vectors (see kMotionQuadFs).
void motionQuad(GLuint texture, const xm::Mat4& mvp, float alphaMin, const xm::Mat4& curToPrev, float tag) {
    glUseProgram(sMotionQuadProgram);
    glUniformMatrix4fv(sMotionQuadMvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(sMotionQuadMatrix, 1, GL_FALSE, curToPrev.m);
    glUniform1f(sMotionQuadAlphaMin, alphaMin);
    glUniform2f(sMotionQuadInvSize, 1.0f / sMotionW, 1.0f / sMotionH);
    glUniform1f(sMotionQuadTag, tag);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, 0);
    glUniform1i(sMotionQuadTex, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

}  // namespace

void notePerformance(float appGpuMs, float compositorGpuMs, float gpuUtilization, int eyesPerFrame, int refreshesPerFrame, float refreshMs) {
    // Values no frame can have are not samples (the headset's first app GPU
    // time read 1497960 ms).
    float frameMs = refreshMs * (refreshesPerFrame > 0 ? refreshesPerFrame : 1);
    if (appGpuMs > 4.0f * frameMs) appGpuMs = -1.0f;
    if (compositorGpuMs > 2.0f * refreshMs) compositorGpuMs = -1.0f;
    if (gpuUtilization > 100.0f) gpuUtilization = -1.0f;
    if (compositorGpuMs >= 0.0f) {
        sLogCompGpuSum += compositorGpuMs;
        sLogCompGpuCount++;
    }
    if (gpuUtilization >= 0.0f) {
        sLogUtilSum += gpuUtilization;
        sLogUtilMax = fmaxf(sLogUtilMax, gpuUtilization);
        sLogUtilCount++;
        if (gpuUtilization > 95.0f && sScaled != kNoScale) sLastBusyAt = sNow;  // a full GPU (whoever fills it): no step up
    }
    if (appGpuMs <= 0.0f || appGpuMs == sLastAppGpuMs || eyesPerFrame <= 0 || refreshesPerFrame <= 0) return;  // no new sample
    sLastAppGpuMs = appGpuMs;
    sLogAppGpuSum += appGpuMs;
    sLogAppGpuMax = fmaxf(sLogAppGpuMax, appGpuMs);
    sLogAppGpuCount++;
    float compositor = compositorGpuMs > 0.0f ? compositorGpuMs : 0.0f;
    sMetricBudgetMs = (refreshMs - compositor) * refreshesPerFrame / eyesPerFrame;
    sMetricsAt = sNow;
    // Frames drawn at a scale that follows the load only (the diorama, the
    // screen's 3D pictures), at the current scale, and not just after a
    // change (the counter trails a frame or two).
    if (sScaled == kNoScale || sNow - sChangedAt < 0.3) return;
    float ms = appGpuMs / eyesPerFrame;
    sTimedEyes++;
    if (ms > sWorstMs) {
        sSecondMs = sWorstMs;
        sWorstMs = ms;
    } else if (ms > sSecondMs) {
        sSecondMs = ms;
    }
    if (ms > kSlowM * sMetricBudgetMs) sSlowEyes++;
    if (ms >= kBusyM * sMetricBudgetMs) sLastBusyAt = sNow;
}

float renderScale() { return sScale; }
float screenPictureScale() { return sScreenScale; }

void noteMissedRefreshes(int count) {
    // Not while the screen shows menus or a loading screen, nor right after
    // a scale change (reallocating the targets).
    if (count <= 0 || sScaled == kNoScale || sNow - sChangedAt < 0.3) return;
    if (sNow - sLastMissAt > 1.0) sMissed = 0;
    sMissed++;
    sLogMissed += count;
    sLastMissAt = sNow;
}

void init() {
    sCutawayTest = gpu::debugEnv("PETARI_CUTTEST") != nullptr;
    sBlit[kBlitPlainProgram] = linkBlit(kBlitPlain);
    sBlit[kBlitCas1Program] = linkBlit(kBlitCas1);
    sBlit[kBlitCasScaledProgram] = linkBlit(kBlitCasScaled);
    sQuadProgram = link(kQuadVs, kQuadFs);
    sQuadTex = glGetUniformLocation(sQuadProgram, "uTex");
    sQuadMvp = glGetUniformLocation(sQuadProgram, "uMvp");
    sQuadOpaque = glGetUniformLocation(sQuadProgram, "uOpaque");
    sQuadBrightness = glGetUniformLocation(sQuadProgram, "uBrightness");
    sQuadAlpha = glGetUniformLocation(sQuadProgram, "uAlpha");
    sLayerProgram = link(kQuadVs, kLayerFs);
    sLayerTex = glGetUniformLocation(sLayerProgram, "uTex");
    sLayerMvp = glGetUniformLocation(sLayerProgram, "uMvp");
    sLayerUvScale = glGetUniformLocation(sLayerProgram, "uUvScale");
    sLaserProgram = link(kLaserVs, kLaserFs);
    sLaserMvp = glGetUniformLocation(sLaserProgram, "uMvp");
    sLaserCorners = glGetUniformLocation(sLaserProgram, "uCorners");
    sLaserColor = glGetUniformLocation(sLaserProgram, "uColor");
    sLaserShape = glGetUniformLocation(sLaserProgram, "uShape");
    sLaserRing = glGetUniformLocation(sLaserProgram, "uRing");
    sMotionProgram = link(kBlitVs, kMotionFs);
    sMotionDepth = glGetUniformLocation(sMotionProgram, "uDepth");
    sMotionMatrix = glGetUniformLocation(sMotionProgram, "uCurToPrev");
    sMotionDepthOverride = glGetUniformLocation(sMotionProgram, "uDepthOverride");
    sMotionMaxDepth = glGetUniformLocation(sMotionProgram, "uMaxDepth");
    sMotionTag = glGetUniformLocation(sMotionProgram, "uTag");
    sMotionQuadProgram = link(kQuadVs, kMotionQuadFs);
    sMotionQuadTex = glGetUniformLocation(sMotionQuadProgram, "uTex");
    sMotionQuadMvp = glGetUniformLocation(sMotionQuadProgram, "uMvp");
    sMotionQuadAlphaMin = glGetUniformLocation(sMotionQuadProgram, "uAlphaMin");
    sMotionQuadMatrix = glGetUniformLocation(sMotionQuadProgram, "uCurToPrev");
    sMotionQuadInvSize = glGetUniformLocation(sMotionQuadProgram, "uInvSize");
    sMotionQuadTag = glGetUniformLocation(sMotionQuadProgram, "uTag");
    glGenFramebuffers(1, &sMotionFbo);
    glGenVertexArrays(1, &sVao);
    gpu::renderer().init();
    // The game draws a 16:9 picture (its camera's aspect: the console is set
    // to widescreen) squeezed into its 640x456 frame, and the TV stretched it
    // back out.  These targets are 16:9, so the panels showing them keep the
    // picture's proportions (at 640:456 everything was 27% too narrow).
    ensureTarget(sHud, 1600, 900);
    ensureTarget(sFlat, 2048, 1152);
    initTimers();
    takeDefaults();
    port_input_set_camera_inverted(sInvertCamera);
    settingsInit(sSettingsPath.c_str());
}

// Render-thread time spent on the game frame being presented, summed over
// beginFrame and both eyes (which may be rendered on separate display
// refreshes), for port_perf_render.
static int64_t sRenderNs = 0, sUploadNs = 0;

void beginFrame(const FrameInfo& frame) {
    int64_t frameStartNs = port_host_time_ns();
    static double lastTime = 0;
    double now = frame.time;
    float dt = lastTime > 0 ? (float)(now - lastTime) : 0.0f;
    lastTime = now;
    if (dt > 0.1f) dt = 0.1f;

    gpu::Renderer& r = gpu::renderer();
    // Frames are prepared on the renderer's worker thread; this picks up the
    // newest finished one.
    if (r.update()) {
        sLastFrame = r.frameNumber();
    }
    sUploadNs = port_host_time_ns() - frameStartNs;
    bool wantVr = r.hasFrame() && r.camera().valid && (r.camera().flags & PORT_GX_CAMERA_DIORAMA) && !sGiantScreen;
    // Presentation changes happen at the darkest point of a fade.
    if (wantVr != sShownVrMode && !sFadingOut && sFade <= 0.0f) {
        sFadingOut = true;
    }
    if (sFadingOut) {
        sFade += dt / kFadeHalf;
        if (sFade >= 1.0f) {
            sFade = 1.0f;
            sFadingOut = false;
            sShownVrMode = wantVr;
            sRig.valid = false;  // start the diorama framed on the player
        }
    } else if (sFade > 0.0f) {
        sFade -= dt / kFadeHalf;
        if (sFade < 0.0f) sFade = 0.0f;
    }
    updateSkipIndicator(dt);
    sVrMode = sShownVrMode && r.hasFrame() && r.camera().valid;
    sDioramaShown.store(sVrMode ? 1 : 0);
    // The room around the giant screen (passthrough) fades in and out with
    // the setting; the diorama fills the view itself.
    float room = sPassthrough && sPassAvailable && sGiantScreen && !sShownVrMode ? 1.0f : 0.0f;
    sPassAmount = room > sPassAmount ? fminf(room, sPassAmount + dt / 0.3f) : fmaxf(room, sPassAmount - dt / 0.3f);
    if (sVrMode) {
        bool wasValid = sRig.valid;
        xm::Vec3 prevPivot = sRig.pivot;
        sStageFromView = updateRig(sRig, sRigParams, r.camera(), dt);
        static const bool sRigLog = getenv("PETARI_RIGLOG") != nullptr;
        if (sRigLog && wasValid && (sRig.snapped || sRig.turnRate > 1.0f)) {
            const float* w = r.camera().watch;
            port_log("vr: rig %s: watch moved %.0f from the pivot, turn %.2f rad/s, up %.2f %.2f %.2f", sRig.snapped ? "snapped" : "turning",
                     xm::length(xm::Vec3{w[0], w[1], w[2]} - prevPivot), sRig.turnRate, sRig.up.x, sRig.up.y, sRig.up.z);
        }
        // Comfort vignette while the world turns (0.35 rad/s .. 1.5 rad/s),
        // easing in and out.
        float target = fminf(1.0f, fmaxf(0.0f, (sRig.turnRate - 0.35f) / 1.15f)) * 0.85f * sVignetteStrength;
        sVignette += (target - sVignette) * fminf(1.0f, dt * (target > sVignette ? 12.0f : 4.0f));
        // Grow the cutaway in over ~0.15 s while level geometry hides the
        // player, and out over ~0.3 s once it no longer does.
        bool hidden = sCutawayOn && (sCutawayTest || (r.camera().flags & PORT_GX_CAMERA_OCCLUDED));
        sCutAmount = hidden ? fminf(1.0f, sCutAmount + dt / 0.15f) : fmaxf(0.0f, sCutAmount - dt / 0.3f);
        if (sRig.snapped && sFade <= 0.0f) {
            sFade = 1.0f;  // cut to black and fade back in
        }
        publishCullView(frame);
    } else {
        std::lock_guard<std::mutex> lock(sCullLock);
        sCullValid = false;
    }
    // The render scale of what the frame is: the diorama's eyes, or a 3D
    // picture on the screen.
    bool screen3d = !sVrMode && r.hasFrame() && r.camera().valid;
    updateScale(frame, sVrMode ? kEyeScale : screen3d ? kScreenScale : kNoScale);
    // The giant screen's gameplay (any frame of it with a 3D camera) as a
    // stereo pair; the screen's 3D pictures at the screen's render scale.
    sStereoFrame = sStereoScreen && sGiantScreen && screen3d;
    float picture = (sStereoFrame ? sStereoSize : 1.0f) * (screen3d ? sScreenScale : 1.0f);
    int pictureW = (int)lroundf(sFlat.width * picture), pictureH = (int)lroundf(sFlat.height * picture);
    sScaledFrame = screen3d && !sStereoFrame && pictureW < sFlat.width;
    if (sStereoFrame) {
        for (gpu::EfbTarget& t : sFlatPair) {
            ensureTarget(t, pictureW, pictureH);
        }
        updateStereo(frame, r.camera(), dt);
    } else if (sScaledFrame) {
        ensureTarget(sFlatPair[0], pictureW, pictureH);
    }
    // The eye targets at the current render scale (the swapchain images are
    // at swapchainScale()).
    for (int e = 0; e < 2; e++) {
        float f = sScale / swapchainScale();
        ensureTarget(sEye[e], (int)lroundf(frame.eyes[e].width * f), (int)lroundf(frame.eyes[e].height * f));
    }
    // SpaceWarp: this frame's view of the world, for its motion vectors.
    sMotionOn = frame.motion && sMotionW > 0;
    if (sMotionOn) {
        // After a gap (frames without motion vectors, a pause) the last
        // frame with them is no base for motion.
        if (sMotionPrev.valid && frame.time - sMotionPrev.time > 0.05) {
            sMotionPrev.valid = false;
        }
        MotionFrame m;
        m.valid = true;
        m.time = frame.time;
        m.vr = sVrMode;
        for (int e = 0; e < 2; e++) {
            m.eyeView[e] = frame.eyes[e].view;
        }
        if (sVrMode) {
            const gpu::CameraInfo& cam = r.camera();
            m.snapped = sRig.snapped;
            m.stageFromWorld = sStageFromView * xm::fromRows3x4(cam.view);
            if (cam.flags & PORT_GX_CAMERA_PLAYER) {
                m.player = true;
                m.playerStage = xm::transformPoint(m.stageFromWorld, {cam.player[0], cam.player[1], cam.player[2]});
            }
        }
        sMotionCur = m;
        for (int e = 0; e < 2; e++) {
            if (sSnap[e].width != sMotionW || sSnap[e].height != sMotionH) {
                if (sSnap[e].fbo) r.destroyTarget(sSnap[e]);
                sSnap[e] = r.createDepthTarget(sMotionW, sMotionH);
            }
            sSnapTaken[e] = false;
        }
    }
    sRenderNs = port_host_time_ns() - frameStartNs;
}

Extent renderEye(int eye, const FrameInfo& frame, GLuint fbo, int width, int height) {
    int64_t eyeStartNs = port_host_time_ns();
    sWipeDim = 0.0f;
    gpu::Renderer& r = gpu::renderer();
    beginEyeTimer(r.hasFrame() && sVrMode);
    const EyeInfo& ei = frame.eyes[eye];
    xm::Mat4 viewProj = ei.proj * ei.view;
    Extent used{width, height};

    if (r.hasFrame() && sVrMode) {
        // With Super Resolution the diorama goes out at its render size (the
        // lower left part of the image) and the compositor does the one
        // scaling to the display; bilinear here first only blurred it.  Not
        // while the settings panel is up: it and the pause menu are drawn
        // into the same image, and their text lost a third of its pixels.
        if (sSuperRes && (sUiLayers || !settingsShown()) && sEye[eye].width <= width && sEye[eye].height <= height) {
            used = {sEye[eye].width, sEye[eye].height};
        }
        gpu::EyeView ev;
        ev.index = eye;
        xm::Mat4 eyeFromView = ei.view * sStageFromView;
        toRowMajor(eyeFromView, ev.view);
        toRowMajor(ei.proj, ev.proj);
        setCutaway(ev, eyeFromView, r.camera(), sCutAmount, sCutawayTest ? 3.0f : 1.0f);
        // The eye in game view space (game units): lights on the game's
        // camera are lit from it.
        xm::Mat4 viewFromEye = xm::inverse(eyeFromView);
        ev.eyePos[0] = viewFromEye.at(0, 3);
        ev.eyePos[1] = viewFromEye.at(1, 3);
        ev.eyePos[2] = viewFromEye.at(2, 3);
        ev.depthSnapshot = sMotionOn ? &sSnap[eye] : nullptr;
        r.render(sEye[eye], &ev, &sHud, eye == 0 ? gpu::HudMode::Target : gpu::HudMode::Skip);
        if (eye == 0) {
            sHudFresh = true;
            sHudDraws = r.hudDrawCount();
        }
        sSnapTaken[eye] = sMotionOn && r.snapshotTaken();

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        // The composite covers the whole image: no need to load its old
        // contents into the tiles.
        const GLenum color[1] = {GL_COLOR_ATTACHMENT0};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, color);
        glViewport(0, 0, used.width, used.height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // FidelityFX CAS sharpens the diorama as it goes in (and scales it
        // when the sizes differ).
        bool sameSize = used.width == sEye[eye].width && used.height == sEye[eye].height;
        const BlitProgram& blit = sBlit[!sSharpen ? kBlitPlainProgram : sameSize ? kBlitCas1Program : kBlitCasScaledProgram];
        glUseProgram(blit.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sEye[eye].color);
        glBindSampler(0, 0);
        glUniform1i(blit.tex, 0);
        glUniform1f(blit.brightness, (1.0f - sFade) * (1.0f - sSkipDim));
        glUniform1f(blit.vignette, sVignette);
        if (sSharpen) {
            glUniform1f(blit.casPeak, -1.0f / (8.0f + (5.0f - 8.0f) * sSharpness));
            glUniform2f(blit.casScale, (float)sEye[eye].width / used.width, (float)sEye[eye].height / used.height);
        }
        // The game's screen wipe, across the whole view.
        int wipeKind;
        float wipeClosed;
        unsigned int wipeRgb;
        currentWipe(&wipeKind, &wipeClosed, &wipeRgb);
        sWipeDim = wipeClosed;
        float cx = 0.5f, cy = 0.5f;
        if (wipeKind == 1 && (r.camera().flags & PORT_GX_CAMERA_PLAYER)) {
            const float* v = r.camera().view;
            const float* p = r.camera().player;
            xm::Vec3 inView = {v[0] * p[0] + v[1] * p[1] + v[2] * p[2] + v[3], v[4] * p[0] + v[5] * p[1] + v[6] * p[2] + v[7],
                               v[8] * p[0] + v[9] * p[1] + v[10] * p[2] + v[11]};
            xm::Vec3 e = xm::transformPoint(eyeFromView, inView);
            const xm::Mat4& pr = ei.proj;
            float clipX = pr.m[0] * e.x + pr.m[4] * e.y + pr.m[8] * e.z + pr.m[12];
            float clipY = pr.m[1] * e.x + pr.m[5] * e.y + pr.m[9] * e.z + pr.m[13];
            float clipW = pr.m[3] * e.x + pr.m[7] * e.y + pr.m[11] * e.z + pr.m[15];
            if (clipW > 1e-4f) {
                cx = clipX / clipW * 0.5f + 0.5f;
                cy = clipY / clipW * 0.5f + 0.5f;
            }
        }
        auto lin = [](unsigned int c) { float f = c / 255.0f; return f * f; };  // near enough for black / white
        glUniform4f(blit.wipe, wipeClosed, wipeKind == 0 ? 0.0f : 1.0f, cx, cy);
        glUniform4f(blit.wipeColor, lin((wipeRgb >> 16) & 255), lin((wipeRgb >> 8) & 255), lin(wipeRgb & 255), (float)used.width / (float)used.height);
        glBindVertexArray(sVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!sUiLayers) {
            drawPanel(sHud, viewProj * panelModel(kHudCenter, kHudWidth, (float)sHud.width / sHud.height), false);
        }
    } else {
        if (r.hasFrame() && sStereoFrame) {
            // The eye's picture of the stereo pair.  The pair goes to the
            // screen's layers once both are drawn (the two eyes may be
            // rendered a display refresh apart), so they never show
            // different game frames.
            gpu::EyeView ev;
            ev.index = eye;
            ev.flatStereo = true;
            ev.stereo[0] = eye == 0 ? sStereoShift : -sStereoShift;
            ev.stereo[1] = sConvergence;
            ev.stereo[2] = kStereoNear / sStereoFar;
            ev.stereo[3] = eye == 0 ? sStereoHudShift : -sStereoHudShift;
            ev.pointer[0] = ev.stereo[0] * sPointerParallax;
            ev.pointer[1] = sPointerOnHud ? 1.0f : 0.0f;
            r.render(sFlatPair[eye], &ev, nullptr, gpu::HudMode::Inline);
            if (eye == 1) {
                sFlatFresh = sFlatRightFresh = true;
                sStereoShown = true;
                sScaledShown = false;
                sPictureW = sFlatPair[0].width;
                sPictureH = sFlatPair[0].height;
            }
        } else if (eye == 0 && r.hasFrame()) {
            r.render(sScaledFrame ? sFlatPair[0] : sFlat, nullptr, nullptr, gpu::HudMode::Inline);
            sFlatFresh = true;
            sStereoShown = false;
            sScaledShown = sScaledFrame;
            sPictureW = sFlatPair[0].width;
            sPictureH = sFlatPair[0].height;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // The dark around the screen; with the passthrough setting the image
        // is see-through there instead (premultiplied alpha), and the
        // compositor shows the room behind it.
        float dark = 1.0f - sPassAmount;
        glClearColor(0.004f * dark, 0.004f * dark, 0.012f * dark, dark);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (r.hasFrame() && !sUiLayers) {
            drawPanel(sStereoFrame ? sFlatPair[eye] : sScaledShown ? sFlatPair[0] : sFlat,
                      viewProj * panelModel(screenCenter(), screenWidth(), (float)sFlat.width / sFlat.height),
                      true);
        }
    }
    if (!sUiLayers) {
        settingsDraw(viewProj);
        setupDraw(viewProj);
    }
    drawLaser(ei, viewProj);
    drawSkipIndicator(frame, viewProj);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    endEyeTimer();
    sRenderNs += port_host_time_ns() - eyeStartNs;
    if (eye == 1) {
        port_perf_render(sRenderNs, sUploadNs);
    }
    return used;
}

void setMotionSize(int width, int height) {
    sMotionW = width;
    sMotionH = height;
}

void renderMotion(int eye, const FrameInfo& frame, GLuint motionTex, GLuint depthTex) {
    gpu::Renderer& r = gpu::renderer();
    const EyeInfo& ei = frame.eyes[eye];
    glBindFramebuffer(GL_FRAMEBUFFER, sMotionFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, motionTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
    glViewport(0, 0, sMotionW, sMotionH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glBindVertexArray(sVao);

    // This frame's normalized device coordinates -> the room, and the room
    // -> the last frame's clip space (the head moved in between).
    xm::Mat4 viewProj = ei.proj * ei.view;
    xm::Mat4 ndcToStage = xm::inverse(viewProj);
    const MotionFrame& prev = sMotionOn && sMotionPrev.valid ? sMotionPrev : sMotionCur;
    xm::Mat4 prevViewProj = ei.proj * (sMotionOn ? prev.eyeView[eye] : ei.view);
    xm::Mat4 still = prevViewProj * ndcToStage;  // points fixed in the room
    xm::Mat4 worldDelta, playerDelta;
    motionDeltas(&worldDelta, &playerDelta);

    glUseProgram(sMotionProgram);
    glUniform1f(sMotionMaxDepth, kFarDepth);
    glUniform1i(sMotionDepth, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    if (sMotionOn && sMotionCur.vr && sSnapTaken[eye]) {
        // The scene's depth and player tags (the same size: an exact copy).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sSnap[eye].fbo);
        glBlitFramebuffer(0, 0, sMotionW, sMotionH, 0, 0, sMotionW, sMotionH, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, sMotionFbo);
        glBindTexture(GL_TEXTURE_2D, sSnap[eye].depth);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        motionPass(prevViewProj * worldDelta * ndcToStage, -1.0f, 0.0f);
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        motionPass(prevViewProj * playerDelta * ndcToStage, -1.0f, 1.0f);
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        // The HUD panel, where it covers the view, stays in the room (unless
        // it is a compositor layer of its own).
        if (!sUiLayers) {
            motionQuad(sHud.color, viewProj * panelModel(kHudCenter, kHudWidth, (float)sHud.width / sHud.height), 0.3f, still, 0.5f);
        }
    } else {
        // No scene depth: the virtual screen in the room, or (a frame the
        // game drew no scene into) the far background.
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepthf(1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, 0);
        bool vr = sMotionOn && sMotionCur.vr;
        motionPass(prevViewProj * (vr ? worldDelta : xm::Mat4::identity()) * ndcToStage, kFarDepth, vr ? 0.0f : 0.5f);
        if (!vr && r.hasFrame() && !sUiLayers) {
            motionQuad(sFlat.color, viewProj * panelModel(screenCenter(), screenWidth(), (float)sFlat.width / sFlat.height), -1.0f, still, 0.5f);
        }
    }
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool finishMotion(xm::Quat* deltaOrientation, xm::Vec3* deltaPosition) {
    *deltaOrientation = {0.0f, 0.0f, 0.0f, 1.0f};
    *deltaPosition = {0.0f, 0.0f, 0.0f};
    if (!sMotionOn) {
        return true;
    }
    bool noSnapshot = sMotionCur.vr && !(sSnapTaken[0] && sSnapTaken[1]);
    bool cut = !sMotionPrev.valid || sMotionPrev.vr != sMotionCur.vr || sMotionCur.snapped || noSnapshot || sPlayerJumped;
    if (sMotionPrev.valid && sMotionPrev.vr && sMotionCur.vr) {
        // The room's pose in the world relative to the last frame's: the
        // last frame's stage from this one's (a rigid motion: the diorama's
        // scale cancels out).
        xm::Mat4 d = sMotionPrev.stageFromWorld * xm::inverse(sMotionCur.stageFromWorld);
        *deltaOrientation = xm::quatFromMatrix(d);
        *deltaPosition = {d.at(0, 3), d.at(1, 3), d.at(2, 3)};
    }
    sMotionPrev = sMotionCur;
    sLogMotionFrames++;
    sLogMotionCuts += cut;
    sLogNoSnapshot += noSnapshot;
    sLogJumps += sPlayerJumped;
    if (sNow - sLogMotionAt >= 10.0) {
        if (sLogMotionAt > 0.0) {
            port_log("vr: SpaceWarp: %d frames with motion vectors in the last 10 s, %d not to extrapolate (%d without the scene's depth, %d Mario jumps)",
                     sLogMotionFrames, sLogMotionCuts, sLogNoSnapshot, sLogJumps);
        }
        sLogMotionAt = sNow;
        sLogMotionFrames = sLogMotionCuts = sLogNoSnapshot = sLogJumps = 0;
    }
    return cut;
}

void motionDebugImage(GLuint motionTex, std::vector<unsigned char>* rgba, std::string* summary) {
    int w = sMotionW, h = sMotionH;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, motionTex, 0);
    std::vector<float> mv((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, mv.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    rgba->assign((size_t)w * h * 4, 0);
    double sum[3] = {0, 0, 0};
    int count[3] = {0, 0, 0};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float* m = &mv[((size_t)y * w + x) * 4];
            unsigned char* o = &(*rgba)[((size_t)y * w + x) * 4];
            for (int c = 0; c < 2; c++) {
                float v = 0.5f + m[c] * 20.0f;
                o[c] = (unsigned char)(v < 0.0f ? 0 : v > 1.0f ? 255 : v * 255.0f + 0.5f);
            }
            o[2] = (unsigned char)(fminf(1.0f, fmaxf(0.0f, m[3])) * 255.0f + 0.5f);
            o[3] = 255;
            int kind = m[3] > 0.75f ? 2 : m[3] > 0.25f ? 1 : 0;
            sum[kind] += sqrt((double)m[0] * m[0] + (double)m[1] * m[1]);
            count[kind]++;
        }
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "world %d px avg %.5f, room %d px avg %.5f, player %d px avg %.5f (NDC per frame)", count[0],
             count[0] ? sum[0] / count[0] : 0.0, count[1], count[1] ? sum[1] / count[1] : 0.0, count[2], count[2] ? sum[2] / count[2] : 0.0);
    *summary = buf;
}

bool pointerFromRayImpl(xm::Vec3 origin, xm::Vec3 dir, float* x, float* y);

bool takePointerTouch() {
    int64_t at = sTouchPulseAt.load();
    if (at == 0 || at == sTouchTaken.load()) {
        return false;
    }
    sTouchTaken.store(at);
    return true;
}

void pointerLost() {
    sAimValid = false;
    std::lock_guard<std::mutex> lock(sRayLock);
    sRayValid = false;
}

bool pointerFromRay(xm::Vec3 origin, xm::Vec3 dir, float* x, float* y) {
    bool hit = pointerFromRayImpl(origin, dir, x, y);
    sAimValid = true;
    sAimOnPanel = false;  // until setAimLength says so
    sAimOrigin = origin;
    sAimDir = xm::normalize(dir);
    {
        std::lock_guard<std::mutex> lock(sRayLock);
        sRayValid = sVrMode && sRig.valid;
        if (sRayValid) {
            xm::Vec3 o = stageToWorld(sRig, sRigParams, origin);
            xm::Vec3 d = xm::normalize(stageDirToWorld(sRig, sAimDir));
            float w[6] = {o.x, o.y, o.z, d.x, d.y, d.z};
            memcpy(sRayWorld, w, sizeof(w));
        }
    }
    (void)hit;
    return hit;
}

bool pointerFromRayImpl(xm::Vec3 origin, xm::Vec3 dir, float* x, float* y) {
    const gpu::CameraInfo& cam = gpu::renderer().camera();
    sPointerInWorld = sVrMode && !(cam.flags & PORT_GX_CAMERA_POINTER_UI);
    if (sVrMode && !(cam.flags & PORT_GX_CAMERA_POINTER_UI)) {
        // Aiming into the diorama: take the point of the ray nearest the
        // watched point (Mario) and project it with the game camera, so the
        // game's pointer ray passes through what the player aims at.
        xm::Vec3 d = xm::normalize(dir);
        float t = xm::dot(sRigParams.anchor - origin, d);
        if (t < 0.05f) t = 0.05f;
        // The laser runs on to where the game found the first surface in its
        // way, or to where star bits aim in open space.
        bool reachKnown = port_host_time_ns() - sReachAt.load() < 250000000;
        sAimLength = reachKnown ? fmaxf(0.05f, sReachWorld.load() * sRigParams.scale) : t;
        xm::Vec3 w = stageToWorld(sRig, sRigParams, origin + d * t);
        const float* v = cam.view;
        float vx = v[0] * w.x + v[1] * w.y + v[2] * w.z + v[3];
        float vy = v[4] * w.x + v[5] * w.y + v[6] * w.z + v[7];
        float vz = v[8] * w.x + v[9] * w.y + v[10] * w.z + v[11];
        if (vz > -1.0f) return false;  // behind the game camera
        float ty = tanf(cam.fovy * 0.5f * 3.14159265f / 180.0f);
        float aspect = cam.aspect > 0.1f ? cam.aspect : 16.0f / 9.0f;
        // Kept on screen: the game only tests its pointer targets while the
        // cursor is on screen, and those tests use the 3D aim ray here (see
        // port_vr_pointer_ray), so aiming far to the side still works.
        *x = fminf(0.98f, fmaxf(-0.98f, (vx / -vz) / (ty * aspect)));
        *y = fminf(0.98f, fmaxf(-0.98f, -(vy / -vz) / ty));
        return true;
    }
    if (sVrMode) {
        sAimLength = panelDistance(origin, dir, kHudCenter);
        return rayPanel(origin, dir, kHudCenter, kHudWidth, (float)sHud.width / sHud.height, x, y);
    }
    sAimLength = panelDistance(origin, dir, screenCenter());
    return rayPanel(origin, dir, screenCenter(), screenWidth(), (float)sFlat.width / sFlat.height, x, y);
}

// ---------------------------------------------------------------------------
// UI layers (see vr_renderer.h)
// ---------------------------------------------------------------------------
void setUiLayers(bool on) {
    sUiLayers = on;
    port_log("vr: the HUD, settings panel and virtual screen %s", on ? "go out as compositor layers" : "are drawn into the eye images");
}

bool uiLayers() { return sUiLayers; }

void uiLayerSize(int which, int* width, int* height) {
    if (which == kHudLayer) {
        *width = sHud.width;
        *height = sHud.height;
    } else if (which == kScreenLayer || which == kScreenRightLayer) {
        *width = sFlat.width;
        *height = sFlat.height;
    } else if (which == kSetupLayer) {
        setupLayerSize(width, height);
    } else {
        settingsLayerSize(width, height);
    }
}

UiLayer uiLayer(int which) {
    UiLayer l;
    if (which == kHudLayer) {
        // Only while it holds something: a layer costs the compositor its
        // area on every refresh, drawn or not.
        l.visible = sUiLayers && sVrMode && sHudDraws > 0 && overlayBrightness() > 0.001f;
        l.changed = sHudFresh;
        l.position = kHudCenter;
        l.width = kHudWidth;
        l.height = kHudWidth * (float)sHud.height / (float)sHud.width;
        return l;
    }
    if (which == kScreenLayer || which == kScreenRightLayer) {
        // The virtual screen (menus, cutscenes, the giant screen's gameplay):
        // sampled once by the compositor, and steady at the display's rate.
        // A stereo pair goes out as a layer for each eye.
        bool right = which == kScreenRightLayer;
        l.visible = sUiLayers && !sVrMode && gpu::renderer().hasFrame() && (!right || sStereoShown);
        l.changed = right ? sFlatRightFresh : sFlatFresh;
        l.eye = !sStereoShown ? kBothEyes : right ? kRightEye : kLeftEye;
        if (sStereoShown || sScaledShown) {
            l.imageWidth = sPictureW;
            l.imageHeight = sPictureH;
        }
        l.position = screenCenter();
        l.width = screenWidth();
        l.height = screenWidth() * (float)sFlat.height / (float)sFlat.width;
        return l;
    }
    l = which == kSetupLayer ? setupLayer() : settingsLayer();
    l.visible = l.visible && sUiLayers;
    return l;
}

void drawUiLayer(int which, GLuint fbo) {
    int w, h;
    uiLayerSize(which, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // The picture fills the part of the image the layer shows.
    UiLayer part = uiLayer(which);
    glViewport(0, 0, part.imageWidth ? part.imageWidth : w, part.imageHeight ? part.imageHeight : h);
    if (which == kHudLayer) {
        drawQuad(sHud.color, xm::scale(2.0f), false, 1.0f);  // the unit quad over the whole image
        sHudFresh = false;
    } else if (which == kScreenLayer) {
        drawQuad(sStereoShown || sScaledShown ? sFlatPair[0].color : sFlat.color, xm::scale(2.0f), true, 1.0f);
        sFlatFresh = false;
    } else if (which == kScreenRightLayer) {
        drawQuad(sFlatPair[1].color, xm::scale(2.0f), true, 1.0f);
        sFlatRightFresh = false;
    } else if (which == kSetupLayer) {
        setupDrawLayer();
    } else {
        settingsDrawLayer();
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void compositeUiLayer(int which, GLuint texture, const xm::Mat4& viewProj) {
    UiLayer l = uiLayer(which);
    xm::Mat4 size = xm::Mat4::identity();
    size.at(0, 0) = l.width;
    size.at(1, 1) = l.height;
    xm::Mat4 mvp = viewProj * xm::poseMatrix(l.orientation, l.position) * size;
    glUseProgram(sLayerProgram);
    glUniformMatrix4fv(sLayerMvp, 1, GL_FALSE, mvp.m);
    int w, h;
    uiLayerSize(which, &w, &h);
    glUniform2f(sLayerUvScale, l.imageWidth ? (float)l.imageWidth / w : 1.0f, l.imageHeight ? (float)l.imageHeight / h : 1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, 0);
    glUniform1i(sLayerTex, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(sVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

void drawReticle2d(float x, float y, float rx, float ry) {
    glUseProgram(sLaserProgram);
    xm::Mat4 id = xm::Mat4::identity();
    glUniformMatrix4fv(sLaserMvp, 1, GL_FALSE, id.m);
    float corners[12] = {x - rx, y - ry, 0.0f, x + rx, y - ry, 0.0f, x - rx, y + ry, 0.0f, x + rx, y + ry, 0.0f};
    glUniform3fv(sLaserCorners, 4, corners);
    glUniform4f(sLaserColor, 1.0f, 0.95f, 0.55f, 0.95f * overlayBrightness());
    glUniform1i(sLaserShape, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(sVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

}  // namespace vr
