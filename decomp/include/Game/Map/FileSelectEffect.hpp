#pragma once

#include "Game/LiveActor/LiveActor.hpp"

class FileSelectEffect : public LiveActor {
public:
    /// @brief Creates a new `FileSelectEffect`.
    /// @param pName A pointer to the null-terminated name of the object.
    FileSelectEffect(const char* pName = "\x91\x49\x91\xf0\x8e\x9e\x83\x47\x83\x74\x83\x46\x83\x4e\x83\x67");

    virtual void init(const JMapInfoIter&);
    virtual void appear();
    virtual void calcAndSetBaseMtx();

    void disappear();

    void exeAppear();
    void exeWait();
    void exeDisappear();

    /* 0x8C */ f32 mEffectFrame;
};
