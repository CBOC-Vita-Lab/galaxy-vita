#pragma once

#include <JSystem/JGeometry/TVec.hpp>
#include <revolution/types.h>

class JMapIdInfo;
class JMapInfoIter;
class LiveActor;
class ViewGroupCtrlDataEntry;

class ClippingActorInfo {
public:
    ClippingActorInfo(LiveActor*);

    void judgeClipping();
    bool isJudgedToClip() const;
    bool isGroupClipping() const;
    void setTypeToSphere(f32, const TVec3f*);
    void setGroupClippingNo(const JMapInfoIter&);

    void setFarClipLevel(s32 level) {
        mFarClipLevel = level;
    }

    LiveActor* mActor;  // 0x0
    const TVec3f* _4;
    f32 _8;
    JMapIdInfo* mInfo;  // 0xC
    s16 mFarClipLevel;  // 0x10
    s16 _12;
    ViewGroupCtrlDataEntry* _14;
#ifdef TARGET_PC
    bool mPbDecor = false;  // Set only by PlantGroup::initAfterPlacement; identifies this actor as a PlantGroup.
    mutable bool mPbFar = false;  // (popin24, GXV_POPIN_HYST: GXV_PB_DECOR held it far at the last judgement)
    bool mPortZoomCulled = false;  // (GXV_ZOOM_DRAWCULL, zoomclip23: its model is stamped not to be drawn in the next picture)
    u8 mPortHorizon = 0;           // (GXV_HORIZON, horizon25: 1 = hidden behind an occluder planet and its model stamped, 2 = hidden (counted only))
    u8 mCrClip = 0;                // (GXV_CLIPSCALE, cliprange25: 1 = held clipped by the lever, with hysteresis)
    u8 mCrDraw = 0;                // (GXV_FARDRAW, cliprange25: 1 = its model is stamped not to be drawn by the lever)
    u8 mCrKind = 0xFF;             // (cliprange25: port::cliprange::Kind of the actor, 0xFF = not classified yet; refreshed every 32 turns)
    u8 mCrAge = 0;                 // (cliprange25: turns since the class was read)
    u8 mFfDwell = 0;               // (far30: turns an object that has faded back in stays in, whatever its distance; counts down)
    u8 mCrCls = 0;                 // (cpucuts25: port::cpucuts class bits the GXV_FARDRAW cut needs to take this actor, 0 = never; read with mCrKind)
    u16 mPortFloorAt = 0;          // (popin31, GXV_PB_MINPX: the turn the model's extent was measured; port/popin_site.h CachedExtent)
    f32 mPortFloorExt = -1.0f;     // (... the extent, -1 = never)
#endif
};

class ClippingActorInfoList {
public:
    ClippingActorInfoList(int);

    void add(ClippingActorInfo*);
    ClippingActorInfo* find(const LiveActor*, s32*) const;
    ClippingActorInfo* findOrNone(const LiveActor*) const;
    bool isInList(const LiveActor*) const;

    ClippingActorInfo* remove(LiveActor*);

    /* 0x00 */ int _0;
    /* 0x04 */ s32 _4;
    /* 0x08 */ ClippingActorInfo** mClippingActorList;
};
