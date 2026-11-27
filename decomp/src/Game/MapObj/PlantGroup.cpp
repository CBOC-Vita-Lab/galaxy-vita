#include "Game/MapObj/PlantGroup.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#ifdef TARGET_PC
#include "Game/LiveActor/ClippingActorHolder.hpp"
#include "Game/LiveActor/ClippingActorInfo.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#endif
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util.hpp"
#include <JSystem/JMath/JMATrigonometric.hpp>
#ifdef TARGET_PC
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include "port/diagnostics.h"
#include "port/popin.h"
#include "port/popin_site.h"  // (fix31: port_pb_floor, the floor's / release's guard word)
#include "port/turn_prof.h"  // (GXV_HR_PLANTMOVE, GXV_TURN_PROF: perf.cpp)
#include <cstdlib>
extern "C" const TVec3f* port_safe_player_pos(void);  // (decomp/src/Game/LiveActor/LodCtrl.cpp; flow23)
#include "port/pose_phase.h"  // (ratecut31: GXV_POSE_PHASE, GXV_HR_PLANTMOVE's left-out movements on the skip turn; perf.cpp)
extern "C" int port_pose_phase_mode(void);
extern "C" int port_pose_phase_skip;
extern "C" int port_pose_phase_census_flag;
extern "C" void port_pose_phase_note(int kind, int age, int tagged);
extern "C" int port_pb_budget_on(void);
extern "C" int64_t port_host_time_ns(void);  // (popin31b: the census's price line)
#endif

namespace {
    const s32 hIsShakableStep = 20;
    const f32 sSize = 160.0f;
    const f32 sCollisionRadius = 50.0f;
    // sWindRadius
    // sMaxAnimRate
    // sMinAnimRate
    const f32 sCheckLineLength = 1000.0f;
    const f32 sCoinVel = 30.0f;
    const f32 sDPDTouchRadius = 100.0f;
    const f32 sScratchVel = 6.0f;
    const s32 hGenerateItemTouch = 1;
    const s32 sHintEffectEmitInterval = 300;
    const f32 hShakeStartFrame = 3.0f;
    const f32 hWeakRate = 0.5f;
    const f32 hMiddleRate = 1.0f;
    const f32 hStrongRate = 1.5f;
    const f32 hStrongDist = 100.0f;
    const f32 hMiddleDist = 300.0f;
    const f32 hWeakDist = 500.0f;
}  // namespace

void PlantGroup_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)3.8146973e-06f;
    (void)-3.8146973e-06f;
    (void)0.5f;
    (void)3.0f;
    (void)PI;
    (void)HALF_PI;
    (void)2.0f;
    (void)-HALF_PI;
}

namespace NrvPlantMember {
    NEW_NERVE(HostTypeNrvWait, PlantMember, Wait);
    NEW_NERVE(HostTypeNrvHint, PlantMember, Hint);
    NEW_NERVE(HostTypeNrvShakeWeak, PlantMember, ShakeWeak);
    NEW_NERVE(HostTypeNrvShakeMiddle, PlantMember, ShakeMiddle);
    NEW_NERVE(HostTypeNrvShakeStrong, PlantMember, ShakeStrong);
}  // namespace NrvPlantMember

PlantGroup::PlantGroup(const char* pName)
    : LiveActor(pName), mMembers(), mMemberCount(7), mPlantType(), mIsStarPiece(), mHintPosition(0.0f), mHintRotation(0.0f),
      mHintTimer(::sHintEffectEmitInterval), mHintIndex() {
}

void PlantGroup::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_MapObj, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
    MR::initDefaultPos(this, rIter);
    const char* name = nullptr;
    MR::getObjectName(&name, rIter);

    if (MR::isEqualString(name, "FlowerGroup")) {
        mPlantType = 1;
    } else if (MR::isEqualString(name, "FlowerBlueGroup")) {
        mPlantType = 2;
    } else {
        mPlantType = 0;
    }

    s32 count = -1;
    MR::getJMapInfoArg0NoInit(rIter, &count);

    if (count > 0) {
        mMemberCount = count;
    } else if (count == -1) {
        mMemberCount = 7;
    }

    s32 itemCount = 0;
    MR::getJMapInfoArg1NoInit(rIter, &itemCount);

    if (itemCount < 0) {
        itemCount = 0;
    }
#ifdef TARGET_PC
    if (itemCount == 0 && (MR::isEqualString(name, "CutBushGroup") || MR::isEqualString(name, "FlowerBlueGroup")) &&
        MR::isEqualStageName("HoneyBeeKingdomGalaxy")) {
        const char* mode = std::getenv("GXV_HONEY_PLANT_REVEAL");
        mPortSplitNeutral = !(mode && *mode == '0');
    }
#endif

    s32 itemType = -1;
    MR::getJMapInfoArg2NoInit(rIter, &itemType);

    if (itemType == 1) {
        mIsStarPiece = true;
        MR::declareStarPiece(this, itemCount);
    } else {
        mIsStarPiece = false;
        MR::declareCoin(this, itemCount);
    }

    initMember(itemCount, rIter);
    initSound(4, false);
    initEffectKeeper(0, "Bush", false);
    MR::setEffectHostSRT(this, "HintShakeLeaf", &mHintPosition, &mHintRotation, nullptr);
    MR::initStarPointerTarget(this, 0.0f, TVec3f(0.0f));
    MR::useStageSwitchSleep(this, rIter);
    MR::useStageSwitchReadAppear(this, rIter);

    if (MR::isValidSwitchAppear(this)) {
        makeActorDead();
        MR::syncStageSwitchAppear(this);
    } else {
        makeActorAppeared();
    }

    MR::tryRegisterDemoCast(this, rIter);
    mHintTimer = MR::getRandom(3, 10) * 10;
}

void PlantGroup::makeActorAppeared() {
#ifdef TARGET_PC
    portPayOwed();
#endif
    LiveActor::makeActorAppeared();

    for (s32 i = 0; i < mMemberCount; i++) {
        mMembers[i]->makeActorAppeared();
    }
}

void PlantGroup::makeActorDead() {
#ifdef TARGET_PC
    portPayOwed();
#endif
    LiveActor::makeActorDead();

    for (s32 i = 0; i < mMemberCount; i++) {
        mMembers[i]->makeActorDead();
    }
}

void PlantGroup::initMember(s32 itemCount, const JMapInfoIter&) {
    mMembers = new PlantMember*[mMemberCount];

    for (s32 i = 0; i < mMemberCount; i++) {
        if (mPlantType == 1) {
            mMembers[i] = new PlantMember("\x89\xd4", "Flower", false, nullptr);
        } else if (mPlantType == 2) {
            mMembers[i] = new PlantMember("\x90\xc2\x82\xa2\x89\xd4", "FlowerBlue", false, nullptr);
        } else {
            mMembers[i] = new PlantMember("\x91\x90", "CutBush", true, nullptr);
        }

        mMembers[i]->initWithoutIter();
#ifdef TARGET_PC
        mMembers[i]->mDetailIdx = static_cast< u16 >(i);  // (detail25: the stable choice of the thinning)
#endif
        MR::invalidateClipping(mMembers[i]);

        if (i < itemCount) {
            mMembers[i]->mHasItem = true;
        } else {
            mMembers[i]->mHasItem = false;
        }
    }

    for (s32 i = 0; i < mMemberCount; i++) {
        s32 index = MR::getRandom(0, i + 1);
        bool hasItem = mMembers[i]->mHasItem;
        mMembers[i]->mHasItem = mMembers[index]->mHasItem;
        mMembers[index]->mHasItem = hasItem;
    }

    initHitSensor(1);
    MR::addHitSensorMapObj(this, "\x8b\xab\x8a\x45\x8b\x85", 16, 100.0f, TVec3f(gZeroVec));
}

#pragma push
#pragma global_optimizer off
s32 PlantGroup::placeOnCollisionFormCircle(TVec3f* pCenter, const TVec3f& rGravity, const TVec3f& rAxisX, const TVec3f& rAxisY) {
    pCenter->set(0, 0, 0);
    s32 ring = 0;
    f32 angle = 0.0f;
    f32 angleStep = 2.0f * PI;
    f32 radius = ::sSize * (s16)ring;
    s32 placedCount = 0;

    for (s32 i = 0; i < mMemberCount; i++) {
        TVec3f offset(rAxisX);
        offset.scale(JMACosRadian(angle));
        offset += rAxisY * JMASinRadian(angle);
        offset.scale(radius);
        TVec3f start(mPosition);
        start += offset;
        TVec3f up = rGravity * 100.0f;
        const TVec3f* pUp = &up;
        start -= *pUp;

        TVec3f ray = rGravity * ::sCheckLineLength;
        const TVec3f* pRay = &ray;

        if (MR::getFirstPolyOnLineToMap(&mMembers[i]->mPosition, nullptr, start, *pRay)) {
            *pCenter += mMembers[i]->mPosition;
            placedCount++;
        } else {
            mMembers[i]->kill();
        }

        mMembers[i]->initPosture();
        angle += angleStep;

        if (angle >= 2.0f * PI) {
            ring++;
            angle = 0.0f;
            angleStep = 2.0f * PI / (ring * 6);
            radius = ::sSize * ring;
        }
    }

    pCenter->scale(1.0f / placedCount);
    return placedCount;
}
#pragma pop

f32 PlantGroup::calcBoundingSphereRadius(const TVec3f& rCenter) {
    TVec3f min(rCenter);
    TVec3f max(rCenter);

    for (s32 i = 0; i < mMemberCount; i++) {
        if (!MR::isDead(mMembers[i])) {
            const TVec3f& pos = mMembers[i]->mPosition;

            if (pos.x < min.x) {
                min.x = pos.x;
            } else if (max.x < pos.x) {
                max.x = pos.x;
            }

            if (pos.y < min.y) {
                min.y = pos.y;
            } else if (max.y < pos.y) {
                max.y = pos.y;
            }

            if (pos.z < min.z) {
                min.z = pos.z;
            } else if (max.z < pos.z) {
                max.z = pos.z;
            }

            MR::isNoCalcAnim(mMembers[i]);
            mMembers[i]->calcAnim();
        }
    }

    mPosition.set((max + min) / 2.0f);
    TVec3f size(max - min);

    if (size.x < size.y) {
        if (size.y < size.z) {
            return size.z / 2.0f;
        }

        return size.y / 2.0f;
    }

    if (size.x < size.z) {
        return size.z / 2.0f;
    }

    return size.x / 2.0f;
}

void PlantGroup::initAfterPlacement() {
    TVec3f gravity;
    TVec3f axisX;
    axisX.set(1, 0, 0);
    TVec3f axisY;
    axisY.set(0, 1, 0);
    MR::calcGravityVector(this, mPosition, &gravity, nullptr, 0);
    MR::makeAxisCrossPlane(&axisX, &axisY, gravity);
    TVec3f center;
    placeOnCollisionFormCircle(&center, gravity, axisX, axisY);
    f32 radius = calcBoundingSphereRadius(center);
    f32 scale = mScale.y;
    HitSensor* sensor = getSensor("\x8b\xab\x8a\x45\x8b\x85");
    sensor->mRadius = ::sSize + radius * scale;
    MR::setStarPointerTargetRadius3d(this, ::sSize + radius * scale);
    MR::setClippingTypeSphere(this, ::sSize + radius * scale);
#ifdef TARGET_PC
    mPortHrRadius = ::sSize + radius * scale;
    // GXV_PB_DECOR (Performance-mode visual budget): flower and bush groups far from the camera and the player are clipped (ClippingActorInfo.cpp).
    // The group has a sensor and a star-pointer target (both at its own position); a clipped group's are off like any clipped actor's.
    MR::getClippingDirector()->mActorHolder->find(this)->mPbDecor = true;
#endif
}

#ifdef TARGET_PC
// The budget resumes an entire group on one turn. Leave at least half on that
// first picture, then let the remaining draw stamps expire on the next picture.
// This does not change the group's clip verdict or any member's simulation.
extern "C" int port_pb_demo_off(void);  // (popin25, LodCtrl.cpp: GXV_PB_DEMO=2 in a flight: the budget is off)
extern "C" void port_popin_event(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius);  // (LodCtrl.cpp: the pop-in census)
void PlantGroup::portSplitDecorReveal(const TVec3f& camera, u32 picture) {
    // (fix31) Not in a GXV_PB_DEMO=2 flight: PortPbDecorFar clears mPbFar there, so every held group "comes back" on the flight's first turn and would stagger;
    // not while the popin31 floor or the release after a flight is in force (popin31b: the plant groups' guard word, GXV_PB_MINPX_DECOR or the release):
    // a group the floor gives back is under the floor's size
    // (the stagger would be invisible work) and the release decides what may change in the picture. Both are off by default.
    if (!mPortSplitNeutral || picture == 0 || mMemberCount < 3 || mMemberCount > 7 || !port_pb_budget_on() || port_pb_demo_off() || port::popin::GuardOn(port::popin::kFlDecor)) {
        return;
    }
    const TVec3f* player = port_safe_player_pos();
    const f32 near = mPortHrRadius + 900.0f;
    if (!player || !((mPosition - *player).squared() > near * near && (mPosition - camera).squared() > near * near)) {
        return;
    }
    J3DModel* visible[7];
    s32 member[7];  // (fix31: which member each visible model is, for the census)
    s32 count = 0;
    for (s32 i = 0; i < mMemberCount; ++i) {
        PlantMember* m = mMembers[i];
        if (m->mHasItem || m->mDetailThin || m->mDetailTgt) {
            return;  // Never combine with a collectible or an already thinned cluster.
        }
        if (!MR::isDead(m) && !MR::isClipped(m) && !MR::isHiddenModel(m)) {
            J3DModel* model = MR::getJ3DModel(m);
            if (model && model->mPortCullPicture != picture) {
                member[count] = i;
                visible[count++] = model;
            }
        }
    }
    if (count < 3) {
        return;
    }
    for (s32 i = 1; i < count; i += 2) {
        visible[i]->mPortCullPicture = picture;
    }
    // (fix31) the census: each member held back appears one picture later than the game would show it, a port-made appearance (kPlantReveal) in the play or
    // flight window. Without this the reveal changed pictures that no census line counted.
    if (port::popin::LogOn()) {
        for (s32 i = 1; i < count; i += 2) {
            port_popin_event(port::popin::kPlantReveal, 1, mMembers[member[i]], &mMembers[member[i]]->mPosition, 0.0f);
        }
    }
#if PORT_LOGGING
    if (port::popin::LogOn()) {
        static unsigned reported = 0;
        if (reported < 64) {
            ++reported;
            port_log_summary("honey-plant: %s native picture %u at %.0f,%.0f,%.0f eligible %d, first %d, delayed one picture %d",
                             mPlantType == 2 ? "FlowerBlue" : "CutBush", picture, mPosition.x, mPosition.y, mPosition.z,
                             count, (count + 1) / 2, count / 2);
        }
    }
#endif
}

// GXV_HR_PLANTMOVE (gamepar, 8 Nov; Performance mode by default, perf.cpp port_hr_plantmove_mode). A member resting in its looping Wait
// animation (past its first 30 steps, no light blend, no animation keeper, not stopped) moves on every second turn only, half of the
// group's resting members each turn; the group must be farther than GXV_HR_PLANT_PDIST beyond its touch sensor from the player and not
// scratched by the second player's pointer. What a left-out movement does, and why leaving it out is the same game:
//  - ModelManager::update (the BCK clock, XanimePlayer::updateBefore/AfterMovement, and any material players): REPLAYED before the
//    member's next movement (portPayOwed): update(), then the latch _88 cleared as that turn's calcAnim cleared it (the deferral or
//    calcAnm). So the clock makes the same steps in the same order. The picture of the left-out turn poses getFrame() (= f, _88 clear)
//    where the original posed _84 (= f, the frame before that turn's step): the same frame. Owed steps are also paid before the group is
//    clipped, killed or made to appear, and before a member leaves Wait (any nerve change: the gate is checked every turn).
//  - Spine::update: the nerve step counts up; in Wait nothing reads it but isFirstStep (exeWait), and the gate needs step > 30.
//  - updateBinder (no velocity), effect keeper / camera / sensors / shadows (none for a member): nothing.
//  - updateLightCtrl (cut bushes): with no blend in progress and a member that never moves, the area-light find gives the same light;
//    an area light switched on or off is taken one turn later (a far plant: invisible).
//  - actorSoundMovement: the member's sound object is updated every second turn (a far plant's rustle keys, if any, may be skipped).
// So: the same clock, poses and nerve logic; the light and sound differences above are the "minor change".
bool PlantMember::portCanRest() const {
    if (MR::isDead(this) || mModelManager == nullptr || mAnimKeeper != nullptr || mFlag.mIsStoppedAnim) {
        return false;
    }
    if (!isNerve(GET_NERVE(PlantMember, HostTypeNrvWait)) || getNerveStep() <= 30) {
        return false;
    }
    if (mActorLightCtrl != nullptr && mActorLightCtrl->_1C != nullptr) {
        return false;
    }
    return true;
}
void PlantMember::portPayOwed() {
    if (mPortOwed == 0) {
        return;
    }
    mPortOwed = 0;
    if (mModelManager == nullptr) {
        return;
    }
    mModelManager->update();  // (the left-out turn's step: LiveActor::movement's first statement)
    if (mModelManager->mXanimePlayer != nullptr) {
        mModelManager->mXanimePlayer->_88 = false;  // (as that turn's calcAnim cleared it)
    }
}
void PlantGroup::portPayOwed() {
    if (mMembers == nullptr) {
        return;
    }
    for (s32 i = 0; i < mMemberCount; i++) {
        mMembers[i]->portPayOwed();
    }
}
#endif

#ifdef TARGET_PC
namespace {
u32 sFloorCostTick = 0;  // (popin31b: the sample counter of the kept groups timed for the census)
}
#endif

void PlantGroup::control() {
#ifdef TARGET_PC
    // popin31b: a group the decor floor kept (it would be clipped without it): its control (its members' movement) timed for the census's price line
    const int64_t floorT0 = __builtin_expect(mPortFloorKept != 0, 0) && port::popin::CostSample(sFloorCostTick) ? port_host_time_ns() : 0;
#endif
    bool pointing = MR::isStarPointerPointing2POnPressButton(this, nullptr, false, false);
#ifdef TARGET_PC
    bool rest = false;
    if (!pointing && port_hr_plantmove_mode() != 0) {
        // (flow23: MR::getPlayerPos() never returns null, it reads through the MarioActor; a stage without one, or before it is placed,
        // read a null actor. port_safe_player_pos: null when there is none; then the group moves every turn, as the game.)
        const TVec3f* player = port_safe_player_pos();
        if (player != nullptr) {
            const f32 d = port_hr_plant_pdist() + mPortHrRadius;
            rest = (mPosition - *player).squared() > d * d;
        }
    }
    mPortHrTick ^= 1;
    u32 nSkip = 0, nMoved = 0;
    const int ppMode = rest ? port_pose_phase_mode() : 0;  // (ratecut31: read only for a resting group)
    const bool ppSkip = port_pose_phase_skip != 0;
#endif

    for (s32 i = 0; i < mMemberCount; i++) {
        mMembers[i]->animControl(this);
#ifdef TARGET_PC
        // (ratecut31, GXV_POSE_PHASE: a resting member's movement is left out on the skip turn only, all of them then, so the movement before every picture is
        // made and its light and sound are current there too; mode 0: the group's half by its tick, as before; port/pose_phase.h PlantRestTurn)
        if (rest && mMembers[i]->mPortOwed == 0 && port::posephase::PlantRestTurn(i, mPortHrTick, ppMode, ppSkip) && mMembers[i]->portCanRest()) {
            mMembers[i]->mPortOwed = 1;  // (this turn's movement left out; its clock step is paid before the next one)
            ++nSkip;
            if (__builtin_expect(port_pose_phase_census_flag != 0, 0)) {
                port_pose_phase_note(6, 1, 0);
            }
        } else {
            mMembers[i]->portPayOwed();
            mMembers[i]->movement();
            ++nMoved;
            if (__builtin_expect(rest && port_pose_phase_census_flag != 0, 0)) {
                port_pose_phase_note(6, 0, 0);
            }
        }
#else
        mMembers[i]->movement();
#endif

        if (pointing) {
            TVec2f velocity(*MR::getStarPointerScreenVelocity(1));

            if (::sScratchVel * ::sScratchVel < velocity.squared()) {
                TVec3f pos(*MR::getStarPointerWorldPosUsingDepth(1));

                if (mMembers[i]->tryPush(pos, ::sDPDTouchRadius, 2) == true) {
                    MR::tryRumblePadVeryWeak(this, 1);
                    break;
                }
            }
        }
    }

    emitHintEffect();
#ifdef TARGET_PC
    if (nSkip != 0 || rest) {
        port_turn_prof_count(PORT_TC_PLANT_SKIP, nSkip);
        port_turn_prof_count(PORT_TC_PLANT_MOVED, nMoved);
    }
    if (floorT0 != 0) {
        port_pb_floor_cost(port::popin::kFlDecor, static_cast< u32 >(port_host_time_ns() - floorT0));
    }
#endif
}

void PlantGroup::emitHintEffect() {
    mHintTimer--;

    if (mHintTimer <= 0) {
        mHintTimer = ::sHintEffectEmitInterval;
        s32 index = (mMemberCount + mHintIndex) % mMemberCount;
        do {
            if (mMembers[index]->mHasItem && mMembers[index]->tryEmitHint()) {
                mHintIndex = (index + mMemberCount + 1) % mMemberCount;
                mHintPosition.set(mMembers[index]->mPosition);
                mHintRotation.set(mMembers[index]->mRotation);
                MR::emitEffect(this, "HintShakeLeaf");
                break;
            }

            index = (index + mMemberCount + 1) % mMemberCount;
        } while (index != mHintIndex);
    }
}

bool PlantGroup::receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor*) {
    if (msg == ACTMES_TORNADO_ATTACK || msg == ACTMES_TORNADO_STORM_RANGE || msg == ACTMES_SPIN_STORM_RANGE) {
        for (s32 i = 0; i < mMemberCount; i++) {
            mMembers[i]->tryShake(pSender);
        }
    }

    return false;
}

void PlantGroup::attackSensor(HitSensor* pSender, HitSensor* pReceiver) {
    if (MR::isSensorPlayer(pReceiver) || MR::isSensorEnemy(pReceiver)) {
        TVec3f velocity(pReceiver->mHost->mVelocity);
        MR::vecKillElement(velocity, pReceiver->mHost->mGravity, &velocity);

        if (velocity.squared() < 2.0f) {
            return;
        }

        s32 touchType = 0;

        if (MR::isSensorPlayer(pReceiver) || MR::isSensorNpc(pReceiver)) {
            touchType = 1;
        } else if (MR::sendArbitraryMsg(ACTMES_PLANT_GROUP_EMIT_ITEM, pReceiver, pSender)) {
            touchType = 1;
        }

        for (s32 i = 0; i < mMemberCount; i++) {
            mMembers[i]->tryPush(pReceiver->mPosition, pReceiver->mRadius, touchType);
        }
    }
}

void PlantGroup::startClipped() {
#ifdef TARGET_PC
    portPayOwed();  // (GXV_HR_PLANTMOVE: a clipped group moves nothing; the step its members owe is the turn before's)
#endif
    LiveActor::startClipped();

    for (s32 i = 0; i < mMemberCount; i++) {
        mMembers[i]->startClipped();
    }
}

void PlantGroup::endClipped() {
    LiveActor::endClipped();

    for (s32 i = 0; i < mMemberCount; i++) {
        mMembers[i]->endClipped();
    }
}

void PlantMember::init(const JMapInfoIter&) {
    _90 = 1.0f;
    mTouchType = 3;
    _9C = false;
    mHasItem = false;
#ifdef TARGET_PC
    // GXV_ANIM_LOD_FREE (cpuhive, 6 Nov; LiveActor.cpp AnimLodDefers): nothing but the picture reads a plant's joints or base matrix.
    // Its logic (PlantGroup::control, tryPush / tryShake, the nerves) reads its position, gravity, nerve and BCK clock (_20, kept
    // running by GXV_ANIM_LOD_XLATCH); it has no sensors, effects, shadows, collision or star-pointer target of its own (the group
    // has them, at the group's position), and no other actor follows its joints. So its joints are computed once per picture, by the
    // drawn turn's flush (port_anim_flush_stale), from the same state, at any distance: the picture and the game state are those of
    // computing it every turn.
    mLogicFreeJoints = 1;
#endif
    MR::onCalcAnim(this);
    initNerve(GET_NERVE(PlantMember, HostTypeNrvWait));
    appear();
}

bool PlantMember::tryEmitHint() {
    if (isNerve(GET_NERVE(PlantMember, HostTypeNrvWait))) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvHint));
        return true;
    }

    return false;
}

void PlantMember::exeWait() {
    if (MR::isFirstStep(this)) {
        MR::startBck(this, "Wait");
    }
}

void PlantMember::exeHint() {
    if (MR::isFirstStep(this)) {
        MR::startBck(this, "HintShake");
    }

    if (MR::isBckStopped(this)) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvWait));
    }
}

void PlantMember::exeShakeWeak() {
    if (MR::isFirstStep(this)) {
        MR::startBck(this, "Shake");
        MR::setBckFrame(this, ::hShakeStartFrame);
        MR::startSound(this, "SE_OJ_LEAVES_SWING");
        MR::setBckRate(this, ::hWeakRate);
    }

    if (MR::isBckStopped(this)) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvWait));
    }
}

void PlantMember::exeShakeMiddle() {
    if (MR::isFirstStep(this)) {
        MR::startBck(this, "Shake");
        MR::setBckFrame(this, ::hShakeStartFrame);
        MR::startSound(this, "SE_OJ_LEAVES_SWING");
        MR::setBckRate(this, ::hMiddleRate);
    }

    if (MR::isBckStopped(this)) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvWait));
    }
}

void PlantMember::exeShakeStrong() {
    if (MR::isFirstStep(this)) {
        MR::startBck(this, "Shake");
        MR::setBckFrame(this, ::hShakeStartFrame);
        MR::startSound(this, "SE_OJ_LEAVES_SWING");
        MR::setBckRate(this, ::hStrongRate);
    }

    if (MR::isBckStopped(this)) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvWait));
    }
}

bool PlantMember::generateItem(PlantGroup* pGroup) {
    if (mHasItem == true) {
        TVec3f velocity(-mGravity);
        velocity.scale(::sCoinVel);

        if (pGroup->mIsStarPiece) {
            MR::startSound(pGroup, "SE_OJ_STAR_PIECE_BURST");
            MR::appearStarPiece(pGroup, mPosition, 1, 10.0f, 40.0f, false);
        } else {
            MR::appearCoinPop(pGroup, mPosition, 1);
        }

        mHasItem = false;
        return true;
    }

    return false;
}

void PlantMember::initPosture() {
    MR::calcGravityVector(this, &mGravity, nullptr, 0);
    TVec3f axis;
    axis.set(1, 0, 0);

    if (MR::isSameDirection(mGravity, axis, 0.01f)) {
        axis.set(0, 1, 0);
    }

    TPos3f posture;
    MR::calcMtxFromGravityAndZAxis(&posture, this, mGravity, axis);
    TVec3f euler;
    TPos3f rotation;
    f32 angle = MR::getRandom(-PI, PI);
    rotation.makeRotate(mGravity, angle);
    posture.concat(rotation, posture);
    posture.getEuler(euler);
    f32 x = euler.x;
    f32 y = euler.y;
    f32 z = euler.z;
    x = _180_PI * x;
    y = _180_PI * y;
    z = _180_PI * z;
    mRotation.set(x, y, z);
    calcAnim();
}

bool PlantMember::tryShake(HitSensor* pSensor) {
    if (MR::isDead(this)) {
        return false;
    }

    if ((isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeWeak)) || isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeMiddle)) ||
         isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong))) &&
        MR::isLessStep(this, ::hIsShakableStep)) {
        return false;
    }

    TVec3f diff(pSensor->mPosition - mPosition);
    MR::vecKillElement(diff, mGravity, &diff);
    f32 distance = diff.length();

    if (distance < ::hStrongDist) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong));
    } else if (distance < ::hMiddleDist) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeMiddle));
    } else if (distance < ::hWeakDist) {
        setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeWeak));
    }

    return true;
}

bool PlantMember::tryPush(const TVec3f& rPosition, f32 radius, s32 touchType) {
    if (MR::isDead(this)) {
        return false;
    }

    if ((isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeWeak)) || isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeMiddle)) ||
         isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong))) &&
        MR::isLessStep(this, ::hIsShakableStep)) {
        return false;
    }

    TVec3f diff(rPosition - mPosition);
    radius += ::sCollisionRadius * mScale.y;
    radius *= radius;

    if (diff.squared() < radius) {
        mTouchType = touchType;

        if (isNerve(GET_NERVE(PlantMember, HostTypeNrvWait)) || isNerve(GET_NERVE(PlantMember, HostTypeNrvHint))) {
            setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeWeak));
        } else if (isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeWeak))) {
            setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeMiddle));
        } else if (isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeMiddle))) {
            setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong));
        } else if (isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong))) {
            setNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong));
        }

        return true;
    }

    return false;
}

void PlantMember::animControl(PlantGroup* pGroup) {
    if (isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeWeak))) {
        if (mTouchType == ::hGenerateItemTouch) {
            generateItem(pGroup);
        }
    } else if (isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeMiddle)) || isNerve(GET_NERVE(PlantMember, HostTypeNrvShakeStrong))) {
        if (mTouchType == 1 || mTouchType == 2) {
            generateItem(pGroup);
        }
    }
}

PlantGroup::~PlantGroup() {
}

PlantMember::~PlantMember() {
}
