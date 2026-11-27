// sharp24 host tool: a small 3D test scene rendered with Mesa (GLES2, surfaceless EGL) at any size, with a texture LOD bias and an
// anisotropy level, so the picture the game would draw at a rung (608x348 .. 960x544) can be compared with the native 960x544 picture and
// with a supersampled (3840x2176 -> box) reference. Content (all procedural, deterministic): a grazing-angle ground (grass-like noise), brick
// walls, a textured sphere with a hard silhouette, a thin fence (about one pixel wide at 960x544), distant mountains with long straight
// edges, a sky gradient. The camera moves a little each frame (sub-pixel steps) for the shimmer measure.
//   sharp24_scene W H out.ppm [bias_levels=0] [aniso=1] [frame=0] [mipmode=2 (0 none,1 nearest-mip,2 trilinear)] [scene=0]
// bias_levels: added to the texture's LOD in the fragment shader (negative = sharper); aniso: GL_TEXTURE_MAX_ANISOTROPY_EXT when present.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static uint32_t g_rng = 12345;
static float Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) * (1.0f / 16777216.0f); }
static float Smooth(float t) { return t * t * (3 - 2 * t); }
// periodic value noise on an n x n lattice
struct Noise {
    int n; std::vector<float> v;
    explicit Noise(int n_) : n(n_), v(n_ * n_) { for (auto& x : v) x = Rnd(); }
    float At(float x, float y) const {
        const float fx = std::floor(x), fy = std::floor(y);
        const int ix = (int)fx, iy = (int)fy;
        const float tx = Smooth(x - fx), ty = Smooth(y - fy);
        auto g = [&](int a, int b) { return v[(((b % n) + n) % n) * n + (((a % n) + n) % n)]; };
        const float a = g(ix, iy) * (1 - tx) + g(ix + 1, iy) * tx, b = g(ix, iy + 1) * (1 - tx) + g(ix + 1, iy + 1) * tx;
        return a * (1 - ty) + b * ty;
    }
};
static float Fbm(const std::vector<Noise>& ns, float u, float v, int oct, float gain) {
    float s = 0, a = 1, tot = 0;
    for (int o = 0; o < oct; ++o) {
        const Noise& n = ns[o];
        const float f = (float)(1 << o); // lattice n = 4 << o keeps the tile periodic on [0,1)
        s += a * n.At(u * 4 * f, v * 4 * f); tot += a; a *= gain;
    }
    return s / tot;
}
static std::vector<uint8_t> MakeTexture(int kind, int S) {
    std::vector<Noise> ns;
    for (int o = 0; o < 8; ++o) ns.emplace_back(4 << o);
    std::vector<uint8_t> px((size_t)S * S * 4);
    const Noise blade(256);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const float u = (x + 0.5f) / S, v = (y + 0.5f) / S;
            float r = 0.5f, g = 0.5f, b = 0.5f;
            if (kind == 0) { // grass / dirt: fbm with fine detail and short dark blades
                const float n = Fbm(ns, u, v, 8, 0.62f);
                const float m = Fbm(ns, u + 0.3f, v + 0.7f, 3, 0.5f);
                const float blades = blade.At(u * 256, v * 64) > 0.78f ? 0.25f : 0.0f;
                r = 0.18f + 0.45f * n + 0.25f * m - blades * 0.6f;
                g = 0.30f + 0.55f * n + 0.10f * m - blades * 0.4f;
                b = 0.10f + 0.25f * n - blades * 0.3f;
            } else if (kind == 1) { // brick: 8 x 16 bricks, mortar lines 2 texels, per-brick colour, fine grain
                const int bw = S / 8, bh = S / 16;
                const int row = y / bh;
                const int xo = (x + (row & 1) * (bw / 2)) % S;
                const int bx = xo / bw;
                const bool mortar = (y % bh) < 2 || (xo % bw) < 2;
                g_rng = 777u + (unsigned)(bx * 31 + row * 17);
                const float tint = 0.75f + 0.25f * Rnd();
                const float grain = Fbm(ns, u, v, 8, 0.7f);
                if (mortar) { r = g = b = 0.55f + 0.2f * grain; }
                else { r = tint * (0.55f + 0.35f * grain); g = tint * (0.28f + 0.25f * grain); b = tint * (0.2f + 0.2f * grain); }
            } else if (kind == 3) { // stress: high-contrast fine detail (1-texel lines every 6 texels both ways, a hashed speckle, a diagonal grating)
                const bool line = (x % 6) == 0 || (y % 6) == 0;
                g_rng = 99u + (unsigned)(x * 73856093u ^ y * 19349663u);
                const bool speck = Rnd() > 0.93f;
                const bool diag = ((x + y) % 10) < 2;
                const float v0 = (line || speck || diag) ? 0.08f : 0.85f;
                r = v0; g = v0 * 0.95f; b = v0 * 0.8f;
            } else { // planet surface: continents with sharp coast lines, fine texture
                const float n = Fbm(ns, u, v, 6, 0.55f);
                const float f = Fbm(ns, u + 0.5f, v + 0.2f, 8, 0.7f);
                const bool land = n > 0.52f;
                if (land) { r = 0.25f + 0.4f * f; g = 0.5f + 0.4f * f; b = 0.15f + 0.2f * f; }
                else { r = 0.1f + 0.1f * f; g = 0.25f + 0.2f * f; b = 0.6f + 0.3f * f; }
                if (std::fabs(n - 0.52f) < 0.008f) { r = 0.9f; g = 0.85f; b = 0.6f; } // a beach line
            }
            uint8_t* p = &px[((size_t)y * S + x) * 4];
            auto q = [](float t) { return (uint8_t)std::lround(std::fmin(1.f, std::fmax(0.f, t)) * 255.f); };
            p[0] = q(r); p[1] = q(g); p[2] = q(b); p[3] = 255;
        }
    return px;
}

struct V { float x, y, z, u, v, l; };
static std::vector<V> g_ground, g_wall, g_ball, g_fence, g_mount;
static void Quad(std::vector<V>& o, V a, V b, V c, V d) { o.push_back(a); o.push_back(b); o.push_back(c); o.push_back(a); o.push_back(c); o.push_back(d); }

static const char* kVs =
    "attribute vec3 a_p; attribute vec2 a_uv; attribute float a_l; uniform mat4 u_vp; varying vec2 v_uv; varying float v_l;\n"
    "void main(){ v_uv=a_uv; v_l=a_l; gl_Position=u_vp*vec4(a_p,1.0);} \n";
static const char* kFs =
    "precision highp float; uniform sampler2D u_t; uniform float u_bias; uniform float u_flat; uniform vec3 u_col; varying vec2 v_uv; varying float v_l;\n"
    "void main(){ vec3 c = u_flat > 0.5 ? u_col : texture2D(u_t, v_uv, u_bias).rgb; gl_FragColor = vec4(c*v_l,1.0);} \n";
static const char* kSkyVs = "attribute vec3 a_p; attribute vec2 a_uv; attribute float a_l; varying float v_y; void main(){ v_y=a_p.y*0.5+0.5; gl_Position=vec4(a_p.xy,0.9999,1.0);} \n";
static const char* kSkyFs = "precision highp float; varying float v_y; void main(){ gl_FragColor=vec4(mix(vec3(0.78,0.86,0.95),vec3(0.18,0.34,0.72),pow(v_y,0.8)),1.0);} \n";

static GLuint Prog(const char* vs, const char* fs) {
    auto sh = [](GLenum t, const char* s) { GLuint h = glCreateShader(t); glShaderSource(h, 1, &s, nullptr); glCompileShader(h); GLint ok; glGetShaderiv(h, GL_COMPILE_STATUS, &ok);
        if (!ok) { char b[1024]; glGetShaderInfoLog(h, 1024, nullptr, b); fprintf(stderr, "shader: %s\n", b); exit(2);} return h; };
    GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "a_p"); glBindAttribLocation(p, 1, "a_uv"); glBindAttribLocation(p, 2, "a_l"); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link\n"); exit(2); } return p;
}
static void Mat4Persp(float* m, float fovy, float asp, float n, float f) {
    const float t = 1.0f / std::tan(fovy * 0.5f);
    std::memset(m, 0, 64); m[0] = t / asp; m[5] = t; m[10] = (f + n) / (n - f); m[11] = -1; m[14] = 2 * f * n / (n - f);
}
static void Mul(const float* a, const float* b, float* o) { for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) { float s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; } }
static void ViewYawPitch(float* m, float ex, float ey, float ez, float yaw, float pitch) {
    // camera looks along +z (yaw 0), x right, y up; OpenGL view looks down -z: flip z.
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // world->view = R * T ; R rows: right, up, -forward
    const float fwd[3] = {sy * cp, sp, cy * cp};
    const float right[3] = {cy, 0, -sy};
    const float up[3] = {-sy * sp, cp, -cy * sp};
    float r[16] = {right[0], up[0], -fwd[0], 0, right[1], up[1], -fwd[1], 0, right[2], up[2], -fwd[2], 0, 0, 0, 0, 1};
    r[12] = -(right[0] * ex + right[1] * ey + right[2] * ez);
    r[13] = -(up[0] * ex + up[1] * ey + up[2] * ez);
    r[14] = (fwd[0] * ex + fwd[1] * ey + fwd[2] * ez);
    std::memcpy(m, r, 64);
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: W H out.ppm [bias] [aniso] [frame] [mipmode] [scene]\n"); return 1; }
    const int W = atoi(argv[1]), H = atoi(argv[2]);
    const char* out = argv[3];
    const float bias = argc > 4 ? (float)atof(argv[4]) : 0.f;
    const float aniso = argc > 5 ? (float)atof(argv[5]) : 1.f;
    const int frame = argc > 6 ? atoi(argv[6]) : 0;
    const int mipmode = argc > 7 ? atoi(argv[7]) : 2;
    const int stress = argc > 8 ? atoi(argv[8]) : 0;
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint mj, mn; eglInitialize(dpy, &mj, &mn); eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n; eglChooseConfig(dpy, ca, &cfg, 1, &n);
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE}; EGLSurface s = eglCreatePbufferSurface(dpy, cfg, pb); eglMakeCurrent(dpy, s, s, ctx); }
    const bool hasAniso = strstr((const char*)glGetString(GL_EXTENSIONS), "GL_EXT_texture_filter_anisotropic") != nullptr;

    GLuint fbo, col, dep;
    glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &col); glGenRenderbuffers(1, &dep);
    glBindRenderbuffer(GL_RENDERBUFFER, col); glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /*RGBA8*/, W, H);
    glBindRenderbuffer(GL_RENDERBUFFER, dep); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, col);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); return 2; }

    // textures
    GLuint tex[3];
    glGenTextures(3, tex);
    for (int k = 0; k < 3; ++k) {
        g_rng = 1000u + k * 97u;
        const std::vector<uint8_t> px = MakeTexture(k == 0 && stress ? 3 : k, 512);
        glBindTexture(GL_TEXTURE_2D, tex[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmode == 0 ? GL_LINEAR : mipmode == 1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
        if (hasAniso) glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, aniso);
    }
    // geometry
    // ground
    Quad(g_ground, {-60, 0, -3, -60 / 1.5f, -3 / 1.5f, 1.0f}, {60, 0, -3, 60 / 1.5f, -3 / 1.5f, 1.0f}, {60, 0, 220, 60 / 1.5f, 220 / 1.5f, 1.0f}, {-60, 0, 220, -60 / 1.5f, 220 / 1.5f, 1.0f});
    // wall on the right (x = 7, facing -x) with a stepped top, and a wall across (z = 46), each lit differently
    Quad(g_wall, {7, 0, 6, 0, 0, 0.85f}, {7, 0, 70, 64 / 1.5f, 0, 0.85f}, {7, 4.2f, 70, 64 / 1.5f, 4.2f / 1.5f, 0.85f}, {7, 4.2f, 6, 0, 4.2f / 1.5f, 0.85f});
    Quad(g_wall, {-12, 0, 46, 0, 0, 0.7f}, {4, 0, 46, 16 / 1.5f, 0, 0.7f}, {4, 5.0f, 46, 16 / 1.5f, 5.0f / 1.5f, 0.7f}, {-12, 5.0f, 46, 0, 5.0f / 1.5f, 0.7f});
    // a slanted roof piece for diagonal edges
    Quad(g_wall, {-12, 5.0f, 46, 0, 0, 0.95f}, {4, 5.0f, 46, 16 / 1.5f, 0, 0.95f}, {2, 7.2f, 52, 16 / 1.5f, 2, 0.95f}, {-10, 6.0f, 52, 0, 2, 0.95f});
    // sphere
    {
        const int NL = 48, NO = 96; const float R = 3.0f, cx = -6.0f, cy = 3.6f, cz = 26.0f;
        for (int i = 0; i < NL; ++i) for (int j = 0; j < NO; ++j) {
            auto P = [&](int a, int b) { const float th = (float)M_PI * a / NL, ph = 2.f * (float)M_PI * b / NO;
                const float nx = std::sin(th) * std::cos(ph), ny = std::cos(th), nz = std::sin(th) * std::sin(ph);
                const float l = 0.35f + 0.65f * std::fmax(0.f, 0.5f * nx + 0.6f * ny - 0.6f * nz);
                return V{cx + R * nx, cy + R * ny, cz + R * nz, 4.f * b / NO, 2.f * a / NL, l}; };
            Quad(g_ball, P(i, j), P(i, j + 1), P(i + 1, j + 1), P(i + 1, j));
        }
    }
    // fence: thin posts and two rails (flat colour)
    for (int i = -8; i <= 8; ++i) {
        const float x = i * 0.5f - 0.015f;
        Quad(g_fence, {x, 0, 14, 0, 0, 1}, {x + 0.03f, 0, 14, 0, 0, 1}, {x + 0.03f, 1.3f, 14, 0, 0, 1}, {x, 1.3f, 14, 0, 0, 1});
    }
    Quad(g_fence, {-4.2f, 0.95f, 14, 0, 0, 1}, {4.2f, 0.95f, 14, 0, 0, 1}, {4.2f, 0.97f, 14, 0, 0, 1}, {-4.2f, 0.97f, 14, 0, 0, 1});
    Quad(g_fence, {-4.2f, 0.45f, 14, 0, 0, 1}, {4.2f, 0.45f, 14, 0, 0, 1}, {4.2f, 0.462f, 14, 0, 0, 1}, {-4.2f, 0.462f, 14, 0, 0, 1});
    // mountains: big flat-lit triangles with straight edges at several angles
    for (int i = 0; i < 7; ++i) {
        const float x0 = -90 + i * 28.f, hgt = 18.f + 9.f * (i % 3), w = 30.f + 6.f * (i % 2);
        const float l = 0.5f + 0.1f * (i % 4);
        g_mount.push_back({x0, 0, 200, 0, 0, l}); g_mount.push_back({x0 + w, 0, 200, 0, 0, l}); g_mount.push_back({x0 + w * (0.3f + 0.1f * (i % 3)), hgt, 200, 0, 0, l * 0.8f});
    }

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1); glClearDepthf(1.f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    // sky: a full-screen quad at the far plane
    {
        static const float q[] = {-1, -1, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, -1, -1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, -1, 1, 0, 0, 0, 0};
        const GLuint p = Prog(kSkyVs, kSkyFs); glUseProgram(p);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, q); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, q + 3); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 24, q + 5);
        glDisable(GL_DEPTH_TEST); glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    const GLuint P = Prog(kVs, kFs); glUseProgram(P);
    float proj[16], view[16], vp[16];
    Mat4Persp(proj, 50.f * (float)M_PI / 180.f, 960.f / 544.f, 0.1f, 400.f);
    const float f = (float)frame;
    ViewYawPitch(view, 0.4f + 0.0137f * f, 1.7f, 0.0f + 0.031f * f, 0.06f + 0.0011f * f, -0.07f);
    Mul(proj, view, vp);
    glUniformMatrix4fv(glGetUniformLocation(P, "u_vp"), 1, GL_FALSE, vp);
    glUniform1f(glGetUniformLocation(P, "u_bias"), bias);
    glUniform1i(glGetUniformLocation(P, "u_t"), 0);
    auto draw = [&](const std::vector<V>& v, int texture, bool flat, float r, float g, float b) {
        glUniform1f(glGetUniformLocation(P, "u_flat"), flat ? 1.f : 0.f); glUniform3f(glGetUniformLocation(P, "u_col"), r, g, b);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex[texture]);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), &v[0].x); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), &v[0].u); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(V), &v[0].l);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
    };
    draw(g_mount, 0, true, 0.45f, 0.5f, 0.55f);
    draw(g_ground, 0, false, 0, 0, 0);
    draw(g_wall, 1, false, 0, 0, 0);
    draw(g_ball, 2, false, 0, 0, 0);
    draw(g_fence, 0, true, 0.92f, 0.9f, 0.8f);
    std::vector<uint8_t> px((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* fo = fopen(out, "wb");
    fprintf(fo, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; --y) for (int x = 0; x < W; ++x) fwrite(&px[((size_t)y * W + x) * 4], 1, 3, fo);
    fclose(fo);
    fprintf(stderr, "%dx%d bias %.3f aniso %.0f (%s) frame %d mip %d -> %s\n", W, H, bias, aniso, hasAniso ? "ext" : "no ext", frame, mipmode, out);
    return 0;
}
