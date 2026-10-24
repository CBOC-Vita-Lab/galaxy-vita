#pragma once

#include "Game/LiveActor/LiveActor.hpp"

class LargeChainParts : public LiveActor {
public:
    LargeChainParts(const char* pName = "\x82\xc5\x82\xa9\x82\xa2\x8d\xbd\x83\x70\x81\x5b\x83\x63");

    virtual void kill();

    void breakChainParts();
    void initChainParts(TVec3f*, TVec3f*, TVec3f*, bool);
};
