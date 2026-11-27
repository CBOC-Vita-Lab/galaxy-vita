// halo32: the HUD reader's depth pre-pass (back end: KeepdPre), drawn in GLES (Mesa software) against the game's own order.
//   A (the game):  3D with depth, depth clear to c, the HUD draw (LEQUAL + write, blended, a fragment discard, overlapping triangles of other depths).
//   B (halo32):    the same 3D, NO clear, the pre-pass (same program and vertices, ALWAYS + write, colour masked, depth range collapsed to c), the HUD draw.
// Must agree: every colour of the picture, and, on every pixel the HUD draw reaches, the depth after it (probed with full-screen LESS tests at 9 depths).
// Mutants: HALO32_MUTANT=1 no pre-pass; 2 pre-pass keeps the HUD's own depth range (not collapsed); 3 pre-pass LEQUAL instead of ALWAYS.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#ifndef HALO32_MUTANT
#define HALO32_MUTANT 0
#endif
static const int W = 64, H = 64;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL %s (GL %x EGL %x)\n", why, glGetError(), eglGetError()); std::exit(1); } }
static uint32_t g_seed;
static float Rnd() { g_seed = g_seed * 1664525u + 1013904223u; return static_cast<float>((g_seed >> 8) & 0xFFFF) / 65535.0f; }
static GLuint Prog(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    const char* srcs[2] = {vs, fs};
    GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; ++i) {
        GLuint s = glCreateShader(ty[i]); glShaderSource(s, 1, &srcs[i], nullptr); glCompileShader(s);
        int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char m[1024]; glGetShaderInfoLog(s, sizeof m, nullptr, m); check(false, m); }
        glAttachShader(p, s);
    }
    glBindAttribLocation(p, 0, "a_pos"); glLinkProgram(p);
    return p;
}
static const char* kVs = "attribute vec3 a_pos; varying vec2 v_p; void main(){ v_p = a_pos.xy; gl_Position = vec4(a_pos, 1.0); }";
static const char* kFs = "precision highp float; varying vec2 v_p; uniform vec4 u_col; uniform float u_flag;\n"
                         "void main(){ if (sin(v_p.x * 37.0 + v_p.y * 11.0) > 0.55) discard;\n"
                         "  gl_FragColor = u_flag > 0.5 ? vec4(1.0) : vec4(u_col.rgb * (0.5 + 0.5 * sin(v_p.x * 9.0)), u_col.a); }";
struct Tri { float v[9]; };
static std::vector<Tri> MakeTris(int n, float z0, float z1) {
    std::vector<Tri> t(n);
    for (auto& q : t) { const float cx = Rnd() * 2 - 1, cy = Rnd() * 2 - 1; for (int k = 0; k < 3; ++k) { q.v[3 * k] = cx + (Rnd() - 0.5f) * 1.2f; q.v[3 * k + 1] = cy + (Rnd() - 0.5f) * 1.2f; q.v[3 * k + 2] = z0 + (z1 - z0) * Rnd(); } }
    return t;
}
static void DrawTris(const std::vector<Tri>& t) {
    glEnableVertexAttribArray(0);
    for (const Tri& q : t) { glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, q.v); glDrawArrays(GL_TRIANGLES, 0, 3); }
}
static GLint g_col, g_flag;
static void Col(float r, float g, float b, float a, float flag = 0) { glUniform4f(g_col, r, g, b, a); glUniform1f(g_flag, flag); }
static std::vector<uint8_t> Read() { std::vector<uint8_t> p(W * H * 4); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, p.data()); return p; }
// the 3D: opaque, LESS + write, many depths
static void Scene3D(const std::vector<Tri>& t3) {
    glDisable(GL_BLEND); glDepthMask(GL_TRUE); glColorMask(1, 1, 1, 1); glDepthRangef(0, 1);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glClearColor(0.1f, 0.1f, 0.3f, 1); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (size_t i = 0; i < t3.size(); ++i) { Col(0.2f + 0.02f * (i % 30), 0.8f - 0.02f * (i % 30), 0.3f, 1); std::vector<Tri> one{t3[i]}; DrawTris(one); }
}
static void Hud(const std::vector<Tri>& th, float c, int arm, bool depthProbe) {
    (void)depthProbe;
    if (arm == 0) { glDepthMask(GL_TRUE); glClearDepthf(c); glClear(GL_DEPTH_BUFFER_BIT); }  // the game's clear (a constant)
    if (arm == 1 && HALO32_MUTANT != 1) {  // the pre-pass
        glEnable(GL_DEPTH_TEST); glDepthFunc(HALO32_MUTANT == 3 ? GL_LEQUAL : GL_ALWAYS); glDepthMask(GL_TRUE); glColorMask(0, 0, 0, 0); glDisable(GL_BLEND);
        if (HALO32_MUTANT == 2) glDepthRangef(0, 1); else glDepthRangef(c, c);
        Col(1, 1, 1, 1);
        DrawTris(th);
    }
    glDepthRangef(0, 1); glColorMask(1, 1, 1, 1); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (size_t i = 0; i < th.size(); ++i) { Col(0.9f - 0.01f * i, 0.2f + 0.02f * i, 0.5f, 0.6f); std::vector<Tri> one{th[i]}; DrawTris(one); }
    glDisable(GL_BLEND);
}
// the depth after the HUD, probed: a full-screen LESS test at depth z (window), colour 1 where it passes
static std::vector<uint8_t> Probe(float zw) {
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_FALSE); glColorMask(1, 1, 1, 1); glDepthRangef(zw, zw);
    const Tri q1 = {{-1, -1, 0, 1, -1, 0, -1, 1, 0}}, q2 = {{1, -1, 0, 1, 1, 0, -1, 1, 0}};
    Col(1, 1, 1, 1, 1);
    DrawTris({q1, q2});
    glDepthRangef(0, 1);
    return Read();
}
int main() {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(eglInitialize(d, nullptr, nullptr), "init"); check(eglBindAPI(EGL_OPENGL_ES_API), "api");
    EGLConfig cfg; EGLint num;
    EGLint at[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    check(eglChooseConfig(d, at, &cfg, 1, &num) && num == 1, "config");
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE}, sa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca); EGLSurface surf = eglCreatePbufferSurface(d, cfg, sa);
    check(eglMakeCurrent(d, surf, surf, ctx), "current");
    glDisable(GL_DITHER); glViewport(0, 0, W, H);
    GLuint p = Prog(kVs, kFs); glUseProgram(p); g_col = glGetUniformLocation(p, "u_col"); g_flag = glGetUniformLocation(p, "u_flag");
    int bad = 0, total = 0; long footprint = 0, wrongDepth = 0;
    for (int seed = 1; seed <= 24; ++seed) {
        g_seed = 1000u + seed * 7919u;
        const std::vector<Tri> t3 = MakeTris(40, -0.9f, 0.9f), th = MakeTris(8 + seed % 6, -1.0f, 1.0f);
        const float c = (seed % 3 == 0) ? 1.0f : (seed % 3 == 1 ? 0.97f : 0.9999f);  // the clear's constant (the game's is 1.0)
        // footprint: the HUD geometry, flat colour, no depth test, no blend, same discard
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glColorMask(1, 1, 1, 1); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        Col(1, 1, 1, 1, 1); DrawTris(th);
        const std::vector<uint8_t> fp = Read();
        std::vector<uint8_t> img[2], pr[2][9];
        for (int arm = 0; arm < 2; ++arm) {
            Scene3D(t3);
            Hud(th, c, arm, false);
            img[arm] = Read();
            for (int k = 0; k < 9; ++k) pr[arm][k] = Probe(0.1f * (k + 1));
        }
        total += W * H;
        for (int i = 0; i < W * H; ++i) {
            for (int ch = 0; ch < 4; ++ch) if (img[0][4 * i + ch] != img[1][4 * i + ch]) { ++bad; break; }
            if (fp[4 * i]) {
                ++footprint;
                for (int k = 0; k < 9; ++k) if (pr[0][k][4 * i] != pr[1][k][4 * i]) { ++wrongDepth; break; }
            }
        }
    }
    std::printf("halo32 prepass (mutant %d): 24 scenes, %d pixels; colour differences %d; HUD footprint %ld px, depth differences there %ld\n", HALO32_MUTANT, total, bad, footprint, wrongDepth);
    const bool ok = bad == 0 && wrongDepth == 0 && footprint > 1000;
    std::printf(ok ? "halo32 prepass: PASS\n" : "halo32 prepass: FAIL\n");
    return ok ? 0 : 1;
}
