#include "Game/Map/LightFunction.hpp"
#include "Game/AreaObj/LightAreaHolder.hpp"
#include "Game/Map/LightDirector.hpp"
#include "Game/Map/LightPointCtrl.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "port/hot_knob.h"
#include "port/light_memo.h"

const GXLightID cLightDataIDs[8] = {GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3, GX_LIGHT4, GX_LIGHT5, GX_LIGHT6, GX_LIGHT7};

namespace {};  // namespace

void LightFunction::initLightRegisterAll() {
    const GXLightID cLightDataIDs[8] = {GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3, GX_LIGHT4, GX_LIGHT5, GX_LIGHT6, GX_LIGHT7};
    Color8 c(255, 255, 255, 255);

    for (s32 i = 0; i < 8; i++) {
        GXLightObj obj;
        GXInitLightColor(&obj, c);
        GXInitLightPos(&obj, 0.0f, 0.0f, 0.0f);
        GXInitLightDir(&obj, 0.0f, -1.0f, 0.0f);
        GXInitLightAttn(&obj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        GXInitSpecularDir(&obj, 0.0f, -1.0f, 0.0f);
        GXInitSpecularDirHA(&obj, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f);
        GXInitLightAttn(&obj, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
        GXLoadLightObjImm(&obj, cLightDataIDs[i]);
    }
}

void LightFunction::initLightData() {
    MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->initData();
}

ResourceHolder* LightFunction::loadLightArchive() {
    char buf[0x100];
    snprintf(buf, sizeof(buf), "LightData.arc");
    return MR::createAndAddResourceHolder(buf);
}

s32 LightFunction::createLightDataParser(JMapInfo** pOut) {
    ResourceHolder* holder = MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mResourceHolder;
    JMapInfo* info = MR::tryCreateCsvParser(holder, "LightData.bcsv");
    *pOut = info;
    if (info->mData != nullptr) {
        return info->mData->mNumEntries;
    }

    return 0;
}

s32 LightFunction::createZoneDataParser(const char* pZone, JMapInfo** pOut) {
    ResourceHolder* holder = MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mResourceHolder;
    JMapInfo* info = MR::tryCreateCsvParser(holder, "Light%s.bcsv", pZone);
    *pOut = info;

    if (info != nullptr) {
        if (info->mData != nullptr) {
            return info->mData->mNumEntries;
        }

        return 0;
    }

    return 0;
}

namespace {
    void getDataLightInfo(JMapInfo* pInfo, int id, LightInfo* pLightInfo, const char* pName) {
        char colorBuf[256];
        snprintf(colorBuf, sizeof(colorBuf), "%sColor", pName);
        MR::getCsvDataColor(&pLightInfo->mColor, pInfo, colorBuf, id);
        char posBuf[256];
        snprintf(posBuf, sizeof(posBuf), "%sPos", pName);
        MR::getCsvDataVec(&pLightInfo->mPos, pInfo, posBuf, id);
        char camBuf[128];
        snprintf(camBuf, sizeof(camBuf), "%sFollowCamera", pName);
        MR::getCsvDataBool(&pLightInfo->mIsFollowCamera, pInfo, camBuf, id);
    }

    void getDataActorLightInfo(JMapInfo* pInfo, int lightID, ActorLightInfo* pActorInfo, const char* pName) {
        char lightBuf[256];
        snprintf(lightBuf, sizeof(lightBuf), "%sLight%d", pName, 0);
        getDataLightInfo(pInfo, lightID, &pActorInfo->mInfo0, lightBuf);
        snprintf(lightBuf, sizeof(lightBuf), "%sLight%d", pName, 1);
        getDataLightInfo(pInfo, lightID, &pActorInfo->mInfo1, lightBuf);
        char alphaBuf[256];
        snprintf(alphaBuf, sizeof(alphaBuf), "%sAlpha2", pName);
        MR::getCsvDataU8(&pActorInfo->mAlpha2, pInfo, alphaBuf, lightID);
        char ambBuf[256];
        snprintf(ambBuf, sizeof(ambBuf), "%sAmbient", pName);
        MR::getCsvDataColor(&pActorInfo->mColor, pInfo, ambBuf, lightID);
    }
};  // namespace

void LightFunction::getAreaLightLightData(JMapInfo* pInfo, int idx, AreaLightInfo* pLightInfo) {
    MR::getCsvDataStr(&pLightInfo->mAreaLightName, pInfo, "AreaLightName", idx);
    MR::getCsvDataS32(&pLightInfo->mInterpolate, pInfo, "Interpolate", idx);
    ::getDataActorLightInfo(pInfo, idx, &pLightInfo->mPlayerLight, "Player");
    ::getDataActorLightInfo(pInfo, idx, &pLightInfo->mStrongLight, "Strong");
    ::getDataActorLightInfo(pInfo, idx, &pLightInfo->mWeakLight, "Weak");
    ::getDataActorLightInfo(pInfo, idx, &pLightInfo->mPlanetLight, "Planet");
    MR::getCsvDataBool(&pLightInfo->mFix, pInfo, "Fix", idx);
}

const char* LightFunction::getDefaultAreaLightName() {
    return MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mDataHolder->getDefaultAreaLightName();
}

s32 LightFunction::getDefaultStepInterpolate() {
    return MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mDataHolder->getDefaultStepInterpolate();
}

void LightFunction::registerPlayerLightCtrl(const ActorLightCtrl* pCtrl) {
    MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->_1C = pCtrl;
}

void LightFunction::registerLightAreaHolder(LightAreaHolder* pHolder) {
    MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->_C = pHolder;
}

bool LightFunction::tryFindNewAreaLightID(const TVec3f& rPos, ZoneLightID* pId) {
    return MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->_C->tryFindLightID(rPos, pId);
}

AreaLightInfo* LightFunction::getAreaLightInfo(const ZoneLightID& rId) {
    const char* name = MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mZoneDataHolder->getAreaLightNameInZoneData(rId);
    return MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mDataHolder->findAreaLight(name);
}

namespace {
    void blendActorLightPos(const LightInfo& rFrom, const LightInfo& rTo, LightInfo* pResult, f32 rate) {
        if (pResult->mIsFollowCamera) {
            if (rFrom.mIsFollowCamera) {
                MR::blendVec(&pResult->mPos, rFrom.mPos, rTo.mPos, rate);
            } else {
                TVec3f pos(rFrom.mPos);
                MR::getCameraViewMtx().mult(pos, pos);
                MR::blendVec(&pResult->mPos, pos, rTo.mPos, rate);
            }
        } else if (rFrom.mIsFollowCamera) {
            TVec3f pos(rFrom.mPos);
            MR::getCameraInvViewMtx().mult(pos, pos);
            MR::blendVec(&pResult->mPos, pos, rTo.mPos, rate);
        } else {
            MR::blendVec(&pResult->mPos, rFrom.mPos, rTo.mPos, rate);
        }
    }
};  // namespace

void LightFunction::blendActorLightInfo(ActorLightInfo* pResult, const ActorLightInfo& rFrom, const ActorLightInfo& rTo, f32 rate) {
    MR::blendColor(&pResult->mInfo0.mColor, rFrom.mInfo0.mColor, rTo.mInfo0.mColor, rate);
    MR::blendColor(&pResult->mInfo1.mColor, rFrom.mInfo1.mColor, rTo.mInfo1.mColor, rate);
    MR::blendColor(&pResult->mColor, rFrom.mColor, rTo.mColor, rate);
    ::blendActorLightPos(rFrom.mInfo0, rTo.mInfo0, &pResult->mInfo0, rate);
    ::blendActorLightPos(rFrom.mInfo1, rTo.mInfo1, &pResult->mInfo1, rate);
    pResult->mAlpha2 = MR::getInterpolateValue(rate, rFrom.mAlpha2, rTo.mAlpha2);
}

namespace {
    void loadLightDiffuse(GXColor color, const TVec3f& rPos, GXLightID lightID) NO_INLINE {
        GXLightObj lightObj;

        GXInitLightPos(&lightObj, rPos.x, rPos.y, rPos.z);
        GXInitLightAttn(&lightObj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

        GXColor new_color(color);
        GXInitLightColor(&lightObj, new_color);
        GXLoadLightObjImm(&lightObj, lightID);
    }

#ifdef TARGET_PC
    // In the VR diorama the actors are seen from the headset, not from the
    // game camera.  Lights that follow the camera (positions given in view
    // space: nearly all character lights) follow the headset instead, and
    // so does the rim light on the camera (GX_LIGHT2): lit from the game
    // camera, a face seen from the front took the rim light across it.
    // Returns the view space position of `rCameraRel` placed relative to the
    // headset (x right, y up, z back, as in view space).
    bool vrCameraLightPos(TVec3f* pViewPos, const TVec3f& rCameraRel) {
        const bool sOff = PORT_HOT_KNOB(bool, getenv("PETARI_VRLIGHT_OFF") != nullptr);  // debug: the game camera's lights (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        f32 head[12], tanX, tanY;
        if (sOff || !port_vr_cull_view(head, &tanX, &tanY)) {
            return false;
        }
        // Columns: the headset's left, up and forward axes and its position.
        TVec3f world(head[3], head[7], head[11]);
        for (int i = 0; i < 3; i++) {
            f32 axis[3] = {-rCameraRel.x, rCameraRel.y, -rCameraRel.z};
            world.x += head[0 * 4 + i] * axis[i];
            world.y += head[1 * 4 + i] * axis[i];
            world.z += head[2 * 4 + i] * axis[i];
        }
        MR::getCameraViewMtx().mult(world, *pViewPos);
        return true;
    }
#endif

    inline void loadCameraLight(const LightInfo& rInfo, GXLightID lightID) {
        TVec3f position(rInfo.mPos);
#ifdef TARGET_PC
        vrCameraLightPos(&position, TVec3f(rInfo.mPos));
#endif
        ::loadLightDiffuse(GXColor(rInfo.mColor), position, lightID);
    }

    void loadLightInfoDiffuse(const LightInfo& rInfo, GXLightID lightID) NO_INLINE {
        if (rInfo.mIsFollowCamera) {
            ::loadCameraLight(rInfo, lightID);
        } else {
            TVec3f v14(rInfo.mPos);
            MR::getCameraViewMtx().mult(v14, v14);
            ::loadLightDiffuse(GXColor(rInfo.mColor), v14, lightID);
        }
    }
}  // namespace

#ifdef TARGET_PC
// GXV_LIGHT_DEDUP (default 0; 1: the draw-buffer passes; 2: all of GameScene::draw): inside a scope (port_light_scope_enter / _exit) a
// light load whose words are already in light memory is not sent (decomp/src/RVL_SDK/gx/GXLight.c, the reasoning is there), and a
// loadActorLightInfo with the inputs of the last one, when no light was sent or dropped since, sends nothing but the ambient colour
// (which it always sets, as before: the material lists between write the ambient register themselves).
//   Inputs of loadActorLightInfo: the ActorLightInfo's bytes (colours, positions, follow-camera flags, alpha), the camera's view
// matrix (MR::getCameraViewMtx) and the headset (vrCameraLightPos: never on a handheld, and the memo is not used while it is).
// Everything it sends is a function of these: lights 0, 1 and 2 and the ambient colour. So when the inputs are the ones the last
// call had and light memory has not moved since (__PortGXLightGen, which moves at every light word sent and every drop), the three
// lights hold what this call would send, and only the ambient colour is set again.
extern "C" int __PortGXLightScope;
extern "C" u32 __PortGXLightGen;
extern "C" u32 __PortGXLightSent;
extern "C" u32 __PortGXLightSkipped;
extern "C" void __PortGXLightShadowDrop(void);
extern "C" void port_log_summary(const char* fmt, ...);
namespace {
    // 0: off; 1: the draw-buffer passes; 2: also the whole of GameScene::draw (see port_light_scope_enter).
    int lightDedupMode() {
        return PORT_HOT_KNOB(int, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
            const char* e = getenv("GXV_LIGHT_DEDUP");
            const int v = (e != nullptr && *e != '\0') ? atoi(e) : 0;
            return v < 0 ? 0 : (v > 2 ? 2 : v);
        }());
    }
    port::LightMemo sActorLightMemo = {0, 0, {}};  // (port/light_memo.h)
    u32 sActorLightMemoHits = 0, sActorLightLoads = 0, sLightScopes = 0;
    // false: not memoised (the caller makes the set-up as before); else *pSame says whether it may be left out.
    bool actorLightMemoKey(const ActorLightInfo* pInfo, bool* pSame) {
        f32 head[12], tanX, tanY;
        if (port_vr_cull_view(head, &tanX, &tanY)) {
            port::LightMemoDrop(sActorLightMemo);
            return false;  // (the headset moves the camera lights: not memoised)
        }
        const Mtx* view = MR::getCameraViewMtx().toCMtx();
        *pSame = port::LightMemoSame(sActorLightMemo, __PortGXLightGen, pInfo, sizeof(ActorLightInfo), *view, sizeof(Mtx));
        return true;
    }
}  // namespace

// The scopes: DrawBufferGroup::drawOpa / drawXlu (level 1: the passes of DrawBufferExecuter -> DrawBuffer -> DrawBufferShapeDrawer)
// and GameScene::draw (level 2). Exactness does not depend on what runs inside a scope: every write to light memory in this game is a
// GXLoadLightObjImm (to the FIFO, which keeps the shadow, or into a display list, after which every list call drops it), and the front
// end's light registers change only with the words it decodes (vgx_state.cpp WriteXFWord; set once at start-up). The scope only bounds
// where the shadow is trusted: it is dropped when an outermost scope opens and closes. Returns 0 (no scope), 1 (outermost), 2 (nested).
extern "C" int port_light_scope_enter(int level) {
    if (lightDedupMode() < level) {
        return 0;
    }
    if (__PortGXLightScope > 0) {
        ++__PortGXLightScope;
        return 2;
    }
    __PortGXLightShadowDrop();
    ++__PortGXLightScope;
    return 1;
}
extern "C" void port_light_scope_exit(int opened) {
    if (!opened) {
        return;
    }
    --__PortGXLightScope;
    if (opened != 1) {
        return;  // (nested: the outermost scope goes on)
    }
    __PortGXLightShadowDrop();
    port::LightMemoDrop(sActorLightMemo);
    // (a line every 8192 scopes or 30000 set-ups, whichever comes first: about every 1-2 minutes of play at either level)
    if (++sLightScopes >= 8192 || sActorLightLoads >= 30000) {
        port_log_summary("light-dedup: last %u scopes: light set-ups %u, %u of them with the inputs of the one before (nothing sent but "
                         "the ambient colour); light objects sent %u, %u not sent (already in light memory) (GXV_LIGHT_DEDUP=%d)",
                         sLightScopes, sActorLightLoads, sActorLightMemoHits, __PortGXLightSent, __PortGXLightSkipped, lightDedupMode());
        sLightScopes = sActorLightLoads = sActorLightMemoHits = 0;
        __PortGXLightSent = __PortGXLightSkipped = 0;
    }
}
#endif

void LightFunction::loadActorLightInfo(const ActorLightInfo* pInfo) {
#ifdef TARGET_PC
    bool memo = false;
    if (__PortGXLightScope > 0) {
        bool same = false;
        memo = actorLightMemoKey(pInfo, &same);
        ++sActorLightLoads;
        if (memo && same) {
            ++sActorLightMemoHits;
            GXSetChanAmbColor(GX_COLOR0A0, GXColor(pInfo->mColor));  // (lights 0-2 hold this call's; the ambient colour as below)
            return;
        }
    }
#endif
    ::loadLightInfoDiffuse(pInfo->mInfo0, GX_LIGHT0);
    ::loadLightInfoDiffuse(pInfo->mInfo1, GX_LIGHT1);

    u8 alpha = pInfo->mAlpha2;
    GXColor c = {0, 0, 0, alpha};
    TVec3f rimPos(0.0f, 0.0f, 0.0f);  // on the camera
#ifdef TARGET_PC
    vrCameraLightPos(&rimPos, TVec3f(0.0f, 0.0f, 0.0f));
#endif
    ::loadLightDiffuse(c, rimPos, GX_LIGHT2);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor(pInfo->mColor));
#ifdef TARGET_PC
    if (memo) {
        port::LightMemoDone(sActorLightMemo, __PortGXLightGen);  // (lights 0-2 now hold what these inputs send, sent or already there)
    }
#endif
}

void LightFunction::loadLightInfoCoin(const LightInfoCoin* pInfo) {
    ::loadLightInfoDiffuse(pInfo->base, GX_LIGHT0);
    TVec3f v7(0.0f, 0.0f, -1.0f);
    GXLightObj obj;
    f32 v = pInfo->_18;
    GXInitLightColor(&obj, GXColor(pInfo->_14));
    GXInitSpecularDir(&obj, v7.x, v7.y, v7.z);
    GXInitLightAttn(&obj, 0.0f, 0.0f, 1.0f, (v / 2.0f), 0.0f, (1.0f - (v / 2.0f)));
    GXLoadLightObjImm(&obj, GX_LIGHT3);
}

void LightFunction::loadPointLightInfo(const PointLightInfo* pInfo) {
    TVec3f v7(pInfo->mPos);
    MR::getCameraViewMtx().mult(v7, v7);
    GXLightObj obj;
    GXInitLightPos(&obj, v7.x, v7.y, v7.z);
    GXInitLightDistAttn(&obj, pInfo->mRefDistance, pInfo->mRefBrightness, pInfo->mDistAttnFn);
    GXInitLightSpot(&obj, 0.0, GX_SP_OFF);
    GXInitLightColor(&obj, pInfo->mColor);
    GXLoadLightObjImm(&obj, GX_LIGHT4);
}

void LightFunction::loadAllLightWhite() {
    const GXLightID cLightDataIDs[8] = {GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3, GX_LIGHT4, GX_LIGHT5, GX_LIGHT6, GX_LIGHT7};

    for (u32 i = 0; i < ARRAY_SIZE(cLightDataIDs); i++) {
        GXLightObj lightObj;
        GXInitLightPos(&lightObj, 0.0f, 0.0f, 0.0f);

        GXColor color = {255, 255, 255, 255};
        GXInitLightColor(&lightObj, color);
        GXLoadLightObjImm(&lightObj, cLightDataIDs[i]);
    }
}

void LightFunction::calcLightWorldPos(TVec3f* pPos, const LightInfo& rInfo) {
    pPos->x = rInfo.mPos.x;
    pPos->y = rInfo.mPos.y;
    pPos->z = rInfo.mPos.z;

    if (rInfo.mIsFollowCamera) {
        TPos3f mtx;
        mtx.setInline(MR::getCameraViewMtx());
        mtx.invert(mtx);
        mtx.mult(*pPos, *pPos);
    }
}
