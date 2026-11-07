// hivelogic host test (tools/test_hivelogic_cuts.sh): GXV_HL_SENSOR and GXV_HL_AREACUBE against the original paths, compiling the real
// SensorHitChecker.cpp and AreaForm.cpp. Each scenario runs the original and the new path on the same data; the hit lists (sensors) and
// the answers (cubes) must be identical. A mutation run (two sensors swapped between the runs) must be detected.
#include "Game/LiveActor/SensorHitChecker.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/AreaObj/AreaForm.hpp"
#include "Game/NameObj/NameObj.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static int gSensorKnob = 0, gAreaKnob = 1;
extern "C" int port_hl_sensor_on(void) { return gSensorKnob; }
extern "C" int port_hl_areacube_on(void) { return gAreaKnob; }
// the verify hooks (perf.cpp on the console): counted here
static long gVArea = 0, gVAreaMis = 0, gVAreaStale = 0, gVSensTurns = 0, gVSensCompared = 0, gVSensMis = 0;
extern "C" void port_hl_verify_area(int outsideByBox, int insideByFull, int stale, const char*, const float*, const float*) {
    ++gVArea;
    gVAreaMis += outsideByBox && insideByFull;
    gVAreaStale += stale;
}
extern "C" void port_hl_verify_sensor(unsigned compared, unsigned mismatched, const char*, unsigned, unsigned, unsigned) {
    if (compared) { ++gVSensTurns; gVSensCompared += compared; }
    gVSensMis += mismatched;
}
static long gClipCalls = 0, gClipFlipAt = -1;  // (detector check: one host's clipping flips at that isClipped call)
extern "C" void port_logic_count(int, unsigned) {}
static unsigned char gClip[4096];
static char gHostBase[4096];
namespace MR {
bool isClipped(const LiveActor* p) {
    if (++gClipCalls == gClipFlipAt) gClip[reinterpret_cast<const char*>(p) - gHostBase] ^= 1;
    return gClip[reinterpret_cast<const char*>(p) - gHostBase];
}
bool isSensorRide(const HitSensor*) { return false; }
bool isSensorRush(const HitSensor*) { return false; }
bool isSensorMapObj(const HitSensor*) { return false; }
bool isSensorPlayer(const HitSensor*) { return false; }
bool isSensorAutoRush(const HitSensor*) { return false; }
void connectToScene(NameObj*, int, int, int, int) {}
SceneObjHolder* getSceneObjHolder() { return nullptr; }
// (the real makeMtxRotate: s16 angles, table sine; here the C library's sine of the same quantized angles)
void makeMtxRotate(MtxPtr m, f32 rx, f32 ry, f32 rz) {
    const float k = 65536.0f / 360.0f, a = 3.14159265358979f / 32768.0f;
    const short ax = (short)(rx * k), ay = (short)(ry * k), az = (short)(rz * k);
    const float sx = sinf(ax * a), sy = sinf(ay * a), sz = sinf(az * a), cx = cosf(ax * a), cy = cosf(ay * a), cz = cosf(az * a);
    m[0][0] = cz * cy; m[1][0] = sz * cy; m[2][0] = -sy;
    m[0][1] = (sx * sy) * cz - cx * sz; m[1][1] = (sx * sy) * sz + cx * cz; m[2][1] = sx * cy;
    m[0][2] = (cx * sy) * cz + sx * sz; m[1][2] = (cx * sy) * sz - sx * cz; m[2][2] = cx * cy;
}
MtxPtr tmpMtxRotXDeg(f32) { return nullptr; }
MtxPtr tmpMtxRotYDeg(f32) { return nullptr; }
MtxPtr tmpMtxRotZDeg(f32) { return nullptr; }
f32 vecKillElement(const TVec3f&, const TVec3f&, TVec3f*) { return 0; }
void getJMapInfoScale(const JMapInfoIter&, TVec3f*) {}
void getJMapInfoTrans(const JMapInfoIter&, TVec3f*) {}
void getJMapInfoRotate(const JMapInfoIter&, TVec3f*) {}
bool isInRange(f32, f32, f32) { return false; }
void makeMtxTR(MtxPtr, f32, f32, f32, f32, f32, f32) {}
void normalize(TVec3f*) {}
}  // namespace MR
NameObj::NameObj(const char*) {}
NameObj::~NameObj() {}
void NameObj::calcViewAndEntry() {}
void NameObj::initAfterPlacement() {}
void NameObj::calcAnim() {}
void NameObj::draw() const {}
NameObj* SceneObjHolder::getObj(int) const { return nullptr; }
bool HitSensor::isType(u32 type) const { return mType == type; }
void HitSensor::addHitSensor(HitSensor* p) { if (mSensorCount < mGroupSize) { mSensors[mSensorCount] = p; mSensorCount++; } }
extern "C" void PSMTXMultVec(const Mtx, const Vec*, Vec*) {}
extern "C" f32 PSVECMag(const Vec*) { return 0; }
extern "C" void PSVECNormalize(const Vec*, Vec*) {}
s32 JMapInfo::searchItemInfo(const char*) const { return -1; }

static unsigned gRng = 1;
static unsigned rnd() { gRng = gRng * 1664525u + 1013904223u; return gRng >> 8; }
static float frand(float lo, float hi) { return lo + (hi - lo) * (rnd() & 0xFFFF) / 65535.0f; }

static int SensorTest(bool mutate) {
    const int kGroups = 6, kMax[kGroups] = {0x10, 0x80, 0x200, 0x800, 0x400, 0x400};
    alignas(16) static unsigned char raw[sizeof(SensorHitChecker)];
    SensorHitChecker* chk = reinterpret_cast<SensorHitChecker*>(raw);
    SensorGroup* groups[kGroups];
    for (int g = 0; g < kGroups; g++) groups[g] = new SensorGroup(kMax[g], "t");
    chk->mPlayerGroup = groups[0]; chk->mRideGroup = groups[1]; chk->mEyeGroup = groups[2];
    chk->mSimpleGroup = groups[3]; chk->mMapObjGroup = groups[4]; chk->mCharacterGroup = groups[5];
    long pairsHit = 0, mismatches = 0;
    for (int scen = 0; scen < 3000; scen++) {
        std::vector<HitSensor*> all;
        const int hosts = 1 + rnd() % 60;
        for (int h = 0; h < hosts; h++) gClip[h] = (rnd() % 4) == 0;
        const float spread = (scen % 3 == 0) ? 4.0f : (scen % 3 == 1) ? 50.0f : 400.0f;
        for (int g = 0; g < kGroups; g++) {
            int n = (g == 0) ? rnd() % 3 : rnd() % (scen % 10 == 0 ? 200 : 40); if (n > kMax[g]) n = kMax[g];
            groups[g]->mSensorCount = n;
            for (int i = 0; i < n; i++) {
                HitSensor* s = static_cast<HitSensor*>(::operator new(sizeof(HitSensor)));
                std::memset(s, 0, sizeof(HitSensor));
                s->mType = (rnd() % 5 == 0) ? ATYPE_EYE : rnd() % 0x90;
                // positions on a coarse grid (exact ties) or anywhere, now and then a special value
                const int mode = rnd() % 10;
                if (mode < 4) { s->mPosition.x = (float)(rnd() % 9) * 25.0f; s->mPosition.y = (float)(rnd() % 9) * 25.0f; s->mPosition.z = (float)(rnd() % 3) * 25.0f; }
                else { s->mPosition.x = frand(-spread, spread); s->mPosition.y = frand(-spread, spread); s->mPosition.z = frand(-spread, spread); }
                if (mode == 9) { const float sp[4] = {NAN, INFINITY, -0.0f, 1e-40f}; s->mPosition.y = sp[rnd() % 4]; }
                s->mRadius = (rnd() % 7 == 0) ? 0.0f : (mode < 4 ? 25.0f * (1 + rnd() % 3) * 0.5f : frand(0.0f, spread * 0.6f));
                s->mGroupSize = rnd() % 9;
                s->mSensors = new HitSensor*[s->mGroupSize + 1];
                s->mValidByHost = rnd() % 6 != 0;
                s->mValidBySystem = rnd() % 8 != 0;
                s->mHost = reinterpret_cast<LiveActor*>(gHostBase + rnd() % hosts);
                s->mSensorCount = rnd() % 3;  // (stale counts: movement's clear() resets them)
                groups[g]->mSensors[i] = s;
                all.push_back(s);
            }
        }
        gSensorKnob = 0;
        chk->SensorHitChecker::movement();
        std::vector<std::vector<HitSensor*> > ref;
        for (HitSensor* s : all) { ref.push_back(std::vector<HitSensor*>(s->mSensors, s->mSensors + s->mSensorCount)); pairsHit += s->mSensorCount; }
        if (mutate && groups[5]->mSensorCount > 2) std::swap(groups[5]->mSensors[0], groups[5]->mSensors[1]);
        gSensorKnob = 1;
        chk->SensorHitChecker::movement();
        for (size_t k = 0; k < all.size(); k++) {
            HitSensor* s = all[k];
            if (ref[k] != std::vector<HitSensor*>(s->mSensors, s->mSensors + s->mSensorCount)) { ++mismatches; break; }
        }
        for (HitSensor* s : all) { delete[] s->mSensors; ::operator delete(s); }
    }
    std::printf("sensor%s: 3000 scenarios, %ld hit-list entries, %ld scenarios differ\n", mutate ? " (mutation: one swap)" : "", pairsHit, mismatches);
    return mutate ? (mismatches > 0 ? 0 : 1) : (mismatches == 0 ? 0 : 1);
}

static int AreaTest() {
    long tests = 0, rejected = 0, mismatches = 0;
    TPos3f follow;
    follow.identity();
    for (int scen = 0; scen < 4000; scen++) {
        AreaFormCube* c = new AreaFormCube(scen & 1);
        const float far = (scen % 4 == 0) ? 100.0f : (scen % 4 == 1) ? 5000.0f : (scen % 4 == 2) ? 60000.0f : 400000.0f;
        c->mTranslation.set(frand(-far, far), frand(-far, far), frand(-far, far));
        c->mRotation.set(frand(-360, 360), frand(-360, 360), frand(-360, 360));
        if (scen % 7 == 0) c->mRotation.set(0, 90.0f * (rnd() % 4), 0);
        const float sc = (scen % 5 == 0) ? 0.01f : (scen % 5 == 1) ? 40.0f : 3.0f;
        c->mScale.set(frand(-0.2f, sc), frand(0.0f, sc), frand(0.0f, sc));
        if (scen % 11 == 0) c->mScale.x = 0.0f;
        c->updateBoxParam();
        c->_4 = (scen % 13 == 0) ? &follow : nullptr;
        for (int p = 0; p < 400; p++) {
            TVec3f local, pos;
            const int mode = rnd() % 6;
            // local points at the bounds +- a few ulps, inside, near outside, far outside
            float lx = frand(c->mBounding.i.x, c->mBounding.f.x), ly = frand(c->mBounding.i.y, c->mBounding.f.y), lz = frand(c->mBounding.i.z, c->mBounding.f.z);
            if (mode == 0) lx = (rnd() & 1) ? c->mBounding.f.x : c->mBounding.i.x;
            if (mode == 1) ly = nextafterf((rnd() & 1) ? c->mBounding.f.y : c->mBounding.i.y, (rnd() & 1) ? INFINITY : -INFINITY);
            if (mode == 2) lz = ((rnd() & 1) ? c->mBounding.f.z : c->mBounding.i.z) + frand(-30.0f, 30.0f);
            if (mode == 3) lx += frand(-3000.0f, 3000.0f);
            local.set(lx, ly, lz);
            TPos3f w;
            c->calcWorldMtx(&w);
            w.mult(local, pos);
            if (mode == 4) pos.set(frand(-2 * far, 2 * far), frand(-2 * far, 2 * far), frand(-2 * far, 2 * far));
            if (mode == 5 && rnd() % 4 == 0) { const float sp[3] = {NAN, INFINITY, -INFINITY}; pos.z = sp[rnd() % 3]; }
            const bool fast = c->isInVolume(pos);
            const u8 saved = c->mPortRejectMode;
            c->mPortRejectMode = 0;
            const bool full = c->isInVolume(pos);
            c->mPortRejectMode = 2;
            const long misBefore = gVAreaMis, staleBefore = gVAreaStale;
            const bool verified = c->isInVolume(pos);  // (verify mode: must give the full answer and report no mismatch)
            if (saved && (verified != full || gVAreaMis != misBefore || gVAreaStale != staleBefore)) ++mismatches;
            c->mPortRejectMode = saved;
            const bool saved1 = saved != 0;
            ++tests;
            if (fast != full) ++mismatches;
            if (saved1 && !c->_4 && !full) {
                const bool out = pos.x < c->mPortRejectLo[0] || pos.x > c->mPortRejectHi[0] || pos.y < c->mPortRejectLo[1] || pos.y > c->mPortRejectHi[1] ||
                                 pos.z < c->mPortRejectLo[2] || pos.z > c->mPortRejectHi[2];
                rejected += out;
            }
        }
        delete c;
    }
    std::printf("area cube: %ld tests, %ld answered by the world box, %ld differ\n", tests, rejected, mismatches);
    return mismatches == 0 && rejected > 0 ? 0 : 1;
}

// GXV_HL_SENSOR=2: the hit lists must equal the original loops' and no mismatch be reported; a host whose clipping flips during
// movement (between the copies' loops and the original's) must be reported.
static int SensorVerifyTest() {
    const int kMax[6] = {0x10, 0x80, 0x200, 0x800, 0x400, 0x400};
    alignas(16) static unsigned char raw[sizeof(SensorHitChecker)];
    SensorHitChecker* chk = reinterpret_cast<SensorHitChecker*>(raw);
    SensorGroup* groups[6];
    for (int g = 0; g < 6; g++) groups[g] = new SensorGroup(kMax[g], "t");
    chk->mPlayerGroup = groups[0]; chk->mRideGroup = groups[1]; chk->mEyeGroup = groups[2];
    chk->mSimpleGroup = groups[3]; chk->mMapObjGroup = groups[4]; chk->mCharacterGroup = groups[5];
    long differ = 0, flipRuns = 0, flipCaught = 0, flipChanged = 0;
    for (int scen = 0; scen < 1500; scen++) {
        std::vector<HitSensor*> all;
        const int hosts = 1 + rnd() % 30;
        for (int h = 0; h < hosts; h++) gClip[h] = (rnd() % 4) == 0;
        for (int g = 0; g < 6; g++) {
            int n = (g == 0) ? 1 + rnd() % 2 : rnd() % 30;
            if (n > kMax[g]) n = kMax[g];
            groups[g]->mSensorCount = n;
            for (int i = 0; i < n; i++) {
                HitSensor* s = static_cast<HitSensor*>(::operator new(sizeof(HitSensor)));
                std::memset(s, 0, sizeof(HitSensor));
                s->mType = (rnd() % 5 == 0) ? ATYPE_EYE : rnd() % 0x90;
                s->mPosition.x = frand(-60, 60); s->mPosition.y = frand(-60, 60); s->mPosition.z = frand(-60, 60);
                s->mRadius = frand(0.0f, 40.0f);
                s->mGroupSize = 1 + rnd() % 8;
                s->mSensors = new HitSensor*[s->mGroupSize + 1];
                s->mValidByHost = rnd() % 6 != 0;
                s->mValidBySystem = true;
                s->mHost = reinterpret_cast<LiveActor*>(gHostBase + rnd() % hosts);
                groups[g]->mSensors[i] = s;
                all.push_back(s);
            }
        }
        unsigned char clipSave[64];
        std::memcpy(clipSave, gClip, sizeof clipSave);
        gSensorKnob = 0; gClipFlipAt = -1;
        chk->SensorHitChecker::movement();
        std::vector<std::vector<HitSensor*> > ref;
        for (HitSensor* s : all) ref.push_back(std::vector<HitSensor*>(s->mSensors, s->mSensors + s->mSensorCount));
        gSensorKnob = 2;
        const long misBefore = gVSensMis;
        chk->SensorHitChecker::movement();
        for (size_t k = 0; k < all.size(); k++) {
            if (ref[k] != std::vector<HitSensor*>(all[k]->mSensors, all[k]->mSensors + all[k]->mSensorCount)) { ++differ; break; }
        }
        if (gVSensMis != misBefore) ++differ;
        // detector: flip one host's clipping at a random isClipped call of the verify run's original loops
        gSensorKnob = 1; gClipCalls = 0;
        chk->SensorHitChecker::movement();
        const long fastCalls = gClipCalls;  // (the copies' collection)
        std::memcpy(gClip, clipSave, sizeof clipSave);
        gSensorKnob = 2; gClipCalls = 0; gClipFlipAt = fastCalls + 1 + rnd() % 8;
        const long misBefore2 = gVSensMis;
        chk->SensorHitChecker::movement();
        ++flipRuns;
        bool changed = false;  // (did the flip change any list compared with an unflipped run?)
        std::vector<std::vector<HitSensor*> > flipped;
        for (HitSensor* s : all) flipped.push_back(std::vector<HitSensor*>(s->mSensors, s->mSensors + s->mSensorCount));
        for (size_t k = 0; k < all.size(); k++) changed |= flipped[k] != ref[k];
        flipChanged += changed;
        if (changed && gVSensMis != misBefore2) ++flipCaught;
        if (!changed && gVSensMis != misBefore2) ++differ;  // (a report with no difference: a false alarm)
        gClipFlipAt = -1;
        std::memcpy(gClip, clipSave, sizeof clipSave);
        for (HitSensor* s : all) { delete[] s->mSensors; ::operator delete(s); }
    }
    std::printf("sensor verify mode: 1500 scenarios (%ld turns compared, %ld sensors), %ld differ or false reports; detector: %ld of %ld flips "
                "that changed a hit list reported\n", gVSensTurns, gVSensCompared, differ, flipCaught, flipChanged);
    return differ == 0 && flipChanged > 0 && flipCaught == flipChanged ? 0 : 1;
}

// GXV_HL_AREACUBE: a follow cube (_4 set) never uses the box, whatever its follow matrix does; a cube whose matrix is changed behind
// updateBoxParam's back is reported as stale by the verify mode (and the box would then be wrong: why mode 1 needs the audit).
static int AreaFollowStaleTest() {
    long followWrong = 0, staleSeen = 0, staleRuns = 0;
    for (int scen = 0; scen < 2000; scen++) {
        AreaFormCube* c = new AreaFormCube(scen & 1);
        c->mTranslation.set(frand(-5000, 5000), frand(-5000, 5000), frand(-5000, 5000));
        c->mRotation.set(frand(-360, 360), frand(-360, 360), frand(-360, 360));
        c->mScale.set(frand(0.1f, 3.0f), frand(0.1f, 3.0f), frand(0.1f, 3.0f));
        c->updateBoxParam();
        // follow: a matrix that moves the cube far away (a rotating / moving platform's matrix)
        TPos3f follow;
        follow.identity();
        MR::makeMtxRotate(follow, frand(-180, 180), frand(-180, 180), frand(-180, 180));
        follow.setTrans(TVec3f(frand(-20000, 20000), frand(-20000, 20000), frand(-20000, 20000)));
        c->_4 = &follow;
        for (int p = 0; p < 50; p++) {
            TVec3f local(frand(c->mBounding.i.x, c->mBounding.f.x), frand(c->mBounding.i.y, c->mBounding.f.y), frand(c->mBounding.i.z, c->mBounding.f.z)), pos;
            TPos3f w;
            c->calcWorldMtx(&w);
            w.mult(local, pos);
            const u8 saved = c->mPortRejectMode;
            c->mPortRejectMode = 0;
            const bool full = c->isInVolume(pos);
            c->mPortRejectMode = saved;
            if (c->isInVolume(pos) != full) ++followWrong;
        }
        c->_4 = nullptr;
        // stale: move the cube without updateBoxParam, verify mode must report it
        c->mPortRejectMode = 2;
        c->_48.mMtx[0][3] += 500.0f;
        const long before = gVAreaStale;
        TVec3f any(0, 0, 0);
        c->isInVolume(any);
        ++staleRuns;
        staleSeen += gVAreaStale != before;
        delete c;
    }
    std::printf("area cube follow: 100000 points of moved follow cubes, %ld wrong; stale detector: %ld of %ld reported\n", followWrong, staleSeen, staleRuns);
    return followWrong == 0 && staleSeen == staleRuns ? 0 : 1;
}

int main() {
    int bad = 0;
    bad |= SensorTest(false);
    bad |= SensorTest(true);
    bad |= AreaTest();
    bad |= SensorVerifyTest();
    bad |= AreaFollowStaleTest();
    std::printf(bad ? "FAIL\n" : "PASS\n");
    return bad;
}
