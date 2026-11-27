#include "Game/MapObj/MapPartsRailGuideHolder.hpp"
#include "Game/MapObj/MapPartsRailGuideDrawer.hpp"
#include "Game/MP/MpGame.hpp"  // MP3: shared world (the guide points are made by the first row that uses a path)
#include "Game/Util/JMapInfo.hpp"

MapPartsRailGuideHolder::MapPartsRailGuideHolder() : NameObj("\x83\x8c\x81\x5b\x83\x8b\x83\x4b\x83\x43\x83\x68\x95\xdb\x8e\x9d"), mNumRailGuides() {
}

MapPartsRailGuideDrawer* MapPartsRailGuideHolder::createRailGuide(LiveActor* pHost, const char* pModelName, const JMapInfoIter& rIter) {
    s32 railId = -1;
    rIter.getValue("CommonPath_ID", &railId);
    MapPartsRailGuideDrawer* pDrawer = find(railId);
    if (pDrawer == nullptr) {
        const int mpPool = mp_active() ? MpWorldSync::sceneObjBegin() : -1;   // MP3: the drawer and its points belong to no placement row
        pDrawer = new MapPartsRailGuideDrawer(pHost, pModelName);
        pDrawer->init(rIter);
        if (mpPool != -1) MpWorldSync::sceneObjEnd(mpPool);

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
