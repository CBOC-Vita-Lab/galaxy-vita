#include "Game/Screen/LayoutActor.hpp"
#include "Game/LiveActor/Spine.hpp"
#include "Game/Screen/LayoutCoreUtil.hpp"
#include "Game/Screen/LayoutManager.hpp"
#include "Game/Screen/PaneEffectKeeper.hpp"
#include "Game/Screen/StarPointerTarget.hpp"
#include "Game/Util/LayoutUtil.hpp"

LayoutActor::LayoutActor(const char* pName, bool) : NameObj(pName), mLayoutManager(), mSpine(), mEffectKeeper(), mPointingTarget() {
}

#ifdef TARGET_PC
extern "C" void port_hrw_menu_turn(void);   // (vgx_galaxy.cpp)
#endif
void LayoutActor::movement() {
    if (MR::isDead(this)) {
        return;
    }
#ifdef TARGET_PC
    if (mPortMenu && !mFlag.mIsHidden) {
        port_hrw_menu_turn();   // (14 Nov, owner: the galaxy map flickers on the buttons; the Game Over prompt's bottom strip) menus: no in-between pictures
    }
#endif

    updateSpine();

    if (MR::isDead(this)) {
        return;
    }

    control();

    if (mLayoutManager != nullptr && !MR::isStopAnimFrame(this)) {
        mLayoutManager->movement();
    }
}

#ifdef TARGET_PC
// cpu34g (14 Nov 2026), GXV_PAIR_LAYOUT (platform/src/port/perf.cpp PairTurnBegin): in the drawn turn of a drawn + undrawn pair the layout made here (pane animation
// frames applied, pane matrices, follow positions, matrix references) is made again by the undrawn turn before the next picture; movement() advances the frames as always.
// Only the calls of the calc-anim lists 0x0D Layout and 0x0E LayoutDecoration: appear() and every direct call lay out at once.
extern "C" int port_pair_skip_turn;
extern "C" int port_pair_layout_mode(void);
extern "C" int port_anim_lod_raw_category(void);
extern "C" void port_pair_count(int what, unsigned n);
#endif

void LayoutActor::calcAnim() {
#ifdef TARGET_PC
    if (__builtin_expect(port_pair_skip_turn != 0, 0) && port_pair_layout_mode() != 0) {
        const int raw = port_anim_lod_raw_category();
        if (raw == 0x0D || raw == 0x0E) {
            port_pair_count(10, 1);
            return;
        }
    }
#endif
    if (MR::isExecuteCalcAnimLayout(this)) {
        mLayoutManager->calcAnim();
    }
}

void LayoutActor::draw() const {
    if (MR::isExecuteDrawLayout(this)) {
        mLayoutManager->draw();
    }
}

void LayoutActor::appear() {
    mFlag.mIsDead = false;

    calcAnim();
}

void LayoutActor::kill() {
    if (mEffectKeeper != nullptr) {
        mEffectKeeper->clear();
    }

    mFlag.mIsDead = true;
}

void LayoutActor::setNerve(const Nerve* pNerve) const {
    mSpine->setNerve(pNerve);
}

bool LayoutActor::isNerve(const Nerve* pNerve) const {
    return mSpine->getCurrentNerve() == pNerve;
}

s32 LayoutActor::getNerveStep() const {
    return mSpine->mStep;
}

TVec2f LayoutActor::getTrans() const {
    TVec2f trans;

    MR::copyPaneTrans(&trans, this, nullptr);
    MR::convertLayoutPosToScreenPos(&trans, trans);

    return trans;
}

namespace {
    inline void setRootTranslation(LayoutActor* pActor, const TVec2f& rTrans) {
        f32 y = rTrans.y;
        f32 x = rTrans.x;
        TVec2f trans(x, y);
        nw4r::lyt::Pane* pRootPane = MR::getRootPane(pActor);
        TVec3f translation(trans.x, trans.y, 0.0f);
        pRootPane->mTranslate.x = translation.x;
        pRootPane->mTranslate.y = translation.y;
        pRootPane->mTranslate.z = translation.z;
    }
}  // namespace
void LayoutActor::setTrans(const TVec2f& rTrans) {
    TVec2f trans;
    MR::convertScreenPosToLayoutPos(&trans, rTrans);
    ::setRootTranslation(this, trans);
}

LayoutManager* LayoutActor::getLayoutManager() const {
    return mLayoutManager;
}

void LayoutActor::createPaneMtxRef(const char* pParam1) {
    mLayoutManager->createPaneMtxRef(pParam1);
}

MtxPtr LayoutActor::getPaneMtxRef(const char* pParam1) {
    return mLayoutManager->getPaneMtxRef(pParam1);
}

void LayoutActor::initLayoutManager(const char* pName, u32 rootPaneAnimLayerNum) {
    mLayoutManager = new LayoutManager(pName, true, rootPaneAnimLayerNum, 0x100);
}

void LayoutActor::initLayoutManagerNoConvertFilename(const char* pName, u32 rootPaneAnimLayerNum) {
    mLayoutManager = new LayoutManager(pName, false, rootPaneAnimLayerNum, 0x100);
}

void LayoutActor::initLayoutManagerWithTextBoxBufferLength(const char* pName, u32 textBoxBufferLength, u32 rootPaneAnimLayerNum) {
    mLayoutManager = new LayoutManager(pName, false, rootPaneAnimLayerNum, textBoxBufferLength);
}

void LayoutActor::initNerve(const Nerve* pNerve) {
    mSpine = new Spine(this, pNerve);
}

void LayoutActor::initEffectKeeper(int param1, const char* pParam2, const EffectSystem* pEffectSystem) {
    mEffectKeeper = new PaneEffectKeeper(this, mLayoutManager, param1, pParam2);
    mEffectKeeper->init(this, pEffectSystem);
}

void LayoutActor::initPointingTarget(int maxNumTargets) {
    mPointingTarget = new StarPointerLayoutTargetKeeper(maxNumTargets);
}

void LayoutActor::updateSpine() {
    if (mSpine != nullptr) {
        mSpine->update();
    }
}
