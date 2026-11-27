#pragma once

#include "Game/NameObj/NameObj.hpp"
#include <JSystem/JGeometry/THex.hpp>

class ClippingJudge : public NameObj {
public:
    ClippingJudge(const char*);

    virtual ~ClippingJudge();
    virtual void init(const JMapInfoIter&);

    virtual void movement();

    bool isJudgedToClipFrustum(const TVec3f&, f32) const;
    bool isJudgedToClipFrustum(const TVec3f&, f32, s32) const;
    void calcViewingVolume(THex3f*, f32);

    /* 0x0C */ THex3f mFrustum;
    /* 0x06C */ THex3f mClipFrustums[0x8];
    /* 0x36C */ f32 mClipDistances[0x8];
#ifdef TARGET_PC
    // (zoomclip23) The frustums above are the game's logic: with GXV_ZOOM_LOGIC_VIEW they are made from the game's own field of view
    // (MR::getFovyForLogic), never the narrower one of the handheld zoom. The picture's own frustum (the zoomed field of view, the far
    // distance of mFrustum) is made only while the zoom is in force (mPortPictureZoomed); it serves decisions that only change what is
    // drawn or when a pose is computed (GXV_ZOOM_DRAWCULL, GXV_ANIM_LOD_OFFSCREEN), never the game's logic.
    void calcViewingVolumeFovy(THex3f*, f32, f32);
    bool isJudgedToClipPictureFrustum(const TVec3f&, f32) const;
    THex3f mPortPictureFrustum;
    bool mPortPictureZoomed = false;
#endif
};

namespace MR {
    ClippingJudge* getClippingJudge();
};  // namespace MR
