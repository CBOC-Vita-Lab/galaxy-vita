// GXV_BGLAYER (bglayer25): the far-background layer cache and the census, through the production GX front end and GLES back end on Mesa (no game data).
//   tools/test_vgx_bglayer_gl.sh builds and runs it. A 128x128 picture, a perspective camera, a far layer (a sky, three bars and a "planet" 20000 away; a nearer
//   one 8000 away) and a near layer (quads 300 away). Pictures:
//     ref(V)      the whole scene drawn plainly (what the game does with the switch off)
//     render(V)   BG_BEGIN, the far layer, BG_END(slot), then the near layer          (must be bit-identical to ref(V))
//     reuse(V')   BG_REUSE(slot'), then the near layer only; the cache is the last render's  (V' = V: bit-identical to ref(V); a turned or moved V': the error image)
//   and the census: markers 28 round the draws, the log line (port_log_summary captured) must say what was drawn.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
static std::string g_log;
extern "C" void port_log_summary(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log += buf;
    g_log += "\n";
}
extern "C" uint32_t port_camera_cut_count(void) { return 0; }

namespace {
using namespace vgx;
constexpr int W = 128, H = 128;
alignas(32) unsigned char readDestination[32];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void Pass() { // no texture: colour and alpha = C0, depth test LEQUAL with writes
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 1 | (3u << 1) | (1u << 4)); // z compare enable, func LEQUAL (3), update
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    BP(0xF1, 0);
}
struct View {
    float m[12];  // world -> eye
    float cx, cy, cz;
};
// camera at (cx,cy,cz) looking down -z after a turn of `yaw` radians about y and `pitch` about x
View MakeView(float yaw, float pitch, float cx, float cy, float cz) {
    const float cyw = std::cos(yaw), syw = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float ry[9] = {cyw, 0, -syw, 0, 1, 0, syw, 0, cyw};
    const float rx[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp};
    float R[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R[i * 3 + j] = rx[i * 3] * ry[j] + rx[i * 3 + 1] * ry[3 + j] + rx[i * 3 + 2] * ry[6 + j];
    View v{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) v.m[i * 4 + j] = R[i * 3 + j];
        v.m[i * 4 + 3] = -(R[i * 3] * cx + R[i * 3 + 1] * cy + R[i * 3 + 2] * cz);
    }
    v.cx = cx; v.cy = cy; v.cz = cz;
    return v;
}
const float kNear = 100.0f, kFar = 100000.0f, kFov = 60.0f * 3.14159265f / 180.0f;
float P00() { return 1.0f / std::tan(kFov * 0.5f); }
float P11() { return 1.0f / std::tan(kFov * 0.5f); }
void SetCamera(const View& v) {
    uint32_t w[12];
    for (int i = 0; i < 12; ++i) w[i] = f2u(v.m[i]);
    WriteXF(0, 12, w);
    const float e = -kNear / (kFar - kNear), f = -kFar * kNear / (kFar - kNear);
    const uint32_t proj[7] = {f2u(P00()), 0, f2u(P11()), 0, f2u(e), f2u(f), 0};  // GX perspective: A B C D E F, type 0
    WriteXF(0x1020, 7, proj);
    const uint32_t vp[6] = {f2u(W / 2.0f), f2u(-H / 2.0f), f2u(16777215.0f), f2u(342.0f + W / 2.0f), f2u(342.0f + H / 2.0f), f2u(16777215.0f)};
    WriteXF(0x101A, 6, vp);
}
// a quad in the world: centre (x, y), half sizes, depth z (negative: in front of the start pose)
void Quad(float cx, float cy, float hw, float hh, float z) {
    const float v[16] = {cx - hw, cy - hh, z, 0, cx + hw, cy - hh, z, 0, cx + hw, cy + hh, z, 0, cx - hw, cy + hh, z, 0};
    DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
}
void FarLayer() {
    Pass();
    Color(30, 60, 150, 255); Quad(0, 0, 90000, 90000, -60000);        // the sky
    Color(220, 40, 40, 255); Quad(-4000, 0, 2000, 6000, -20000);     // three bars
    Color(40, 200, 60, 255); Quad(0, 1500, 1200, 2500, -20000);
    Color(240, 220, 40, 255); Quad(5000, -500, 2000, 3500, -20000);
    Color(180, 90, 220, 255); Quad(-9000, 7000, 1500, 1500, -8000);   // a nearer far one, 8000 away
}
void NearLayer() {
    Pass();
    Color(250, 140, 20, 255); Quad(-40, 0, 90, 80, -300);
    Color(20, 230, 230, 255); Quad(150, -30, 50, 60, -300);
    Color(255, 255, 255, 255); Quad(5, 90, 40, 20, -250);
}
// B behind A: a near-layer quad that lies 30000 away, behind the bars (the classification must never allow this: the layered picture shows the A bar missing over it)
void BehindLayer() {
    Pass();
    Color(255, 0, 255, 255); Quad(5000, 0, 1500, 1500, -30000);
}
void SetupTarget() {
    ResetState();
    g.efbW = W; g.efbH = H;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (342u + W - 1) | ((342u + H - 1) << 12));
    BP(0x59, 171u | (171u << 10));
    BP(0x51, 0xFFFFFFu);  // the clear depth (far): a cleared EFB must not reject every later fragment
}
std::vector<uint8_t> Finish() { // the picture read back, the EFB cleared after it
    g.texCopySrc = EfbRect{0, 0, W, H};
    g.texCopyDstW = W;
    g.texCopyDstH = H;
    g.texCopyFmt = 6;
    QueueCopyTex(readDestination, true);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!backend::DebugReadTexture(id, pixels, w, h) || w != W || h != H) { pixels.clear(); Check(false, "picture read back"); }
    return pixels;
}
std::vector<uint8_t> Ref(const View& v, bool behind = false) {
    SetCamera(v);
    FarLayer();
    if (behind) BehindLayer();
    NearLayer();
    return Finish();
}
std::vector<uint8_t> Render(const View& v, uint32_t slot, bool behind = false) {
    BgSetView(slot, v.m, P00(), P11());
    SetCamera(v);
    BP(0xFF, 29);
    FarLayer();
    if (!std::getenv("TEST_NOEND")) BP(0xFF, 30 | (slot << 8));
    if (behind) BehindLayer();
    NearLayer();
    return Finish();
}
std::vector<uint8_t> Reuse(const View& v, uint32_t slot, bool behind = false) {
    BgSetView(slot, v.m, P00(), P11());
    SetCamera(v);
    BP(0xFF, 31 | (slot << 8));
    if (behind) BehindLayer();
    NearLayer();
    return Finish();
}
struct Diff { unsigned n = 0, big = 0, maxd = 0; double mean = 0; };
Diff Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, unsigned thr = 24) {
    Diff d;
    double sum = 0;
    for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
        unsigned m = 0;
        for (int k = 0; k < 4; ++k) { const int x = a[i + k] - b[i + k]; m = std::max<unsigned>(m, static_cast<unsigned>(x < 0 ? -x : x)); }
        if (m) ++d.n;
        if (m > thr) ++d.big;
        d.maxd = std::max(d.maxd, m);
        sum += m;
    }
    d.mean = sum / (a.size() / 4);
    return d;
}
void WritePPM(const char* name, const std::vector<uint8_t>& a, const std::vector<uint8_t>* b) { // the picture; or the error image (x4) against b
    const char* dir = std::getenv("BGLAYER_DUMP");
    if (!dir) return;
    char path[512];
    std::snprintf(path, sizeof path, "%s-%s.ppm", dir, name);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int y = H - 1; y >= 0; --y)  // (the read-back is bottom-up)
            for (int x = 0; x < W; ++x) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                for (int k = 0; k < 3; ++k) {
                    int v = a[i + k];
                    if (b) { v = std::abs(a[i + k] - (*b)[i + k]) * 4; if (v > 255) v = 255; }
                    std::fputc(v, f);
                }
            }
        std::fclose(f);
    }
}
}

int main() {
    const long mode = std::getenv("GXV_BGLAYER") ? std::atol(std::getenv("GXV_BGLAYER")) : 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    SetupTarget();
    Finish();  // (the first picture: the target is made and cleared)
    if (std::getenv("TEST_SEQ")) {
        const View v0 = MakeView(0, 0, 0, 0, 0);
        for (int i = 0; i < 4; ++i) {
            const auto r = Ref(v0);
            const size_t k = (64 * W + 64) * 4;
            std::printf("seq %d: center %02x%02x%02x%02x size %zu\n", i, r[k], r[k + 1], r[k + 2], r[k + 3], r.size());
        }
        return 0;
    }
    if (mode & 1) {
        const View v0 = MakeView(0, 0, 0, 0, 0);
        const auto ref0 = Ref(v0);
        const auto ren0 = Render(v0, 1);
        if (std::getenv("TEST_CACHE")) { std::vector<uint8_t> c; int cw=0,ch=0; const bool ok = backend::DebugReadTexture(2, c, cw, ch); const size_t i=(10*W+10)*4; std::printf("cache tex 2: ok %d %dx%d px(10,10) %02x%02x%02x%02x\n", ok, cw, ch, c.size()>i+3?c[i]:0, c.size()>i+3?c[i+1]:0, c.size()>i+3?c[i+2]:0, c.size()>i+3?c[i+3]:0); }
        const auto reu0 = Reuse(v0, 2);
        WritePPM("ref0", ref0, nullptr);
        if (std::getenv("TEST_CACHE")) for (int yy = 0; yy < 128; yy += 32) { for (int xx = 0; xx < 128; xx += 32) { const size_t i=(static_cast<size_t>(yy)*W+xx)*4; std::printf(" %02x%02x%02x/%02x%02x%02x", ref0[i],ref0[i+1],ref0[i+2],reu0[i],reu0[i+1],reu0[i+2]); } std::printf("\n"); }
        auto px = [&](const std::vector<uint8_t>& a, int x, int y) { const size_t i = (static_cast<size_t>(y) * W + x) * 4; return a.size() > i + 3 ? (a[i] << 24 | a[i + 1] << 16 | a[i + 2] << 8 | a[i + 3]) : 0; };
        std::printf("info: pixel (10,10) ref %08x render %08x reuse %08x; (64,64) ref %08x render %08x reuse %08x; sizes %zu %zu %zu\n", px(ref0, 10, 10), px(ren0, 10, 10), px(reu0, 10, 10),
                    px(ref0, 64, 64), px(ren0, 64, 64), px(reu0, 64, 64), ref0.size(), ren0.size(), reu0.size());
        Check(!ref0.empty() && ref0 == ren0, "render picture (BG_BEGIN .. BG_END) is bit-identical to the plain picture");
        Check(ref0 == reu0, "static camera: the reuse picture (backdrop from the cache, near layer on top) is bit-identical to the plain picture");
        uint64_t c[8];
        BgLayerCounters(c);
        Check(c[0] == 1 && c[1] == 1 && c[2] == 0 && c[4] == 1, "counters: 1 rendered, 1 reused, 0 missed, 1 identity reuse");
        const auto reu0b = Reuse(v0, 3);
        const auto ref0b = Ref(v0);
        Check(reu0b == ref0b && ref0b == ref0, "a second reuse, and a plain picture after the reuses, are identical to the first (the state after a reuse is the game's own)");
        const auto refBehind = Ref(v0, true);
        const auto renBehind = Render(v0, 4, true);
        const auto reuBehind = Reuse(v0, 5, true);
        const Diff dBehind = Compare(refBehind, reuBehind);
        std::printf("info: B behind A (breaks the classification's rule): the render picture differs in %u px from the plain one, the reuse picture in %u px (%u over 24/255)\n",
                    Compare(refBehind, renBehind).n, dBehind.n, dBehind.big);
        WritePPM("behind-ref", refBehind, nullptr);
        WritePPM("behind-err", reuBehind, &refBehind);
        std::printf("turned camera (the cache made for the view straight ahead; the plain picture drawn for the new view; f = %.0f px):\n", static_cast<double>(W * 0.5f * P00()));
        const float degs[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
        for (float dg : degs) {
            const float rad = dg * 3.14159265f / 180.0f;
            const View vt = MakeView(rad, rad * 0.5f, 0, 0, 0);
            Render(v0, 6);
            const auto refT = Ref(vt);
            Render(v0, 6);
            const auto reuT = Reuse(vt, 7);
            const Diff d = Compare(refT, reuT);
            const double px = std::tan(rad) * W * 0.5f * P00();
            std::printf("  turn %.2f deg (%.2f px at the centre): %u px differ, %u by over 24/255, max %u, mean %.3f\n", static_cast<double>(dg), px, d.n, d.big, d.maxd, d.mean);
            if (dg == 1.0f) { WritePPM("turn1-ref", refT, nullptr); WritePPM("turn1-err", reuT, &refT); }
            if (dg == 4.0f) WritePPM("turn4-err", reuT, &refT);
        }
        std::printf("moved camera (sideways), GXV_BGLAYER_D0=%s:\n", std::getenv("GXV_BGLAYER_D0") ? std::getenv("GXV_BGLAYER_D0") : "0");
        const float moves[] = {10.0f, 20.0f, 60.0f, 100.0f};
        for (float t : moves) {
            const View vm = MakeView(0, 0, t, 0, 0);
            Render(v0, 6);
            const auto refM = Ref(vm);
            Render(v0, 6);
            const auto reuM = Reuse(vm, 7);
            const Diff d = Compare(refM, reuM);
            std::printf("  move %.0f units: expected shift at D 20000 %.3f px, at D 8000 %.3f px: %u px differ, %u by over 24/255, max %u, mean %.3f\n", static_cast<double>(t),
                        static_cast<double>(t * W * 0.5f * P00() / 20000.0f), static_cast<double>(t * W * 0.5f * P00() / 8000.0f), d.n, d.big, d.maxd, d.mean);
            if (t == 100.0f) WritePPM("move100-err", reuM, &refM);
        }
        BgLayerCounters(c);
        Check(c[1] >= 1 && c[2] == 0, "no missed reuse in the run (every reuse had its cache)");
    }
    if (mode & 2) {
        g_log.clear();
        const View v0 = MakeView(0, 0, 0, 0, 0);
        for (int pic = 0; pic < 3; ++pic) {
            SetCamera(v0);
            BP(0xFF, 28 | (2u << 8)); FarLayer(); BP(0xFF, 28);   // 'Sky' (1 + buffer type 1): 5 quads
            BP(0xFF, 28 | (9u << 8)); NearLayer(); BP(0xFF, 28);  // 'MapObj' (1 + 8): 3 quads
            Pass(); Color(1, 2, 3, 255); Quad(0, 0, 10, 10, -50); // outside any category
            Finish();
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        std::printf("%s", g_log.c_str());
        Check(g_log.find("bglayer-fe: ") != std::string::npos, "the census line is logged");
        Check(g_log.find("Sky 5.0/") != std::string::npos, "the 5 draws under the Sky marker are counted under Sky (and as background by nature)");
        Check(g_log.find("MapObj 3.0/") != std::string::npos, "the 3 draws under the MapObj marker are counted under MapObj");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
