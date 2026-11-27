// GXV_FLUSH_OFFSCREEN (gamepar): host check that port::viewcone::Outside never calls a sphere outside when any point of it is in the
// picture (the game's perspective: GX view space, camera down -z, fovy, aspect). Random cameras and spheres; every sphere is sampled (its
// centre, 26 surface directions, 200 random interior points). "neg": the test calls Outside with a third of the radius and must fail.
#include "port/view_cone.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }
static void lookAt(float m[3][4], const float eye[3], const float at[3], const float up0[3]) {
    float f[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};  // (GX: z axis points from the target to the eye)
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++) f[i] /= l;
    float x[3] = {up0[1] * f[2] - up0[2] * f[1], up0[2] * f[0] - up0[0] * f[2], up0[0] * f[1] - up0[1] * f[0]};
    l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= l;
    float y[3] = {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
    const float* rows[3] = {x, y, f};
    for (int r = 0; r < 3; r++) {
        m[r][0] = rows[r][0]; m[r][1] = rows[r][1]; m[r][2] = rows[r][2];
        m[r][3] = -(rows[r][0] * eye[0] + rows[r][1] * eye[1] + rows[r][2] * eye[2]);
    }
}
static bool inPicture(const float m[3][4], float t, float aspect, const float p[3]) {
    const float vx = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
    const float vy = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
    const float vz = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
    const float d = -vz;
    return d > 1e-3f && fabsf(vx) <= d * t * aspect && fabsf(vy) <= d * t;
}
int main(int argc, char** argv) {
    const bool neg = argc > 1 && strcmp(argv[1], "neg") == 0;
    unsigned long long spheres = 0, outside = 0, bad = 0;
    srand(12345);
    for (int cam = 0; cam < 2000; cam++) {
        const float eye[3] = {frand(-5000, 5000), frand(-5000, 5000), frand(-5000, 5000)};
        const float at[3] = {eye[0] + frand(-1, 1), eye[1] + frand(-1, 1), eye[2] + frand(-1, 1)};
        float up[3] = {frand(-1, 1), frand(-1, 1), frand(-1, 1)};
        float m[3][4];
        lookAt(m, eye, at, up);
        const float fovy = frand(20.0f, 100.0f);
        const float aspect = cam & 1 ? 16.0f / 9.0f : frand(1.0f, 2.0f);
        port::viewcone::Cone c;
        port::viewcone::Make(&c, m, fovy, aspect);
        const float t = tanf(fovy * (3.14159265f / 360.0f));
        for (int s = 0; s < 400; s++) {
            const float r = frand(50.0f, 800.0f);
            const float dist = frand(0.0f, 8000.0f);
            float dir[3] = {frand(-1, 1), frand(-1, 1), frand(-1, 1)};
            float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const float ctr[3] = {eye[0] + dir[0] / l * dist, eye[1] + dir[1] / l * dist, eye[2] + dir[2] / l * dist};
            const bool out = port::viewcone::Outside(c, ctr[0], ctr[1], ctr[2], neg ? r * 0.3f : r);
            spheres++;
            if (!out) continue;
            outside++;
            bool seen = inPicture(m, t, aspect, ctr);
            for (int k = 0; k < 26 + 200 && !seen; k++) {
                float v[3];
                if (k < 26) {
                    const int a = k < 13 ? k : k + 1;  // (27 lattice directions without the centre)
                    v[0] = (float)(a % 3 - 1); v[1] = (float)((a / 3) % 3 - 1); v[2] = (float)(a / 9 - 1);
                    const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    for (int i = 0; i < 3; i++) v[i] = v[i] / vl * r;
                } else {
                    do { v[0] = frand(-r, r); v[1] = frand(-r, r); v[2] = frand(-r, r); } while (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > r * r);
                }
                const float p[3] = {ctr[0] + v[0], ctr[1] + v[1], ctr[2] + v[2]};
                seen = inPicture(m, t, aspect, p);
            }
            if (seen) {
                if (bad++ < 3) printf("  sphere called outside but a point of it is in the picture (camera %d, sphere %d, r %.1f)\n", cam, s, r);
            }
        }
    }
    printf("view_cone%s: %llu spheres, %llu called outside, %llu wrong\n", neg ? " (neg)" : "", spheres, outside, bad);
    if (neg) {
        printf("%s\n", bad ? "view_cone neg: FAIL as it must (the check sees a too small radius)" : "view_cone neg: PASSED, which it must not");
        return bad ? 0 : 1;
    }
    printf("%s\n", bad ? "view_cone: FAIL" : "view_cone: PASS");
    return bad ? 1 : 0;
}
