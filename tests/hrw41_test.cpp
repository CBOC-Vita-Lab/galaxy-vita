// hrw41 host test: port/hrw41.h (the depth code, its 16-bit form, the box key) and a ray-cast model of the pair's second picture:
// how far each way of re-projecting leaves a pixel's source from where the first picture really has the point (rotation only = the Performance mode's scene warp
// today; the depth mask with one tap = GXV_HRW41_DEPTH=1; with the nearer of two taps = 2), on the camera moves the console logs have (a pan about the player).
// g++ -std=c++17 -O2 -Iplatform/include tests/hrw41_test.cpp && ./a.out
#include "port/hrw41.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

namespace {
using namespace port::hrw41;
const int W = 960, H = 544;
const double kNear = 100.0, kFar = 800158.0;
const double kFovY = 45.0 * 3.14159265358979 / 180.0;
struct V3 { double x, y, z; };
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
// A camera: position, yaw about +y; looks down -z at yaw 0. View = R^-1 (p - pos).
struct Cam { V3 pos; double yaw; };
V3 ToView(const Cam& c, V3 p) {
    const V3 d = p - c.pos;
    const double cs = std::cos(c.yaw), sn = std::sin(c.yaw);
    return {cs * d.x - sn * d.z, d.y, sn * d.x + cs * d.z};
}
V3 Ray(const Cam& c, double px, double py) {  // world direction of the pixel's ray (not unit; its view z is -1)
    const double ty = std::tan(kFovY / 2), tx = ty * W / H;
    const double vx = (px / W * 2 - 1) * tx, vy = (1 - py / H * 2) * ty;
    const double cs = std::cos(c.yaw), sn = std::sin(c.yaw);
    return {cs * vx - sn * 1.0, vy, -sn * vx - cs * 1.0};   // R (vx, vy, -1)
}
bool Project(const Cam& c, V3 p, double& px, double& py, double& dist) {
    const V3 v = ToView(c, p);
    if (!(-v.z > 1.0)) return false;
    const double ty = std::tan(kFovY / 2), tx = ty * W / H;
    px = (v.x / -v.z / tx * 0.5 + 0.5) * W;
    py = (0.5 - v.y / -v.z / ty * 0.5) * H;
    dist = -v.z;
    return true;
}
// The scene: the ground y = -160 out to 6000 units, a pillar (a box) 120 wide and 400 high standing 700 units ahead, a wall across at 3500, the sky beyond.
// Returns the ray parameter t (point = pos + dir * t; dir's view z is -1 so t is the view distance in the casting camera) or a huge t for the sky.
double Cast(const Cam& c, V3 dir, int* what = nullptr) {
    double best = 1e9; int w = 0;
    const V3 o = c.pos;
    if (dir.y < -1e-9) {  // ground
        const double t = (-160.0 - o.y) / dir.y;
        const V3 p = o + dir * t;
        if (t > 0 && t < best && std::fabs(p.x) < 6000 && p.z > -6000) { best = t; w = 1; }
    }
    {   // pillar: x in [-60, 60], z in [-760, -640], y in [-160, 240]: slab test
        double t0 = 0, t1 = 1e9;
        const double lo[3] = {-60, -160, -760}, hi[3] = {60, 240, -640}, od[3] = {o.x, o.y, o.z}, dd[3] = {dir.x, dir.y, dir.z};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            if (std::fabs(dd[i]) < 1e-12) { ok = od[i] >= lo[i] && od[i] <= hi[i]; continue; }
            double a = (lo[i] - od[i]) / dd[i], b = (hi[i] - od[i]) / dd[i];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a); t1 = std::min(t1, b);
            ok = t0 <= t1;
        }
        if (ok && t0 > 0 && t0 < best) { best = t0; w = 2; }
    }
    if (std::fabs(dir.z) > 1e-12) {  // wall z = -3500, y < 900
        const double t = (-3500.0 - o.z) / dir.z;
        const V3 p = o + dir * t;
        if (t > 0 && t < best && p.y < 900 && p.y > -160) { best = t; w = 3; }
    }
    if (what) *what = w;
    return best;
}
struct Res { double mean = 0, p95 = 0, max = 0, over2 = 0; int n = 0, hidden = 0; };
// way: 0 rotation only, 1 the mask with one tap, 2 the nearer of two taps. div: the mask's size against the picture.
Res Run(const Cam& A, const Cam& B, int way, int div) {
    // the first picture's mask: view distance in A, the nearest of four taps one pixel either way, as a 16-bit code in two bytes
    const int mw = W / div, mh = H / div;
    std::vector<uint8_t> mHi(mw * mh), mLo(mw * mh);
    for (int y = 0; y < mh; ++y)
        for (int x = 0; x < mw; ++x) {
            double d = 1e9;
            for (int k = 0; k < 4; ++k) {
                const double px = (x + 0.5) * div + (k & 1 ? 1 : -1), py = (y + 0.5) * div + (k & 2 ? 1 : -1);
                d = std::min(d, Cast(A, Ray(A, px, py)));
            }
            Pack16(DepthCode(std::min(d, kFar)), mHi[y * mw + x], mLo[y * mw + x]);
        }
    const auto mask = [&](double px, double py) {  // bilinear, clamped: the filtered code
        const double fx = std::clamp(px / div - 0.5, 0.0, mw - 1.0), fy = std::clamp(py / div - 0.5, 0.0, mh - 1.0);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), x1 = std::min(x0 + 1, mw - 1), y1 = std::min(y0 + 1, mh - 1);
        const double ax = fx - x0, ay = fy - y0;
        const auto at = [&](int x, int y) { return Unpack16(mHi[y * mw + x] / 255.0, mLo[y * mw + x] / 255.0); };
        return (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
    };
    // the source in A of B's pixel if its point is `dist` in front of B (exactly what M = P V_A V_B^-1 P^-1 does with zn of that distance)
    const auto source = [&](double px, double py, double dist, double& sx, double& sy) {
        const V3 p = B.pos + Ray(B, px, py) * dist;
        double d;
        return Project(A, p, sx, sy, d);
    };
    // GXV_HRW_WARP_NEAR's second tap: from the source of a point at infinity towards the source of a point 600 units away, at the screen's centre, at most 16 px
    double nx = 0, ny = 0;
    {
        double fx, fy, qx, qy;
        if (source(W / 2.0, H / 2.0, 1e7, fx, fy) && source(W / 2.0, H / 2.0, 600.0, qx, qy)) {
            nx = qx - fx; ny = qy - fy;
            const double l = std::hypot(nx, ny);
            if (l > 16.0) { nx *= 16.0 / l; ny *= 16.0 / l; }
            if (l < 1.0) nx = ny = 0;
        }
    }
    std::vector<double> errs;
    Res r;
    for (int y = 2; y < H; y += 4)
        for (int x = 2; x < W; x += 4) {
            const V3 dir = Ray(B, x + 0.5, y + 0.5);
            int what = 0;
            const double t = Cast(B, dir, &what);
            const V3 p = B.pos + dir * std::min(t, 1e7);
            double tx, ty, td;
            if (!Project(A, p, tx, ty, td) || tx < 0 || tx >= W || ty < 0 || ty >= H) continue;   // (off the first picture: the edge strip, another matter)
            // is the point seen in A at all? (a disocclusion has no right answer)
            const double seen = Cast(A, Ray(A, tx, ty));
            if (what != 0 && std::fabs(seen - td) > 0.02 * td + 2.0) { ++r.hidden; continue; }
            double sx, sy;
            if (way == 0) { if (!source(x + 0.5, y + 0.5, 1e7, sx, sy)) continue; }
            else {
                double ix, iy;
                if (!source(x + 0.5, y + 0.5, 1e7, ix, iy)) continue;   // the tap: where the pixel's content stood at infinity
                double code = mask(ix, iy);
                if (way == 2) code = std::min(code, mask(ix + nx, iy + ny));
                if (!source(x + 0.5, y + 0.5, CodeDist(code), sx, sy)) continue;

            }
            const double e = std::hypot(sx - tx, sy - ty);
            errs.push_back(e);
            r.mean += e; r.max = std::max(r.max, e); if (e > 2.0) r.over2 += 1;
        }
    r.n = static_cast<int>(errs.size());
    if (r.n) {
        std::sort(errs.begin(), errs.end());
        r.mean /= r.n; r.p95 = errs[static_cast<size_t>(r.n * 0.95)]; r.over2 = 100.0 * r.over2 / r.n;
    }
    return r;
}
}  // namespace

int main() {
    {   // the code and its 16-bit form
        CHECK(std::fabs(CodeDist(DepthCode(1234.5)) - 1234.5) < 1e-6);
        CHECK(DepthCode(10.0) == 0.0 && DepthCode(5e6) == 1.0);
        double worst = 0;
        for (double d = 40.0; d < 9e5; d *= 1.013) {
            uint8_t hi, lo;
            Pack16(DepthCode(d), hi, lo);
            const double back = CodeDist(Unpack16(hi / 255.0, lo / 255.0));
            worst = std::max(worst, std::fabs(back - d) / d);
        }
        CHECK(worst < 0.0002);   // 16 bits: 0.016 % of the distance a step (8 bits: 4 %)
        // linear filtering of the two channels = filtering of the code, across a step of the high byte
        uint8_t h0, l0, h1, l1;
        const double c0 = 10.9 / 255.0, c1 = 11.1 / 255.0;
        Pack16(c0, h0, l0); Pack16(c1, h1, l1);
        CHECK(h0 == 10 && h1 == 11);
        const double mid = Unpack16((h0 + h1) / 2.0 / 255.0, (l0 + l1) / 2.0 / 255.0);
        CHECK(std::fabs(mid - (c0 + c1) / 2) < 1.0 / 255.0 / 200.0);
        // zn: -1 at the near plane, +1 at the far plane for the game's projection (e = -(f+n)/(f-n), f = -2fn/(f-n))
        const double e = -(kFar + kNear) / (kFar - kNear), f = -2.0 * kFar * kNear / (kFar - kNear);
        CHECK(std::fabs(ZnOfCode(DepthCode(kNear), e, f) + 1.0) < 1e-6 && std::fabs(ZnOfCode(DepthCode(kFar), e, f) - 1.0) < 1e-6);
    }
    {   // the box key
        const Key k = MakeKey(1200.0, 70.0);
        const double his = DepthCode(1200.0);
        CHECK(std::fabs(k.code - his) < 1e-6);
        CHECK(KeyWeight(k, his, 1.0) == 1.0);                               // his body at the box's source
        CHECK(KeyWeight(k, 1.0, his) == 1.0);                               // the strip he left (the scene would show him there)
        CHECK(KeyWeight(k, DepthCode(1260.0), 1.0) == 1.0);                 // inside the tolerance
        CHECK(KeyWeight(k, DepthCode(1150.0), 1.0) == 1.0);
        CHECK(KeyWeight(k, DepthCode(1400.0), DepthCode(1400.0)) == 0.0);   // the ground 200 units behind him: the scene's own warp
        CHECK(KeyWeight(k, DepthCode(5000.0), 1.0) == 0.0);                 // the background
        CHECK(KeyWeight(k, DepthCode(900.0), DepthCode(900.0)) == 0.0);     // something well in front of him
        const double w = KeyWeight(k, DepthCode(1290.0), 1.0);
        CHECK(w > 0.0 && w < 1.0);                                          // soft between 70 and about 105 units
        const Key kn = MakeKey(400.0, 70.0);                                // nearer camera: the same 70 units are more codes
        CHECK(kn.tol > k.tol * 2.5);
        // Move the camera towards Mario on a jump. The mask and the body pixel
        // are both from A: using B's distance as the key would reject him.
        CHECK(KeyWeight(k, DepthCode(1200.0), 1.0) == 1.0);
        CHECK(KeyWeight(MakeKey(1000.0, 70.0), DepthCode(1200.0), 1.0) == 0.0);
    }
    {   // the model: a pan about a player 1200 units ahead (the camera turns and moves sideways so that he stays in the middle), and a run (the camera moves on)
        struct Move { const char* name; double yawDeg, side, fwd; } moves[] = {
            {"pan 1.0 deg about the player (21 units sideways)", 1.0, 20.9, 0.0},
            {"pan 2.0 deg about the player (42 units sideways)", 2.0, 41.9, 0.0},
            {"run: 15 units forward, no turn", 0.0, 0.0, 15.0},
            {"strafe: 15 units sideways, no turn", 0.0, 15.0, 0.0},
        };
        printf("hrw41 model (960x544, a pillar 700 units ahead, ground 160 below, a wall at 3500, sky): px between the warp's source and the true one, pixels whose point the first picture shows\n");
        for (const Move& m : moves) {
            const Cam A{{0, 0, 0}, 0.0};
            const double yaw = m.yawDeg * 3.14159265358979 / 180.0;
            const Cam B{{m.side, 0, -m.fwd}, yaw};
            const Res r0 = Run(A, B, 0, 4), r1 = Run(A, B, 1, 4), r2 = Run(A, B, 2, 4), r1h = Run(A, B, 1, 2);
            printf("  %-52s rotation only: mean %5.2f p95 %5.2f max %5.1f (%4.1f %% over 2 px) | depth mask 1 tap: mean %4.2f p95 %4.2f max %5.1f (%4.1f %%) | 2 taps: mean %4.2f p95 %4.2f max %5.1f (%4.1f %%) | "
                   "half-size mask 1 tap: mean %4.2f p95 %4.2f (%4.1f %%) | not in the first picture %d of %d\n",
                   m.name, r0.mean, r0.p95, r0.max, r0.over2, r1.mean, r1.p95, r1.max, r1.over2, r2.mean, r2.p95, r2.max, r2.over2, r1h.mean, r1h.p95, r1h.over2, r1.hidden, r1.n + r1.hidden);
            CHECK(r1.mean < 0.35 * r0.mean + 0.05);      // the depth takes most of the error away
            CHECK(r2.p95 < 2.0);                          // ... and with two taps what is left is under 2 px for 95 % of the pixels, on a 2 degree pan too
            CHECK(r1.p95 < 8.0);                          // (one tap: the ground's own slope on the fastest pan)
            CHECK(r1.over2 < 0.5 * r0.over2 + 0.5);
            CHECK(r2.mean < 0.6 * r0.mean + 0.05);       // the two-tap form is never the worse choice by much
        }
        // no camera move: every way is exact
        const Cam A{{0, 0, 0}, 0.0};
        const Res s0 = Run(A, A, 0, 4), s1 = Run(A, A, 1, 4);
        CHECK(s0.max < 1e-6 && s1.max < 0.01);
    }
    {
        DetailBudget b;
        b.Reset(0xfffffe08u);
        CHECK(b.Admit(0xffffff20u));  // first partial window of a new scene
        CHECK(!b.Admit(0x00000149u)); // 24-bit wrap skips sequence zero
        CHECK(!b.Admit(0x00000164u)); // same window, despite its score
        CHECK(!b.Admit(0x00000260u));
        CHECK(!b.Admit(0x00000460u)); // missing window breaks the good run
        CHECK(!b.Admit(0x00000560u));
        CHECK(b.Admit(0x00000660u));
        CHECK(!b.Admit(0x00000732u));
        for (unsigned seq = 8; seq < 13; ++seq) CHECK(!b.Admit((seq << 8) | 95u));
        CHECK(b.Admit(0x00000d5fu));
        b.Reset(0);
        CHECK(b.Admit(0));
        CHECK(b.Admit(0x100u));
        CHECK(!b.Admit(0x200u));
        {   // (c183fix F2) a finished window under 90 switches off (75-89 used to be accepted for ever); the third switch-off gives up for the stage visit
            DetailBudget g;
            g.Reset(0x100u);
            CHECK(g.Admit(0x0000025au));  // first window after the load: skipped (score 90)
            CHECK(!g.Admit(0x00000350u)); // 80: off (1)
            for (unsigned seq = 4; seq < 6; ++seq) CHECK(!g.Admit((seq << 8) | 95u));
            CHECK(g.Admit(0x0000065fu));  // three calm windows: back
            CHECK(!g.Admit(0x00000759u)); // 89: off (2)
            for (unsigned seq = 8; seq < 13; ++seq) CHECK(!g.Admit((seq << 8) | 100u));
            CHECK(g.Admit(0x00000d64u));  // six calm windows: back
            CHECK(g.Admit(0x00000e5au));  // exactly 90 is calm
            CHECK(g.Fails() == 2 && !g.GaveUp());
            CHECK(!g.Admit(0x00000f4bu)); // 75: off (3) = given up
            CHECK(g.GaveUp() && g.Fails() == 3);
            for (unsigned seq = 16; seq < 16 + 40; ++seq) CHECK(!g.Admit((seq << 8) | 100u));  // calm for 400 s: stays off for the visit
            g.Reset(0x3800u);             // next stage load: a new visit
            CHECK(!g.GaveUp() && g.Fails() == 0 && g.Admit(0x00003864u));
            CHECK(g.Admit(0x00003964u) && g.Admit(0x00003a5au));
        }
        const double cutoff = ForegroundLimit(1200.0, 1100.0, 140.0);
        CHECK(cutoff > DepthCode(950.0) && cutoff < DepthCode(970.0));
        CHECK(ForegroundLimit(1200.0, 100.0, 140.0) < 0);
        CHECK(ForegroundLimit(NAN, 1200.0, 140.0) < 0);
        ProjectionMemo memo;
        float matrix[16]{};
        int a = 1, bbuf = 2, calls = 0;
        auto fetch = [&](float* p) { p[15] = float(++calls); return true; };
        CHECK(memo.Get(&a, 2, matrix, fetch) && matrix[15] == 1);
        CHECK(memo.Get(&a, 2, matrix, fetch) && matrix[15] == 1);
        CHECK(memo.Get(&bbuf, 2, matrix, fetch) && matrix[15] == 2);
        CHECK(memo.Get(&bbuf, 3, matrix, fetch) && matrix[15] == 3);
        memo.Reset();
        CHECK(!memo.Get(&bbuf, 3, matrix, [](float*) { return false; }));
        CHECK(memo.Get(&bbuf, 3, matrix, fetch) && matrix[15] == 4);
        CHECK(DepthIndependent(0.f, -0.f, 0.f));
        CHECK(!DepthIndependent(1.e-9f, 0.f, 0.f));
        CHECK(!DepthIndependent(0.f, NAN, 0.f));
        CHECK(MarkerLookahead(true, false, false, false, true) == MaskStep::Stop);      // skipped/hoisted copy clears depth
        CHECK(MarkerLookahead(false, true, true, false, false) == MaskStep::Stop);      // enters another framebuffer
        CHECK(MarkerLookahead(false, false, false, true, false) == MaskStep::Stop);     // intervening depth writer
        CHECK(MarkerLookahead(true, false, true, false, true) == MaskStep::BeforeCopy); // real copy, before its clear
        CHECK(MarkerLookahead(false, false, false, false, false) == MaskStep::KeepLooking);
    }
    {   // hrwfeel36, GXV_HRW_WARP_SHARP: SharpCoord(t, k)
        for (double t = -3.0; t < 3.0; t += 0.0625) CHECK(std::fabs(SharpCoord(t, 1.0) - (std::floor(t) + (t - std::floor(t)) * 1.0 + 0.0)) < 1e-12);   // k = 1 is the identity (weight f)
        for (double k : {1.0, 2.0, 4.0, 8.0}) {
            double prev = -1e9;
            for (double t = -2.0; t < 2.0; t += 1.0 / 256) {
                const double c = SharpCoord(t, k);
                CHECK(c >= prev - 1e-12);                          // monotone
                CHECK(std::fabs(c - t) <= 0.5 + 1e-12);             // never moves a read by more than half a texel
                prev = c;
            }
            for (int i = -2; i <= 2; ++i) {
                CHECK(std::fabs(SharpCoord(i + 0.5, k) - (i + 0.5)) < 1e-12);       // fixed at a texel centre (the mid-point of the blend: weight 0.5 -> i + 0.5)
                CHECK(std::fabs(SharpCoord(i + 0.0, k) - i) < 1e-12);               // and at the texel edge: whole texel i
                CHECK(std::fabs(SharpCoord(i + 1.0 - 1e-9, k) - (i + 1)) < 1e-6 * k);   // up to the next edge: whole texel i + 1 (continuous across it)
            }
            // the blend zone is 1 / k wide, centred on the half
            const double lo = 0.5 - 0.5 / k, hi = 0.5 + 0.5 / k;
            CHECK(std::fabs(SharpWeight(lo, k)) < 1e-12 && std::fabs(SharpWeight(hi, k) - 1.0) < 1e-12);
            CHECK(SharpWeight(lo - 0.01, k) == 0.0 || k == 1.0);
            CHECK(SharpWeight(hi + 0.01, k) == 1.0 || k == 1.0);
            int blend = 0;
            for (int n = 0; n < 1000; ++n) { const double w = SharpWeight((n + 0.5) / 1000.0, k); if (w > 1e-9 && w < 1.0 - 1e-9) ++blend; }
            CHECK(std::abs(blend - 1000.0 / k) <= 2);
        }
        // a sub-texel shift read with k = 4 keeps texels whole outside the quarter-texel blend: a random line of texels, shifted by 0.3 texel
        // (bilinear would give every output a 30 % mix; k = 4: only the outputs near the edge are mixed)
        {
            int whole = 0, mixed = 0;
            for (int n = 0; n < 100; ++n) { const double w = SharpWeight(std::fmod(n + 0.3, 1.0) , 4.0); (w <= 1e-9 || w >= 1.0 - 1e-9) ? ++whole : ++mixed; }
            CHECK(whole > 0 && mixed >= 0);
        }
        CHECK(SnapPair(0.2, 0.5) && SnapPair(0.0, 0.5) && !SnapPair(0.5, 0.5) && !SnapPair(3.0, 0.5) && !SnapPair(1e9, 0.5) && !SnapPair(NAN, 0.5) && !SnapPair(0.2, 0.0));
    }
    {   // hrwfeel36, GXV_HRW41_BUDGET_SKIP: the first windows after the load are not judged
        DetailBudget b;
        b.Reset(0x100u, 2);
        CHECK(b.Admit(0x100u));      // the same window: unchanged
        CHECK(b.Admit(0x00000240u)); // first finished window, score 64: skipped
        CHECK(b.Admit(0x00000330u)); // second: skipped (score 48)
        CHECK(!b.Admit(0x00000440u)); // third: judged, off
        CHECK(b.Fails() == 1);
        DetailBudget c;
        c.Reset(0x100u);             // default 1: as before
        CHECK(c.Admit(0x00000240u) && !c.Admit(0x00000340u));
        DetailBudget d;
        d.Reset(0x100u, 0);          // a nonsense count is 1
        CHECK(d.Admit(0x00000240u) && !d.Admit(0x00000340u));
    }
    {   // finish44 (S3c, log only): a line for every Off / On of the detail budget, none for None / GaveUp
        using E = DetailBudget::Event;
        char t[320];
        DetailBudget b;
        b.Reset(0x100u, 2);
        CHECK(b.Admit(0x240u) && b.LastEvent() == E::None && BudgetEventLine(t, sizeof t, b.LastEvent(), "S", 0x240u, b.Fails(), b.Need()) == 0);   // skipped window: no event, no line
        CHECK(b.Admit(0x330u));
        CHECK(!b.Admit(0x440u) && b.LastEvent() == E::Off);
        int n = BudgetEventLine(t, sizeof t, b.LastEvent(), "HeavenlyBeachGalaxy", 0x440u, b.Fails(), b.Need());
        CHECK(n > 0 && n == static_cast<int>(strlen(t)) && strstr(t, "detail budget: OFF in stage HeavenlyBeachGalaxy") && strstr(t, "window score 64 under 90") && strstr(t, "switch-off 1 of 3") && strstr(t, "until 3 calm windows"));
        CHECK(!b.Admit(0x5f0u) && !b.Admit(0x6f0u) && b.LastEvent() == E::None);
        CHECK(b.Admit(0x7f0u) && b.LastEvent() == E::On);
        n = BudgetEventLine(t, sizeof t, b.LastEvent(), "HeavenlyBeachGalaxy", 0x7f0u, b.Fails(), b.Need());
        CHECK(n > 0 && strstr(t, "detail budget: ON again in stage HeavenlyBeachGalaxy") && strstr(t, "window score 240") && strstr(t, "1 switch-offs") && strstr(t, "needs 6 calm windows"));
        CHECK(BudgetEventLine(t, sizeof t, E::GaveUp, "S", 0, 3, 3) == 0 && BudgetEventLine(t, sizeof t, E::None, "S", 0, 0, 3) == 0);
        CHECK(BudgetEventLine(nullptr, 0, E::Off, "S", 0, 1, 3) == 0);
        char small[24];
        n = BudgetEventLine(small, sizeof small, E::Off, "S", 0x440u, 1, 3);   // a short buffer: cut, terminated, the length is what is there
        CHECK(n == 23 && strlen(small) == 23);
        CHECK(BudgetEventLine(t, sizeof t, E::On, nullptr, 0x7f0u, 1, 6) > 0 && strstr(t, "stage ?"));
    }
    {   // finish44, GXV_HRW41_MASK_PASSFIX: PassFixMark(on, ev, passBegin, marked, depthBits). Two positions a command; bits here: 1 = depth written, 2 = whole depth
        // overwritten, 4 = leaves the EFB, 8 = reads depth (only 1 | 2 are depth events).
        const uint8_t W = 1, F = 2, L = 4, R = 8, D = W | F;
        std::vector<uint8_t> ev(2 * 12, 0);
        ev[2 * 3] = W;            // a 3D draw before the pass (a depth write before the begin does not matter)
        ev[2 * 5] = L;            // the pass's begin: leaves the EFB
        ev[2 * 8 + 1] = F;        // the clear of the copy that ends the pass, AFTER the marked record at 7
        CHECK(PassFixMark(true, ev.data(), 5, 7, D) == 5);                 // the mask goes before the pass's begin
        CHECK(PassFixMark(false, ev.data(), 5, 7, D) == SIZE_MAX);         // the switch off: no place
        CHECK(PassFixMark(true, ev.data(), SIZE_MAX, 7, D) == SIZE_MAX);   // no pass begin known
        CHECK(PassFixMark(true, ev.data(), 7, 7, D) == SIZE_MAX && PassFixMark(true, ev.data(), 8, 7, D) == SIZE_MAX);   // a begin that is not before the record
        CHECK(PassFixMark(true, nullptr, 5, 7, D) == SIZE_MAX);
        CHECK(PassFixMark(true, ev.data(), 5, 9, D) == SIZE_MAX);          // a marked record after the copy's whole-depth clear: refused
        for (size_t p = 2 * 5; p < 2 * 7; ++p)                             // any depth event at any position from the begin up to the record refuses,
            for (uint8_t bit : {W, F}) {
                std::vector<uint8_t> e = ev; e[p] |= bit;
                CHECK(PassFixMark(true, e.data(), 5, 7, D) == SIZE_MAX);
            }
        for (size_t p = 2 * 5; p < 2 * 7; ++p) {                           // a read or a leave there does not (the depth is not changed by them)
            std::vector<uint8_t> e = ev; e[p] |= L | R;
            CHECK(PassFixMark(true, e.data(), 5, 7, D) == 5);
        }
        {   // events at the record's own positions and after it are not looked at (the mask is made before them)
            std::vector<uint8_t> e = ev; e[2 * 7] |= D; e[2 * 7 + 1] |= D;
            CHECK(PassFixMark(true, e.data(), 5, 7, D) == 5);
        }
        CHECK(PassFixMark(true, ev.data(), 6, 7, D) == 6);                 // the caller gives the latest begin: taken as given
    }
    printf(fails ? "hrw41_test: %d FAILED\n" : "hrw41_test: ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
