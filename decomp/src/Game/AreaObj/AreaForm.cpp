#include "Game/AreaObj/AreaForm.hpp"
#include "Game/Util.hpp"
#ifdef TARGET_PC
#include <cmath>
#include <cstring>
extern "C" int port_hl_areacube_on(void);  // GXV_HL_AREACUBE (hivelogic), platform/src/port/perf.cpp: 0 off, 1 on, 2 verify
// GXV_HL_AREACUBE=2 (perf.cpp): counts a check, and logs the first mismatches / stale boxes with the area's name.
extern "C" void port_hl_verify_area(int outsideByBox, int insideByFull, int stale, const char* owner, const float* pos, const float* cubeTrans);
#endif

void AreaForm_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.000003814697265625f;
    (void)-0.000003814697265625f;
    (void)0.5f;
    (void)-1.0f;
    (void)1023.5f;
    (void)1.5707963705062866f;
    (void)-1.5707963705062866f;
    (void)57.295780181884766f;
    (void)1000.0f;
    (void)500.0f;
}

AreaFormCube::AreaFormCube(int a1) {
    _8 = a1;
    mTranslation.x = 0.0f;
    mTranslation.y = 0.0f;
    mTranslation.z = 0.0f;
    mRotation.x = 0.0f;
    mRotation.y = 0.0f;
    mRotation.z = 0.0f;
    mScale.x = 1.0f;
    mScale.y = 1.0f;
    mScale.z = 1.0f;
    mBounding.i.x = 0.0f;
    mBounding.i.y = 0.0f;
    mBounding.i.z = 0.0f;
    mBounding.f.x = 0.0f;
    mBounding.f.y = 0.0f;
    mBounding.f.z = 0.0f;
#ifdef TARGET_PC
    mPortRejectMode = 0;
    mPortOwner = nullptr;
#endif
}

void AreaFormCube::init(const JMapInfoIter& rIter) {
    MR::getJMapInfoTrans(rIter, &mTranslation);
    MR::getJMapInfoRotate(rIter, &mRotation);
    MR::getJMapInfoScale(rIter, &mScale);
    updateBoxParam();
}

bool AreaFormCube::isInVolume(const TVec3f& rPos) const {
#ifdef TARGET_PC
    // GXV_HL_AREACUBE (portUpdateReject below): a point outside the cube's world box with margin is outside the cube. Only a cube
    // that follows nothing (_4 null, checked at every call: a follow cube's world matrix moves each turn) uses the box; a NaN
    // coordinate fails every comparison here and takes the full test, as before.
    bool portBoxOutside = false;
    if (mPortRejectMode != 0 && _4 == nullptr) {
        portBoxOutside = rPos.x < mPortRejectLo[0] || rPos.x > mPortRejectHi[0] || rPos.y < mPortRejectLo[1] || rPos.y > mPortRejectHi[1] ||
                         rPos.z < mPortRejectLo[2] || rPos.z > mPortRejectHi[2];
        if (portBoxOutside && mPortRejectMode == 1) {
            return false;
        }
    }
#endif
    TPos3f pos;
    calcWorldMtx(&pos);

    TVec3f transpose;
    pos.multTranspose(rPos, transpose);

#ifdef TARGET_PC
    const bool portFull = transpose.x >= mBounding.i.x && transpose.y >= mBounding.i.y && transpose.z >= mBounding.i.z &&
                          transpose.x < mBounding.f.x && transpose.y < mBounding.f.y && transpose.z < mBounding.f.z;
    if (mPortRejectMode == 2 && _4 == nullptr) {
        // GXV_HL_AREACUBE=2: the full answer is used; the box's is compared with it, and the box's inputs with what it was built from
        float now[18];
        portSnapshot(now);
        const int stale = std::memcmp(now, mPortSnap, sizeof(now)) != 0;
        const float p[3] = {rPos.x, rPos.y, rPos.z};
        const float t[3] = {_48.mMtx[0][3], _48.mMtx[1][3], _48.mMtx[2][3]};
        port_hl_verify_area(portBoxOutside ? 1 : 0, portFull ? 1 : 0, stale, mPortOwner, p, t);
    }
    return portFull;
#else
    return transpose.x >= mBounding.i.x && transpose.y >= mBounding.i.y && transpose.z >= mBounding.i.z && transpose.x < mBounding.f.x &&
           transpose.y < mBounding.f.y && transpose.z < mBounding.f.z;
#endif
}

void AreaFormCube::calcWorldPos(TVec3f* pPos) const {
    if (_4 != nullptr) {
        _4->mult(mTranslation, *pPos);
    } else {
        pPos->set(mTranslation);
    }
}

void AreaFormCube::calcWorldRotate(TVec3f* pOut) const {
    const TPos3f* pFollowMtx = _4;
    if (pFollowMtx != nullptr) {
        TPos3f stack = _48;
        stack.zeroTrans();
        stack.concat(*pFollowMtx, stack);
        stack.getEulerXYZ(*pOut);
        *pOut = *pOut * _180_PI;
    } else {
        pOut->set(mRotation);
    }
}

void AreaFormCube::calcWorldBox(TDirBox3f* pBox) const {
    TPos3f pos;
    calcWorldMtx(&pos);

    pBox->_0.x = pos.mMtx[0][0];
    pBox->_C.x = pos.mMtx[0][1];
    pBox->_18.x = pos.mMtx[0][2];
    pBox->_24.x = pos.mMtx[0][3];
    pBox->_0.y = pos.mMtx[1][0];
    pBox->_C.y = pos.mMtx[1][1];
    pBox->_18.y = pos.mMtx[1][2];
    pBox->_24.y = pos.mMtx[1][3];
    pBox->_0.z = pos.mMtx[2][0];
    pBox->_C.z = pos.mMtx[2][1];
    pBox->_18.z = pos.mMtx[2][2];
    pBox->_24.z = pos.mMtx[2][3];

    pBox->_30.sub(mBounding.f, mBounding.i);
    pos.mult(mBounding.i, pBox->_24);
}

void AreaFormCube::calcLocalPos(TVec3f* pPos, const TVec3f& rPos) const {
    TPos3f worldMtx;
    calcWorldMtx(&worldMtx);
    worldMtx.multTranspose(rPos, *pPos);
}

f32 AreaFormCube::getBaseSize() {
    return 1000.0f;
}

void AreaFormCube::updateBoxParam() {
    MR::makeMtxRotate(_48, mRotation.x, mRotation.y, mRotation.z);

    _48.setTrans(mTranslation);

    TVec3f v10(0.5f * (-mScale.x * getBaseSize()), 0.5f * (-mScale.y * getBaseSize()), 0.5f * (-mScale.z * getBaseSize()));
    TVec3f v9(0.5f * (mScale.x * getBaseSize()), 0.5f * (mScale.y * getBaseSize()), 0.5f * (mScale.z * getBaseSize()));

    mBounding.i.set(v10);
    mBounding.f.set(v9);

    if (_8 == 1) {
        TVec3f temp(0.0f, 0.5f * (mScale.y * getBaseSize()), 0.0f);

        mBounding.i += temp;
        mBounding.f += temp;
    }
#ifdef TARGET_PC
    portUpdateReject();
#endif
}

#ifdef TARGET_PC
// GXV_HL_AREACUBE (hivelogic, 6 Nov; default 1, exact): most area lookups (AreaObjMgr::find_in: the light areas of every lit actor
// every turn, the camera, water, death and switch areas of the player and others) test a point against cubes it is far from. The
// full test copies the cube's matrix and turns the point into the cube's frame (local = A (p - t), A = the transpose of _48's 3x3)
// before six comparisons. For a cube that follows nothing, this keeps a world-space box that holds every point the full test can
// accept, grown by a margin, and answers "outside" from six comparisons when the point is outside it.
// Why exact: the box comes from M = A^-1 in double: p - t = M local, so a local point inside the cube's bounds lies within
// t + M c +- |M| h (c, h: the bounds' centre and half size). The full test computes local in float with a relative error of a few
// 2^-24 of |p - t|, which M turns into a world error of the same order (|M| is about 1: _48 is a rotation); a point it accepts is
// therefore within about 1e-5 of the box's scale of that world box. The margin is 16 units plus 1e-3 of that scale, more than a
// hundred times the error, so a point beyond it is one the full test refuses. An undefined matrix (|det A| < 0.5: never a rotation)
// or a non-finite value leaves the box off, and so do a follow matrix (_4: isInVolume checks it at every call) and GXV_HL_AREACUBE=0.
// Static only: the full test reads _4, _48 and mBounding. _4 is checked at each call (AreaObj::setFollowMtx, AreaObjFollower: set at
// init for every area with a FollowId, its matrix rewritten every turn, so those cubes always take the full test). _48 and mBounding
// are written only by updateBoxParam (init and nothing else; 7 Nov audit: no other writer in decomp/src, MR::getCubeLocalBox's one
// caller, MercatorTransformCube::calcLocalBoxSize, only reads), and updateBoxParam rebuilds the box. GXV_HL_AREACUBE=2 checks this on
// the console: every call compares _48 and mBounding with the copy taken here ("stale" in the hl-verify lines).
void AreaFormCube::portSnapshot(f32* pOut) const {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            pOut[i * 4 + j] = _48.mMtx[i][j];
        }
    }
    pOut[12] = mBounding.i.x;
    pOut[13] = mBounding.i.y;
    pOut[14] = mBounding.i.z;
    pOut[15] = mBounding.f.x;
    pOut[16] = mBounding.f.y;
    pOut[17] = mBounding.f.z;
}

void AreaFormCube::portUpdateReject() {
    mPortRejectMode = 0;
    const int knob = port_hl_areacube_on();
    if (knob == 0) {
        return;
    }
    portSnapshot(mPortSnap);
    // A[j][i] = _48(i, j): local_j = sum_i A[j][i] (p_i - t_i), the full test's multTranspose
    double a[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            a[j][i] = _48.mMtx[i][j];
        }
    }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) >= 0.5)) {
        return;
    }
    double m[3][3];  // A^-1 (adjugate over the determinant)
    m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    const double lo[3] = {mBounding.i.x, mBounding.i.y, mBounding.i.z};
    const double hi[3] = {mBounding.f.x, mBounding.f.y, mBounding.f.z};
    double c[3], h[3];
    for (int j = 0; j < 3; j++) {
        c[j] = 0.5 * (lo[j] + hi[j]);
        h[j] = 0.5 * std::fabs(hi[j] - lo[j]);
    }
    double center[3], half[3], scale = 0.0;
    for (int i = 0; i < 3; i++) {
        center[i] = static_cast< double >(_48.mMtx[i][3]) + m[i][0] * c[0] + m[i][1] * c[1] + m[i][2] * c[2];
        half[i] = std::fabs(m[i][0]) * h[0] + std::fabs(m[i][1]) * h[1] + std::fabs(m[i][2]) * h[2];
        const double extent = std::fabs(center[i]) + half[i] + std::fabs(static_cast< double >(_48.mMtx[i][3]));
        if (!(extent < 1e30)) {
            return;  // (not finite)
        }
        if (extent > scale) {
            scale = extent;
        }
    }
    const double margin = 16.0 + 1e-3 * scale;
    for (int i = 0; i < 3; i++) {
        // (float rounding of the bounds: a further 1 ulp outward)
        mPortRejectLo[i] = std::nextafter(static_cast< f32 >(center[i] - half[i] - margin), -HUGE_VALF);
        mPortRejectHi[i] = std::nextafter(static_cast< f32 >(center[i] + half[i] + margin), HUGE_VALF);
    }
    mPortRejectMode = knob == 2 ? 2 : 1;
}
#endif

void AreaFormCube::calcWorldMtx(TPos3f* pPos) const {
    const TPos3f* pFollowMtx = _4;
    if (pFollowMtx != nullptr) {
        pPos->concat(*pFollowMtx, _48);
    } else {
        pPos->set(_48);
    }
}

AreaFormSphere::AreaFormSphere() {
    mTranslation.x = 0.0f;
    mTranslation.y = 0.0f;
    mTranslation.z = 0.0f;
    mRadius = 0.0f;
}

void AreaFormSphere::calcUpVec(TVec3f* pOut) const {
    if (_4 != nullptr) {
        _4->mult33(mUp, *pOut);
        MR::normalize(pOut);
    } else {
        pOut->set(mUp);
    }
}

void AreaFormSphere::calcPos(TVec3f* pOut) const {
    if (_4 != nullptr) {
        _4->mult(mTranslation, *pOut);
    } else {
        pOut->set(mTranslation);
    }
}

bool AreaFormSphere::isInVolume(const TVec3f& rVector) const {
    TVec3f pos;
    calcPos(&pos);

    return (rVector - pos).length() < mRadius;
}

AreaFormBowl::AreaFormBowl() {
    mTranslation.x = 0.0f;
    mTranslation.y = 0.0f;
    mTranslation.z = 0.0f;
    _20 = 0.0f;
}

void AreaFormBowl::init(const JMapInfoIter& rIter) {
    MR::getJMapInfoTrans(rIter, &mTranslation);

    f32 scale_x;
    rIter.getValue< f32 >("scale_x", &scale_x);
    _20 = scale_x;
    _20 *= 500.0f;

    TVec3f rotation;
    MR::getJMapInfoRotate(rIter, &rotation);
    calcUpVec(rotation);
}

bool AreaFormBowl::isInVolume(const TVec3f& rPos) const {
    if ((rPos - mTranslation).length() > _20) {
        return false;
    }

    TVec3f relative(rPos - mTranslation);
    return relative.dot(mUp) < 0.0f;
}

void AreaFormBowl::calcUpVec(const TVec3f& rPos) {
    TVec3f up(0.0f, 1.0f, 0.0f);

    Mtx mtx;
    MR::makeMtxTR(mtx, 0.0f, 0.0f, 0.0f, rPos.x, rPos.y, rPos.z);
    PSMTXMultVec(mtx, (const Vec*)&up, (Vec*)&up);
    mUp = up;
}

AreaFormCylinder::AreaFormCylinder() {
    mTranslation.x = 0.0f;
    mTranslation.y = 0.0f;
    mTranslation.z = 0.0f;
    mRotation.x = 0.0f;
    mRotation.y = 0.0f;
    mRotation.z = 0.0f;
    mRadius = 0.0f;
    mHeight = 0.0f;
}

void AreaFormCylinder::calcPos(TVec3f* pPos) const {
    if (_4 != nullptr) {
        _4->mult(mTranslation, *pPos);
    } else {
        pPos->set(mTranslation);
    }
}

void AreaFormCylinder::calcCenterPos(TVec3f* pCenterPos) const {
    calcPos(pCenterPos);

    TVec3f upVec;
    calcUpVec(&upVec);
    pCenterPos->add(upVec * mHeight * 0.5f);
}

void AreaFormCylinder::calcUpVec(TVec3f* pUpVec) const {
    if (_4 != nullptr) {
        _4->mult33(mRotation, *pUpVec);
        MR::normalize(pUpVec);
    } else {
        pUpVec->set(mRotation);
    }
}

bool AreaFormCylinder::isInVolume(const TVec3f& rVec) const {
    TVec3f pos;
    calcPos(&pos);

    TVec3f up;
    calcUpVec(&up);

    TVec3f stack_8 = rVec - pos;
    f32 v6 = MR::vecKillElement(stack_8, up, &stack_8);
    f32 v7 = stack_8.length();

    bool ret;

    if (MR::isInRange(v6, 0.0f, mHeight) && (v7 < mRadius)) {
        ret = true;
    } else {
        ret = false;
    }

    return ret;
}

void AreaFormCylinder::calcDir(const TVec3f& rRotation) {
    mRotation.x = 0.0f;
    mRotation.y = 1.0f;
    mRotation.z = 0.0f;

    MtxPtr mtx_x = MR::tmpMtxRotXDeg(rRotation.x);
    PSMTXMultVec(mtx_x, (const Vec*)&mRotation, (Vec*)&mRotation);
    MtxPtr mtx_y = MR::tmpMtxRotYDeg(rRotation.y);
    PSMTXMultVec(mtx_y, (const Vec*)&mRotation, (Vec*)&mRotation);
    MtxPtr mtx_z = MR::tmpMtxRotZDeg(rRotation.z);
    PSMTXMultVec(mtx_z, (const Vec*)&mRotation, (Vec*)&mRotation);
    mRotation.normalize();
}

void AreaFormSphere::init(const JMapInfoIter& rIter) {
    MR::getJMapInfoTrans(rIter, &mTranslation);

    f32 scale_x;
    rIter.getValue< f32 >("scale_x", &scale_x);

    mRadius = scale_x;
    mRadius *= 500.0f;

    TVec3f rotation;
    MR::getJMapInfoRotate(rIter, &rotation);

    TVec3f up(0.0f, 1.0f, 0.0f);

    Mtx mtx;
    MR::makeMtxTR(mtx, 0.0f, 0.0f, 0.0f, rotation.x, rotation.y, rotation.z);
    PSMTXMultVec(mtx, (const Vec*)&up, (Vec*)&up);
    mUp = up;
}

void AreaFormCylinder::init(const JMapInfoIter& rIter) {
    MR::getJMapInfoTrans(rIter, &mTranslation);
    TVec3f rotate;

    MR::getJMapInfoRotate(rIter, &rotate);
    calcDir(rotate);

    f32 temp;
    rIter.getValue< f32 >("scale_x", &temp);

    mRadius = temp;
    mRadius *= 500.0f;

    rIter.getValue< f32 >("scale_y", &temp);
    mHeight = temp;
    mHeight *= 500.0f;
}
