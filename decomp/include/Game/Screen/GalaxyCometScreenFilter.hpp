#pragma once

#include "Game/Screen/LayoutActor.hpp"

class GalaxyCometScreenFilter : public LayoutActor {
public:
    /// @brief Creates a new `GalaxyCometScreenFilter`.
    GalaxyCometScreenFilter();

    void setCometType(const char*);
#ifdef TARGET_PC
    virtual void draw() const;
#endif

    /* 0x20 */ bool _20;
};
