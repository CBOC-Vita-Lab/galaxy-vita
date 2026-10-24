#include "Game/Map/CollisionDirector.hpp"
#include "Game/Map/CollisionCategorizedKeeper.hpp"
#include "Game/Map/CollisionCode.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ObjUtil.hpp"

#define CATEGORY_KEEPER_NUM 4

CollisionDirector::CollisionDirector() : NameObj("\x92\x6e\x8c\x60\x83\x52\x83\x8a\x83\x57\x83\x87\x83\x93"), mCategoryKeeper(), mCode() {
    mCode = new CollisionCode();
    mCategoryKeeper = new CollisionCategorizedKeeper*[CATEGORY_KEEPER_NUM];

    for (s32 i = 0; i < CATEGORY_KEEPER_NUM; i++) {
        mCategoryKeeper[i] = new CollisionCategorizedKeeper(i);
    }

    MR::connectToScene(this, MR::MovementType_CollisionDirector, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
}

void CollisionDirector::init(const JMapInfoIter& rIter) {
}

void CollisionDirector::initAfterPlacement() {
}

void CollisionDirector::movement() {
    for (s32 i = 0; i < CATEGORY_KEEPER_NUM; i++) {
        mCategoryKeeper[i]->movement();
    }
}

CollisionDirector* MR::getCollisionDirector() {
    return MR::getSceneObj< CollisionDirector >(SceneObj_CollisionDirector);
}
