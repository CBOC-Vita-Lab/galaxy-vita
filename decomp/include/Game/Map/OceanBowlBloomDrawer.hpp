#pragma once

#include "Game/NameObj/NameObj.hpp"

class OceanBowl;

class OceanBowlBloomDrawer : public NameObj {
public:
    OceanBowlBloomDrawer(OceanBowl*);

    virtual void init(const JMapInfoIter&);
    virtual void draw() const;
#ifdef TARGET_PC
    void rehearse() const;  // (dive27 GXV_REHEARSE: the underwater-only material and one strip, in a warm-up scope)
#endif

    /* 0x0C */ OceanBowl* mHost;
};