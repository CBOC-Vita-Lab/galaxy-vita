#include "Game/Camera/CameraMpWatch.hpp"
#include "Game/Camera/CameraLocalUtil.hpp"
#include "Game/Camera/CameraTargetObj.hpp"
#include "Game/MP/MpPresentLogic.hpp"   // MP3 ingame: the pull-in
#include "Game/Util/MapUtil.hpp"

u32 CameraMpWatch::sRestartToken = 0;

namespace {
    MpSpectate::FollowIn readTarget(CameraTargetObj* pTarget) {
        MpSpectate::FollowIn in;
        const TVec3f& pos = pTarget->getPosition();
        const TVec3f& up = pTarget->getUpVec();
        const TVec3f& front = pTarget->getFrontVec();
        const TVec3f& move = pTarget->getLastMove();
        in.feet = MpSpectate::v3(pos.x, pos.y, pos.z);
        in.up = MpSpectate::v3(up.x, up.y, up.z);
        in.front = MpSpectate::v3(front.x, front.y, front.z);
        in.vel = MpSpectate::v3(move.x, move.y, move.z);
        return in;
    }

    void applyPose(CameraMpWatch* pCamera, const MpSpectate::FollowOut& rOut) {
        CameraLocalUtil::setPos(pCamera, TVec3f(rOut.pos.x, rOut.pos.y, rOut.pos.z));
        CameraLocalUtil::setWatchPos(pCamera, TVec3f(rOut.watch.x, rOut.watch.y, rOut.watch.z));
        CameraLocalUtil::setUpVecAndWatchUpVec(pCamera, TVec3f(rOut.up.x, rOut.up.y, rOut.up.z));
    }
}  // namespace

CameraMpWatch::CameraMpWatch(const char* pName) : Camera(pName), mToken(0), mPull(1.0f) {
}

void CameraMpWatch::reset() {
    CameraTargetObj* target = CameraLocalUtil::getTarget(this);
    mToken = sRestartToken;
    mState.init = false;
    mPull = 1.0f;
    if (target == nullptr) {
        return;
    }
    const MpSpectate::FollowIn in = readTarget(target);
    if (!MpSpectate::finite3(in.feet) || !MpSpectate::finite3(in.up) || !MpSpectate::finite3(in.front)) {
        return;
    }
    MpSpectate::followReset(&mState, in, mParams);
    applyPose(this, MpSpectate::followPose(mState, mParams));
}

CameraTargetObj* CameraMpWatch::calc() {
    CameraTargetObj* target = CameraLocalUtil::getTarget(this);
    if (target == nullptr) {
        return nullptr;
    }
    if (mToken != sRestartToken) {   // another character: start behind it (the view interpolator glides from the old view)
        mToken = sRestartToken;
        mState.init = false;
    }
    const MpSpectate::FollowIn in = readTarget(target);
    MpSpectate::FollowOut out = MpSpectate::followStep(&mState, in, mParams);
    // MP3 ingame: never inside a wall: the line from the focus to the camera against the map, the camera pulled in front of a hit
    const TVec3f focus(out.watch.x, out.watch.y, out.watch.z);
    const f32 dx = out.pos.x - focus.x, dy = out.pos.y - focus.y, dz = out.pos.z - focus.z;
    const f32 length = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    f32 hit = 2.0f;
    if (MpSpectate::finite3(out.pos) && MpSpectate::finite3(out.watch) && length > 1.0f) {
        TVec3f at;
        if (MR::getFirstPolyOnLineToMap(&at, nullptr, focus, TVec3f(dx, dy, dz))) {
            const f32 hx = at.x - focus.x, hy = at.y - focus.y, hz = at.z - focus.z;
            hit = __builtin_sqrtf(hx * hx + hy * hy + hz * hz) / length;
        }
    }
    mPull = MpPresent::pullStep(mPull, hit, length);
    if (mPull < 1.0f) out.pos = MpSpectate::v3(focus.x + dx * mPull, focus.y + dy * mPull, focus.z + dz * mPull);
    applyPose(this, out);
    return target;
}
