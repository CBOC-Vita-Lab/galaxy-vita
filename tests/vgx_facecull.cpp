// GXV_FACECULL (farlod32): vgx_facecull.h's view lists against a software model of what GX draws.
//   The oracle: a triangle's clip coordinates (the front end's own: x = p0 ex + p1 ez, y = p2 ey + p3 ez, w = -ez) give its winding on
//   the screen as the sign of det[(x, y, w) rows] (valid for the visible part of a triangle that crosses the eye plane too), turned by
//   the viewport's scale signs; GX_CULL_BACK draws the clockwise ones, GX_CULL_FRONT the counter-clockwise ones.
//   * for random closed and open meshes, random rigid matrices (rotated, scaled, mirrored), projections (plain, off-centre, mirrored)
//     and both cull modes: every triangle a list leaves out is culled by the oracle for the eye the list was made for AND for random
//     eyes within the list's margin (the matrix moved); a sphere seen with back culling from two radii or more loses 15-70 % of its indices;
//   * the list's triangles are the run's, in the run's order, with the vertex bytes of the run (compact, each used vertex once);
//   * the inverted list (the test picture) is exactly the complement;
//   * zero-area triangles are kept; EyeOf refuses a singular matrix; Handedness is 0 for a zero scale; Valid follows margin and side.
// Build: clang++ -std=c++17 -O1 -Iplatform/vgx/src tests/vgx_facecull.cpp -o vgx_facecull && ./vgx_facecull
#include "vgx_facecull.h"
#include <cstdio>
#include <cstdlib>

using namespace vgx;

namespace {
unsigned failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
uint32_t g_r = 2463534242u;
float Rnd() { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return (g_r & 0xFFFFFF) / 16777216.0f; }
struct V { float p[4]; uint8_t c[4]; float uv[2]; };
struct Mesh { std::vector<V> v; std::vector<uint16_t> i; };
Mesh Sphere(uint32_t rings, uint32_t segs, float r, float cx, bool open) {
    Mesh m;
    for (uint32_t ring = 0; ring <= rings; ++ring)
        for (uint32_t s = 0; s < segs; ++s) {
            const float th = 3.14159265f * (open ? 0.5f : 1.0f) * ring / rings, ph = 6.2831853f * s / segs;
            V v{};
            v.p[0] = cx + r * std::sin(th) * std::cos(ph); v.p[1] = r * std::cos(th); v.p[2] = r * std::sin(th) * std::sin(ph);
            v.c[0] = static_cast<uint8_t>(ring); v.c[1] = static_cast<uint8_t>(s);
            v.uv[0] = static_cast<float>(m.v.size());
            m.v.push_back(v);
        }
    for (uint32_t ring = 0; ring < rings; ++ring)
        for (uint32_t s = 0; s < segs; ++s) {
            const uint16_t a = static_cast<uint16_t>(ring * segs + s), b = static_cast<uint16_t>(ring * segs + (s + 1) % segs), c = static_cast<uint16_t>(a + segs), d = static_cast<uint16_t>(b + segs);
            for (uint16_t q : {a, b, c, c, b, d}) m.i.push_back(q); // (pole rows give zero-area triangles: kept)
        }
    return m;
}
struct Cam { float m[3][4]; float p[6]; float vpx, vpy; };
// model -> eye: a random rotation about a random axis, scale s (negative on x when mirror), then the eye at `eyeWorld`
Cam MakeCam(const float eyeModel[3], float scale, bool mirror, bool projMirror, bool offCentre, bool vpFlip) {
    // R = S * Rot; t = -R * eyeModel (so that the eye is at eyeModel in model space)
    const float ax[3] = {Rnd() - 0.5f, Rnd() - 0.5f, Rnd() - 0.5f};
    const float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f, x = ax[0] / l, y = ax[1] / l, z = ax[2] / l, a = Rnd() * 6.28f, c = std::cos(a), s = std::sin(a), t = 1 - c;
    float R[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y}, {t * x * y + s * z, t * y * y + c, t * y * z - s * x}, {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    Cam cam{};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) cam.m[r][k] = R[r][k] * scale * ((mirror && k == 0) ? -1.0f : 1.0f);
        cam.m[r][3] = -(cam.m[r][0] * eyeModel[0] + cam.m[r][1] * eyeModel[1] + cam.m[r][2] * eyeModel[2]);
    }
    cam.p[0] = projMirror ? -1.3f : 1.3f; cam.p[1] = offCentre ? 0.2f : 0.0f; cam.p[2] = 2.414f; cam.p[3] = offCentre ? -0.1f : 0.0f;
    cam.vpx = 320.0f; cam.vpy = vpFlip ? 228.0f : -228.0f;
    return cam;
}
// +1 counter-clockwise on the screen, -1 clockwise, 0 edge-on (within rounding)
int Winding(const Cam& cam, const float* p0, const float* p1, const float* p2) {
    double q[3][3];
    const float* v[3] = {p0, p1, p2};
    for (int i = 0; i < 3; ++i) {
        const double ex = double(cam.m[0][0]) * v[i][0] + double(cam.m[0][1]) * v[i][1] + double(cam.m[0][2]) * v[i][2] + cam.m[0][3];
        const double ey = double(cam.m[1][0]) * v[i][0] + double(cam.m[1][1]) * v[i][1] + double(cam.m[1][2]) * v[i][2] + cam.m[1][3];
        const double ez = double(cam.m[2][0]) * v[i][0] + double(cam.m[2][1]) * v[i][1] + double(cam.m[2][2]) * v[i][2] + cam.m[2][3];
        q[i][0] = (cam.p[0] * ex + cam.p[1] * ez) * (cam.vpx < 0 ? -1.0 : 1.0);
        q[i][1] = (cam.p[2] * ey + cam.p[3] * ez) * (cam.vpy > 0 ? -1.0 : 1.0); // (y up on the screen: GX's own scale is minus half the height)
        q[i][2] = -ez;
    }
    const double det = q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    const double mag = std::fabs(q[0][0] * q[1][1] * q[2][2]) + std::fabs(q[0][1] * q[1][2] * q[2][0]) + std::fabs(q[0][2] * q[1][0] * q[2][1]) + 1e-30;
    if (std::fabs(det) < 1e-9 * mag) return 0;
    return det > 0 ? 1 : -1;
}
bool MakeRun(const Mesh& m, facecull::Run& r) {
    r = facecull::Run();
    r.idx = m.i;
    r.vtx.assign(reinterpret_cast<const uint8_t*>(m.v.data()), reinterpret_cast<const uint8_t*>(m.v.data()) + m.v.size() * sizeof(V));
    r.stride = sizeof(V);
    r.vtxCount = static_cast<uint32_t>(m.v.size());
    return facecull::BuildPlanes(r.vtx.data(), r.vtxCount, r.stride, r.idx.data(), static_cast<uint32_t>(r.idx.size()), r.planes, r.c, &r.r);
}
}  // namespace

int main() {
    std::vector<uint16_t> oi, remap, oi2;
    std::vector<uint8_t> ov, ov2;
    uint64_t dropped = 0, checked = 0, wrong = 0, total = 0;
    double minShare = 1.0, maxShare = 0.0;
    for (int it = 0; it < 400; ++it) {
        const bool open = (it % 5) == 4;
        const float rad = 200.0f + Rnd() * 6000.0f;
        const Mesh mesh = Sphere(12 + it % 9, 16 + it % 13, rad, (Rnd() - 0.5f) * 40000.0f, open);
        facecull::Run run;
        Check(MakeRun(mesh, run), "the planes of a mesh are made");
        // the eye: from just outside the surface to 30 radii away (and sometimes inside)
        const float dist = rad * (it % 7 == 6 ? 0.3f : 1.05f + Rnd() * Rnd() * 30.0f);
        float dir[3] = {Rnd() - 0.5f, Rnd() - 0.5f, Rnd() - 0.5f};
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-6f;
        const float eyeModel[3] = {run.c[0] + dir[0] / dl * dist, run.c[1] + dir[1] / dl * dist, run.c[2] + dir[2] / dl * dist};
        const bool mirror = (it & 1) != 0, projMirror = (it & 2) != 0, offCentre = (it & 4) != 0, vpFlip = (it % 11) == 10, front = (it % 3) == 2;
        const float scale = 0.25f + Rnd() * 3.0f;
        Cam cam = MakeCam(eyeModel, scale, mirror, projMirror, offCentre, vpFlip);
        const float* rows[3] = {cam.m[0], cam.m[1], cam.m[2]};
        float eye[3];
        int det = 0;
        Check(facecull::EyeOf(rows, eye, &det), "the eye of a rigid matrix is found");
        const float tol = 1e-3f * (std::fabs(eyeModel[0]) + std::fabs(eyeModel[1]) + std::fabs(eyeModel[2]) + rad);
        Check(std::fabs(eye[0] - eyeModel[0]) < tol && std::fabs(eye[1] - eyeModel[1]) < tol && std::fabs(eye[2] - eyeModel[2]) < tol, "the eye is where the matrix puts it");
        Check(det == (mirror ? -1 : 1), "the determinant's sign follows the mirror");
        const int k = facecull::Handedness(det, cam.p[0], cam.p[2], cam.vpx, cam.vpy);
        const int side = front ? -k : k;
        const float reach = facecull::Reach(run, eye), margin = std::max(50.0f, 0.06f * reach), thr = facecull::Threshold(margin, reach);
        const uint32_t kept = facecull::Filter(run, eye, side, thr, false, oi, ov, remap);
        const uint32_t inv = facecull::Filter(run, eye, side, thr, true, oi2, ov2, remap);
        Check(kept + inv == mesh.i.size(), "the list and the inverted list are the run");
        total += mesh.i.size();
        if (!open && !front && dist >= rad * 2.0f) { const double sh = 1.0 - double(kept) / mesh.i.size(); minShare = std::min(minShare, sh); maxShare = std::max(maxShare, sh); }
        // the list: the run's triangles in order, vertex bytes the run's, each vertex once
        bool same = ov.size() % sizeof(V) == 0;
        size_t at = 0;
        std::vector<int> seen(mesh.v.size(), 0);
        for (size_t t = 0; t + 2 < oi.size() && same; t += 3) {
            // (find the next triangle of the run whose vertices have these bytes; uv[0] names the model vertex)
            uint16_t org[3];
            for (int c = 0; c < 3; ++c) {
                same = same && static_cast<size_t>(oi[t + c]) * sizeof(V) < ov.size();
                if (!same) break;
                V v;
                std::memcpy(&v, ov.data() + static_cast<size_t>(oi[t + c]) * sizeof(V), sizeof(V));
                org[c] = static_cast<uint16_t>(v.uv[0]);
                same = same && org[c] < mesh.v.size() && std::memcmp(&v, &mesh.v[org[c]], sizeof(V)) == 0;
            }
            while (same && at + 2 < mesh.i.size() && !(mesh.i[at] == org[0] && mesh.i[at + 1] == org[1] && mesh.i[at + 2] == org[2])) at += 3;
            same = same && at + 2 < mesh.i.size();
            at += 3;
        }
        for (size_t v = 0; v * sizeof(V) < ov.size(); ++v) { V q; std::memcpy(&q, ov.data() + v * sizeof(V), sizeof(V)); same = same && ++seen[static_cast<size_t>(q.uv[0])] == 1; }
        Check(same, "the list is the run's triangles in the run's order with the run's vertex bytes, each vertex once");
        // every left-out triangle is culled: for the eye of the list and for eyes within 0.9 of the margin
        for (int e = 0; e < 6; ++e) {
            float eye2[3] = {eyeModel[0], eyeModel[1], eyeModel[2]};
            if (e) {
                float d[3] = {Rnd() - 0.5f, Rnd() - 0.5f, Rnd() - 0.5f};
                const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f, len = (e == 1 ? 0.9f : 0.9f * Rnd()) * margin;
                for (int a = 0; a < 3; ++a) eye2[a] += d[a] / n * len;
            }
            const Cam cam2 = MakeCam(eye2, scale, mirror, projMirror, offCentre, vpFlip);
            // the triangles NOT in the list: the inverted list's (its vertices name the model's through uv[0])
            for (size_t t = 0; t + 2 < oi2.size(); t += 3) {
                V a, b, c;
                std::memcpy(&a, ov2.data() + static_cast<size_t>(oi2[t]) * sizeof(V), sizeof(V));
                std::memcpy(&b, ov2.data() + static_cast<size_t>(oi2[t + 1]) * sizeof(V), sizeof(V));
                std::memcpy(&c, ov2.data() + static_cast<size_t>(oi2[t + 2]) * sizeof(V), sizeof(V));
                const int w = Winding(cam2, a.p, b.p, c.p);
                // GX_CULL_BACK draws clockwise (-1): a left-out triangle must be counter-clockwise (+1), or edge-on; GX_CULL_FRONT the reverse
                const bool drawn = front ? w > 0 : w < 0;
                ++checked;
                if (drawn) ++wrong;
            }
            if (!e) dropped += oi2.size();
        }
    }
    Check(wrong == 0, "no left-out triangle would have been drawn (list eye and eyes within the margin, 400 meshes and cameras)");
    Check(minShare > 0.15 && maxShare < 0.70, "a closed sphere seen with back culling from two radii or more loses 15-70 % of its indices (small spheres: the 50-unit margin is a large share of them)");
    std::printf("400 cameras: %llu of %llu indices left out (%.0f%%), %llu triangle checks, %llu wrong; closed spheres, back culling, from 2 radii or more: %.0f%% .. %.0f%% left out\n",
                (unsigned long long)dropped, (unsigned long long)total, 100.0 * dropped / total, (unsigned long long)checked, (unsigned long long)wrong, 100.0 * minShare, 100.0 * maxShare);
    { // Valid, EyeOf's refusals, Handedness
        const Mesh mesh = Sphere(8, 12, 1000.0f, 0.0f, false);
        facecull::Run run;
        MakeRun(mesh, run);
        const float e0[3] = {5000, 0, 0}, e1[3] = {5000, 80, 0}, e2[3] = {5000, 120, 0};
        Check(!facecull::Valid(run, e0, 1), "no list: not valid");
        run.margin = 100.0f; run.side = 1;
        for (int a = 0; a < 3; ++a) run.eye[a] = e0[a];
        Check(facecull::Valid(run, e1, 1) && !facecull::Valid(run, e2, 1) && !facecull::Valid(run, e1, -1), "a list is valid within its margin and for its side only");
        const float z0[4] = {1, 2, 3, 4}, z1[4] = {2, 4, 6, 1}, z2[4] = {0, 0, 1, 0};
        const float* sing[3] = {z0, z1, z2};
        float eye[3];
        int det;
        Check(!facecull::EyeOf(sing, eye, &det), "a singular matrix has no eye point");
        Check(facecull::Handedness(1, 1.3f, 2.4f, 320.0f, -228.0f) == 1 && facecull::Handedness(-1, 1.3f, 2.4f, 320.0f, -228.0f) == -1 && facecull::Handedness(1, -1.3f, 2.4f, 320.0f, -228.0f) == -1 &&
              facecull::Handedness(1, 1.3f, 2.4f, 320.0f, 228.0f) == -1 && facecull::Handedness(1, 0.0f, 2.4f, 320.0f, -228.0f) == 0, "the handedness follows each sign; a zero scale is not known");
        std::vector<facecull::Plane> pl;
        float c[3], r;
        const uint16_t bad[3] = {0, 1, 999};
        Check(!facecull::BuildPlanes(run.vtx.data(), run.vtxCount, run.stride, bad, 3, pl, c, &r), "an index outside the block is refused");
        // a plain camera, back culling, a single triangle facing the eye and one facing away
        Mesh two;
        V v{};
        const float pts[6][3] = {{0, 0, 0}, {0, 100, 0}, {100, 0, 0}, {0, 0, 0}, {100, 0, 0}, {0, 100, 0}}; // first: n = -z ... clockwise from +z
        for (int i = 0; i < 6; ++i) { v.p[0] = pts[i][0]; v.p[1] = pts[i][1]; v.p[2] = pts[i][2]; v.uv[0] = static_cast<float>(i); two.v.push_back(v); two.i.push_back(static_cast<uint16_t>(i)); }
        facecull::Run tr;
        MakeRun(two, tr);
        const float eyeZ[3] = {30, 30, 4000};
        const uint32_t kept = facecull::Filter(tr, eyeZ, 1, facecull::Threshold(50.0f, facecull::Reach(tr, eyeZ)), false, oi, ov, remap);
        V first;
        std::memcpy(&first, ov.data(), sizeof(V));
        Check(kept == 3 && first.uv[0] == 0.0f, "seen from +z with back culling the clockwise triangle (0, +y, +x) stays, the counter-clockwise one goes");
    }
    std::printf(failures ? "vgx_facecull: %u FAILED\n" : "vgx_facecull: all passed\n", failures);
    return failures ? 1 : 0;
}
