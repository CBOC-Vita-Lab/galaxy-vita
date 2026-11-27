#include "Game/Screen/MoviePlayerSimple.hpp"
#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include <cstdlib>
#endif
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/THPSimplePlayerWrapper.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/HomeButtonStateNotifier.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRDisposer.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <revolution/gx/GXEnum.h>

namespace {
    static const s16 sCinemaFrameHeightTop = 44;
    static const s16 sCinemaFrameHeightBottom = 412;
    static const GXColor sCinemaFrameColor = {0, 0, 0, 255};
};  // namespace

namespace {
    NEW_NERVE(MoviePlayerSimpleOff, MoviePlayerSimple, SimpleOff);
    NEW_NERVE(MoviePlayerSimpleOpen, MoviePlayerSimple, Open);
    NEW_NERVE(MoviePlayerSimplePreload, MoviePlayerSimple, Preload);
    NEW_NERVE(MoviePlayerSimplePlaying, MoviePlayerSimple, Playing);
    NEW_NERVE(MoviePlayerSimpleSuspend, MoviePlayerSimple, SimpleSuspend);
};  // namespace

MoviePlayerSimple::MoviePlayerSimple()
    : LayoutActor("\x83\x80\x81\x5b\x83\x72\x81\x5b\x83\x76\x83\x8c\x83\x43\x83\x84\x81\x5b", true), JKRDisposer(), mMovie(nullptr), mPlayerWrapper(nullptr), _44(false), _45(false) {
    mMovie = new Movie();
    mMovie->mMovieName = "";
    mMovie->mBuffer = nullptr;
    mMovie->mCurrentFrame = -1;
    mMovie->_18 = false;
    mMovie->_19 = false;
    mMovie->mFrameRateDefault = 1;
    mMovie->_20 = 0;
    mMovie->_24 = 0;
}

MoviePlayerSimple::~MoviePlayerSimple() {
    SingletonHolder< GameSystem >::get()->mHomeButtonStateNotifier->unregisterMoviePlayerSimple(this);
    mPlayerWrapper->quit();
    delete mPlayerWrapper;
    JKRHeap::destroy(mHeap);
}

void MoviePlayerSimple::init(const JMapInfoIter& rIter) {
    MR::connectToSceneMovie(this);

    mPlayerWrapper = new THPSimplePlayerWrapper("THP\x83\x56\x83\x93\x83\x76\x83\x8b\x83\x76\x83\x8c\x83\x43\x83\x84\x81\x5b\x82\xcc\x83\x89\x83\x62\x83\x70");
    mPlayerWrapper->init(0);

    mHeap = JKRExpHeap::create(calcNeedMemoryForMovieWorks(), MR::getCurrentHeap(), true);

    initNerve(GET_NERVE_ANON(MoviePlayerSimpleOff));
    kill();
}

void MoviePlayerSimple::draw() const {
    if (!isNerve(GET_NERVE_ANON(MoviePlayerSimplePlaying)) && !isNerve(GET_NERVE_ANON(MoviePlayerSimpleSuspend))) {
        GXColor fillColor;
        fillColor.r = 0;
        fillColor.g = 0;
        fillColor.b = 0;
        fillColor.a = 255;

        MR::fillScreen(fillColor);
    } else {
        drawCinemaFrame();

        u32 frameBufferWidth;

        if (MR::isScreen16Per9()) {
            frameBufferWidth = MR::getFrameBufferWidth();
        } else {
            frameBufferWidth = 832;
        }

        u32 ySize = mMovie->mVideoInfo.ySize;
        u32 v4 = (MR::getFrameBufferWidth() - frameBufferWidth) / 2;
        u32 v5 = (MR::getFrameBufferHeight() - ySize) / 2;
        THPGXSetTexObjFilter(GX_LINEAR);
        mMovie->mCurrentFrame = mPlayerWrapper->drawCurrentFrame(JUTVideo::getManager()->getRenderMode(), v4, v5, frameBufferWidth, ySize);
    }
}

void MoviePlayerSimple::startMovie(const char* pMovieName, bool a2) {
    if (!MR::isFileExist(pMovieName, false)) {
        OSPanic(__FILE__, 0xDB, "Moive File NOT found.");
    }

    mMovie->mMovieName = pMovieName;
    mMovie->_18 = a2;
    mMovie->_24 = 0;

    play(a2);
    appear();
    MR::requestMovementOn(this);
    SingletonHolder< GameSystem >::get()->mHomeButtonStateNotifier->registerMoviePlayerSimple(this);
}

void MoviePlayerSimple::stopMovie() {
    stop();
    kill();
    setNerve(GET_NERVE_ANON(MoviePlayerSimpleOff));
}

bool MoviePlayerSimple::isMovieActive() const {
    if (MR::isDead(this)) {
        return false;
    }

    return isNerve(GET_NERVE_ANON(MoviePlayerSimpleOpen)) || isNerve(GET_NERVE_ANON(MoviePlayerSimplePreload)) ||
           isNerve(GET_NERVE_ANON(MoviePlayerSimplePlaying)) || isNerve(GET_NERVE_ANON(MoviePlayerSimpleSuspend));
}

bool MoviePlayerSimple::isMoviePlaying() const {
    if (isMovieActive()) {
        return isNerve(GET_NERVE_ANON(MoviePlayerSimplePlaying));
    }

    return false;
}

s32 MoviePlayerSimple::getCurrentFrame() const {
    if (isMovieActive()) {
        return mMovie->mCurrentFrame;
    }

    return -1;
}

s32 MoviePlayerSimple::getTotalFrame() const {
    return mPlayerWrapper->getTotalFrame();
}

void MoviePlayerSimple::setFrameRateDefault() {
    f32 frameRate = mPlayerWrapper->getFrameRate();

    if (MR::isNearZero(frameRate - 59.94f)) {
        mMovie->mFrameRateDefault = 1;
    } else if (MR::isNearZero(frameRate - 29.97f)) {
        mMovie->mFrameRateDefault = 2;
    }
}

void MoviePlayerSimple::setVolume(f32 volume, s32 time) {
    mPlayerWrapper->setVolume(volume * 127.0f, time);
}

void MoviePlayerSimple::setUnpauseHomeButtonFlag() {
    mPlayerWrapper->setUnpauseFrameFlag();
}

void MoviePlayerSimple::exeSimpleOff() {
}

void MoviePlayerSimple::exeOpen() {
    if (MR::isFirstStep(this)) {
        mPlayerWrapper->open(mMovie->mMovieName);
    }

    if (mPlayerWrapper->_9) {
        mPlayerWrapper->getVideoInfo(&mMovie->mVideoInfo);
        mMovie->_20 = 0;
        mMovie->_19 = false;
        setFrameRateDefault();
        mMovie->mBuffer = new (mHeap, 32) u8[mPlayerWrapper->calcNeedMemory()];
        mPlayerWrapper->setBuffer(mMovie->mBuffer);
        mPlayerWrapper->preLoad(_44 != false);
        setNerve(GET_NERVE_ANON(MoviePlayerSimplePreload));
    }
}

void MoviePlayerSimple::exePreload() {
    if (mPlayerWrapper->isPreLoading()) {
        return;
    }

    setNerve(GET_NERVE_ANON(MoviePlayerSimplePlaying));
    appear();
}

#ifdef TARGET_PC
#if PORT_RUNTIME_DIAGNOSTICS
void port_movie_log_turn(s32 result);  // (THPSimplePlayerWrapper.cpp: the "movie:" log line)
#endif
namespace {
    // PETARI_MOVIE_INHAND (read once; default 1, 0 = one frame a turn as before; from Producdevity's fork, 4c7d7e4): a THP movie's
    // sound in the opening cutscenes broke up (the fork's console capture of PrologueB: 438 one-block gaps, 7.8 % of the blocks
    // silent) although the pictures kept up. The player takes one frame a turn, exactly what the sound uses, so a frame read late
    // was lost for good and from then on the sound had nothing in hand for the next late one: a block of silence each time. With
    // the switch, after a turn's frame the player decodes the sound of the next one too (its picture left out: that frame's
    // picture is not shown) while fewer than 6 frames of sound are in hand, and takes no frame while 8 or more are: the console's
    // turns come at 60.1 a second and the movie's sound at 59.94, so left alone the pictures would pull ahead of the sound (the
    // original lets 20 frames, 1/3 s, pile up over a long movie). Fork, console, with its read-ahead ring too: silent blocks
    // 559 of 7211 -> 0 of 6292 (PrologueB), pictures new on their turn 90 % -> 99 %. A movie without sound is not affected.
    bool movieInHandOn() {
        return PORT_HOT_KNOB(bool, [] {
            const char* e = getenv("PETARI_MOVIE_INHAND");
            return !(e && *e && atoi(e) == 0);
        }());
    }
}  // namespace
#endif

void MoviePlayerSimple::exePlaying() {
    if (mMovie->mCurrentFrame + 1 == mPlayerWrapper->getTotalFrame()) {
        mMovie->_24 = 0;

        if (!mMovie->_18) {
            stop();
            setNerve(GET_NERVE_ANON(MoviePlayerSimpleOff));
            return;
        }
    }

    if (!mMovie->_19) {
#ifdef TARGET_PC
        const bool inHandOn = movieInHandOn();
        if (inHandOn && mPlayerWrapper->audioFramesInHand() >= 8) {
            return;  // (PETARI_MOVIE_INHAND: the pictures do not run ahead of the sound)
        }
#endif
        s32 result = mPlayerWrapper->decode(0);
#ifdef TARGET_PC
#if PORT_RUNTIME_DIAGNOSTICS
        port_movie_log_turn(result);
#endif
        if (inHandOn && result == 0) {
            const s32 inHand = mPlayerWrapper->audioFramesInHand();
            if (inHand >= 0 && inHand < 6) {
                mPlayerWrapper->decodeAudioOnly(0);
            }
        }
#endif

        switch (result) {
        case 0:
            mMovie->_19 = true;
            break;
        case 2:
            mMovie->_24++;
            break;
        case 3:
            return;
        }
    }

    mMovie->_20++;

    if (mMovie->_19) {
        if (mMovie->mFrameRateDefault <= mMovie->_20) {
            mMovie->_20 = 0;
            mMovie->_19 = false;
        }
    }
}

void MoviePlayerSimple::exeSimpleSuspend() {
}

void MoviePlayerSimple::control() {
    mPlayerWrapper->updateNerve();
}

void MoviePlayerSimple::play(bool a1) {
    _44 = a1;
    setNerve(GET_NERVE_ANON(MoviePlayerSimpleOpen));
    MR::startStarPointerModeDemo(this);
    _45 = GameSystemFunction::setPermissionToCheckWiiRemoteConnectAndScreenDimming(false);
}

void MoviePlayerSimple::stop() {
    mPlayerWrapper->loadStop();
    mPlayerWrapper->close();
    delete[] mMovie->mBuffer;
    mMovie->mBuffer = nullptr;
    MR::endStarPointerMode(this);
    GameSystemFunction::setPermissionToCheckWiiRemoteConnectAndScreenDimming(_45);
}

u32 MoviePlayerSimple::calcNeedMemoryForMovieWorks() {
    u32 needMemory = 0;
    needMemory += 0x140000;

    s32 texCount = mPlayerWrapper->getUseTextureCount();

    needMemory += OSRoundUp32B(texCount * (MR::getFrameBufferWidth() * MR::getFrameBufferHeight()));
    needMemory += OSRoundUp32B(texCount * (MR::getFrameBufferWidth() * MR::getFrameBufferHeight() / 4));
    needMemory += OSRoundUp32B(texCount * (MR::getFrameBufferWidth() * MR::getFrameBufferHeight() / 4));

    return needMemory + 0x1188;
}

void MoviePlayerSimple::drawCinemaFrame() const {
    MR::fillScreenSetup(::sCinemaFrameColor);

    s16 width = MR::getFrameBufferWidth();
    TBox2s top(0, 0, width, ::sCinemaFrameHeightTop);
    MR::fillScreenArea(top.i, top.f);

    s16 height = MR::getFrameBufferHeight();
    TBox2s bottom(0, ::sCinemaFrameHeightBottom, width, height);
    MR::fillScreenArea(bottom.i, bottom.f);
}
