#pragma once

#ifdef TARGET_PC
#include "Game/Map/LightFunction.hpp"
#include "Game/Map/LightPointCtrl.hpp"
#include <JSystem/JGeometry.hpp>
#include <revolution.h>

// GXV_DRAWPIPE (dpengine, 11 Nov 2026; port/draw_pipe.h): the scene state a J3D pass reads besides its lists, and the rest of the
// small state the draw reads, as it stood at the hand-over (the game's glue fills it: SceneFunction.cpp). While a pass is drawn
// from a frozen frame gPortPipeScene points at it and the pass's readers take their values from it instead of the live objects
// the next update writes: the lights' positions and the light memo's key (LightFunction.cpp: the camera's view matrix), the
// player's light and the point light (MR::loadLight, LightUtil.cpp), the user lists' texture matrices (ShapePacketUserData.cpp:
// the inverse view matrix), the scene / fog / fade serial of GXV_J3DNATIVE's brackets (the frame's extSerial). The other fields
// are read by the game thread's own draw lists, which still read the live objects; they are copied here for the census
// (GXV_DRAWPIPE=2 names the phases that write them, and counts the turns in which the draw itself changed one after the
// hand-over) and for the day those lists move to the draw thread.
struct PortPipeScene {
    TPos3f mView;     // CameraContext::getViewMtx()
    TPos3f mInvView;  // CameraContext::getInvViewMtx()
    Mtx44 mProjection;
    u8 mPose[128];    // CameraDirector::mPoseParam1's bytes (as many as fit)
    u32 mPoseBytes;
    bool mLights;     // the scene has a light director: the two lights below are what MR::loadLight(player) loads
    bool mPlayerLight;  // false: the player's light loader loads nothing (no area light yet)
    ActorLightInfo mPlayerLightInfo;
    PointLightInfo mPointLight;
    u32 mExtSerial;   // J3DPortExtSerial()
    u32 mPointer[8];  // the star pointer: its mode, in the screen or not, its position
    u32 mBranch;      // the flags that pick GameScene::draw's branches (GameScene.cpp port_drawpipe_scene_flags)
};
extern const PortPipeScene* gPortPipeScene;  // (DrawBufferHolder.cpp) not null while a pass is drawn from a frozen frame
#endif
