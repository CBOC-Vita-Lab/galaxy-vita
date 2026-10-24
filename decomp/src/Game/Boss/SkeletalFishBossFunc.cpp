#include "Game/Boss/SkeletalFishBossFunc.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <cstdio>

namespace {
    static const char* sNamePosName = "\x83\x4b\x81\x5b\x83\x68\x8f\x6f\x8c\xbb\x83\x7c\x83\x43\x83\x93\x83\x67";
};  // namespace

s32 SkeletalFishBossFunc::getNearestGuardPosID(const TVec3f& rVec) {
    f32 closest = 100000.0f;
    s32 posID = -1;

    for (int i = 0; i < 16; i++) {
        TVec3f pos;

        if (!copyGuardPos(&pos, i)) {
            continue;
        }

        f32 dist = rVec.distance(pos);

        if (dist < closest) {
            closest = dist;
            posID = i;
        }
    }

    return posID;
}

bool SkeletalFishBossFunc::copyGuardPos(TVec3f* pOut, s32 idx) {
    char name[128];
    snprintf(name, sizeof(name), "%s%d", ::sNamePosName, idx);

    return MR::tryFindNamePos(name, pOut, nullptr);
}
