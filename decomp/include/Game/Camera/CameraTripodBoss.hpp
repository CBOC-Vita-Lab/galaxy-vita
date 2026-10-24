#pragma once

#include "Game/Camera/CameraTower.hpp"

class CameraTripodBoss : public CameraTower {
public:
    CameraTripodBoss(const char* pName = "\x8e\x4f\x8b\x72\x83\x7b\x83\x58\x83\x4a\x83\x81\x83\x89");
    virtual ~CameraTripodBoss();

    virtual CamTranslatorBase* createTranslator();

    void arrangeRound();

    /* 0x8C */ f32 mAngleY;
};
