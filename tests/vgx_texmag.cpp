// GXV_TEXMAG_CENSUS on the host (tools/test_vgx_texmag.sh): a quad of known size on the screen with a texture of known size and UV scale,
// through the real front end, must give the analytic magnification (within a few percent): at several distances, with a texture matrix
// scale, clipped by the picture's edge and by the near plane (against a brute-force pixel count), with the vertices transformed on the GPU
// (GXV_SOFTXFORM=0: the draw's own model-view block) and on the CPU, minified (no magnified bin, the mip level), culled by facing.
//   vgx_texmag                      (GXV_SOFTXFORM=0 / 32 chosen by the script; GXV_SPLIT likewise)
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_workload.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <time.h>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char* fmt, ...) {
    if (!std::getenv("TM_LOG")) return;
    va_list ap; va_start(ap, fmt); std::vfprintf(stderr, fmt, ap); va_end(ap); std::fputc('\n', stderr);
}
extern "C" void port_log(const char*, ...) {}
extern "C" uint32_t port_scene_key(void) { return 0x1234ABCDu; }
namespace vgx { bool LogEnabled() { return false; } void SubmitToBackend(CmdBuf& b) { backend::Execute(b); } }

namespace {
constexpr uint32_t kRam = 8u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
uint32_t Alloc(uint32_t n) { const uint32_t a = g_brk; g_brk += (n + 31u) & ~31u; return a; }
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0; address &= 0x3FFFFFFFu;
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address; return g_ram + address;
}
uint32_t F2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
struct Bytes : std::vector<uint8_t> {
    void U8(uint32_t v) { push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { U8(v >> 8); U8(v); }
    void U32(uint32_t v) { U16(v >> 16); U16(v); }
    void F(float f) { U32(F2u(f)); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61); U32(reg << 24 | (value & 0xFFFFFF)); }
    void Masked(uint32_t reg, uint32_t mask, uint32_t value) { BP(0xFE, mask); BP(reg, value); }
    void CP(uint32_t reg, uint32_t value) { U8(0x08); U8(reg); U32(value); }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) { U8(0x10); U16(static_cast<uint32_t>(w.size() - 1)); U16(addr); for (uint32_t x : w) U32(x); }
};

struct Tex { uint32_t addr, w, h; uint64_t key; };
Tex MakeTex(uint32_t w, uint32_t h, uint32_t seed) {
    Tex t{Alloc(w * h), w, h, 0};
    for (uint32_t i = 0; i < w * h; ++i) g_ram[t.addr + i] = static_cast<uint8_t>((i * 2654435761u + seed * 97u) >> 13);
    vgx::hle::NotifyGuestWrite(g_ram + t.addr, w * h);
    t.key = XXH3_64bits_withSeed(g_ram + t.addr, w * h, 1u | (w << 8) | (h << 20)); // I8
    return t;
}
// the whole state of a draw: viewport 640x456, scissor, projection (perspective 1.2 / 1.6), model-view, texture, cull, texture matrix scale
struct Setup { float fx = 1.2f, fy = 1.6f; float mvz = 0, mvx = 0; float sS = 1, sT = 1; int cull = 0; bool tmtx = false; float near_ = 0; };
void Material(Bytes& b, const Tex& t, const Setup& s) {
    b.BP(0x94, t.addr >> 5);
    b.BP(0x88, (t.w - 1) | ((t.h - 1) << 10) | (1u << 20));
    b.BP(0x80, 0x000195u); b.BP(0x84, 0);
    b.BP(0x28, 0x049040u);
    b.Masked(0x30, 0x03FFFF, t.w - 1); b.Masked(0x31, 0x03FFFF, t.h - 1);
    for (int r = 0; r < 3; ++r) { b.BP(0xE2 + 2 * r, 0); b.BP(0xE3 + 2 * r, 0); }
    b.BP(0xC0, 0xFFF | (8u << 12) | (1u << 19)); // d = TEXC
    b.BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (1u << 19)); // d = TEXA
    b.BP(0x10, 0);
    for (int i = 0; i < 8; ++i) b.BP(0xF6 + i, 0xE338C4u);
    b.BP(0x27, 0xFFFFC9); b.BP(0x0F, 2);
    for (int i = 0; i < 5; ++i) b.BP(0xEE + i, i == 0 ? 0 : i == 1 ? 0x40000Fu : i == 4 ? 0xFFFFFFu : i == 2 ? 1u : 0u);
    b.BP(0xE8, 0x156); b.BP(0xF3, 0x770000u);
    b.BP(0x41, 0x0034B8u); b.BP(0x40, 0x07u); b.Masked(0x43, 0x000040, 0x40);
    // GenMode: texgens 1, chans 0, stages 1, cull
    b.Masked(0x00, 0x00FFFF, 1u | (0u << 4) | (0u << 10) | (static_cast<uint32_t>(s.cull) << 14));
    b.XF(0x1040, {0x5280}); b.XF(0x1050, {0x3D}); b.XF(0x103F, {1}); b.XF(0x1009, {0});
    b.XF(0x1008, {1u | (1u << 4)});
    // tex matrix 30 (XF 0x78) when used, else the identity matrix index 60 is as the fixture's: load our own at 30 always
    b.XF(0x78, {F2u(s.sS), 0, 0, 0, 0, F2u(s.sT), 0, 0});
    b.XF(0x1018, {0u | (30u << 6)});
    // viewport 640x456 at 0,0; scissor; offsets
    b.BP(0x59, 171u | (171u << 10));
    b.BP(0x20, (342u << 12) | 342u); b.BP(0x21, (981u << 12) | 797u);
    b.XF(0x101A, {F2u(320), F2u(-228), F2u(16777215), F2u(662), F2u(570), F2u(16777215)});
    b.XF(0x1020, {F2u(s.fx), 0, F2u(s.fy), 0, F2u(-0.0101f), F2u(-1.0101f), 0});
    b.CP(0x50, 1u << 9); b.CP(0x60, 1u);
    b.CP(0x70, 1u | (4u << 1) | (1u << 21) | (4u << 22));
    b.CP(0x80, 0); b.CP(0x90, 0);
}
// a quad centred at (cx, cy) with half sizes hx, hy at distance D, texture coordinates 0..us x 0..vs; `cw`: clockwise order
void Quad(Bytes& b, float cx, float cy, float D, float hx, float hy, float us, float vs, bool flipOrder, float tilt = 0) {
    float x[4] = {-hx, hx, hx, -hx}, y[4] = {-hy, -hy, hy, hy}, u[4] = {0, us, us, 0}, v[4] = {vs, vs, 0, 0};
    int order[4] = {0, 1, 2, 3};
    if (flipOrder) { order[1] = 3; order[3] = 1; }
    b.U8(0x80); b.U16(4);
    for (int k = 0; k < 4; ++k) {
        const int i = order[k];
        b.F(cx + x[i]); b.F(cy + y[i]); b.F(-D - tilt * y[i]); b.F(u[i]); b.F(v[i]);
    }
}
// position matrix 0: identity
void Ident(Bytes& b) { b.XF(0x0, {F2u(1), 0, 0, 0, 0, F2u(1), 0, 0, 0, 0, F2u(1), 0}); }

struct Rec { double px[5], mini[3], meanMag, meanLod, maxMag, minMag; unsigned pics; bool found = false; };
std::map<uint64_t, Rec> Parse(const std::string& path) {
    std::map<uint64_t, Rec> m;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return m;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !std::strchr(line, '\t')) continue;
        unsigned long long key; unsigned w, h, pl, pics; char fmt[16]; double a[5], b[3], mm, ml, mlm, mx, mn;
        if (std::sscanf(line, "%llx\t%u\t%u\t%15s\t%u\t%u\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf", &key, &w, &h, fmt, &pl, &pics, &a[0], &a[1], &a[2], &a[3], &a[4], &b[0], &b[1], &b[2], &mm, &ml, &mlm, &mx, &mn) >= 19) {
            Rec r{}; std::memcpy(r.px, a, sizeof a); std::memcpy(r.mini, b, sizeof b); r.meanMag = mm; r.meanLod = ml; r.maxMag = mx; r.minMag = mn; r.pics = pics; r.found = true;
            m[key] = r;
        }
    }
    std::fclose(f);
    return m;
}
int g_fail = 0, g_n = 0;
void Check(bool ok, const char* what, double got, double want) {
    ++g_n;
    std::printf("%s %-62s got %12.4f want %12.4f%s\n", ok ? "ok  " : "FAIL", what, got, want, ok ? "" : "  <<<");
    if (!ok) ++g_fail;
}
void NearTo(const char* what, double got, double want, double tol) { Check(std::fabs(got - want) <= tol * std::fabs(want), what, got, want); }

vgx::EfbRect g_disp{0, 0, 640, 456};
void Picture(Bytes& f) {
    size_t rd = 0;
    while (rd < f.size()) {
        const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
        if (!n) { std::fprintf(stderr, "stream stuck\n"); std::exit(2); }
        rd += n;
    }
    vgx::g.dispCopySrc = g_disp;
    vgx::QueueCopyDisp(true);
    vgx::FrontPresent(false, nullptr);
}
void Sink(vgx::CmdBuf& b) {
    if (std::getenv("TM_DUMP"))
        for (const vgx::DrawRec& d : b.draws) {
            std::fprintf(stderr, "draw vp %g %g %g %g sc %d %d %d %d cull %d vtx %u idx %u lm %x ub %u %u\n", d.rs.vpX, d.rs.vpY, d.rs.vpW, d.rs.vpH, d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH, d.rs.cull, d.vtxCount, d.idxCount, d.layoutMask, d.ublock[0], d.ublock[1]);
            const float* P = b.uni.data() + d.ublock[0];
            std::fprintf(stderr, " P:"); for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %g", P[i]);
            const float* v = reinterpret_cast<const float*>(b.vtx.data());
            std::fprintf(stderr, "\n v0: %g %g %g %g\n", v[0], v[1], v[2], v[3]);
        }
    vgx::backend::Execute(b);
}
} // namespace

int main() {
    setenv("GXV_TEXMAG_CENSUS", "1", 1);
    setenv("GXV_TEXMAG_DIR", "./", 0);
    setenv("GXV_TEXMAG_TRIS", "64", 1);
    setenv("GXV_TEXMAG_MINSIDE", "8", 1);
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{}; hooks.resolve = Resolve; vgx::SetGuestHooks(hooks);
    vgx::ResetState(); vgx::SetFrameSink(&Sink);
    vgx::g.efbW = 640; vgx::g.efbH = 456; vgx::QueueSetTarget();
    const double sx = 960.0 / 640, sy = 544.0 / 456, vpW = 640, vpH = 456, fx = 1.2, fy = 1.6;
    auto analytic = [&](double hx, double hy, double D, double texW, double texH, double us, double vs, double sS = 1, double sT = 1) {
        const double pxw = vpW * fx * (2 * hx) / (2 * D), pxh = vpH * fy * (2 * hy) / (2 * D);
        const double areaPx = pxw * pxh * sx * sy, areaTex = (texW * us * sS) * (texH * vs * sT);
        return std::sqrt(areaPx / areaTex);
    };
    struct Case { const char* name; uint32_t w, h; float D, hx, hy, us, vs; Setup s; bool flip; double wantM; double wantPx; };
    std::vector<Case> cases;
    uint32_t seed = 1;
    auto add = [&](const char* name, uint32_t w, uint32_t h, float D, float hx, float hy, float us, float vs, Setup s, bool flip, double wantPx = -1) {
        Case c{name, w, h, D, hx, hy, us, vs, s, flip, analytic(hx, hy, D, w, h, us, vs, s.sS, s.sT), wantPx};
        cases.push_back(c);
    };
    Setup base;
    add("D=10 128x128 uv 0..1", 128, 128, 10, 2, 2, 1, 1, base, false);
    add("D=20 128x128 uv 0..1", 128, 128, 20, 2, 2, 1, 1, base, false);
    add("D=40 128x128 uv 0..1", 128, 128, 40, 2, 2, 1, 1, base, false);
    add("D=10 64x32 uv 0..3 x 0..2 (a non-square quad)", 64, 32, 10, 3, 1.5f, 3, 2, base, false);
    Setup tm = base; tm.sS = 2.0f; tm.sT = 0.5f;
    add("D=10 128x128 texture matrix scale 2 x 0.5", 128, 128, 10, 2, 2, 1, 1, tm, false);
    add("D=10 256x256 clipped by the picture's edges (quad 6x6 units)", 256, 256, 10, 30, 30, 1, 1, base, false, 960.0 * 544.0);
    add("D=6 minified: 256x256 over 0.3x0.3 units", 256, 256, 60, 3, 3, 1, 1, base, false);
    Setup cb = base; cb.cull = 2;
    add("cull mode 2, one winding", 128, 128, 10, 2, 2, 1, 1, cb, false);
    add("cull mode 2, the other winding", 128, 128, 10, 2, 2, 1, 1, cb, true);
    std::vector<Tex> texs;
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        Tex t = MakeTex(c.w, c.h, seed++);
        texs.push_back(t);
        Bytes f;
        Ident(f);
        Material(f, t, c.s);
        Quad(f, 0, 0, c.D, c.hx, c.hy, c.us, c.vs, c.flip);
        Picture(f);
    }
    // the near plane and a tilted quad: a floor seen from above, partly behind the eye; compared with a pixel count
    Setup fl = base;
    const float tilts[2] = {1.6f, 0.5f};
    Tex fts[2] = {MakeTex(128, 128, 99), MakeTex(128, 128, 98)};
    for (int fi = 0; fi < 2; ++fi) {
        Tex& ft = fts[fi];
        Bytes f; Ident(f); Material(f, ft, fl);
        // a quad in the plane z = -D0 - tilt*y: y from -hy..hy, so that its lower edge (y < 0) goes behind the eye when tilt is large
        Quad(f, 0, 0, 8, 6, 6, 4, 4, false, tilts[fi]); // z = -8 - tilt y: with 1.6, y = -6 is at z = +1.6: behind the eye
        Picture(f);
    }
    vgx::TexMagSceneEnd();
    for (int i = 0; i < 200; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); if (std::fopen("./texmag-1234abcd.txt", "r")) break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto recs = Parse("./texmag-1234abcd.txt");
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        const auto it = recs.find(texs[i].key);
        std::printf("-- %s\n", c.name);
        if (it == recs.end()) {
            const bool culledCase = std::strstr(c.name, "other winding") != nullptr;
            Check(culledCase, "texture absent from the census (only the culled one may be)", 0, culledCase);
            continue;
        }
        const Rec& r = it->second;
        if (std::strstr(c.name, "one winding") != nullptr || std::strstr(c.name, "other winding") != nullptr) {
            Check(true, "this winding is drawn with the cull mode", r.px[0], 1);
            continue;
        }
        NearTo("mean magnification", r.meanMag, c.wantM, 0.03);
        NearTo("mean lod = -log2(m)", -r.meanLod, std::log2(c.wantM), 0.03 * std::max(1.0, std::fabs(std::log2(c.wantM))) + 0.03);
        if (c.wantPx > 0) NearTo("visible display pixels", r.px[0], c.wantPx, 0.02);
        else {
            const double wpx = vpW * fx * (2 * c.hx) / (2 * c.D) * sx * vpH * fy * (2 * c.hy) / (2 * c.D) * sy;
            NearTo("display pixels of the quad", r.px[0], wpx, 0.02);
        }
        if (c.wantM < 1.0) Check(r.px[1] == 0 && r.px[2] == 0 && r.px[4] == 0, "minified: no pixel in any magnified bin", r.px[1] + r.px[2] + r.px[4], 0);
        if (c.wantM >= 1.0) Check(r.px[1] > 0.98 * r.px[0], "magnified: its pixels are in the >= 1 bin", r.px[1], r.px[0]);
        if (c.wantM >= 1.5 && c.wantM < 2.0) Check(r.px[2] > 0.98 * r.px[0] && r.px[3] == 0, ">= 1.5 bin only", r.px[2], r.px[0]);
    }
    for (int fi = 0; fi < 2; ++fi) {
        const Tex& ft = fts[fi];
        const double TILT = tilts[fi];
        // brute force for the floor: every display pixel's ray against the plane z = -8 - 1.6 y; inside the quad, in front of the eye
        const auto it = recs.find(ft.key);
        std::printf("-- floor, tilt %.1f%s, vs a pixel count\n", TILT, TILT > 1 ? " (crosses the near plane)" : "");
        Check(it != recs.end(), "found", it != recs.end(), 1);
        if (it != recs.end()) {
            double cnt = 0, texArea = 0, sumM = 0;
            const int W = 960, H = 544;
            auto uvAt = [&](double X, double Y, double& u, double& v, bool& ok) {
                // display pixel -> EFB pixel -> NDC -> ray (x = ndcx * (-z) / fx..., y likewise) ; plane z = -8 - 1.6 y with eye y = -ndcy*(-z)/fy ...
                const double ex = X / sx, ey = Y / sy;
                const double nx = ex / 320.0 - 1.0, ny = ey / 228.0 - 1.0; // (Y grows downwards: the same sign as the census's y)
                // eye-space point at depth d = -z (> 0): x = nx * d / fx, y = ny * d / fy
                // (u_proj flips Y: y = -ny * d / fy) plane: d = 8 + 1.6 y = 8 - 1.6 ny d / fy  =>  d (1 + 1.6 ny / fy) = 8
                const double den = 1.0 + TILT * ny / fy;
                ok = false;
                if (std::fabs(den) < 1e-9) return;
                const double d = 8.0 / den;
                if (d <= 0) return;
                const double x = nx * d / fx, y = -ny * d / fy;
                if (std::fabs(x) > 6 || std::fabs(y) > 6) return;
                u = (x + 6) / 12 * 4; v = (6 - y) / 12 * 4; // (uv as Quad makes them: u = 0 at x = -hx, v = vs at y = -hy)
                ok = true;
            };
            // the sign of y: Quad's v = vs at y = -hy; only |area| matters for the check below
            for (int Y = 0; Y < H; ++Y)
                for (int X = 0; X < W; ++X) {
                    double u, v, u2, v2, u3, v3; bool ok, ok2, ok3;
                    uvAt(X + 0.5, Y + 0.5, u, v, ok);
                    if (!ok) continue;
                    const double e = 0.02;
                    uvAt(X + 0.5 + e, Y + 0.5, u2, v2, ok2);
                    uvAt(X + 0.5, Y + 0.5 + e, u3, v3, ok3);
                    if (!ok2 || !ok3) { cnt += 1; continue; }
                    u2 = (u2 - u) / e; v2 = (v2 - v) / e; u3 = (u3 - u) / e; v3 = (v3 - v) / e; u = v = 0;
                    cnt += 1;
                    const double ta1 = std::fabs(u2 * v3 - u3 * v2) * 128.0 * 128.0;
                    texArea += ta1;
                    if (ta1 > 1e-12) sumM += 1.0 / std::sqrt(ta1); // this pixel's magnification (display px per texel, linear)
                }
            // the same by integrating over the plane (no per-pixel differences): visible plane area x (4 / 12)^2 uv per unit^2
            double plane = 0;
            const double st = 0.004;
            for (double x = -6 + st / 2; x < 6; x += st)
                for (double y = -6 + st / 2; y < 6; y += st) {
                    const double d = 8 + TILT * y;
                    if (d <= 0) continue;
                    if (std::fabs(fx * x / d) <= 1 && std::fabs(fy * y / d) <= 1) plane += st * st;
                }
            const double texArea2 = plane * (4.0 / 12) * (4.0 / 12) * 128.0 * 128.0;
            std::printf("   (pixel-difference texel area %.0f, plane integral %.0f)\n", texArea, texArea2);
            texArea = texArea2;
            const double want = std::sqrt(cnt / texArea);
            NearTo("visible display pixels (near-plane clipped)", it->second.px[0], cnt, 0.03);
            // the census's mean is the display-area-weighted mean of the per-piece magnification (the quantity a pixel sees on average),
            // not sqrt(total pixels / total texels): the brute force computes both, and the census must match the first
            std::printf("   (sqrt of the area ratio %.4f, per-pixel mean %.4f, census mean %.4f)\n", want, sumM / cnt, it->second.meanMag);
            NearTo("mean magnification (per-pixel mean)", it->second.meanMag, sumM / cnt, 0.06);
        }
    }
    if (const char* bn = std::getenv("TM_BENCH")) {
        // cost: TM_BENCH=<draws per picture> small quads (about 40 x 45 display pixels: no subdivision) over 40 pictures, census 1 in 1; the census's own clock
        // (census_ms_per_sampled_picture) and the file's triangle counts. TM_BENCH_BIG=<n>: n quads of 300 x 300 display pixels added (they are subdivided).
        const int nd = std::atoi(bn), nbig = std::getenv("TM_BENCH_BIG") ? std::atoi(std::getenv("TM_BENCH_BIG")) : 0;
        Tex bt = MakeTex(64, 64, 777), bt2 = MakeTex(64, 64, 778);
        Setup bs;
        for (int pic = 0; pic < 40; ++pic) {
            Bytes f; Ident(f); Material(f, bt, bs);
            for (int d = 0; d < nd; ++d) { Material(f, (d & 1) ? bt2 : bt, bs); Quad(f, -3.0f + 0.01f * (d % 600), -2.0f + 0.01f * (d / 600), 10, 0.5f, 0.5f, 1, 1, false); }
            for (int d = 0; d < nbig; ++d) { Material(f, (d & 1) ? bt2 : bt, bs); Quad(f, 0, 0, 10, 3, 3, 1, 1, false); }
            Picture(f);
        }
        vgx::TexMagSceneEnd();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::FILE* fp = std::fopen("./texmag-1234abcd.txt", "r");
        char l[1024];
        while (fp && std::fgets(l, sizeof l, fp))
            if (!std::strncmp(l, "census_ms", 9) || !std::strncmp(l, "triangles_sampled", 17) || !std::strncmp(l, "draws=", 6) || !std::strncmp(l, "pictures_sampled", 16)) std::printf("bench %s", l);
        if (fp) std::fclose(fp);
    }
    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
