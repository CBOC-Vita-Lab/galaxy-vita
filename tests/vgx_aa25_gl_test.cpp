// aa25 host test: the depth-gated edge pass (GXV_EDGE_DEPTH) on the real vgx back end (Mesa llvmpipe, surfaceless EGL), against a 4x4
// supersampled reference of an analytic scene with real depth. The scene (all abstract, procedural): a sky (cleared depth, 1-pixel
// stars), a ground plane whose depth is affine in screen space (fine texture), slanted polygons in front (2 .. 70 degree edges), a near
// figure with 1-4 pixel texture marks (dots, a 1-pixel line, a small emblem: the "face detail" an edge pass must not blur), a sign with
// 1-pixel strokes, thin fence bars (geometry, depth written) and an alpha-tested leaf cluster (its shape from a per-pixel alpha test,
// depth written only where it passes, as the game's foliage).
// For every pixel the test computes the topmost surface at the pixel centre (colour and depth: what the GPU's single-sample EFB holds),
// uploads the colour into the EFB's texture and the depth through a full-screen pass that writes gl_FragDepth, then runs the display copy
// at 960x544 1:1 through the back end's present. Panels written (PPM, 960x544): <tag>_p1.ppm (the present), plus once ref_ss.ppm (the 4x4
// supersampled reference), ms4.ppm / ms2.ppm (what 4x / 2x hardware multisampling would store: coverage per sample, colour shaded once a
// pixel and surface at the pixel centre, alpha test per pixel), aa25_mask.pgm (pixels whose 3x3 neighbourhood spans two surfaces:
// "geometry edge"; the rest "interior").
//   vgx_aa25_gl_test <out-dir>     env: PU_TAG (name), the GXV_* switches of the run
#include "vgx_autoq.h"
#include "vgx_cmd.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return std::getenv("GXV_LOG") && std::atoi(std::getenv("GXV_LOG")) != 0; }
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t*) {}
void VtxPackDrawCounters(uint64_t*) {}
} // namespace vgx

using namespace vgx;

namespace {
constexpr int W = 960, H = 544;
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void Push(CmdBuf& b, CmdType t, uint32_t arg) { b.cmds.push_back(Cmd{t, arg}); }

struct Rgb { float r, g, b; };
struct Poly { std::vector<float> x, y; float z0, zx, zy; int tex; }; // (convex; window depth z0 + zx * px + zy * py: affine)
std::vector<Poly> g_polys;
float Hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
bool Inside(const Poly& p, float x, float y) {
    const size_t n = p.x.size();
    bool pos = false, neg = false;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const float c = (p.x[j] - p.x[i]) * (y - p.y[i]) - (p.y[j] - p.y[i]) * (x - p.x[i]);
        if (c > 0) pos = true;
        if (c < 0) neg = true;
    }
    return !(pos && neg);
}
// The leaf cluster: alpha-tested, its alpha a function of the position (lobes); evaluated at the shading point.
bool LeafAlpha(float x, float y) {
    const float cx = 790, cy = 300;
    const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy), a = std::atan2(dy, dx);
    return r < 46.0f + 16.0f * std::sin(a * 7.0f) + 6.0f * std::sin(a * 19.0f + 1.0f);
}
// The surface's texture at (x, y) (screen space; tex picks the material). Detail is 1-4 pixels wide on purpose.
Rgb Texture(int tex, float x, float y) {
    switch (tex) {
    case 0: { // ground: fine noise and stripes
        const float n = Hash(static_cast<int>(std::floor(x * 0.7f)), static_cast<int>(std::floor(y * 1.3f)));
        const float s = 0.5f + 0.5f * std::sin(x * 0.21f + y * 0.05f);
        return Rgb{0.30f + 0.25f * n, 0.45f + 0.20f * n * s, 0.15f + 0.10f * s};
    }
    case 1: // far hill: flat-ish green with soft bands
        return Rgb{0.25f, 0.55f + 0.08f * std::sin(y * 0.3f), 0.25f};
    case 2: { // near figure: base colour with marks (2x3 dots, a 1-px line, a 4x4 emblem with a 1-px stroke, 2x2 buttons)
        const float fx = x - 480, fy = y - 300;
        Rgb c{0.85f, 0.25f, 0.20f};
        if (fy > 10) c = Rgb{0.20f, 0.30f, 0.80f};
        auto box = [&](float x0, float y0, float w, float h) { return fx >= x0 && fx < x0 + w && fy >= y0 && fy < y0 + h; };
        if (box(-8, -22, 2, 3) || box(5, -22, 2, 3)) c = Rgb{0.05f, 0.05f, 0.10f};      // dots
        if (box(-6, -12, 12, 1)) c = Rgb{0.30f, 0.05f, 0.05f};                           // a 1-pixel line
        if (box(-2, -34, 5, 5)) c = Rgb{0.95f, 0.95f, 0.95f};                            // emblem ...
        if (box(-1, -33, 1, 3) || box(1, -33, 1, 3)) c = Rgb{0.80f, 0.10f, 0.10f};       // ... with 1-pixel strokes
        if (box(-5, 16, 2, 2) || box(3, 16, 2, 2) || box(-1, 24, 2, 2)) c = Rgb{0.95f, 0.85f, 0.20f}; // buttons
        return c;
    }
    case 3: { // sign: light board with 1-pixel dark strokes (text-like)
        const int px = static_cast<int>(std::floor(x)) - 150, py = static_cast<int>(std::floor(y)) - 120;
        const bool on = (py % 9 >= 2 && py % 9 <= 6) && ((px % 5 == 1) || (py % 9 == 4 && px % 7 < 4)) && px > 4 && px < 110;
        return on ? Rgb{0.10f, 0.08f, 0.05f} : Rgb{0.85f, 0.78f, 0.55f};
    }
    case 4: // fence: dark wood
        return Rgb{0.35f, 0.20f, 0.10f};
    case 5: // slanted blocks: light grey with a 1-pixel grid
        return (static_cast<int>(std::floor(x)) % 8 == 0 || static_cast<int>(std::floor(y)) % 8 == 0) ? Rgb{0.45f, 0.45f, 0.50f} : Rgb{0.75f, 0.75f, 0.80f};
    case 6: // leaves
        return Rgb{0.15f, 0.50f + 0.2f * Hash(static_cast<int>(x), static_cast<int>(y)), 0.12f};
    default:
        return Rgb{1, 0, 1};
    }
}
Rgb Sky(float x, float y) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    if (Hash(ix, iy) > 0.996f) return Rgb{1.0f, 1.0f, 0.95f}; // (1-pixel stars)
    return Rgb{0.35f + 0.2f * y / H, 0.55f + 0.2f * y / H, 0.95f};
}
// The topmost surface at (x, y): index (-1 sky), depth. The leaf (index 1000) is tested with its alpha at (ax, ay) (the shading point).
int Topmost(float x, float y, float& z, float ax, float ay) {
    int best = -1;
    z = 1.0f;
    for (size_t i = 0; i < g_polys.size(); ++i) {
        const Poly& p = g_polys[i];
        if (!Inside(p, x, y)) continue;
        if (p.tex == 6 && !LeafAlpha(ax, ay)) continue;
        const float d = p.z0 + p.zx * x + p.zy * y;
        if (d < z) { z = d; best = static_cast<int>(i); }
    }
    return best;
}
Rgb Shade(int s, float x, float y) { return s < 0 ? Sky(x, y) : Texture(g_polys[static_cast<size_t>(s)].tex, x, y); }

void BuildScene() {
    auto quad = [](std::vector<float> xs, std::vector<float> ys, float z0, float zx, float zy, int tex) { g_polys.push_back(Poly{xs, ys, z0, zx, zy, tex}); };
    // ground: below a horizon at y 250 (slightly tilted), depth affine: far at the horizon
    quad({-10, 970, 970, -10}, {244, 256, 560, 560}, 0.995f + 0.0004f * 250, 0.0f, -0.0004f, 0);
    // a far hill (slanted silhouette edges: 3 and 12 degrees), constant depth
    quad({40, 420, 430, 30}, {210, 190, 250, 250}, 0.985f, 0, 0, 1);
    quad({500, 900, 960, 520}, {200, 115, 245, 250}, 0.983f, 0, 0, 1);
    // slanted blocks: 30, 45, 70 degree edges
    quad({600, 680, 640, 560}, {330, 376, 445, 399}, 0.90f, 0.0001f, 0.0f, 5);
    quad({240, 300, 240, 180}, {330, 390, 450, 390}, 0.88f, 0.0f, 0.0f, 5);
    quad({330, 352, 380, 358}, {300, 290, 366, 376}, 0.86f, 0.0f, 0.0001f, 5);
    // the sign
    quad({150, 262, 262, 150}, {120, 120, 165, 165}, 0.95f, 0, 0, 3);
    // the near figure (convex octagon about 72 pixels tall)
    {
        std::vector<float> xs, ys;
        for (int k = 0; k < 8; ++k) {
            const float a = (k + 0.5f) * 3.14159265f / 4.0f;
            xs.push_back(480 + 24 * std::cos(a));
            ys.push_back(300 + 36 * std::sin(a));
        }
        g_polys.push_back(Poly{xs, ys, 0.50f, 0, 0, 2});
    }
    // fence: thin bars (1.2 .. 2 pixels wide), slanted a little
    for (int i = 0; i < 6; ++i) {
        const float x0 = 700 + i * 22.0f, wdt = 1.2f + 0.16f * i;
        quad({x0, x0 + wdt, x0 + wdt + 6, x0 + 6}, {380, 380, 520, 520}, 0.80f, 0, 0, 4);
    }
    quad({690, 830, 830, 690}, {420, 410, 413, 423}, 0.79f, 0, 0, 4); // (a nearly horizontal rail: a 4-degree edge 3 pixels thick)
    // the alpha-tested leaf cluster (a square whose alpha test cuts the shape)
    quad({730, 850, 850, 730}, {240, 240, 360, 360}, 0.85f, 0, 0, 6);
}

std::vector<float> g_px, g_depth;     // 1x: colour (rgb), depth
std::vector<float> g_ref, g_ms4, g_ms2; // references
std::vector<uint8_t> g_edgeMask;
void Render() {
    g_px.assign(static_cast<size_t>(W) * H * 3, 0);
    g_depth.assign(static_cast<size_t>(W) * H, 1);
    g_ref = g_px;
    g_ms4 = g_px;
    g_ms2 = g_px;
    std::vector<int> surf(static_cast<size_t>(W) * H, -1);
    // (rotated-grid sample positions of 4x and the 2x pattern, as the usual hardware patterns)
    const float s4[4][2] = {{-0.125f, -0.375f}, {0.375f, -0.125f}, {-0.375f, 0.125f}, {0.125f, 0.375f}};
    const float s2[2][2] = {{-0.25f, -0.25f}, {0.25f, 0.25f}};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            const float cx = x + 0.5f, cy = y + 0.5f;
            float z = 1;
            const int s = Topmost(cx, cy, z, cx, cy);
            surf[i] = s;
            g_depth[i] = z;
            const Rgb c = Shade(s, cx, cy);
            g_px[i * 3] = c.r; g_px[i * 3 + 1] = c.g; g_px[i * 3 + 2] = c.b;
            float a[3] = {0, 0, 0};
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    float zz;
                    const int t = Topmost(px, py, zz, px, py);
                    const Rgb q = Shade(t, px, py);
                    a[0] += q.r; a[1] += q.g; a[2] += q.b;
                }
            for (int k = 0; k < 3; ++k) g_ref[i * 3 + k] = a[k] / 16.0f;
            // multisampling: coverage per sample, the surface shaded once at the pixel centre (alpha test at the centre too)
            auto ms = [&](const float (*sp)[2], int n, std::vector<float>& out) {
                float m[3] = {0, 0, 0};
                for (int k = 0; k < n; ++k) {
                    float zz;
                    const int t = Topmost(cx + sp[k][0], cy + sp[k][1], zz, cx, cy);
                    const Rgb q = Shade(t, cx, cy);
                    m[0] += q.r; m[1] += q.g; m[2] += q.b;
                }
                for (int k = 0; k < 3; ++k) out[i * 3 + k] = m[k] / static_cast<float>(n);
            };
            ms(s4, 4, g_ms4);
            ms(s2, 2, g_ms2);
        }
    g_edgeMask.assign(static_cast<size_t>(W) * H, 0);
    for (int y = 1; y < H - 1; ++y)
        for (int x = 1; x < W - 1; ++x) {
            const int s = surf[static_cast<size_t>(y) * W + x];
            bool e = false;
            for (int dy = -1; dy <= 1 && !e; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (surf[static_cast<size_t>(y + dy) * W + x + dx] != s) { e = true; break; }
            g_edgeMask[static_cast<size_t>(y) * W + x] = e ? 255 : 0;
        }
}
void WritePpmF(const std::string& path, const std::vector<float>& px) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < px.size(); ++i) std::fputc(static_cast<int>(std::lround(std::clamp(px[i], 0.0f, 1.0f) * 255.0f)), f);
    std::fclose(f);
}

// The EFB: colour by upload (as the upscale test), depth by a full-screen pass writing gl_FragDepthEXT from a 24-bit packed texture.
GLuint g_depthProg = 0, g_depthTex = 0;
bool FillEfb() {
    const float black[4] = {0, 0, 0, 1};
    backend::DebugDrawEfbTriangle(black); // (binds the EFB's framebuffer)
    GLint fbo = 0, tex = 0, prevTex = 0, align = 4, prog = 0, abuf = 0, vp[4], dfunc = 0, unit = 0;
    GLboolean dtest = 0, dmask = 0, cmask[4], blend = 0, scissor = 0, cull = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
    dtest = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    GLint en0 = 0;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en0);
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4), dz(static_cast<size_t>(W) * H * 4);
    for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
        for (int k = 0; k < 3; ++k) px[i * 4 + k] = static_cast<uint8_t>(std::lround(std::clamp(g_px[i * 3 + k], 0.0f, 1.0f) * 255.0f));
        px[i * 4 + 3] = 255;
        const uint32_t q = static_cast<uint32_t>(std::lround(static_cast<double>(g_depth[i]) * 16777215.0));
        dz[i * 4] = static_cast<uint8_t>(q >> 16); dz[i * 4 + 1] = static_cast<uint8_t>(q >> 8); dz[i * 4 + 2] = static_cast<uint8_t>(q); dz[i * 4 + 3] = 255;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (!g_depthTex) {
        glGenTextures(1, &g_depthTex);
        glBindTexture(GL_TEXTURE_2D, g_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, g_depthTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, dz.data());
    if (!g_depthProg) {
        const char* vs = "attribute vec2 a_pos;\nvarying vec2 v_uv;\nvoid main() { v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        const char* fs = "#extension GL_EXT_frag_depth : enable\nprecision highp float;\nuniform sampler2D u_d;\nvarying vec2 v_uv;\n"
                         "void main() { vec4 q = floor(texture2D(u_d, v_uv) * 255.0 + 0.5); gl_FragDepthEXT = (q.r * 65536.0 + q.g * 256.0 + q.b) / 16777215.0; gl_FragColor = vec4(0.0); }\n";
        auto sh = [](GLenum t, const char* src) {
            const GLuint s = glCreateShader(t);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::printf("shader: %s\n", log); }
            return s;
        };
        g_depthProg = glCreateProgram();
        glAttachShader(g_depthProg, sh(GL_VERTEX_SHADER, vs));
        glAttachShader(g_depthProg, sh(GL_FRAGMENT_SHADER, fs));
        glBindAttribLocation(g_depthProg, 0, "a_pos");
        glLinkProgram(g_depthProg);
        GLint ok = 0;
        glGetProgramiv(g_depthProg, GL_LINK_STATUS, &ok);
        if (!ok) { std::printf("depth program did not link\n"); return false; }
    }
    glUseProgram(g_depthProg);
    glUniform1i(glGetUniformLocation(g_depthProg, "u_d"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glViewport(0, 0, W, H);
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // (the back end's view of the GL state restored)
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
    glDepthFunc(static_cast<GLenum>(dfunc));
    if (!dtest) glDisable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (!en0) glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(abuf));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glUseProgram(static_cast<GLuint>(prog));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glActiveTexture(static_cast<GLenum>(unit));
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    return glGetError() == GL_NO_ERROR;
}

void Present(CmdBuf& b, bool clear, bool edge) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clear;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    d.edge = edge;
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT, 0);
}
uint64_t g_frame = 1000;
} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const std::string tag = std::getenv("PU_TAG") ? std::getenv("PU_TAG") : "run";
    if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
    const int forceRung = std::getenv("PU_RUNG") ? std::atoi(std::getenv("PU_RUNG")) : -1; // (a rung through the controller: 6, 7 = 2x, 8 = 4x)
    if (!backend::Init(forceRung >= 0 ? 0 : W, forceRung >= 0 ? 0 : H)) { std::printf("back end Init failed\n"); return 2; }
    {
        CmdBuf b;
        b.frame = 1;
        b.targets.push_back(SetTargetCmd{640, 456});
        Push(b, CMD_SET_TARGET, 0);
        Present(b, true, false);
        backend::Execute(b);
        CmdBuf a; a.frame = g_frame++; Present(a, true, false); backend::Execute(a);
        if (forceRung >= 0)
            for (int r : {forceRung == 5 ? 4 : 5, forceRung}) {
                backend::ForceQualityRung(r);
                CmdBuf c1; c1.frame = g_frame++; Present(c1, true, false); backend::Execute(c1);
                CmdBuf c2; c2.frame = g_frame++; Present(c2, true, false); backend::Execute(c2);
            }
    }
    int rw = 0, rh = 0, rs = 0;
    backend::QualityRungSize(rw, rh, rs);
    Check(rw == W && rh == H, "the EFB is 960x544 (1:1 with the screen)");
    std::printf("  EFB %dx%d, %d sample(s) a pixel\n", rw, rh, rs);
    BuildScene();
    Render();
    if (std::getenv("PU_REFS")) {
        WritePpmF(out + "/ref_ss.ppm", g_ref);
        WritePpmF(out + "/ref_1x.ppm", g_px);
        WritePpmF(out + "/ms4.ppm", g_ms4);
        WritePpmF(out + "/ms2.ppm", g_ms2);
        FILE* f = std::fopen((out + "/aa25_mask.pgm").c_str(), "wb");
        if (f) { std::fprintf(f, "P5\n%d %d\n255\n", W, H); std::fwrite(g_edgeMask.data(), 1, g_edgeMask.size(), f); std::fclose(f); }
    }
    Check(FillEfb(), "colour and depth written into the EFB");
    CmdBuf b;
    b.frame = g_frame++;
    Present(b, false, true);
    backend::RequestCapture(true);
    backend::Execute(b);
    backend::RequestCapture(false);
    int cw = 0, ch = 0;
    const std::vector<uint8_t> px = backend::LastDisplayCopy(cw, ch);
    Check(cw == W && ch >= H, "the display copy was captured");
    if (cw == W && ch >= H) {
        // (GXV_CAP_AB=1: two panels, the second is the full present; else one)
        const int panel = ch >= 2 * H ? 1 : 0;
        FILE* f = std::fopen((out + "/" + tag + "_p1.ppm").c_str(), "wb");
        if (f) {
            std::fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) std::fwrite(&px[(static_cast<size_t>(panel * H + y) * W + x) * 4], 1, 3, f);
            std::fclose(f);
        }
    }
    std::printf("%s: %s (%d failed)\n", tag.c_str(), g_fail ? "SOME CHECKS FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
