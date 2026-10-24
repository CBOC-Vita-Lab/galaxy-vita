#pragma once

#include "Game/Screen/LayoutActor.hpp"

class ValueControl;

struct Timing {
    /* 0x0 */ u32 mScaleStartFrame;
    /* 0x4 */ u32 mScaleKeepFrame;
    /* 0x8 */ bool mIsScaleUp;
    /* 0x9 */ bool mIsScaleDown;
    /* 0xA */ bool _A;
};

class TimeUpLayout : public LayoutActor {
public:
    /// @brief Creates a new `TimeUpLayout`.
    TimeUpLayout() : LayoutActor("\x83\x5e\x83\x43\x83\x80\x83\x41\x83\x62\x83\x76\x89\xe6\x96\xca", true) {
    }

    virtual void init(const JMapInfoIter& rIter);
};

class TimeLimitLayout : public LayoutActor {
public:
    TimeLimitLayout(u32 timeLimit);

    virtual void init(const JMapInfoIter& rIter);
    virtual void appear();
    virtual void kill();
    virtual void control();

    void setTimeLimit(u32);
    void setDisplayModeOnNormal(bool);
    bool isReadyToTimeUp() const;
    void resetFrame();
    void addFrame();
    void updateTextBox();
    void exeAppear();
    void exeCountDown();
    void exeScaleUp();
    void exeScaleKeep();
    void exeScaleDown();
    void exeFadeout();
    void exeTimeUpReady();
    const Timing* getCurrentTiming() const;
    bool updateNormal();

private:
    /* 0x20 */ u32 mTime;
    /* 0x24 */ u32 mTimeLimit;
    /* 0x28 */ ValueControl* mScaleControl;
    /* 0x2C */ ValueControl* mFadeControl;
    /* 0x30 */ const Timing* mCurrentTiming;
    /* 0x34 */ bool mIsSuspend;
    /* 0x35 */ bool _35;
};
