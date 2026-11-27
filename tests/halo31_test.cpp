// halo31 (18 Nov 2026): GXV_HRW_HALO's rules (platform/include/port/halo31.h) on a ray-cast scene, against a TRUE redraw of the pair's second
// picture, next to the c181 player box, the depth-keyed box (GXV_HRW_BOXDEPTH, port/keepdepth.h) and no box at all. The scene warp is the
// back end's (the rotation homography M = P V_A V_B^-1 P^-1, the ground plane of port/hrw36.h), the box is the back end's (the locked plane,
// ShapedBox, the feather), the fresh player is the true picture's player pixels.
//   halo31_test [out-dir]      PPMs of every scenario into out-dir (A, truth, each method, the fill painted solid, x3 crops)
// The scene: textured ground, a brick wall and pillars behind the player, a starry sky; a player of spheres whose limbs swing (his pose changes
// between the two pictures); a follow camera. Scenarios: run (the camera follows), jump (it does not follow his rise), pan (it turns about
// him), air (no ground under him: rotation only), post (a pillar in front of him), far (a warp to another place: the plan must refuse).
// Error: against the true second picture, inside his box grown by 48 px: the mean absolute difference (levels) and the pixels more than 24
// levels off. Built with -DHALO31_MUTANT=n the checks must FAIL (tools/test_halo31.sh).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "port/halo31.h"
#include "port/hrw36.h"
#include "port/keepdepth.h"

namespace {
constexpr int W = 960, H = 544;
struct V3 { double x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) { const double l = std::sqrt(dot(a, a)); return a * (1.0 / l); }

// 4x4 column major (m[c * 4 + r]), as the back end's
void Mul4(const double* a, const double* b, double* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) { double s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; }
}
bool Inv4(const double* m, double* inv) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { a[r][c] = m[c * 4 + r]; a[r][4 + c] = r == c ? 1.0 : 0.0; }
    for (int i = 0; i < 4; ++i) {
        int piv = i;
        for (int r = i + 1; r < 4; ++r) if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        if (!(std::fabs(a[piv][i]) > 1e-30)) return false;
        if (piv != i) for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[piv][c]);
        const double d = 1.0 / a[i][i];
        for (int c = 0; c < 8; ++c) a[i][c] *= d;
        for (int r = 0; r < 4; ++r) { if (r == i) continue; const double f = a[r][i]; if (f != 0.0) for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) inv[c * 4 + r] = a[r][4 + c];
    return true;
}

struct Cam {
    V3 eye, right, up, back;  // (the view looks down -back)
    double V[16];
    void Look(V3 e, V3 at) {
        eye = e;
        back = norm(e - at);
        right = norm(cross({0, 1, 0}, back));
        up = cross(back, right);
        const V3 r[3] = {right, up, back};
        for (int i = 0; i < 3; ++i) { V[0 + i] = r[i].x; V[4 + i] = r[i].y; V[8 + i] = r[i].z; V[12 + i] = -dot(r[i], e); }
        V[3] = V[7] = V[11] = 0; V[15] = 1;
    }
    V3 ToView(V3 p) const { const V3 d = p - eye; return {dot(right, d), dot(up, d), dot(back, d)}; }
};
double g_P[16];
float g_Pf[16];
void MakeProjection() {
    const double f = 1.0 / std::tan(45.0 * 0.5 * M_PI / 180.0), aspect = static_cast<double>(W) / H, zn = 30.0, zf = 60000.0;
    std::memset(g_P, 0, sizeof g_P);
    g_P[0] = f / aspect; g_P[5] = f; g_P[10] = -(zf + zn) / (zf - zn); g_P[11] = -1.0; g_P[14] = -2.0 * zf * zn / (zf - zn);
    for (int i = 0; i < 16; ++i) g_Pf[i] = static_cast<float>(g_P[i]);
}
// view space -> the picture's pixels (y down the picture as the EFB holds it: row 0 is ndc y = -1, as the back end's proj())
void Project(V3 v, double& x, double& y) {
    const double cw = -v.z;
    x = (g_P[0] * v.x / cw * 0.5 + 0.5) * W;
    y = (g_P[5] * v.y / cw * 0.5 + 0.5) * H;
}

uint32_t Hash(int x, int y, int s) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(s) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
struct Rgb { double r, g, b; };
struct Player {
    V3 pos;        // his feet
    double phase;  // the run cycle
    bool on = true;
};
struct Sphere { V3 c; double r; Rgb col; };
int PlayerSpheres(const Player& p, Sphere* s) {
    const double sw = std::sin(p.phase), cw = std::cos(p.phase);
    int n = 0;
    s[n++] = {p.pos + V3{0, 78, 0}, 44, {225, 30, 30}};                       // body
    s[n++] = {p.pos + V3{0, 138, 0}, 33, {250, 200, 160}};                    // head
    s[n++] = {p.pos + V3{0, 162, 0}, 26, {225, 30, 30}};                      // cap
    s[n++] = {p.pos + V3{28 * sw, 22 + 8 * std::max(0.0, cw), 16}, 21, {40, 60, 210}};    // legs
    s[n++] = {p.pos + V3{-28 * sw, 22 + 8 * std::max(0.0, -cw), -16}, 21, {40, 60, 210}};
    s[n++] = {p.pos + V3{-34 * sw, 86 + 10 * cw, 50}, 16, {250, 250, 250}};   // hands
    s[n++] = {p.pos + V3{34 * sw, 86 - 10 * cw, -50}, 16, {250, 250, 250}};
    return n;
}
struct Scene { bool post = false; };
struct Hit { Rgb col; double dist; bool player; };  // dist: the view depth (-z), 1e9 = the sky

// One ray (world space). The depth returned is along the camera's axis.
Hit Trace(const Cam& cam, V3 dir, const Player& pl, const Scene& sc) {
    Hit h{{0, 0, 0}, 1e9, false};
    double best = 1e18;
    const V3 o = cam.eye;
    const double axis = -dot(dir, cam.back);  // (view depth of a point t along the ray = t * axis)
    {   // the sky: a gradient and stars (at infinity)
        const double e = std::asin(std::clamp(dir.y, -1.0, 1.0)), az = std::atan2(dir.x, -dir.z);
        const int cx = static_cast<int>(std::floor(az * 220.0)), cy = static_cast<int>(std::floor(e * 220.0));
        const bool star = Hash(cx, cy, 7) % 23u == 0;
        const double t = std::clamp(0.5 + dir.y * 1.5, 0.0, 1.0);
        h.col = star ? Rgb{255, 255, 220} : Rgb{20 + 40 * t, 24 + 50 * t, 70 + 90 * t};
    }
    if (dir.y < -1e-6) {  // the ground y = 0, to 9000 units
        const double t = -o.y / dir.y;
        const V3 p = o + dir * t;
        if (t < best && std::hypot(p.x, p.z + 1000.0) < 9000.0) {
            best = t;
            const int cx = static_cast<int>(std::floor(p.x / 90.0)), cz = static_cast<int>(std::floor(p.z / 90.0));
            const uint32_t k = Hash(cx, cz, 1);
            const bool dark = ((cx + cz) & 1) != 0;
            const double fx = p.x / 90.0 - cx, fz = p.z / 90.0 - cz;
            const bool line = fx < 0.07 || fz < 0.07;
            const double v = (dark ? 70.0 : 150.0) + static_cast<double>(k % 50u);
            h.col = line ? Rgb{250, 240, 120} : Rgb{v * 0.55, v, v * 0.45};
            h.dist = t * axis;
        }
    }
    if (dir.z < -1e-6) {  // the wall z = -900, 0 .. 520 high
        const double t = (-900.0 - o.z) / dir.z;
        const V3 p = o + dir * t;
        if (t < best && t > 0 && p.y >= 0.0 && p.y <= 520.0) {
            best = t;
            const int row = static_cast<int>(std::floor(p.y / 40.0));
            const double bx = p.x / 100.0 + (row & 1 ? 0.5 : 0.0);
            const int cx = static_cast<int>(std::floor(bx));
            const bool mortar = bx - cx < 0.06 || p.y / 40.0 - row < 0.12;
            const double v = 120.0 + static_cast<double>(Hash(cx, row, 3) % 90u);
            h.col = mortar ? Rgb{235, 235, 225} : Rgb{v, v * 0.45, v * 0.3};
            h.dist = t * axis;
        }
    }
    {   // pillars (vertical cylinders) behind him, and with sc.post one in front of him
        const double px[4] = {-520.0, 260.0, 820.0, 60.0}, pz[4] = {-420.0, -560.0, -300.0, 330.0}, pr[4] = {45.0, 60.0, 40.0, 14.0};
        for (int i = 0; i < (sc.post ? 4 : 3); ++i) {
            const double ox = o.x - px[i], oz = o.z - pz[i], a = dir.x * dir.x + dir.z * dir.z, b = 2.0 * (ox * dir.x + oz * dir.z), c = ox * ox + oz * oz - pr[i] * pr[i];
            const double disc = b * b - 4.0 * a * c;
            if (a < 1e-12 || disc < 0.0) continue;
            const double t = (-b - std::sqrt(disc)) / (2.0 * a);
            const V3 p = o + dir * t;
            if (t > 0 && t < best && p.y >= 0.0 && p.y <= (i == 3 ? 260.0 : 420.0)) {
                best = t;
                const bool band = static_cast<int>(std::floor(p.y / 30.0)) & 1;
                h.col = i == 3 ? (band ? Rgb{240, 240, 240} : Rgb{60, 60, 60}) : (band ? Rgb{200, 170, 60} : Rgb{90, 70, 160});
                h.dist = t * axis;
            }
        }
    }
    if (pl.on) {
        Sphere s[8];
        const int n = PlayerSpheres(pl, s);
        for (int i = 0; i < n; ++i) {
            const V3 oc = o - s[i].c;
            const double b = dot(oc, dir), c = dot(oc, oc) - s[i].r * s[i].r, disc = b * b - c;
            if (disc < 0.0) continue;
            const double t = -b - std::sqrt(disc);
            if (t > 0 && t < best) {
                best = t;
                const V3 nrm = norm(o + dir * t - s[i].c);
                const double l = 0.55 + 0.45 * std::max(0.0, nrm.y * 0.7 + nrm.z * 0.5 + nrm.x * 0.3);
                h.col = {s[i].col.r * l, s[i].col.g * l, s[i].col.b * l};
                h.dist = t * axis;
                h.player = true;
            }
        }
    }
    return h;
}
struct Image {
    std::vector<uint8_t> rgb;      // W * H * 3
    std::vector<float> wdepth;     // the window depth, 0..1
    std::vector<uint8_t> player;
    Image() : rgb(static_cast<size_t>(W) * H * 3), wdepth(static_cast<size_t>(W) * H), player(static_cast<size_t>(W) * H) {}
};
void Render(const Cam& cam, const Player& pl, const Scene& sc, Image& out) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double nx = (x + 0.5) / W * 2.0 - 1.0, ny = (y + 0.5) / H * 2.0 - 1.0;
            const V3 dv{nx / g_P[0], ny / g_P[5], -1.0};
            const V3 dir = norm(cam.right * dv.x + cam.up * dv.y - cam.back);
            const Hit h = Trace(cam, dir, pl, sc);
            const size_t i = static_cast<size_t>(y) * W + x;
            out.rgb[i * 3] = static_cast<uint8_t>(std::clamp(h.col.r, 0.0, 255.0));
            out.rgb[i * 3 + 1] = static_cast<uint8_t>(std::clamp(h.col.g, 0.0, 255.0));
            out.rgb[i * 3 + 2] = static_cast<uint8_t>(std::clamp(h.col.b, 0.0, 255.0));
            out.wdepth[i] = static_cast<float>(h.dist >= 1e8 ? 1.0 : std::clamp(0.5 * (g_P[14] / h.dist - g_P[10]) + 0.5, 0.0, 1.0));
            out.player[i] = h.player ? 1 : 0;
        }
}
// bilinear, clamped to the picture (the snapshot's CLAMP_TO_EDGE); sx, sy in pixels
void Sample(const Image& a, double sx, double sy, double out[3]) {
    const double fx = std::clamp(sx - 0.5, 0.0, W - 1.0), fy = std::clamp(sy - 0.5, 0.0, H - 1.0);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
    const double ax = fx - x0, ay = fy - y0;
    for (int c = 0; c < 3; ++c) {
        const double t = a.rgb[(static_cast<size_t>(y0) * W + x0) * 3 + c] * (1 - ax) + a.rgb[(static_cast<size_t>(y0) * W + x1) * 3 + c] * ax;
        const double b = a.rgb[(static_cast<size_t>(y1) * W + x0) * 3 + c] * (1 - ax) + a.rgb[(static_cast<size_t>(y1) * W + x1) * 3 + c] * ax;
        out[c] = t * (1 - ay) + b * ay;
    }
}
double DepthAt(const Image& a, double sx, double sy) {  // (a point read)
    const int x = std::clamp(static_cast<int>(std::floor(sx)), 0, W - 1), y = std::clamp(static_cast<int>(std::floor(sy)), 0, H - 1);
    return a.wdepth[static_cast<size_t>(y) * W + x];
}
void WritePpm(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fprintf(f, "P6\n%d %d\n255\n", w, h); std::fwrite(rgb.data(), 1, rgb.size(), f); std::fclose(f); }
}
void WriteCrop(const std::string& path, const std::vector<uint8_t>& rgb, int cx, int cy) {  // (240 x 200 round the player, three times the size)
    const int cw = 240, ch = 200, k = 3, x0 = std::clamp(cx - cw / 2, 0, W - cw), y0 = std::clamp(cy - ch / 2, 0, H - ch);
    std::vector<uint8_t> o(static_cast<size_t>(cw) * k * ch * k * 3);
    for (int y = 0; y < ch * k; ++y)
        for (int x = 0; x < cw * k; ++x)
            std::memcpy(&o[(static_cast<size_t>(y) * cw * k + x) * 3], &rgb[(static_cast<size_t>(y0 + y / k) * W + x0 + x / k) * 3], 3);
    WritePpm(path, o, cw * k, ch * k);
}
// (the pictures are held with row 0 = ndc y -1, the EFB's order as the back end's pixel maths has it; on the way to a file the rows are turned over)
std::vector<uint8_t> Upright(const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> o(rgb.size());
    for (int y = 0; y < H; ++y) std::memcpy(&o[static_cast<size_t>(H - 1 - y) * W * 3], &rgb[static_cast<size_t>(y) * W * 3], static_cast<size_t>(W) * 3);
    return o;
}

struct Scenario {
    const char* name;
    V3 move;           // the player's move between the two turns (units)
    double follow;     // the share of his sideways move the camera makes too
    double yawDeg;     // the camera's turn about him
    bool ground;       // he has ground under him (the plane is used)
    bool post;         // a post in front of him
    double phaseStep;  // the run cycle's advance
    bool mustRefuse;
};
struct Result { double mae[5]; int wrong[5]; int changed[5]; int ghost[5]; int dragged[5]; double dragPx[5]; int n; port::halo31::Plan plan; int boxPx; };
enum { kBox = 0, kBoxDepth, kNoBox, kHalo, kHaloNoDilate };
const char* const kMethod[5] = {"c181 box", "depth-keyed box", "no box", "halo", "halo, one band"};

int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("  CHECK FAILED %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

Result Run(const Scenario& S, const char* outDir) {
    Result res{};
    Scene sc;
    sc.post = S.post;
    Player pa{{0, 0, 0}, 0.9}, pb{pa.pos + S.move, pa.phase + S.phaseStep};
    const V3 centreK{0, 82, 0};
    const double orbit = 1330.0;
    const auto camFor = [&](V3 focus, double yaw) {
        Cam c;
        const double a = yaw * M_PI / 180.0;
        c.Look(focus + V3{std::sin(a) * orbit, 300.0, std::cos(a) * orbit}, focus + V3{0, 40, 0});
        return c;
    };
    const V3 focusA = pa.pos + centreK, focusB = focusA + V3{S.move.x * S.follow, 0, S.move.z * S.follow};
    const Cam ca = camFor(focusA, 0.0), cb = camFor(focusB, S.yawDeg);
    Image A, T, Bg;
    Render(ca, pa, sc, A);
    Render(cb, pb, sc, T);
    // the back end's matrices: D = V_B V_A^-1, M = P D^-1 P^-1; the rows over the picture (no margins): uv = (R0 . p, R1 . p) / (R2 . p), p = (ndc x, ndc y, zn, 1)
    double Vai[16], D[16], Di[16], Pi[16], Tm[16], M[16];
    Inv4(ca.V, Vai); Mul4(cb.V, Vai, D); Inv4(D, Di); Inv4(g_P, Pi); Mul4(g_P, Di, Tm); Mul4(Tm, Pi, M);
    const auto rows = [&](const double* Mx, double R[3][4]) {
        for (int c = 0; c < 4; ++c) { R[0][c] = 0.5 * Mx[c * 4 + 0] + 0.5 * Mx[c * 4 + 3]; R[1][c] = 0.5 * Mx[c * 4 + 1] + 0.5 * Mx[c * 4 + 3]; R[2][c] = Mx[c * 4 + 3]; }
    };
    double R[3][4];
    rows(M, R);
    const auto sourceR = [&](const double Rx[3][4], double x, double y, double zn, double& sx, double& sy) {
        const double d = Rx[2][0] * x + Rx[2][1] * y + Rx[2][2] * zn + Rx[2][3];
        if (!(d > 1e-6)) return false;
        sx = (Rx[0][0] * x + Rx[0][1] * y + Rx[0][2] * zn + Rx[0][3]) / d * W;
        sy = (Rx[1][0] * x + Rx[1][1] * y + Rx[1][2] * zn + Rx[1][3]) / d * H;
        return true;
    };
    // the player's data as the game hands it over: his centre in both views, his feet, his up and the ground point in B's view
    const V3 plA = ca.ToView(pa.pos + centreK), plB = cb.ToView(pb.pos + centreK), feetB = cb.ToView(pb.pos);
    const V3 upB{cb.right.y, cb.up.y, cb.back.y};
    const V3 gndB = cb.ToView({pb.pos.x, 0.0, pb.pos.z});
    float tD[3] = {static_cast<float>(D[12]), static_cast<float>(D[13]), static_cast<float>(D[14])};
    port::hrw36::Plane L{};
    bool plane = false;
    if (S.ground) {
        const float g[3] = {static_cast<float>(gndB.x), static_cast<float>(gndB.y), static_cast<float>(gndB.z)}, n[3] = {static_cast<float>(upB.x), static_cast<float>(upB.y), static_cast<float>(upB.z)};
        plane = port::hrw36::MakePlane(g_Pf, g, n, tD, W, H, 500.0, 40.0, 60.0, L) == port::hrw36::kPlaneOk;
    }
    // the scene's own warp: the source, in pixels, of the second picture's pixel (px, py)
    const auto sceneSrc = [&](double px, double py, double& sx, double& sy) {
        const double x = px / W * 2.0 - 1.0, y = py / H * 2.0 - 1.0;
        const double zn = plane ? 1.0 + g_P[14] * port::hrw36::EvalK(L, x, y) : 1.0;
        return sourceR(R, x, y, zn, sx, sy);
    };
    std::vector<uint8_t> scene(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double sx, sy, c[3] = {0, 0, 0};
            if (sceneSrc(x + 0.5, y + 0.5, sx, sy)) Sample(A, sx, sy, c);
            for (int k = 0; k < 3; ++k) scene[(static_cast<size_t>(y) * W + x) * 3 + k] = static_cast<uint8_t>(std::lround(c[k]));
        }
    // the c181 box: the locked plane (t = pB - R pA), the body-shaped rectangle, the 10 px feather
    double ax, ay, tx, ty, fx, fy;
    Project(plA, ax, ay); Project(plB, tx, ty); Project(feetB, fx, fy);
    const double dist = -plB.z, znHis = g_P[14] / dist - g_P[10];
    double Dl[16], Dli[16], Tl[16], Mlk[16], Rl[3][4];
    std::memcpy(Dl, D, sizeof Dl);
    {
        const double a[3] = {plA.x, plA.y, plA.z}, b[3] = {plB.x, plB.y, plB.z};
        for (int r = 0; r < 3; ++r) Dl[12 + r] = b[r] - (Dl[0 + r] * a[0] + Dl[4 + r] * a[1] + Dl[8 + r] * a[2]);
    }
    Inv4(Dl, Dli); Mul4(g_P, Dli, Tl); Mul4(Tl, Pi, Mlk);
    rows(Mlk, Rl);
    const port::hrw36::Box bx = port::hrw36::ShapedBox(tx, ty, fx, fy, std::fabs(g_P[5]) / dist * 0.5 * H, 85.0, 1.6, 8.0, 8.0, W, H);
    const int box[4] = {bx.x0, bx.y0, bx.x1, bx.y1};
    res.boxPx = (box[2] - box[0]) * (box[3] - box[1]);
    const double feather = 10.0, bfx = 0.5 * (box[2] - box[0]) / feather, bfy = 0.5 * (box[3] - box[1]) / feather;
    const port::hrw41::Key key = port::hrw41::MakeKey(-plA.z, 70.0);
    const double znScene = plane ? znHis : 1.0;
    // the halo's plan: his centre, his feet, his head (the first picture's feet and head: his centre there plus the second picture's offsets)
    port::halo31::Anchor an[3];
    {
        double hx, hy;
        const V3 headB = cb.ToView(pb.pos + V3{0, 165, 0});
        Project(headB, hx, hy);
        an[0] = {ax, ay, tx, ty};
        an[1] = {ax + (fx - tx), ay + (fy - ty), fx, fy};
        an[2] = {ax + (hx - tx), ay + (hy - ty), hx, hy};
    }
    const port::halo31::Plan plan = port::halo31::Make(sceneSrc, an, 3, box, W, H, 0.75, 48.0, 1.5, 12);
    const port::halo31::Plan plan0 = port::halo31::Make(sceneSrc, an, 3, box, W, H, 0.75, 48.0, 1000.0, 12);  // (one band: no lean)
    res.plan = plan;
    const int wx0 = std::max(0, box[0] - 48), wy0 = std::max(0, box[1] - 48), wx1 = std::min(W, box[2] + 48), wy1 = std::min(H, box[3] + 48);
    res.n = (wx1 - wx0) * (wy1 - wy0);
    std::vector<uint8_t> out[5], dbg;
    for (int m = 0; m < 5; ++m) {
        out[m] = scene;
        const port::halo31::Plan& pl = m == kHaloNoDilate ? plan0 : plan;
        if (m == kHalo) dbg = scene;
        int changed = 0, ghost = 0, dragged = 0;
        double dragSum = 0.0;
        for (int y = wy0; y < wy1; ++y)
            for (int x = wx0; x < wx1; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                const bool fresh = T.player[i] != 0;
                // where the pixel's colour comes from in the first picture (the plain scene warp's source unless a method replaces it)
                double qx = 0, qy = 0, q0x = 0, q0y = 0;
                sceneSrc(x + 0.5, y + 0.5, q0x, q0y);
                qx = q0x; qy = q0y;
                const bool haloM = m == kHalo || m == kHaloNoDilate;
                const bool useBox = m == kBox || m == kBoxDepth || (haloM && !pl.on && pl.why != port::halo31::Why::Still);  // (a refused plan: the box as before)
                if (useBox && x >= box[0] && x < box[2] && y >= box[1] && y < box[3]) {
                    const double u = ((x + 0.5) - box[0]) / (box[2] - box[0]) * 2.0 - 1.0, v = ((y + 0.5) - box[1]) / (box[3] - box[1]) * 2.0 - 1.0;
                    double a = std::clamp(std::min((1.0 - std::fabs(u)) * bfx, (1.0 - std::fabs(v)) * bfy), 0.0, 1.0);
                    const double nx = (x + 0.5) / W * 2.0 - 1.0, ny = (y + 0.5) / H * 2.0 - 1.0;
                    double sx, sy, c[3];
                    if (sourceR(Rl, nx, ny, znHis, sx, sy)) {
                        if (m == kBoxDepth) {
                            double px = 0, py = 0;
                            sourceR(R, nx, ny, znScene, px, py);
                            a *= port::keepdepth::BoxDepthWeight(key, DepthAt(A, sx, sy), DepthAt(A, px, py), g_P[10], g_P[14]);
                        }
                        Sample(A, sx, sy, c);
                        if (a > 0.0) ++changed;
                        if (a >= 0.5) { qx = sx; qy = sy; }
                        for (int k = 0; k < 3; ++k) out[m][i * 3 + k] = static_cast<uint8_t>(std::lround(out[m][i * 3 + k] * (1.0 - a) + c[k] * a));
                    }
                }
                if (haloM && pl.on && x >= pl.rect[0] && x < pl.rect[2] && y >= pl.rect[1] && y < pl.rect[3]) {
                    for (int bnd = 0; bnd < pl.n; ++bnd) {
                        const port::halo31::Band& B = pl.band[bnd];
                        if (x < B.rect[0] || x >= B.rect[2] || y < B.rect[1] || y >= B.rect[3]) continue;
                        const int sx0 = x - B.stamp[0], sy0 = y - B.stamp[1];
                        const bool stamped = sx0 >= 0 && sx0 < W && sy0 >= 0 && sy0 < H && T.player[static_cast<size_t>(sy0) * W + sx0] != 0;
                        if (port::halo31::Stencil(stamped, fresh) != port::halo31::kStamp) continue;
                        double sx, sy, c[3] = {0, 0, 0};
                        if (sceneSrc(x + 0.5 - B.fill[0], y + 0.5 - B.fill[1], sx, sy)) Sample(A, sx, sy, c);
                        for (int k = 0; k < 3; ++k) out[m][i * 3 + k] = static_cast<uint8_t>(std::lround(c[k]));
                        ++changed;
                        qx = sx; qy = sy;
                        if (m == kHalo) { dbg[i * 3] = 255; dbg[i * 3 + 1] = 0; dbg[i * 3 + 2] = 255; }
                    }
                }
                if (!fresh) {
                    const int px = std::clamp(static_cast<int>(std::floor(qx)), 0, W - 1), py = std::clamp(static_cast<int>(std::floor(qy)), 0, H - 1);
                    if (A.player[static_cast<size_t>(py) * W + px]) ++ghost;  // (the first picture's player shown beside the fresh one)
                    else { const double d = std::hypot(qx - q0x, qy - q0y); if (d > 1.0) { ++dragged; dragSum += d; } }  // (background moved differently from its surroundings)
                }
                if (fresh) {
                    std::memcpy(&out[m][i * 3], &T.rgb[i * 3], 3);
                    if (m == kHalo) std::memcpy(&dbg[i * 3], &T.rgb[i * 3], 3);
                }
            }
        double sum = 0;
        int wrong = 0;
        for (int y = wy0; y < wy1; ++y)
            for (int x = wx0; x < wx1; ++x) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 3;
                int mx = 0;
                for (int k = 0; k < 3; ++k) { const int d = std::abs(static_cast<int>(out[m][i + k]) - static_cast<int>(T.rgb[i + k])); sum += d; mx = std::max(mx, d); }
                if (mx > 24) ++wrong;
            }
        res.mae[m] = sum / (3.0 * res.n);
        res.wrong[m] = wrong;
        res.changed[m] = changed;
        res.ghost[m] = ghost;
        res.dragged[m] = dragged;
        res.dragPx[m] = dragged ? dragSum / dragged : 0.0;
    }
    if (outDir) {
        const std::string base = std::string(outDir) + "/" + S.name + "_";
        const int cx = static_cast<int>(tx), cy = H - 1 - static_cast<int>(ty);
        const auto put = [&](const char* tag, const std::vector<uint8_t>& img) {
            const std::vector<uint8_t> up = Upright(img);
            WriteCrop(base + tag + ".ppm", up, cx, cy);
        };
        put("0_first", A.rgb);
        put("1_truth", T.rgb);
        put("2_c181box", out[kBox]);
        put("3_boxdepth", out[kBoxDepth]);
        put("4_nobox", out[kNoBox]);
        put("5_halo", out[kHalo]);
        put("6_halo_fill_painted", dbg);
        if (std::strcmp(S.name, "run") == 0) { WritePpm(base + "full_truth.ppm", Upright(T.rgb), W, H); WritePpm(base + "full_halo.ppm", Upright(out[kHalo]), W, H); WritePpm(base + "full_c181box.ppm", Upright(out[kBox]), W, H); }
    }
    return res;
}
}  // namespace

int main(int argc, char** argv) {
    MakeProjection();
    const char* outDir = argc > 1 ? argv[1] : nullptr;
    const Scenario list[] = {
        {"run", {14, 0, 0}, 1.0, 0.0, true, false, 0.26, false},
        {"jump", {8, 16, 0}, 1.0, 0.0, true, false, 0.10, false},
        {"pan", {5, 0, 3}, 1.0, 1.1, true, false, 0.20, false},
        {"air", {14, 6, 0}, 1.0, 0.5, false, false, 0.15, false},
        {"post", {14, 0, 0}, 1.0, 0.0, true, true, 0.26, false},
        {"far", {420, 0, 0}, 0.0, 0.0, true, false, 0.26, true},
    };
    int tGhost[5] = {}, tDrag[5] = {}, tWrong[5] = {};
    double tMae[5] = {};
    for (const Scenario& S : list) {
        const Result r = Run(S, outDir);
        std::printf("%-5s | window %d px, box %d px | old player %.1f px from the fresh one | lean %.1f px | plan: %s, %d bands\n", S.name, r.n, r.boxPx, r.plan.apart, r.plan.lean,
                    r.plan.on ? "on" : r.plan.why == port::halo31::Why::Still ? "still" : r.plan.why == port::halo31::Why::TooFar ? "too far (the box as before)" : "off", r.plan.n);
        std::printf("      bands (stamp / fill):");
        for (int i = 0; i < r.plan.n; ++i) std::printf(" %+d,%+d / %+d,%+d |", r.plan.band[i].stamp[0], r.plan.band[i].stamp[1], r.plan.band[i].fill[0], r.plan.band[i].fill[1]);
        std::printf(" rect %d,%d..%d,%d\n", r.plan.rect[0], r.plan.rect[1], r.plan.rect[2], r.plan.rect[3]);
        for (int m = 0; m < 5; ++m)
            std::printf("      %-22s old player left showing %5d px | background moved apart from its surroundings %5d px (by %4.1f px) | against the true picture: mean error %6.2f levels, over 24 levels off %5d px\n",
                        kMethod[m], r.ghost[m], r.dragged[m], r.dragPx[m], r.mae[m], r.wrong[m]);
        if (S.mustRefuse) {
            CHECK(!r.plan.on && r.plan.why == port::halo31::Why::TooFar, "%s: a move of %.0f px must be refused", S.name, r.plan.apart);
            CHECK(r.dragged[kHalo] == r.dragged[kBox] && r.ghost[kHalo] == r.ghost[kBox], "%s: a refused plan must be the box as before", S.name);
            continue;
        }
        for (int m = 0; m < 5; ++m) { tGhost[m] += r.ghost[m]; tDrag[m] += r.dragged[m]; tWrong[m] += r.wrong[m]; tMae[m] += r.mae[m]; }
        CHECK(r.plan.on, "%s: the plan must be on", S.name);
        CHECK(r.ghost[kNoBox] > 300, "%s: the scenario must show the old player without a box (%d px)", S.name, r.ghost[kNoBox]);
        CHECK(r.ghost[kHalo] * 4 <= r.ghost[kNoBox], "%s: the halo leaves %d px of the old player, no box %d", S.name, r.ghost[kHalo], r.ghost[kNoBox]);
        CHECK(r.ghost[kHalo] <= r.ghost[kHaloNoDilate] + 8, "%s: the bands leave %d px of the old player, one band %d", S.name, r.ghost[kHalo], r.ghost[kHaloNoDilate]);
        CHECK(r.dragged[kHalo] * 4 < r.dragged[kBox], "%s: the halo moves %d px of background apart, the box %d", S.name, r.dragged[kHalo], r.dragged[kBox]);
        CHECK(r.changed[kHalo] > 0 && r.changed[kHalo] < 2500, "%s: the halo fills %d px", S.name, r.changed[kHalo]);
    }
    std::printf("total (5 scenarios)      c181 box / depth-keyed box / no box / halo / halo with one band\n");
    std::printf("  old player left showing, px:                        %6d / %6d / %6d / %6d / %6d\n", tGhost[0], tGhost[1], tGhost[2], tGhost[3], tGhost[4]);
    std::printf("  background moved apart from its surroundings, px:   %6d / %6d / %6d / %6d / %6d\n", tDrag[0], tDrag[1], tDrag[2], tDrag[3], tDrag[4]);
    std::printf("  over 24 levels off the true picture, px:            %6d / %6d / %6d / %6d / %6d\n", tWrong[0], tWrong[1], tWrong[2], tWrong[3], tWrong[4]);
    std::printf("  mean error against the true picture, levels:        %6.2f / %6.2f / %6.2f / %6.2f / %6.2f\n", tMae[0] / 5, tMae[1] / 5, tMae[2] / 5, tMae[3] / 5, tMae[4] / 5);
    CHECK(tGhost[kHalo] + tDrag[kHalo] < (tGhost[kBox] + tDrag[kBox]) / 5, "total: the halo's artifact pixels %d must be under a fifth of the box's %d", tGhost[kHalo] + tDrag[kHalo], tGhost[kBox] + tDrag[kBox]);
    CHECK(tGhost[kHalo] * 2 < tGhost[kHaloNoDilate], "total: the bands must help (%d against %d)", tGhost[kHalo], tGhost[kHaloNoDilate]);
    CHECK(tWrong[kHalo] < tWrong[kBox] && tMae[kHalo] < tMae[kBox], "total: the halo must be nearer the true picture than the box");
    std::printf(g_fail ? "halo31 rules: FAIL (%d)\n" : "halo31 rules: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
