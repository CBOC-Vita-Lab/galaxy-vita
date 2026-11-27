#include "Game/Screen/CaptureScreenDirector.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <cstring>

namespace {
    const TimingInfo cTimingInfo[] = {{"GameScreen", 32, true, 1}, {"Indirect", 1, false, 0},    {"Camera", 4, true, 1},
                                      {"WipeMap", 5, true, 1},     {"SystemWipe", 20, false, 1}, {"Unused", 16, false, 0}};
};  // namespace

CaptureScreenDirector::CaptureScreenDirector() : NameObj("\x89\xe6\x96\xca\x83\x4c\x83\x83\x83\x76\x83\x60\x83\x83"), _C(nullptr), mTimingType("Indirect"), mTexture(nullptr), _18(false) {
    MR::CurrentHeapRestorer heapRestorer(MR::getStationedHeapGDDR3());

    mTexture = new JUTTexture(JUTVideo::getManager()->getRenderMode()->fbWidth, JUTVideo::getManager()->getRenderMode()->efbHeight, GX_TF_RGB565);
}

void CaptureScreenDirector::captureIfAllow(const char* pName) {
    const TimingInfo* pInfo = findFromName(pName);

    if (getUsingTiming() == pInfo && (getUsingTiming()->_C == 0 || _18)) {
#ifdef TARGET_PC
        // gpu23 (GXV_INDCAP_SKIP, vgx_draw.cpp MarkTransientCopies): the "Indirect" capture with no other timing asked for is read
        // only by this picture's draws after it; the front end leaves it out when there are none (the marker is written only with
        // the switch on).
        if (_C == nullptr && pInfo != nullptr && strcmp(pInfo->mName, "Indirect") == 0) {
            // (round 2: tagged with the stage, a 16-bit hash of its name and scenario, never 0: the front end keeps every capture of a
            // stage in which a draw read an earlier picture's capture, IndDummy materials and particles before the capture among them)
            u32 h = 2166136261u;
            const char* pStage = MR::getCurrentStageName();
            for (const char* q = pStage != nullptr ? pStage : ""; *q != 0; ++q) {
                h = (h ^ static_cast< u8 >(*q)) * 16777619u;
            }
            h = (h ^ static_cast< u32 >(MR::getCurrentScenarioNo())) * 16777619u;
            u32 tag = (h ^ (h >> 16)) & 0xFFFFu;
            if (tag == 0) {
                tag = 1;
            }
            port_gx_marker(PORT_GX_MARK_TRANSIENT_COPY | (tag << 8));
        }
#endif
        capture();

        _18 = false;
    }
}

void CaptureScreenDirector::capture() {
    GXRenderModeObj* pRenderModeObj = JUTVideo::getManager()->getRenderMode();

    GXSetCopyFilter(GX_FALSE, pRenderModeObj->sample_pattern, GX_FALSE, pRenderModeObj->vfilter);
    mTexture->capture(0, 0, GX_TF_RGB565, false, 0);
    GXSetCopyFilter(GX_FALSE, pRenderModeObj->sample_pattern, GX_TRUE, pRenderModeObj->vfilter);
}

void CaptureScreenDirector::requestCaptureTiming(const char* pName) {
    const TimingInfo* pInfo = findFromName(pName);

    if (getCurrentTiming() == nullptr || getCurrentTiming()->mTiming < pInfo->mTiming) {
        _C = pName;
        _18 = true;
    }
}

void CaptureScreenDirector::invalidateCaptureTiming(const char* pName) {
    const TimingInfo* pInfo = getCurrentTiming();

    if (pInfo != nullptr) {
        if (strcmp(pInfo->mName, pName) == 0) {
            _C = nullptr;
        }
    }
}

const ResTIMG* CaptureScreenDirector::getResTIMG() const {
    return mTexture->mTIMG;
}

u8* CaptureScreenDirector::getTexImage() const {
    return mTexture->mImage;
}

const TimingInfo* CaptureScreenDirector::getUsingTiming() const {
    if (_C != nullptr) {
        return findFromName(_C);
    }

    return findFromName(mTimingType);
}

const TimingInfo* CaptureScreenDirector::getCurrentTiming() const {
    if (_C != nullptr) {
        return findFromName(_C);
    }

    return nullptr;
}

const TimingInfo* CaptureScreenDirector::findFromName(const char* pName) const {
    for (u32 i = 0; i < ARRAY_SIZE(::cTimingInfo); i++) {
        if (strcmp(::cTimingInfo[i].mName, pName) == 0) {
            return &::cTimingInfo[i];
        }
    }

    return nullptr;
}

CaptureScreenActor::CaptureScreenActor(u32 drawType, const char* pCameraName) : NameObj("\x89\xe6\x96\xca\x83\x4c\x83\x83\x83\x76\x83\x60\x83\x83"), mCameraName(pCameraName) {
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, drawType);
}

void CaptureScreenActor::draw() const {
    MR::captureScreenIfAllow(mCameraName);
}
