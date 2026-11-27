#pragma once

// MP3 spectate: the follow camera of a waiting player's view of a teammate (see Game/MP/MpSpectateLogic.hpp for the maths and
// Game/MP/MpSpectate.cpp for when it runs). Registered as CAM_TYPE_MP_WATCH; its event chunk "MpWatch" exists only in a session.
#include "Game/Camera/Camera.hpp"
#include "Game/MP/MpSpectateLogic.hpp"

class CameraMpWatch : public Camera {
public:
    CameraMpWatch(const char* pName = "MpWatchCamera");

    virtual void reset();
    virtual CameraTargetObj* calc();

    // MpSpectate bumps this when the target changes (the next calc starts behind the new character; the view interpolator glides)
    static u32 sRestartToken;

    MpSpectate::FollowParams mParams;
    MpSpectate::FollowState mState;
    u32 mToken;
    f32 mPull;   // MP3 ingame: the collision pull-in (MpPresent::pullStep)
};
