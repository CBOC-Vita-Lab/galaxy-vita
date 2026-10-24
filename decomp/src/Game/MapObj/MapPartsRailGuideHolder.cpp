#include "Game/MapObj/MapPartsRailGuideHolder.hpp"
#include "Game/MapObj/MapPartsRailGuideDrawer.hpp"
#include "Game/Util/JMapInfo.hpp"

MapPartsRailGuideHolder::MapPartsRailGuideHolder() : NameObj("\x83\x8c\x81\x5b\x83\x8b\x83\x4b\x83\x43\x83\x68\x95\xdb\x8e\x9d"), mNumRailGuides() {
}

MapPartsRailGuideDrawer* MapPartsRailGuideHolder::createRailGuide(LiveActor* pHost, const char* pModelName, const JMapInfoIter& rIter) {
    s32 railId = -1;
    rIter.getValue("CommonPath_ID", &railId);
    MapPartsRailGuideDrawer* pDrawer = find(railId);
    if (pDrawer == nullptr) {
        pDrawer = new MapPartsRailGuideDrawer(pHost, pModelName);
        pDrawer->init(rIter);

        u32 index = mNumRailGuides;
        mNumRailGuides++;
        mDrawers[index] = pDrawer;
    }

    return pDrawer;
}

void MapPartsRailGuideHolder::init(const JMapInfoIter& rIter) {
}

MapPartsRailGuideDrawer* MapPartsRailGuideHolder::find(s32 railId) {
    MapPartsRailGuideDrawer** pEnd = mDrawers + mNumRailGuides;
    for (MapPartsRailGuideDrawer** pDrawer = mDrawers; pDrawer != pEnd; pDrawer++) {
        if (railId == (*pDrawer)->mRailId) {
            return *pDrawer;
        }
    }

    return nullptr;
}

MapPartsRailGuideHolder::~MapPartsRailGuideHolder() {
}
