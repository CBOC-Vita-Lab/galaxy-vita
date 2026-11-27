// hrw26 (GXV_HRW): host test of platform/vgx/src/vgx_hrw.h: the camera maths, the projection patch (against a direct projection with the camera of B),
// Plan Collect / Apply / Restore (byte-exact restore, identity untouched, HUD and orthographic blocks never patched, conflicts refused), the hazard scan, and the governor.
// Negative controls: a plan that patched a HUD block / left a block changed must be caught by the same checks (the checks are run on deliberately broken copies).
#include "../platform/vgx/src/vgx_hrw.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static unsigned rs = 12345;
static float rnd() { rs = rs * 1664525u + 1013904223u; return ((rs >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f; }
static void rot(float m[12], float yaw, float pitch, float tx, float ty, float tz) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float r[12] = {cy, 0, sy, tx, sy * sp, cp, -cy * sp, ty, -sy * cp, sp, cy * cp, tz};
    std::memcpy(m, r, sizeof r);
}
static void persp(float p[16], float n, float f, float fov) {
    std::memset(p, 0, 16 * sizeof(float));
    const float t = 1.0f / std::tan(fov * 0.5f);
    p[0] = t / 1.7778f; p[5] = -t; p[10] = (f + n) / (n - f) * 0.5f - 0.5f; p[11] = -1.0f; p[14] = f * n / (n - f);
}
static void xf(const float m[12], const float v[3], float o[3]) { for (int r = 0; r < 3; ++r) o[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3]; }
static void clip(const float p[16], const float e[3], float o[4]) { for (int r = 0; r < 4; ++r) o[r] = p[r] * e[0] + p[4 + r] * e[1] + p[8 + r] * e[2] + p[12 + r]; }
int main() {
    CHECK(hrw::ParseMode(nullptr) == 0 && hrw::ParseMode("1") == 1 && hrw::ParseMode("2") == 2 && hrw::ParseMode("3") == 3 && hrw::ParseMode("9") == 0 && hrw::ParseMode("12") == 0 && hrw::ParseMode("-1") == 0);
    // ---- maths: identical cameras give exactly the identity
    float vA[12], vB[12], d[12], iv[12], t[12];
    rot(vA, 0.3f, 0.1f, 10, -20, 500);
    CHECK(hrw::Delta(vA, vA, d) && hrw::IsIdent(d));
    // ---- Invert: V * V^-1 = I
    for (int i = 0; i < 200; ++i) {
        rot(vA, rnd() * 3, rnd() * 1.2f, rnd() * 1000, rnd() * 1000, rnd() * 1000);
        CHECK(hrw::Invert(vA, iv)); hrw::Mul(vA, iv, t);
        float e = 0; static const float one[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (int k = 0; k < 12; ++k) e = std::max(e, std::fabs(t[k] - one[k]) / ((k & 3) == 3 ? 1000.0f : 1.0f));
        CHECK(e < 2e-5f);
    }
    float sing[12] = {0}; CHECK(!hrw::Invert(sing, iv));
    // ---- re-projection equals projecting with camera B directly (the whole claim): clip_B(P*D4*e_A) == P*(V_B*w)
    float P[16]; persp(P, 100.0f, 100000.0f, 0.9f);
    double worst = 0;
    for (int i = 0; i < 2000; ++i) {
        rot(vA, rnd() * 3, rnd() * 0.8f, rnd() * 3000, rnd() * 3000, rnd() * 3000);
        rot(vB, 0, 0, 0, 0, 0);
        float yaw = rnd() * 0.02f, pit = rnd() * 0.02f; // up to ~1 deg
        float dd[12]; rot(dd, yaw, pit, rnd() * 20, rnd() * 20, rnd() * 20);
        hrw::Mul(dd, vA, vB);  // V_B = dd * V_A, so D should come back as dd
        CHECK(hrw::Delta(vA, vB, d));
        float w[3] = {rnd() * 5000, rnd() * 5000, rnd() * 5000}, eA[3], eB[3], c1[4], c2[4], PD[16];
        xf(vA, w, eA); xf(vB, w, eB);
        hrw::ProjMulD(P, d, PD);
        clip(PD, eA, c1); clip(P, eB, c2);
        if (c2[3] > 200.0f) { // in front of the camera: compare in NDC pixels (1280 wide)
            const double dx = (c1[0] / c1[3] - c2[0] / c2[3]) * 640.0, dy = (c1[1] / c1[3] - c2[1] / c2[3]) * 360.0;
            worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
        }
    }
    std::printf("hrw26 test: worst re-projection error vs direct projection: %.6f px (float, 2000 random cameras)\n", worst);
    CHECK(worst < 0.5);
    { // motion of a known delta
        float dd[12]; rot(dd, 0.0, 0.5f * 3.14159265f / 180.0f, 3, 4, 0);
        float r = 0, tr = 0; hrw::Motion(dd, &r, &tr);
        CHECK(std::fabs(r - 0.5f) < 0.01f); CHECK(std::fabs(tr - 5.0f) < 1e-4f);
    }
    { // actor delta: an actor that moves with the camera (dW = V^-1 dV V) needs ... check E = V_A dW V_A^-1 maps a point as the world move does
        float wA[12], wB[12], e[12]; rot(vA, 0.2f, 0.1f, 5, 6, 7); rot(wA, 0, 0, 100, 0, 0); rot(wB, 0, 0, 110, 0, 0);
        CHECK(hrw::ActorEyeDelta(vA, wA, wB, e));
        float p[3] = {100, 5, 5}, pw[3], pb[3], ea[3], eb[3], ee[3];
        xf(vA, p, ea); xf(e, ea, ee);
        float pwB[12]; hrw::Mul(wB, [&] { static float i[12]; hrw::Invert(wA, i); return i; }(), pwB); xf(pwB, p, pw); xf(vA, pw, eb); (void)pb;
        CHECK(std::fabs(ee[0] - eb[0]) < 1e-3f && std::fabs(ee[1] - eb[1]) < 1e-3f && std::fabs(ee[2] - eb[2]) < 1e-3f);
    }
    // ---- Plan on a synthetic buffer
    CmdBuf b;
    b.uni.assign(16 * 8, 0.0f);
    float O[16]; std::memset(O, 0, sizeof O); O[0] = 0.001f; O[5] = 0.002f; O[10] = -1; O[15] = 1;  // orthographic
    persp(P, 100.0f, 50000.0f, 1.0f);
    std::memcpy(b.uni.data() + 0, P, sizeof P);        // block 0: perspective, used by two draws
    std::memcpy(b.uni.data() + 16, O, sizeof O);       // block 16: ortho
    persp(P, 10.0f, 5000.0f, 1.2f);
    std::memcpy(b.uni.data() + 32, P, sizeof P);       // block 32: perspective flagged hud (never patched)
    std::memcpy(b.uni.data() + 48, P, sizeof P);       // block 48: perspective, second
    auto addDraw = [&](uint32_t blk, bool hud) {
        DrawRec dr; dr.ublock[UB_PROJ] = blk; dr.hud = hud ? 1 : 0;
        b.draws.push_back(dr);
        Cmd c; c.type = CMD_DRAW; c.arg = static_cast<uint32_t>(b.draws.size() - 1); b.cmds.push_back(c);
    };
    addDraw(0, false); addDraw(0, false); addDraw(16, false); addDraw(32, true); addDraw(48, false);
    UniArena orig = b.uni;
    hrw::Plan pl;
    CHECK(pl.Collect(b));
    CHECK(pl.refs.size() == 2 && pl.persp == 3 && pl.ortho == 1 && pl.hud == 1 && pl.draws == 5);
    const uint64_t h0 = hrw::HashBuf(b);
    float id[12]; hrw::Ident(id);
    pl.Apply(b, id); CHECK(b.uni == orig && hrw::HashBuf(b) == h0);  // D = identity: byte for byte
    rot(d, 0.01f, 0.005f, 3, 0, 1);
    pl.Apply(b, d);
    CHECK(b.uni != orig);
    CHECK(std::memcmp(b.uni.data() + 16, orig.data() + 16, 64) == 0);  // ortho untouched
    CHECK(std::memcmp(b.uni.data() + 32, orig.data() + 32, 64) == 0);  // hud untouched
    pl.Restore(b); CHECK(b.uni == orig && hrw::HashBuf(b) == h0);       // restored byte for byte
    // negative control: a buffer left patched must fail the equality check
    pl.Apply(b, d); CHECK(hrw::HashBuf(b) != h0); pl.Restore(b);
    // conflict: one block used as perspective by one draw and as hud by another
    addDraw(0, true);
    CHECK(!pl.Collect(b));
    b.draws.pop_back(); b.cmds.pop_back();
    // conflict: overlapping blocks
    addDraw(8, false);
    CHECK(!pl.Collect(b));
    b.draws.pop_back(); b.cmds.pop_back();
    // a block outside the arena
    addDraw(16 * 8 - 4, false);
    CHECK(!pl.Collect(b) && pl.badRange == 1);
    b.draws.pop_back(); b.cmds.pop_back();
    // ---- the hazard scan: a clean buffer has one display copy to be replayable
    hrw::Hazard hz; hrw::Scratch sc;
    CHECK(hrw::SkipInReplay(CMD_TEX_UPLOAD) && hrw::SkipInReplay(CMD_BUF_FREE) && !hrw::SkipInReplay(CMD_DRAW) && !hrw::SkipInReplay(CMD_COPY_DISP));
    // ---- the governor: engages after a run of slow native seconds where the pair fits; leaves when it stops fitting
    hrw::Governor g; g.StageBegin(0);
    hrw::GovSample s; s.play = true; s.nativeFps = 30; s.costs.gameDrawnMs = 21.6; s.costs.gameUndrawnMs = 8.7; s.costs.feMs = 19.9; s.costs.beMs = 7.5; s.costs.gpuMs = 14.5;
    int engaged = 0; double tm = 0;
    for (int i = 0; i < 40; ++i) { tm += 1.0; if (g.Tick(tm, s) == hrw::kGovEngage) { engaged = i; break; } }
    CHECK(engaged > 0); CHECK(g.Engaged());
    hrw::Governor g2; g2.StageBegin(0); s.costs.gpuMs = 19.2 + 6; s.nativeFps = 28; tm = 0;  // Good Egg-like: the GPU cannot draw two pictures per pair
    int e2 = 0; for (int i = 0; i < 60; ++i) { tm += 1.0; if (g2.Tick(tm, s) == hrw::kGovEngage) e2 = 1; }
    CHECK(e2 == 0);
    if (fails) { std::printf("hrw26 test: %d FAILED\n", fails); return 1; }
    std::printf("hrw26 test: all passed\n");
    return 0;
}
