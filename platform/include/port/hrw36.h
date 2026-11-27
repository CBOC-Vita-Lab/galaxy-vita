// warp36 (14 Nov 2026): the picture quality of the re-projected 60 (GXV_HRW=5, depth mode 4 "rotation + player box"). Pure logic (no GL, no game types, no clocks):
// host-tested by tests/hrw36_test.cpp (tools/test_hrw36.sh). The glue is platform/vgx/src/vgx_backend_gl.cpp ("warp36"), platform/src/gx/vgx_galaxy.cpp
// (port_hrw_undrawn_begin) and decomp/src/Game/Scene/GameScene.cpp (port_hrw_player36).
//
// The owner, 14 Nov, Bowser's level, running: "the terrain and the close coins look almost like I'm dizzy. The path I'm on gets blurry and I see double"; the Observatory,
// jumping: "a distortion around his feet and the ground where he jumps. Sort of looks like a bubble".
//
// What the code did. The second picture of a pair (B) is the first (A) re-projected by the camera's ROTATION only: every pixel is treated as a point at infinity. A point
// at view depth z is then left where A had it although the camera moved by t between the two turns: it is off by
//      F |t_sideways| / z  +  r |t_forward| / z   pixels      (F = the focal length in pixels, 0.5 * 960 * P[0] = 657 px at the game's 45 degree view; r = the pixel's distance from the picture's centre)
// The comment of mode 4 says "a few units a pair (1-2 px)"; the console logs say otherwise for a run: "camera change per pair ... max 11-18 units" at Honeyhive (2892b), the
// player 1200-1500 units from the camera. At 13 units a pair sideways: 28 px for a surface 300 units away, 8.5 px at 1000, 7 px at the player's own 1200, 1.7 px at 5000.
// So the far scenery and the camera's turn run at 60 a second while the ground and the coins near the player stand still in every second picture and jump two steps in
// the next: a 30 Hz picture inside a 60 Hz one, 7-28 px apart = the double path.
// The "bubble": the player's box (130 units + 16 px around his centre: 200 x 200 px in the Observatory) is drawn again on a plane that moves WITH HIM, so that he lands
// exactly. Everything else inside the box (the ground round his feet, his shadow) is carried along by his own move against the camera: nothing while he runs with the
// camera following, but 10-16 px upwards and a few per cent larger (the plane comes towards the camera) on the first turns of a jump, when the camera does not follow yet.
// Every second picture has it, the box's edge fades over 24 px: a soft-edged patch of lifted, magnified ground = the bubble.
//
// Three remedies, each its own key (all default 0 until a console check):
//  GXV_HRW36_PLANE   the scene is re-projected as the GROUND PLANE under the player where a pixel's ray meets it, at infinity elsewhere (Plane, below). A plane's inverse
//                    depth is linear over the screen, so the vertex shader of a 32 x 18 grid computes it and the pixel program is the same single projective read
//                    as GXV_HRW_WARP_PROJ's: no depth snapshot, no GPU cost a pixel. The ground and what stands on it then move in B as the camera's move asks.
//  GXV_HRW36_BOX     the player's box follows his body (feet to above his head, his width) instead of a square round his centre: no ground below his feet in it.
//  GXV_HRW36_PAR_MAX_PX  a pair whose ground parallax at the player is above this many pixels shows A again (a plain 30 for those pairs), with the edge rule's hysteresis.
// hold36 (15 Nov): GXV_HRW36_HOLD_TURNS (default 45) keeps GXV_HRW36_PLANE's plane through the turns without ground under him (HoldGround, below).
// plane37 (15 Nov): GXV_HRW36_PLANE_GATE keeps the plane off what the first picture's depth says is far behind it (the void past a platform's edge): Gate, below.
#pragma once

#include <algorithm>
#include <cmath>

namespace port {
namespace hrw36 {

// The grid the plane warp is drawn with (cells; the vertex shader evaluates the plane at each vertex, the source is interpolated projectively inside a cell: exact for
// the plane and for infinity, off only across the fade and the clamp, where a cell is 30 px wide).
constexpr int kGridX = 32, kGridY = 18;
constexpr int kGridVerts = kGridY * (2 * (kGridX + 1)) + (kGridY - 1) * 2;  // one triangle strip, two repeated vertices between rows
// The strip's vertices, x and y in -1..1 (out: 2 * kGridVerts floats).
inline void GridStrip(float* out) {
    int n = 0;
    const auto put = [&](int ix, int iy) {
        out[n++] = -1.0f + 2.0f * static_cast<float>(ix) / kGridX;
        out[n++] = -1.0f + 2.0f * static_cast<float>(iy) / kGridY;
    };
    for (int iy = 0; iy < kGridY; ++iy) {
        if (iy) put(0, iy);  // (the row's first vertex again: with the last of the row before, two triangles without area)
        for (int ix = 0; ix <= kGridX; ++ix) { put(ix, iy); put(ix, iy + 1); }
        if (iy + 1 < kGridY) put(kGridX, iy + 1);
    }
}

enum PlaneWhy : int { kPlaneOk = 0, kPlaneNoGround, kPlaneLow, kPlaneProjection, kPlaneWhyCount };
inline const char* PlaneWhyName(int w) {
    static const char* const n[kPlaneWhyCount] = {"ok", "no ground under the player", "camera at or under the ground plane", "projection"};
    return w >= 0 && w < kPlaneWhyCount ? n[w] : "?";
}
// The ground plane of a pair, in the form the vertex shader takes (its uniforms) and EvalK mirrors.
// Everything is in B's view space (the camera at the origin, looking down -z); a pixel is its NDC x, y (-1..1), its ray is (x * ray[0] + ray[2], y * ray[1] + ray[3], -1),
// a point at view depth `dist` on it is dist * ray, and k = 1 / dist is what the warp needs (zn = zn_at_infinity + f * k, f = P[14]).
struct Plane {
    bool on = false;
    float K[3] = {};       // the plane's inverse depth at the pixel: K[0] x + K[1] y + K[2] (under 0: the ray does not meet the plane)
    float kMax = 0.0f;     // the largest inverse depth used: nearer than that would move a pixel more than the pair's limit
    float ray[4] = {};
    float g[3] = {};       // the player's ground point
    float fadeA = 0.0f, fadeB = 0.0f;  // weight = clamp(fadeA - fadeB * (the hit's distance from g), 0, 1): 1 within the radius, 0 from twice the radius
    float camH = 0.0f;     // the camera's height above the plane (units)
};
// P: the picture's projection, column major (clip = P * view; P[11] = -1). g, n: the ground point under the player and the plane's normal (pointing up, any length).
// t: the translation of D = V_B * V_A^-1 (the camera's move, units). cw, ch: the picture in pixels. radius: units round g within which the plane is trusted (it
// fades to infinity by twice that). maxPx: no pixel is moved further than this against the rotation-only warp. minH: the camera must be this far above the plane.
inline int MakePlane(const float P[16], const float g[3], const float n[3], const float t[3], int cw, int ch, double radius, double maxPx, double minH, Plane& out) {
    out = Plane{};
    const double p0 = P[0], p5 = P[5];
    if (!(std::fabs(p0) > 1e-6) || !(std::fabs(p5) > 1e-6) || P[11] != -1.0f || cw <= 0 || ch <= 0) return kPlaneProjection;
    const double nl = std::sqrt(static_cast<double>(n[0]) * n[0] + static_cast<double>(n[1]) * n[1] + static_cast<double>(n[2]) * n[2]);
    const double gl = std::sqrt(static_cast<double>(g[0]) * g[0] + static_cast<double>(g[1]) * g[1] + static_cast<double>(g[2]) * g[2]);
    if (!(nl > 1e-6) || !std::isfinite(nl) || !std::isfinite(gl) || !(gl < 1.0e5) || !(radius > 1.0)) return kPlaneNoGround;
    const double nx = n[0] / nl, ny = n[1] / nl, nz = n[2] / nl;
    const double c = nx * g[0] + ny * g[1] + nz * g[2];  // (the plane: n . X = c; the camera, at the origin, is -c above it)
    if (!(-c >= minH) || !(-c >= 1.0)) return kPlaneLow;
    out.K[0] = static_cast<float>(nx / (p0 * c));
    out.K[1] = static_cast<float>(ny / (p5 * c));
    out.K[2] = static_cast<float>((nx * P[8] / p0 + ny * P[9] / p5 - nz) / c);
    out.ray[0] = static_cast<float>(1.0 / p0);
    out.ray[1] = static_cast<float>(1.0 / p5);
    out.ray[2] = static_cast<float>(P[8] / p0);
    out.ray[3] = static_cast<float>(P[9] / p5);
    for (int i = 0; i < 3; ++i) out.g[i] = g[i];
    out.fadeA = 2.0f;
    out.fadeB = static_cast<float>(1.0 / radius);
    // a point at inverse depth k moves about k * (F |t_xy| + r |t_z|) pixels against a point at infinity; r is at most the picture's half diagonal
    const double F = 0.5 * cw * std::fabs(p0), rMax = 0.5 * std::sqrt(static_cast<double>(cw) * cw + static_cast<double>(ch) * ch);
    const double per = F * std::sqrt(static_cast<double>(t[0]) * t[0] + static_cast<double>(t[1]) * t[1]) + rMax * std::fabs(static_cast<double>(t[2]));
    double kMax = 1.0 / 32.0;  // (nothing nearer than 32 units, the depth code's near end)
    if (per > 1e-9 && maxPx / per < kMax) kMax = maxPx / per;
    out.kMax = static_cast<float>(kMax);
    out.camH = static_cast<float>(-c);
    for (int i = 0; i < 3; ++i) if (!std::isfinite(out.K[i])) return kPlaneProjection;
    out.on = true;
    return kPlaneOk;
}
// The inverse depth the warp gives the pixel (0 = infinity). The vertex shader's arithmetic, line for line (kVsWarpPlane).
inline double EvalK(const Plane& p, double x, double y) {
    const double k = std::clamp(static_cast<double>(p.K[0]) * x + p.K[1] * y + p.K[2], 0.0, static_cast<double>(p.kMax));
    const double rx = x * p.ray[0] + p.ray[2], ry = y * p.ray[1] + p.ray[3];
    const double vx = rx - p.g[0] * k, vy = ry - p.g[1] * k, vz = -1.0 - p.g[2] * k;  // ((the hit) - g) * k
    const double w = std::clamp(p.fadeA - p.fadeB * std::sqrt(vx * vx + vy * vy + vz * vz) / std::max(k, 1e-9), 0.0, 1.0);
    return k * w;
}
// The plane's own inverse depth at the pixel without the clamp and the fade (the log: "pairs whose ground under the player was nearer than the limit").
inline double RawK(const Plane& p, double x, double y) { return static_cast<double>(p.K[0]) * x + p.K[1] * y + p.K[2]; }

// plane37 (15 Nov 2026), GXV_HRW36_PLANE_GATE: the plane only where the first picture HAS a surface about as near as the plane says.
// The console's flip-books (Honeyhive, c174): with the plane the ground band's 30 Hz jump went from 3.1 to 0.85 px (half size), but the stars over the void beyond
// the honey platform's edge, which stood still without it, moved 1.7 px in every second picture and back: a ray through the void past the edge still meets the
// EXTENDED plane inside the fade's radius, so the sky there was re-projected as ground. Galaxy is floating platforms over space: every platform edge has it.
// The gate: the pixel program reads the first picture's own depth d (the EFB's depth as a texture: window depth, 0 near .. 1 far) at the source the plane predicts,
// and the plane's inverse depth k is used in full where the surface there is no farther than (1 + 1/sharp) / ratio of the plane's distance (ratio 1.5, sharp 4:
// within 1.2 times), not at all where it is farther than `ratio` times it (the void, the sky, a platform far below), linearly between:
//      kA = (2 d - 1 + e) / f           (zn = 2 d - 1 = f / dist - e: the first picture's inverse depth; e = P[10], f = P[14])
//      m  = clamp(sharp * (ratio * kA / max(k, kMin) - 1), 0, 1)  =  clamp(d * (a / kg) + (b / kg - sharp), 0, 1)      (kg = max(k, kMin))
// a / kg and b / kg - sharp are made per vertex of the grid (two more interpolated numbers), the pixel program is one multiply-add and a clamp on the depth read.
// kMin (kMax / 1024: a plane that moves a pixel 0.04 px at most) keeps the two coefficients finite at the plane's horizon.
// What is nearer than the plane (the player, coins, walls) passes, as before the gate. k: the plane's inverse depth after its near limit, before its fade.
struct Gate {
    bool on = false;
    float a = 0.0f, b = 0.0f, sharp = 0.0f, kMin = 0.0f;  // the vertex shader's u_g
};
inline Gate MakeGate(const float P[16], float kMax, double ratio, double sharp) {
    Gate g;
    const double e = P[10], f = P[14];
    if (!(std::fabs(f) > 1e-9) || !(kMax > 0.0f) || !(ratio >= 1.0) || !(sharp > 0.0) || P[11] != -1.0f) return g;
    g.a = static_cast<float>(sharp * ratio * 2.0 / f);
    g.b = static_cast<float>(sharp * ratio * (e - 1.0) / f);
    g.sharp = static_cast<float>(sharp);
    g.kMin = kMax / 1024.0f;
    g.on = std::isfinite(g.a) && std::isfinite(g.b) && g.kMin > 0.0f;
    return g;
}
// The two interpolated coefficients at a grid vertex whose plane has the inverse depth k (kVsWarpGate, line for line; single precision as the shader's).
inline void GateCoef(const Gate& g, float k, float& cx, float& cy) {
    const float ig = 1.0f / std::max(k, g.kMin);
    cx = g.a * ig;
    cy = g.b * ig - g.sharp;
}
// The weight of the plane at a pixel whose first-picture depth read is d (kFsWarpGate).
inline float GateM(float cx, float cy, float d) { return std::clamp(d * cx + cy, 0.0f, 1.0f); }
// The plane's inverse depth before the fade, as the gate compares it (EvalK's first line).
inline double ClampK(const Plane& p, double x, double y) { return std::clamp(static_cast<double>(p.K[0]) * x + p.K[1] * y + p.K[2], 0.0, static_cast<double>(p.kMax)); }
// The inverse depth the gated warp gives the pixel: EvalK times the gate's weight for the depth d read at the plane's source.
inline double EvalKGated(const Plane& p, const Gate& g, double x, double y, double d) {
    float cx, cy;
    GateCoef(g, static_cast<float>(ClampK(p, x, y)), cx, cy);
    return EvalK(p, x, y) * GateM(cx, cy, static_cast<float>(d));
}
// plane38 (15 Nov 2026): the gate from a small MASK made at the first picture's display copy instead of the EFB's depth read while the second picture is drawn.
// Console c175: with the depth read at the second picture's draw EVERY pixel went to infinity (ground band 3.09 px = the plane off, void 0.01 px): the depth
// read there was the far end. c177's probe found why: the game clears the depth itself at the end of its 3D (GameScene::draw3D: MR::clearZBuffer), so neither
// the display copy nor the second picture has any scene depth to read. The mask is therefore made INSIDE the first picture, before that clear (the back
// end's PlanFrame finds the place): a quarter-size picture of the depth as the depth modes' code (DepthCode: (log2(dist) - 5) / 15 in 8 bits: steps of
// 0.059 octave = 4 % of the distance over 32 .. 1M units, whatever the projection's near and far), nearest of four taps.
// The gate in the code's terms (full weight up to ratio / (1 + 1 / sharp) = 1.2 times the plane's distance, none from `ratio` = 1.5 times it):
//      m = clamp((log2(ratio) - (15 code + 5) - log2(kg)) / w, 0, 1)  =  clamp(code * a + (b - sharp * log2(kg)), 0, 1)         (w = log2(1 + 1 / sharp), kg = max(k, kMin))
// The Gate's fields then hold a = -15 / w, b = (log2(ratio) - 5) / w, sharp = 1 / w.
inline double DepthCode(double dist) { return std::clamp((std::log2(std::max(dist, 1.0)) - 5.0) / 15.0, 0.0, 1.0); }
inline Gate MakeGateLog(float kMax, double ratio, double sharp) {
    Gate g;
    if (!(kMax > 0.0f) || !(ratio >= 1.0) || !(sharp > 0.0)) return g;
    const double w = std::log2(1.0 + 1.0 / sharp);
    g.a = static_cast<float>(-15.0 / w);
    g.b = static_cast<float>((std::log2(ratio) - 5.0) / w);
    g.sharp = static_cast<float>(1.0 / w);
    g.kMin = kMax / 1024.0f;
    g.on = std::isfinite(g.a) && std::isfinite(g.b) && g.kMin > 0.0f;
    return g;
}
// (the vertex shader's mask form, line for line; the pixel program's GateM is the same multiply-add on the mask's code)
inline void GateCoefLog(const Gate& g, float k, float& cx, float& cy) {
    cx = g.a;
    cy = g.b - g.sharp * std::log2(std::max(k, g.kMin));
}
inline double EvalKGatedLog(const Plane& p, const Gate& g, double x, double y, double code) {
    float cx, cy;
    GateCoefLog(g, static_cast<float>(ClampK(p, x, y)), cx, cy);
    return EvalK(p, x, y) * GateM(cx, cy, static_cast<float>(code));
}
// The window depth (0..1) of a point `dist` units in front of the camera (the tests, the logs).
inline double WindowDepth(const float P[16], double dist) { return 0.5 * (static_cast<double>(P[14]) / dist - static_cast<double>(P[10])) + 0.5; }

// How far (pixels) the rotation-only warp leaves a still world point from where A showed it: the point X (B's view space), d = V_B * V_A^-1 (3 x 4, row major).
// A saw it at D^-1 X = R^T (X - t); the rotation-only warp looks R^T X up. Under 0: the point is not in front of both cameras.
inline double ParallaxPx(const float P[16], const float d[12], const float X[3], int cw, int ch) {
    double a[3], b[3];
    const double q[3] = {X[0] - static_cast<double>(d[3]), X[1] - static_cast<double>(d[7]), X[2] - static_cast<double>(d[11])};
    for (int c = 0; c < 3; ++c) {
        a[c] = d[0 * 4 + c] * q[0] + d[1 * 4 + c] * q[1] + d[2 * 4 + c] * q[2];
        b[c] = static_cast<double>(d[0 * 4 + c]) * X[0] + static_cast<double>(d[1 * 4 + c]) * X[1] + static_cast<double>(d[2 * 4 + c]) * X[2];
    }
    if (!(a[2] < -1.0) || !(b[2] < -1.0)) return -1.0;
    const auto px = [&](const double* v, double& x, double& y) {
        x = ((P[0] * v[0] + P[8] * v[2]) / -v[2] * 0.5 + 0.5) * cw;
        y = ((P[5] * v[1] + P[9] * v[2]) / -v[2] * 0.5 + 0.5) * ch;
    };
    double ax, ay, bx, by;
    px(a, ax, ay);
    px(b, bx, by);
    const double e = std::hypot(ax - bx, ay - by);
    return std::isfinite(e) ? e : -1.0;
}

// GXV_HRW36_PAR_MAX_PX: pairs above the limit show A again; warping resumes under 75 % of it (the edge rule's hysteresis: no flutter at the limit).
struct ParGate {
    bool off = false;
    bool Update(double px, double maxPx) {
        if (!(maxPx > 0.0)) { off = false; return false; }
        if (px > maxPx) off = true;
        else if (px < maxPx * 0.75) off = false;
        return off;
    }
};

// GXV_HRW36_BOX: the player's box from his body. (cx, cy): his centre in B's picture, pixels; (fx, fy): his feet; pxPerUnit: a unit at his distance in pixels.
// The box runs along the line feet -> centre -> head (head = centre + headK * (centre - feet): his cap and raised arms), footPx beyond the feet, halfW units + padPx to
// either side; the rectangle is that oriented strip's bounding box (he may stand sideways or upside down on a planet). Feet and centre on one pixel (the camera looks
// along his axis): a square of the width. Even on every side, inside the picture; ok false: under 8 px.
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool ok = false;
};
inline int EvenBoxEdge(double value, int extent, bool upper) {
    const double edge = upper ? std::ceil(value) + 1.0 : std::floor(value);
    // Clamp before converting: finite world coordinates can project well
    // outside the range of an int on a camera transition.
    return static_cast<int>(std::min(std::max(0.0, edge), static_cast<double>(extent))) & ~1;
}
inline Box ShapedBox(double cx, double cy, double fx, double fy, double pxPerUnit, double halfW, double headK, double footPx, double padPx, int cw, int ch) {
    Box b;
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(fx) || !std::isfinite(fy) || !(pxPerUnit > 0.0) || cw <= 0 || ch <= 0) return b;
    const double hw = std::clamp(halfW * pxPerUnit + padPx, 12.0, 280.0);
    double ux = cx - fx, uy = cy - fy;
    const double len = std::hypot(ux, uy);
    double lo[2], hi[2];
    if (len < 2.0) {
        lo[0] = cx - hw; hi[0] = cx + hw; lo[1] = cy - hw; hi[1] = cy + hw;
    } else {
        ux /= len; uy /= len;
        const double headLen = std::min(len * headK, 280.0), foot = std::min(len + footPx, 280.0);
        const double ex = cx - ux * foot, ey = cy - uy * foot, hx = cx + ux * headLen, hy = cy + uy * headLen;  // (the strip's two ends)
        const double wx = -uy * hw, wy = ux * hw;
        lo[0] = std::min(std::min(ex - wx, ex + wx), std::min(hx - wx, hx + wx)); hi[0] = std::max(std::max(ex - wx, ex + wx), std::max(hx - wx, hx + wx));
        lo[1] = std::min(std::min(ey - wy, ey + wy), std::min(hy - wy, hy + wy)); hi[1] = std::max(std::max(ey - wy, ey + wy), std::max(hy - wy, hy + wy));
    }
    b.x0 = EvenBoxEdge(lo[0], cw, false);
    b.x1 = EvenBoxEdge(hi[0], cw, true);
    b.y0 = EvenBoxEdge(lo[1], ch, false);
    b.y1 = EvenBoxEdge(hi[1], ch, true);
    b.ok = b.x1 - b.x0 >= 8 && b.y1 - b.y0 >= 8;
    return b;
}

// hold36 (15 Nov 2026), GXV_HRW36_HOLD_TURNS: the ground plane through a jump. The console, 15 Nov, GXV_HRW36_PLANE=1: 157-368 of 506-842 pairs a stage had "no ground
// under him" (Bowser's Star Reactor: 338 of 506): the game has no shadow polygon for him on those turns (Mario::calcShadowPos: nothing within 2400 units down his
// gravity, or the point out of the -200..1500 range), the pair is warped by the rotation only and the ground near him is a 30 again: it changes between corrected
// and doubled as he jumps. HoldGround keeps the last ground point and its plane's normal and hands them over while he is in the air, for at most `holdTurns` turns
// after the turn they were seen on, as long as the plane can still be the ground he left:
//   - he is still -200..1500 units above the remembered point along his gravity NOW (the range of a turn with ground);
//   - his gravity is within kHoldGravityCos (25 degrees) of what it was then (a hop to another planet turns it);
//   - the scene, the stage and the scenario are the same (`key`), he is alive, and he has not been moved more than kHoldTeleport units in one turn (a warp, a
//     restart at a checkpoint); the step is called every turn or nearly (a gap over kHoldGapTurns: a demo, a pause in the pairs);
//   - ground38 (15 Nov): ground that is a moving collision (`moving`: a lift, a turning planetoid; many of the game's grounds are) is remembered too, but held for
//     kHoldMovingTurns at most, and the caller gives the point where that collision has carried it by now (`gMoved`): the plane goes with the ground he left. More
//     than kHoldMovedMax units from where it was seen: forgotten. (hold36 remembered nothing from a moving ground: the console's c174 had 82 held pairs of 1049.)
// Each of those forgets the hold but the height range (he may come back into it). Pure: the game thread calls Step once a drawn turn (GameScene.cpp,
// port_hrw_player36); `turn` is any counter that goes up by one a game turn (differences only: it may wrap).
constexpr double kHoldGravityCos = 0.9063;   // cos 25 degrees
constexpr double kHoldTeleport = 1500.0;     // units in one turn
constexpr unsigned kHoldGapTurns = 8;
constexpr int kHoldMovingTurns = 15;         // a ground that moves is held this long at most
constexpr double kHoldMovedMax = 300.0;      // and not further than this from where it was seen
constexpr double kHoldBelow = -200.0, kHoldAbove = 1500.0;  // (port_hrw_player36's range for a turn with ground)
enum HoldResult : int { kHoldNone = 0, kHoldNow = 1, kHoldHeld = 2 };  // no ground for this pair / this turn's own ground / the remembered ground (g, n, up, poly below)
enum HoldWhy : int { kHoldWhyNone = 0, kHoldWhyOff, kHoldWhyNothing, kHoldWhyExpired, kHoldWhyGravity, kHoldWhyRange, kHoldWhyTeleport, kHoldWhyScene, kHoldWhyDead, kHoldWhyGap, kHoldWhyMoving, kHoldWhyCount };
struct HoldGround {
    bool have = false, poly = false;   // a ground is remembered; its normal n is a polygon's face normal (else his up on that turn)
    bool movingGround = false;         // it was on a moving collision
    float g[3] = {}, n[3] = {}, up[3] = {};  // the ground point (where the moving collision has it now), the plane's normal, his up vector on the turn it was seen (world space)
    float g0[3] = {};                  // the ground point as seen
    unsigned seen = 0;                 // that turn
    bool stepped = false;              // feet / last / key are of an earlier step
    float feet[3] = {};
    unsigned last = 0, key = 0;
    int why = kHoldWhyNone;            // why the last step forgot or refused (a log, the tests)
    void Forget(int w = kHoldWhyNone) { have = false; why = w; }
    // turn: this game turn. sceneKey: changes with the scene, the stage or the scenario. feetNow, upNow: his feet and his up vector (unit) now. ground: this turn
    // has a valid ground point gNow (within the range); polyN: its polygon's face normal or null; moving: that polygon belongs to a collision that moved this turn.
    // dead: he is dying or dead. holdTurns: GXV_HRW36_HOLD_TURNS (0 = never hold). gMoved: on a turn without ground, where the moving collision the remembered
    // point was on has it now (null: not known, or the ground did not move: the point as seen).
    int Step(unsigned turn, unsigned sceneKey, const float feetNow[3], const float upNow[3], bool ground, const float gNow[3], const float* polyN, bool moving, bool dead, int holdTurns,
             const float* gMoved = nullptr) {
        why = kHoldWhyNone;
        bool finite = true;
        for (int i = 0; i < 3; ++i) finite = finite && std::isfinite(feetNow[i]) && std::isfinite(upNow[i]);
        if (stepped && have) {
            const double dx = static_cast<double>(feetNow[0]) - feet[0], dy = static_cast<double>(feetNow[1]) - feet[1], dz = static_cast<double>(feetNow[2]) - feet[2];
            if (sceneKey != key) Forget(kHoldWhyScene);
            else if (turn - last > kHoldGapTurns) Forget(kHoldWhyGap);
            else if (!finite || !(dx * dx + dy * dy + dz * dz <= kHoldTeleport * kHoldTeleport)) Forget(kHoldWhyTeleport);
        }
        if (have && dead) Forget(kHoldWhyDead);
        stepped = true;
        last = turn;
        key = sceneKey;
        for (int i = 0; i < 3; ++i) feet[i] = feetNow[i];
        if (ground) {
            if (dead || !finite) { Forget(kHoldWhyDead); return kHoldNow; }
            have = true;
            poly = polyN != nullptr;
            movingGround = moving;
            for (int i = 0; i < 3; ++i) { g[i] = g0[i] = gNow[i]; up[i] = upNow[i]; n[i] = polyN ? polyN[i] : upNow[i]; }
            seen = turn;
            return kHoldNow;
        }
        if (!have) { if (why == kHoldWhyNone) why = holdTurns > 0 ? kHoldWhyNothing : kHoldWhyOff; return kHoldNone; }
        if (holdTurns <= 0) { why = kHoldWhyOff; return kHoldNone; }
        if (turn - seen > static_cast<unsigned>(movingGround ? std::min(holdTurns, kHoldMovingTurns) : holdTurns)) { Forget(kHoldWhyExpired); return kHoldNone; }
        if (movingGround && gMoved) {
            const double mx = static_cast<double>(gMoved[0]) - g0[0], my = static_cast<double>(gMoved[1]) - g0[1], mz = static_cast<double>(gMoved[2]) - g0[2];
            if (!(mx * mx + my * my + mz * mz <= kHoldMovedMax * kHoldMovedMax)) { Forget(kHoldWhyMoving); return kHoldNone; }  // (also a point that is not a number)
            for (int i = 0; i < 3; ++i) g[i] = gMoved[i];
        }
        const double dt = static_cast<double>(upNow[0]) * up[0] + static_cast<double>(upNow[1]) * up[1] + static_cast<double>(upNow[2]) * up[2];
        if (!(dt >= kHoldGravityCos)) { Forget(kHoldWhyGravity); return kHoldNone; }
        const double h = (static_cast<double>(feetNow[0]) - g[0]) * upNow[0] + (static_cast<double>(feetNow[1]) - g[1]) * upNow[1] + (static_cast<double>(feetNow[2]) - g[2]) * upNow[2];
        if (!(h > kHoldBelow && h < kHoldAbove)) { why = kHoldWhyRange; return kHoldNone; }  // (not forgotten: a wall jump may bring him back within the turns left)
        return kHoldHeld;
    }
};

// The "hrw: warp36" line's counters (the render thread; since the last line).
struct Stats {
    unsigned n = 0, bins[5] = {};        // pairs measured; ground parallax at the player < 2, < 4, < 8, < 16, >= 16 px
    double sum = 0.0, max = 0.0;
    unsigned planePairs = 0, planeWhy[kPlaneWhyCount] = {}, planeNoInfo = 0, planeHud = 0, planeMargin = 0, planeProgram = 0, planeSource = 0, planeNear = 0, planeDraws = 0;
    unsigned planeRampUp = 0, planeRampDown = 0;  // warpsteady31: plane pairs below full weight (GXV_HRW_PLANE_RAMP); pairs drawn on the last ground, with no ground of their own
    unsigned planeHeld = 0;              // of planePairs: on the ground remembered through the air (HoldGround, bit 8)
    unsigned planeNoPlayer = 0, planeNoPoly = 0, planeRange = 0;  // planeNoInfo by its reason: no player data for the pair / no shadow polygon under him / the shadow point out of range (bit 16)
    unsigned planeFeet = 0, planeHeldMoving = 0;  // of planePairs: on his ground polygon at his feet (ground38, bit 32: no shadow polygon); of planeHeld: the ground was a moving collision (bit 64)
    unsigned gated = 0;
    unsigned gatePairs = 0, gateDraws = 0, gateCleared = 0, gateProgram = 0, gateNoTex = 0;
    unsigned gateNoMask = 0, masks = 0;  // (plane38: plane pairs whose first picture has no depth mask; masks made)  // (plane37, GXV_HRW36_PLANE_GATE) plane pairs with the depth gate, drawn so; not: the first picture's copy cleared the depth / its program / no depth texture at the draw
    unsigned boxes = 0, boxNoFeet = 0;
    double boxPx = 0.0, boxOldPx = 0.0;
    void Parallax(double px) {
        ++n;
        sum += px;
        if (px > max) max = px;
        ++bins[px < 2.0 ? 0 : px < 4.0 ? 1 : px < 8.0 ? 2 : px < 16.0 ? 3 : 4];
    }
};

}  // namespace hrw36
}  // namespace port
