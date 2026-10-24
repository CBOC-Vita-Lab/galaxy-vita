#include "Game/Screen/SystemWipeHolder.hpp"
#include "Game/Screen/WipeFade.hpp"
#include "Game/Screen/WipeHolderBase.hpp"
#include "Game/Screen/WipeRing.hpp"
#include "Game/Util.hpp"
#include <JSystem/J2DGraph/J2DPicture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>

SystemWipeHolder::SystemWipeHolder() : WipeHolderBase(4, "\x83\x56\x83\x58\x83\x65\x83\x80\x83\x8f\x83\x43\x83\x76\x95\xdb\x8e\x9d"), _1C(false) {
}

void SystemWipeHolder::init(const JMapInfoIter& rIter) {
    addWipeLayout(new WipeFade("\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x8f\x83\x43\x83\x76", Color8(0, 0, 0, 255)));
    addWipeLayout(new WipeRing(false, "\x89\x7e\x83\x8f\x83\x43\x83\x76"));
    addWipeLayout(new WipeFade("\x94\x92\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x8f\x83\x43\x83\x76", Color8(255, 255, 255, 255)));
}

bool SystemWipeHolder::isCurrentAlive() const {
    return getCurrent() != nullptr && !MR::isDead(getCurrent());
}

void SystemWipeHolder::movement() {
    if (_1C) {
        if (!isCurrentAlive() || !getCurrent()->isWipeOut()) {
            _1C = false;
            MR::endToCaptureScreen("SystemWipe");
        }
    }

    if (isCurrentAlive()) {
        getCurrent()->movement();
    }
}

void SystemWipeHolder::calcAnim() {
    if (isCurrentAlive()) {
        getCurrent()->calcAnim();
    }
}

void SystemWipeHolder::draw() const {
    MR::captureScreenIfAllow("SystemWipe");
    drawGameScreenCapture();

    if (isCurrentAlive()) {
        getCurrent()->draw();
    }
}

void SystemWipeHolder::setWipeRingCenter(const TVec3f& rCenter) {
    static_cast< WipeRing* >(findWipe("\x89\x7e\x83\x8f\x83\x43\x83\x76"))->setCenterPos(rCenter);
}

void SystemWipeHolder::startGameScreenCapture() {
    _1C = true;
    MR::startToCaptureScreen("SystemWipe");
}

void SystemWipeHolder::drawGameScreenCapture() const {
    if (!_1C) {
        return;
    }

    J2DOrthoGraphSimple graph;
    graph.setPort();

    JUTTexture texture(MR::getScreenResTIMG(), 0);

    J2DPicture picture(&texture);
    f32 width = MR::getScreenWidth();
    // inlined MR::getscreenHeight
    f32 height = (s32)JUTVideo::getManager()->getEfbHeight();

    picture.draw(0.0f, 0.0f, width, height, false, false, false);
}

void SystemWipeHolder::updateWipe(const char* pWipeName) {
    _1C = false;
    MR::endToCaptureScreen("SystemWipe");
    WipeHolderBase::updateWipe(pWipeName);
}

namespace MR {
    SystemWipeHolder* createSystemWipeHolder() {
        SystemWipeHolder* newHolder = new SystemWipeHolder();
        newHolder->initWithoutIter();
        return newHolder;
    }
}  // namespace MR
