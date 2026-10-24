#include "Game/Player/GroupChecker.hpp"
#include "Game/Util/HashUtil.hpp"

GroupChecker::GroupChecker(const char* pName, u32 a2) : NameObj(pName) {
    mHashTable = new HashSortTable(a2);
}

void GroupChecker::initAfterPlacement() {
    mHashTable->sort();
}

void GroupChecker::add(const NameObj* pObj) {
    const char* name = pObj->mName;
    u32 hashCode = MR::getHashCode(name);
    mHashTable->add(name, 0, true);
}

void GroupCheckManager::add(const NameObj* object, s32 index) {
    mGroups[index]->add(object);
}

bool GroupCheckManager::isExist(const NameObj* object, s32 index) {
    return mGroups[index]->mHashTable->search(object->mName, nullptr);
}

GroupChecker::~GroupChecker() {
}

GroupCheckManager::~GroupCheckManager() {
}

GroupCheckManager::GroupCheckManager(const char* pName) : NameObj(pName) {
    mGroups[0] = new GroupChecker("\x83\x4a\x83\x81\x83\x54\x81\x5b\x83\x60\x91\xce\x8f\xdb\x95\xa8\x83\x4f\x83\x8b\x81\x5b\x83\x76", 0x20);
    mGroups[1] = new GroupChecker("\x83\x58\x83\x73\x83\x6a\x83\x93\x83\x4f\x83\x7b\x83\x62\x83\x4e\x83\x58\x94\xbd\x8e\xcb\x83\x4f\x83\x8b\x81\x5b\x83\x76", 0x8);
    _14 = 2;
}