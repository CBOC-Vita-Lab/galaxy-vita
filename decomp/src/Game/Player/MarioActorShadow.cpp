#ifdef TARGET_PC
#include "Game/MP/MpSelfPuppet.hpp"
#endif
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/JetTurtleShadow.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioAccess.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioShadow.hpp"
#include "Game/Player/MarioSwim.hpp"
#include "Game/Player/ModelHolder.hpp"
#include "Game/Util/AreaObjUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/FootPrint.hpp"
#include "Game/Util/JointUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <JSystem/J3DGraphBase/J3DSys.hpp>

void MarioActorShadow_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)-1.0f;
    (void)500.0f;
    (void)0.75f;
    (void)1000.0f;
    (void)4503599627370496.0;
    (void)4503601774854144.0;
}

#ifdef TARGET_PC
#include <cstdlib>
#include <cstdio>
#include "port/stage_knob.h"
#include "port/mirror25.h"
#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
extern "C" void port_gx_marker(unsigned int kind);
extern "C" void port_log_summary(const char* fmt, ...);
// ---- GXV_SILH_GATE (shadow25): does Mario's silhouette show in this picture? The renderer (vgx_draw.cpp SilhGatePass) leaves the silhouette fill's whole-EFB alpha
// capture and its full-screen quad out of a picture with no shadow volume when this says "cannot show". The silhouette is drawn (drawSilhouette, DrawType_0x28) only
// where Mario is BEHIND what the lists before it drew: the map (planets, map objects: the draw buffers drawn before DrawType_0x28; enemies, NPCs and effects come after
// and neither hide nor are darkened by it). 1: it cannot show: not drawn this picture (exact), or rays from the camera to his body (centre, head, both sides) hit no map
// collision for GXV_SILH_HOLD (default 8) pictures running; 2: it may (a hit, a demo, the camera close to him, the hold still running). Misses: a map object WITHOUT collision
// in front of him (decorative foliage, a model-only bridge), a ray that slips between collision polygons; the renderer's own check cannot see those either.
namespace {
bool sSilhDrawn = false;
u32 sSilhClear = 0, sSilhSeenGen = 0xFFFFFFFFu;
int sSilhOn = 0, sSilhRays = 4, sSilhHold = 8;
u32 sSilhAsked = 0, sSilhExact = 0, sSilhRayClear = 0, sSilhHit = 0;
const char* SilhEnv(const char* name, char* buf, size_t size) {
    long v;
    if (port::stageknob::Overridden(name, &v)) { snprintf(buf, size, "%ld", v); return buf; }
    return getenv(name);
}
}  // namespace

// ---- GXV_MIRROR25 (reflfur25; platform/include/port/mirror25.h has the whole story): is the mirrored model entirely outside the picture's frustum?
// The sphere is centred on the reflection's origin (the base matrix's translation) with the generous bound of mirror25::Radius; it is judged by
// the clipping judge's PICTURE frustum (the zoomed one while the handheld zoom is in force). The camera within NearGuard() + radius of the origin is
// never judged (the judge's near plane is 500 units out).
bool MarioActor::portReflectionOutside(const TVec3f& rOrigin, bool* pNear) {
    *pNear = false;
    if (!MR::isExistSceneObj(SceneObj_CameraContext)) {
        *pNear = true;  // (no camera known: nothing is judged)
        return false;
    }
    const f32 r = port::mirror25::Radius(getCarrySensor() != nullptr);
    const TVec3f cam = MR::getCamPos();
    const f32 dx = rOrigin.x - cam.x, dy = rOrigin.y - cam.y, dz = rOrigin.z - cam.z;
    const f32 guard = port::mirror25::NearGuard() + r;
    if (dx * dx + dy * dy + dz * dz <= guard * guard) {
        *pNear = true;
        return false;
    }
    return MR::getClippingJudge()->isJudgedToClipPictureFrustum(rOrigin, r);
}
namespace {
port::mirror25::Census sMirCensus;
}
int MarioActor::portSilhouetteHint() {
    if (sSilhSeenGen != port::stageknob::Gen()) {
        sSilhSeenGen = port::stageknob::Gen();
        char sb[16];
        const char* e = SilhEnv("GXV_SILH_GATE", sb, sizeof sb);
        sSilhOn = e && *e ? atoi(e) : 0;
        const char* r = getenv("GXV_SILH_RAYS");
        sSilhRays = r && *r ? atoi(r) : 4;
        if (sSilhRays < 1) sSilhRays = 1;
        if (sSilhRays > 5) sSilhRays = 5;
        const char* h = getenv("GXV_SILH_HOLD");
        sSilhHold = h && *h ? atoi(h) : 8;
        sSilhClear = 0;
    }
    if (!sSilhOn) {
        sSilhDrawn = false;
        return 0;
    }
    const bool drawn = sSilhDrawn;
    sSilhDrawn = false;
    ++sSilhAsked;
    int hint = 2;
    if (!drawn) {
        hint = 1;  // (nothing was drawn: no alpha from the silhouette, exact)
        ++sSilhExact;
        sSilhClear = 0;
    } else if (MR::isDemoActive() || mMario->isStatusActive(MarioStatus_Bury)) {
        sSilhClear = 0;
    } else {
        const TVec3f& up0 = getGravityVector();
        TVec3f up(-up0.x, -up0.y, -up0.z);
        TVec3f toMario(mPosition - mCamPos);
        const f32 dist2 = toMario.x * toMario.x + toMario.y * toMario.y + toMario.z * toMario.z;
        bool hit = dist2 < 400.0f * 400.0f;  // (the camera close to him: it may be inside something)
        TVec3f side;
        side.cross(mCamDirZ, up);
        if (MR::isNearZero(side)) side.set< f32 >(1.0f, 0.0f, 0.0f);
        MR::normalizeOrZero(&side);
        TVec3f pts[5];
        pts[0] = mPosition + up * 60.0f;
        pts[1] = mPosition + up * 140.0f;
        pts[2] = pts[0] + side * 70.0f;
        pts[3] = pts[0] - side * 70.0f;
        pts[4] = mPosition + up * 20.0f;
        for (int i = 0; i < sSilhRays && !hit; ++i) {
            const TVec3f seg(pts[i] - mCamPos);
            hit = MR::isExistMapCollision(mCamPos, seg);
        }
        if (hit) {
            sSilhClear = 0;
            ++sSilhHit;
        } else {
            ++sSilhClear;
            if (sSilhClear >= static_cast< u32 >(sSilhHold)) {
                hint = 1;
                ++sSilhRayClear;
            }
        }
    }
    if (sSilhAsked >= 3600) {
        port_log_summary("silhgate (game): %u pictures: hint cannot-show %u (not drawn %u, rays clear %u), rays hit %u", sSilhAsked, sSilhExact + sSilhRayClear, sSilhExact, sSilhRayClear, sSilhHit);
        sSilhAsked = sSilhExact = sSilhRayClear = sSilhHit = 0;
    }
    return hint;
}
extern "C" int port_silhouette_hint(void) {
    MarioActor* p = MarioAccess::getPlayerActor();
    if (p == nullptr) {
        return 0;
    }
    return p->portSilhouetteHint();
}
#endif

void MarioActor::initShadow() {
    _A08 = 1;

    if (gIsLuigi) {
        _A10 = new ModelHolder("LuigiShadow", true);
    } else {
        _A10 = new ModelHolder("MarioShadow", true);
    }

    _A10->initWithoutIter();

    _A14 = reinterpret_cast< J3DModelX* >(MR::getJ3DModel(_A10));
    J3DModelX* pSource = mModels[0];
    J3DModelX* pShadow = _A14;
    pShadow->_DD = pSource->_DD;

    for (u32 idx = 0; idx < pShadow->_DD; idx++) {
        pShadow->mExtraMtxBuffer[idx] = pSource->mExtraMtxBuffer[idx];
    }
}

void MarioActor::calcViewReflectionModel() {
    J3DModelX* pModel = getJ3DModel();
    TPos3f matrix;
    PSMTXCopy(getJ3DModel()->getBaseTRMtx(), matrix.toMtxPtr());
    TPos3f offset;
    PSMTXIdentity(offset.toMtxPtr());
    f32 factor;
    f32 distance = 10.0f;
    if (isAnimationRun("\x83\x5e\x81\x5b\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76")) {
        distance = 25.0f;
    }

    if (_A08 != 6 && _A08 != 7) {
        _A24 = 0;
    }

    if (_A08 != 3) {
        _A25 = 0;
    }

    TVec3f position;
    switch (_A08) {
    case 2: {
        matrix.mMtx[0][1] = -matrix.mMtx[0][1];
        matrix.mMtx[1][1] = -matrix.mMtx[1][1];
        matrix.mMtx[2][1] = -matrix.mMtx[2][1];
        position = mMario->mShadowPos - *mMario->_45C->getNormal(0) * distance;
        TVec3f root;
        getRealPos("All_Root", &root);
        TVec3f translation(mPosition - root);
        PSMTXTrans(offset.toMtxPtr(), translation.x, translation.y, translation.z);
        updateReflectAlphaDL(_A09);
        break;
    }
    case 3: {
        TVec3f x;
        x.set< f32 >(matrix.mMtx[0][0], matrix.mMtx[1][0], matrix.mMtx[2][0]);
        TVec3f y;
        y.set< f32 >(matrix.mMtx[0][1], matrix.mMtx[1][1], matrix.mMtx[2][1]);
        TVec3f z;
        z.set< f32 >(matrix.mMtx[0][2], matrix.mMtx[1][2], matrix.mMtx[2][2]);
        const TVec3f& rNormal = mMario->mSwim->mSurfacePos;
        f32 dx = MR::vecKillElement(x, rNormal, &x);
        f32 dy = MR::vecKillElement(y, rNormal, &y);
        f32 dz = MR::vecKillElement(z, rNormal, &z);
        x -= rNormal * dx;
        y -= rNormal * dy;
        z -= rNormal * dz;
        matrix.mMtx[0][0] = x.x;
        matrix.mMtx[1][0] = x.y;
        matrix.mMtx[2][0] = x.z;
        matrix.mMtx[0][1] = y.x;
        matrix.mMtx[1][1] = y.y;
        matrix.mMtx[2][1] = y.z;
        matrix.mMtx[0][2] = z.x;
        matrix.mMtx[1][2] = z.y;
        matrix.mMtx[2][2] = z.z;
        position = mMario->mSwim->mSurfaceNorm - rNormal * distance;
        f32 depth = -mMario->mSwim->mWaterDepth;
        if (depth < 40.0f) {
            factor = 0.0f;
        } else if (depth < 200.0f) {
            factor = (depth - 40.0f) / 160.0f;
        } else {
            factor = 1.0f;
        }

        f32 alpha = mCamDirZ.dot(rNormal);
        MR::clamp01(&alpha);
        alpha *= 1.414f;
        MR::clamp01(&alpha);
        alpha *= 64.0f;
        f32 target = factor * alpha;
        if (_A25 > static_cast< u8 >(target)) {
            _A25--;
        } else if (_A25 < static_cast< u8 >(target)) {
            _A25++;
        }

        updateSimpleAlphaDL(_A25);
        _B18 = position;
        break;
    }
    case 6:
    case 7: {
        TVec3f x;
        x.set< f32 >(matrix.mMtx[0][0], matrix.mMtx[1][0], matrix.mMtx[2][0]);
        TVec3f y;
        y.set< f32 >(matrix.mMtx[0][1], matrix.mMtx[1][1], matrix.mMtx[2][1]);
        TVec3f z;
        z.set< f32 >(matrix.mMtx[0][2], matrix.mMtx[1][2], matrix.mMtx[2][2]);
        TVec3f normal(mMario->mSwim->_178 - mPosition);
        MR::normalize(&normal);
        f32 dx = MR::vecKillElement(x, normal, &x);
        f32 dy = MR::vecKillElement(y, normal, &y);
        f32 dz = MR::vecKillElement(z, normal, &z);
        x -= normal * dx;
        y -= normal * dy;
        z -= normal * dz;
        matrix.mMtx[0][0] = x.x;
        matrix.mMtx[1][0] = x.y;
        matrix.mMtx[2][0] = x.z;
        matrix.mMtx[0][1] = y.x;
        matrix.mMtx[1][1] = y.y;
        matrix.mMtx[2][1] = y.z;
        matrix.mMtx[0][2] = z.x;
        matrix.mMtx[1][2] = z.y;
        matrix.mMtx[2][2] = z.z;
        position = mMario->mSwim->_178 - normal * distance;
        f32 edgeDistance = mMario->mSwim->getWaterEdgeDist();
        if (edgeDistance < 0.0f) {
            factor = 0.0f;
        } else if (edgeDistance < 1000.0f) {
            factor = 0.5f + 0.5f * (edgeDistance / 1000.0f);
        } else {
            factor = 0.0f;
        }

        f32 alpha = mCamDirZ.dot(normal);
        MR::clamp01(&alpha);
        alpha *= 1.414f;
        MR::clamp01(&alpha);
        alpha *= 127.0f;
        f32 target = factor * alpha;
        if (_A24 > static_cast< u8 >(target)) {
            _A24--;
        } else if (_A24 < static_cast< u8 >(target)) {
            _A24++;
        }

        updateSimpleAlphaDL(_A24);
        _B18 = position;
        break;
    }
    }

    matrix.setTrans(position);
    PSMTXCopy(matrix.toMtxPtr(), _E0C.toMtxPtr());
    if (getCarrySensor() != nullptr) {
        TPos3f carry;
        PSMTXConcat(_BC8.mMtx, MR::getJointMtx(getCarrySensor()->mHost, 0), carry.toMtxPtr());
        PSMTXConcat(_E0C.toMtxPtr(), carry.toMtxPtr(), _E0C.toMtxPtr());
        _9A0->calcType0(_E0C.toMtxPtr());
    }

#ifdef TARGET_PC
    // GXV_MIRROR25 (reflfur25): the reflection of a picture whose mirrored model is outside the frustum is neither calculated nor drawn. The census
    // (GXV_MIRROR25_CENSUS=1, also with the level 0) counts the same decision without acting on it.
    mPortReflSkip = 0;
    {
        const int mirLevel = port::mirror25::Level();
        if (mirLevel >= 1 || port::mirror25::CensusOn()) {
            port::mirror25::Input in;
            in.hidden = isAllHidden();
            in.fading = (_A08 >= 6 && _A24 < 2) || (_A08 == 3 && _A25 < 2) || !(_A08 & 2);
            in.outside = false;
            in.cameraNear = false;
            if (!in.hidden && !in.fading) {
                in.outside = portReflectionOutside(position, &in.cameraNear);
            }
            const port::mirror25::Reason why = port::mirror25::Decide(in);
            ++sMirCensus.reason[why];
            ++sMirCensus.mode[_A08 & 7];
            if (why == port::mirror25::kDrawn && _A08 == 2 && mirLevel >= 2) {
                ++sMirCensus.single;
            }
            if (mirLevel >= 1 && port::mirror25::Skips(why)) {
                mPortReflSkip = 1;
            }
        }
    }
    if (mPortReflSkip) {
        MR::loadViewMtx();  // (as the end of this function: the view matrix, unchanged)
        return;
    }
#endif
    PSMTXConcat(_BC8.mMtx, offset.toMtxPtr(), offset.toMtxPtr());
    PSMTXConcat(matrix.toMtxPtr(), offset.toMtxPtr(), matrix.toMtxPtr());
    PSMTXConcat(j3dSys.mViewMtx, matrix.toMtxPtr(), j3dSys.mViewMtx);
    pModel->viewCalc3(3, nullptr);
    MR::loadViewMtx();
}

void MarioActor::calcViewWallShadowModel() {
    _A0C = 0;
    if (_A08 & 2) {
        return;
    }

    if (mBeeWallWalk) {
        return;
    }

    TVec3f direction;
    s32 length = 0;
    const AreaObj* pArea = MR::getAreaObj("ExtraWallCheckArea", _2A0);
    if (pArea != nullptr) {
        MR::calcCubeAxisZ(pArea, &direction);
        length = MR::getAreaObjArg(pArea, 0);
        _20C = pArea;
        _210 = 0;
    } else {
        pArea = MR::getAreaObj("ExtraWallCheckCylinder", _2A0);
        if (pArea != nullptr) {
            length = calcCylinderToCenter(pArea, &direction);
            _20C = pArea;
            _210 = 1;
        }
    }

    bool fading = false;
    if (pArea == nullptr || static_cast< f32 >(length) < 0.0f) {
        if (_208 >= 1000.0f) {
            return;
        }

        _208 += 10.0f;
        if (_208 >= 1000.0f) {
            _208 = 1000.0f;
        }

        switch (_210) {
        case 0:
            MR::calcCubeAxisZ(_20C, &direction);
            length = MR::getAreaObjArg(_20C, 0);
            break;
        case 1:
            length = calcCylinderToCenter(_20C, &direction);
            break;
        }

        fading = true;
    }

    Triangle triangle;
    TVec3f position;
    if (!MR::getFirstPolyOnLineToMap(&position, &triangle, _2A0, direction * static_cast< f32 >(length))) {
        return;
    }

    _1F0 = position;
    _1FC = *triangle.getNormal(0);
    if (!fading) {
        _208 = 0.9f * _208 + 0.1f * (position - _2A0).length();
    }

    updateRandomTexture(_208);
    TVec3f normal(direction);
    MR::normalizeOrZero(&normal);
    J3DModelX* pModel = getJ3DModel();
    J3DModelX* pSimple = getSimpleModel();
    PSMTXCopy(pModel->getBaseTRMtx(), pSimple->getBaseTRMtx());
    TPos3f& matrix = *reinterpret_cast< TPos3f* >(pSimple->getBaseTRMtx());
    TVec3f x;
    matrix.getXDir(x);
    MR::vecKillElement(x, normal, &x);
    TVec3f y;
    matrix.getYDir(y);
    MR::vecKillElement(y, normal, &y);
    TVec3f z;
    matrix.getZDir(z);
    MR::vecKillElement(z, normal, &z);
    matrix.setXDir(x);
    matrix.setYDir(y);
    matrix.setZDir(z);
    TVec3f horizontal;
    MR::vecKillElement(mPosition - position, getGravityVec(), &horizontal);
    TVec3f translation = mPosition + normal * (10.0f + horizontal.length());
    MR::setMtxTrans(matrix.toMtxPtr(), translation.x, translation.y, translation.z);
    PSMTXConcat(matrix.toMtxPtr(), _BC8.mMtx, matrix.toMtxPtr());
    pSimple->viewCalcRef(3, pModel);
    _A0C = 1;
}

void MarioActor::drawShadow() const {
    bool hidden = _482 || _481;
    if (hidden) {
        return;
    }

    if (!(_A08 & 1)) {
        return;
    }

    if (mPlayerMode == PlayerMode_Teresa) {
        return;
    }

    if (mBeeWallWalk && mMario->mMovementStates._1) {
        return;
    }

    if (mMario->isStatusActive(MarioStatus_FpView) && _1A1) {
        return;
    }

    if (_924 != nullptr && selectNoShadow(_924)) {
        return;
    }

    if (mMario->mMovementStates._F && mMario->_544 > 1) {
        return;
    }

    J3DModelX* pSimple = getSimpleModel();
    TPos3f matrix;
    PSMTXScale(matrix.toMtxPtr(), mScale.x, mScale.y, mScale.z);
    PSMTXConcat(matrix.toMtxPtr(), _BC8.mMtx, matrix.toMtxPtr());
    PSMTXConcat(getJ3DModel()->getBaseTRMtx(), matrix.toMtxPtr(), pSimple->getBaseTRMtx());
    _214->calcView(pSimple, 2, getJ3DModel());
    if (mMario->mSinkTimer > 180) {
        return;
    }

    if (mMario->mSinkTimer > 128) {
        MR::hideJointAndChildren(pSimple, "Hip");
    }

    if (mMario->mSinkTimer > 150) {
        MR::hideJointAndChildren(pSimple, "ShoulderL");
        MR::hideJointAndChildren(pSimple, "ShoulderR");
    }

    if (_A58 || mMario->mSinkTimer > 120) {
        MR::hideJoint(pSimple, "HandL0");
        MR::hideJoint(pSimple, "HandR0");
    }

    _214->drawAndCaptureTex(pSimple, mPosition);
    _214->clearAlphaBuffer();
    if (mBeeWallWalk) {
        _214->_307 = 0;
    } else {
        _214->_307 = 1;
    }

    _214->draw();
    MR::showJointAndChildren(pSimple, "JointRoot");
}

void MarioActor::decideShadowMode() {
#ifdef TARGET_PC
    if (port::mirror25::Level() >= 1 || port::mirror25::CensusOn()) {
        if (port::mirror25::Tick(sMirCensus)) {  // (once a picture: 300 pictures a line)
            char line[480];
            port::mirror25::Format(line, sizeof line, sMirCensus, port::mirror25::Level());
            port_log_summary("%s", line);
            sMirCensus = port::mirror25::Census();
        }
    }
#endif
    if (getMovementStates()._2 && !mMario->isNotReflectGlassGround() && !mMario->isStatusActive(MarioStatus_FpView) &&
        (mMario->_962 == 14 || mMario->_962 == 33)) {
        _A08 = 2;
        f32 distance = mMario->mVerticalSpeed / 500.0f;
        if (distance >= 0.75f) {
            distance = 0.75f;
        }

        u32 maximum = 64;
        distance = 1.0f - distance;
        u8 target = static_cast< f32 >(maximum) * distance;
        for (s32 i = 0; i < 4; i++) {
            if (_A09 < target) {
                _A09++;
            } else if (_A09 > target) {
                _A09--;
            }
        }
    } else if (_A09) {
        _A09--;
        if (_A09) {
            _A09--;
        }

        if (_A09) {
            _A09--;
        }

        if (_A09) {
            _A09--;
        }

        if (!_A09) {
            _A08 = 1;
        } else {
            _A08 = 2;
        }
    } else {
        _A08 = 1;
    }

    if (mMario->isSwimming() && MR::isInWater(mCamPos)) {
        if (_A25 || mCamDirZ.dot(-getGravityVector()) > 0.0f) {
            _A08 = 3;
        }

        if (_A24 || (mMario->mSwim->getWaterEdgeDist() > 0.0f && mMario->mSwim->getWaterEdgeDist() < 1000.0f)) {
            _A08 = 7;
        }
    }

    _214->setUpdateFlag();
}

void MarioActor::calcViewSilhouetteModel() {
    J3DModelX* pModel = getJ3DModel();
    J3DModelX* pSimple = getSimpleModel();
    TVec3f direction(mPosition - mCamPos);
    MR::normalizeOrZero(&direction);
    PSMTXCopy(pModel->getBaseTRMtx(), pSimple->getBaseTRMtx());
    TPos3f& matrix = *reinterpret_cast< TPos3f* >(pSimple->getBaseTRMtx());
    TVec3f x;
    matrix.getXDir(x);
    MR::vecKillElement(x, direction, &x);
    TVec3f y;
    matrix.getYDir(y);
    MR::vecKillElement(y, direction, &y);
    TVec3f z;
    matrix.getZDir(z);
    MR::vecKillElement(z, direction, &z);
    matrix.setXDir(x);
    matrix.setYDir(y);
    matrix.setZDir(z);
    TVec3f root;
    TVec3f translation;
    if (_EA4) {
        getRealPos("All_Root", &root);
        translation = root - direction * getConst().getTable()->mSilhouetteZoffset;
    } else {
        translation = mPosition;
    }

    MR::setMtxTrans(matrix.toMtxPtr(), translation.x, translation.y, translation.z);
    PSMTXConcat(matrix.toMtxPtr(), _BC8.mMtx, matrix.toMtxPtr());
    pSimple->viewCalcRef(1, pModel);
}

f32 MarioActor::calcCylinderToCenter(const AreaObj* pAreaObj, TVec3f* pVec) {
    TVec3f upVec;
    MR::calcCylinderUpVec(&upVec, pAreaObj);

    TVec3f centerPos;
    MR::calcCylinderCenterPos(&centerPos, pAreaObj);

    MR::vecKillElement(centerPos - _2A0, upVec, pVec);

    if (MR::normalizeOrZero(pVec)) {
        if (_208 >= 1000.0f) {
            return -1.0f;
        }

        _208 += 10.0f;

        if (_208 >= 1000.0f) {
            _208 = 1000.0f;
        }

        return -1.0f;
    }

    return MR::getAreaObjArg(pAreaObj, 0);
}

void MarioActor::calcViewBlurModel() {
    J3DModelX* pModel = getJ3DModel();
    if (pModel->_1E5) {
        if (_A6E == 1) {
            _A6E = 2;
        }

        pModel->_1E5 = 0;
    }

    if (MR::isDemoActive()) {
        if (_A6E) {
            _A6E = 5;
        }

        return;
    }

    if (_1C1) {
        return;
    }

    if (!_A6E) {
        return;
    }

    if (_A6E >= 3) {
        _A6E--;
        return;
    }

    if (_A6E == 2) {
        for (u32 i = 0; i < 8; i++) {
            for (u32 j = 0; j < getModelData()->getDrawMtxNum(); j++) {
                PSMTXCopy(pModel->getDrawMtx(j), _A70[i][j]);
                PSMTXCopy(pModel->getDrawMtx(j), _A90[i][j]);
            }
        }

        _B12 = 0;
        _A6E = 1;
    } else if (_37C % 3 == 0) {
        _B12 = (_B12 + 1) & 7;
        for (u32 j = 0; j < getModelData()->getDrawMtxNum(); j++) {
            PSMTXCopy(pModel->getDrawMtx(j), mBlurMatrices[1 - _B10][_B12][j]);
        }
    }

    _B10 = 1 - _B10;
    pModel->_1E4 = 1;
}

void MarioActor::calcViewFootPrint() {
    if (!_934 && mMario->mMovementStates._1 &&
        (mMario->mTargetWalkSpeedIndex || getMario()->_1C.mIsUnderwater || mMario->mWalkSpeed > 0.1f || mMario->isPlayerModeHopper())) {
        if (!mMario->mSinkTimer && !mMario->mDrawStates._9 && !mMario->mDrawStates._A && mMario->_1C._13 && mMario->_1C._14 &&
            (mMario->checkCurrentFloorCodeSevere(13) || mMario->checkCurrentFloorCodeSevere(26))) {
            _B48->addPrint(mPosition, mMario->mFrontVec, mMario->_368, false);
        }
    }

    if (_B48->isValid(_37C) && MR::isInWater(*_B48->getPrintPos(_37C))) {
        _B48->invalidate(_37C);
    }
}

#ifdef TARGET_PC
// MP3: self puppet (MpPuppets): the silhouette behind walls would show the MarioActor's shape. (merge of 3.0 on c181: the test is in front of drawSilhouette, which is registered
// as the draw functor through this wrapper, so drawSilhouette has c181's control flow and its PGO profile.)
void MarioActor::drawSilhouetteMp() const {
    if (MpSelfPuppet::hideLocalBody()) {
        return;
    }
    drawSilhouette();
}
#endif

void MarioActor::drawSilhouette() const {
    bool hidden = _482 || _483 || _481;
    if (hidden) {
        return;
    }

    if (mMario->isStatusActive(MarioStatus_Sukekiyo)) {
        return;
    }

    if (mMario->isStatusActive(MarioStatus_Bury)) {
        if (!MR::isExistMapCollision(mCamPos, mPosition - mCamPos)) {
            return;
        }

        if (!MR::isExistMapCollision(mCamPos, _2AC - mCamPos)) {
            return;
        }
    }

    if (mMario->mSinkTimer) {
        return;
    }

    if (mPlayerMode == PlayerMode_Teresa) {
        return;
    }

    if (mMario->isStatusActive(MarioStatus_FpView) && _1A1) {
        return;
    }

    if (_EA4) {
        return;
    }

    if (_934) {
        return;
    }

#ifdef TARGET_PC
    sSilhDrawn = true;
#endif
    J3DModelX* pModel = getSimpleModel();
    pModel->setDrawView(1);
    pModel->mFlags.clear();
    pModel->mFlags._1C = true;
    pModel->directDraw(nullptr);
    pModel->mFlags._1C = false;
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
}
