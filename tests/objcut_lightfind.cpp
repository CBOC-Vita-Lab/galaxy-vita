// objcut host test (tools/test_objcut_lightfind.sh): GXV_OC_LIGHTFIND against the original walk, compiling the real LightAreaHolder.cpp,
// AreaObj.cpp, AreaForm.cpp and LightArea.cpp. Random scenarios: cubes / spheres / cylinders, random validity / wake / follow changes
// through the real writers (validate, invalidate, awake, sleep, setFollowMtx, the follower's _15), positions drawn from a small set so
// that most lookups repeat. Every lookup runs portFindIn and the original find_in; the pointers must be equal.
// "mutation": a flag written behind the writers' back (raw store) must be detected (proves the generation is what protects the memo).
// argv[1] = 1 (memo) or 2 (verify mode: the in-code mismatch count must stay 0).
#include "Game/AreaObj/LightAreaHolder.hpp"
#include "Game/AreaObj/LightArea.hpp"
#include "Game/AreaObj/AreaForm.hpp"
#include "Game/NameObj/NameObj.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int port_on_frame_thread(void) { return 1; }
extern "C" int64_t port_host_time_ns(void) { static int64_t t = 0; t += 1000000000ll; return t; }  // (1 s a call: the log fires)
extern "C" void port_log_summary(const char* fmt, ...) { (void)fmt; }
extern "C" void port_logic_count(int, unsigned) {}
extern "C" int port_move_sys_flag = 0;
extern "C" void port_move_sys_begin(int) {}
extern "C" void port_move_sys_end(int) {}
// (logic23: GXV_LOGIC_PROF's hooks, off here)
extern "C" int port_logic_prof_flag = 0;
extern "C" void port_logic_prof_sys_begin(int) {}
extern "C" void port_logic_prof_sys_end(int) {}
extern "C" void port_logic_prof_count(int, unsigned) {}
extern "C" int port_hl_areacube_on(void) { return 1; }
extern "C" void port_hl_verify_area(int, int, int, const char*, const float*, const float*) {}
namespace MR {
void makeMtxRotate(MtxPtr m, f32 rx, f32 ry, f32 rz) {
    const float k = 65536.0f / 360.0f, a = 3.14159265358979f / 32768.0f;
    const short ax = (short)(rx * k), ay = (short)(ry * k), az = (short)(rz * k);
    const float sx = sinf(ax * a), sy = sinf(ay * a), sz = sinf(az * a), cx = cosf(ax * a), cy = cosf(ay * a), cz = cosf(az * a);
    m[0][0] = cz * cy; m[1][0] = sz * cy; m[2][0] = -sy;
    m[0][1] = (sx * sy) * cz - cx * sz; m[1][1] = (sx * sy) * sz + cx * cz; m[2][1] = sx * cy;
    m[0][2] = (cx * sy) * cz + sx * sz; m[1][2] = (cx * sy) * sz - sx * cz; m[2][2] = cx * cy;
}
SceneObjHolder* getSceneObjHolder() { return nullptr; }
f32 vecKillElement(const TVec3f& a, const TVec3f& b, TVec3f* o) { float d = a.x * b.x + a.y * b.y + a.z * b.z; o->x = a.x - b.x * d; o->y = a.y - b.y * d; o->z = a.z - b.z * d; return d; }
bool isInRange(f32 v, f32 lo, f32 hi) { return v >= lo && v <= hi; }
void normalize(TVec3f* v) { float l = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); if (l > 0) { v->x /= l; v->y /= l; v->z /= l; } }
}  // namespace MR
extern "C" void PSMTXMultVec(const Mtx m, const Vec* s, Vec* d) {
    const Vec v = *s;
    d->x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3];
    d->y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3];
    d->z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];
}
extern "C" f32 PSVECMag(const Vec* v) { return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); }
extern "C" void PSVECNormalize(const Vec* s, Vec* d) {
    const float l = PSVECMag(s);
    d->x = s->x / l; d->y = s->y / l; d->z = s->z / l;
}
NameObj::NameObj(const char*) {}
NameObj::~NameObj() {}
void NameObj::calcViewAndEntry() {}
void NameObj::initAfterPlacement() {}
void NameObj::calcAnim() {}
void NameObj::draw() const {}
NameObj* SceneObjHolder::getObj(int) const { return nullptr; }
namespace LightFunction { void registerLightAreaHolder(LightAreaHolder*) {} }

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float frand(float lo, float hi) { return lo + (hi - lo) * ((rnd() & 0xFFFF) / 65535.0f); }

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "1";
    setenv("GXV_OC_LIGHTFIND", mode, 1);
    long lookups = 0, bad = 0, badMut = 0, followRuns = 0;
    for (int scenario = 0; scenario < 300; scenario++) {
        static LightAreaHolder* holder;
        holder = new LightAreaHolder(64, "LightArea");
        std::vector<LightArea*> areas;
        std::vector<TPos3f*> follow;
        const int n = 3 + rnd() % 20;
        for (int i = 0; i < n; i++) {
            const int type = rnd() % 3 == 0 ? AreaForm::Type_Sphere : (rnd() % 3 == 0 ? AreaForm::Type_Cylinder : AreaForm::Type_Cube1);
            LightArea* a = new LightArea(type, "LightArea");
            if (type == AreaForm::Type_Cube1) {
                AreaFormCube* c = static_cast< AreaFormCube* >(a->mForm);
                c->mTranslation.set(frand(-3000, 3000), frand(-3000, 3000), frand(-3000, 3000));
                c->mRotation.set(frand(0, 360), frand(0, 360), frand(0, 360));
                c->mScale.set(frand(0.5f, 8), frand(0.5f, 8), frand(0.5f, 8));
                c->updateBoxParam();
            } else if (type == AreaForm::Type_Sphere) {
                AreaFormSphere* s = static_cast< AreaFormSphere* >(a->mForm);
                s->mTranslation.set(frand(-3000, 3000), frand(-3000, 3000), frand(-3000, 3000));
                s->mRadius = frand(100, 3000);
                s->mUp.set(0, 1, 0);
            } else {
                AreaFormCylinder* c = static_cast< AreaFormCylinder* >(a->mForm);
                c->mTranslation.set(frand(-3000, 3000), frand(-3000, 3000), frand(-3000, 3000));
                c->mRadius = frand(100, 2000);
                c->mHeight = frand(100, 2000);
                c->mRotation.set(0, 1, 0);
            }
            if (rnd() % 4 == 0) a->mIsValid = false;
            holder->entry(a);
            areas.push_back(a);
        }
        holder->sort();
        std::vector<TVec3f> spots;
        for (int i = 0; i < 12; i++) spots.push_back(TVec3f(frand(-3500, 3500), frand(-3500, 3500), frand(-3500, 3500)));
        bool followMode = false;
        TPos3f fm;
        fm.identity();
        for (int step = 0; step < 400; step++) {
            const uint32_t r = rnd() % 16;
            LightArea* a = areas[rnd() % areas.size()];
            if (r == 0) a->validate();
            else if (r == 1) a->invalidate();
            else if (r == 2) a->awake();
            else if (r == 3) a->sleep();
            else if (r == 4 && scenario % 5 == 0 && !followMode) { a->setFollowMtx(&fm); followMode = true; followRuns++; }
            else if (r == 5) a->_15 = !a->_15, PortAreaBump();  // (the follower's write, through its helper's effect)
            else if (r == 6 && followMode) { fm.mMtx[0][3] += 700.0f; }  // (a follow matrix moves: the holder must not be memoised)
            else if (r == 7) spots.push_back(TVec3f(frand(-3500, 3500), frand(-3500, 3500), frand(-3500, 3500)));
            for (int q = 0; q < 6; q++) {
                const TVec3f& p = spots[rnd() % spots.size()];
                AreaObj* memo = holder->portFindIn(p);
                AreaObj* walk = holder->find_in(p);
                lookups++;
                if (memo != walk) bad++;
            }
        }
        // mutation: a raw store (no writer): the memo may now be stale, and the test must see it for some position
        if (scenario % 10 == 0 && !followMode) {
            bool seen = false;
            for (int round = 0; round < 40 && !seen; round++) {
                LightArea* a = areas[rnd() % areas.size()];
                for (const TVec3f& p : spots) holder->portFindIn(p);  // (fill)
                a->mIsValid = !a->mIsValid;                             // (unannounced)
                for (const TVec3f& p : spots) {
                    if (holder->portFindIn(p) != holder->find_in(p)) seen = true;
                }
                a->validate(); a->invalidate();  // (announce, so later rounds start clean)
            }
            if (!seen) badMut++;
        }
    }
    std::printf("lookups %ld mismatches %ld, mutation runs undetected %ld, follow scenarios %ld\n", lookups, bad, badMut, followRuns);
    const bool pass = bad == 0 && followRuns > 0;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
