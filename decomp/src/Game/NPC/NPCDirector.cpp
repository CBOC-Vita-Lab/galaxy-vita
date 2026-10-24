#include "Game/NPC/NPCDirector.hpp"
#include "Game/NPC/NPCParameter.hpp"
#include "Game/Util/ObjUtil.hpp"

NPCDirector::NPCDirector() : NameObj("NPC\x8e\x77\x8a\xf6") {
}

void NPCDirector::init(const JMapInfoIter& rIter) {
    mCapsParameterReader = new NPCCapsParameterReader("");
    mItemParameterReader = new NPCItemParameterReader("");
    mDataResourceHolder = MR::createAndAddResourceHolder("NPCData.arc");
}
