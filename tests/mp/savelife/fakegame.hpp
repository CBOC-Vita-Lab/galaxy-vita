// savelife harness (savefix): a small fake of the game classes that decomp/src/Game/MP/MpSaveGame.cpp touches, so the REAL
// MpSaveGame.cpp, MpSaveGate.hpp, MpSaveHook.hpp, mp_savefile.cpp, mp_config.cpp and nand.cpp can be compiled and driven on the
// host (tests/mp/savelife_test.cpp). Only the members MpSaveGame.cpp uses; the image layout is the game's (SaveDataHandler.cpp):
// 18 user-file blocks (mario/luigi 0xF80, config 0x60) + sysconf 0x80 from offset 0x140. Behaviour is modelled, not copied.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

class JMapInfo {
public:
    const void* mData = nullptr;
    int getNumEntries() const { return 0; }
    template < class T > bool getValue(int, const char*, T*) const { return false; }
};

class GameDataHolder {
public:
    JMapInfo* mMapInfo = nullptr;
};

class UserFile {
public:
    UserFile();
    const char* getGameDataName() const { return mGameName; }
    const char* getConfigDataName() const { return mConfigName; }
    void makeGameDataBinary(u8* buf, u32 size) const;
    void makeConfigDataBinary(u8* buf, u32 size) const;
    void resetAllData();
    void loadFromGameDataBinary(const char* name, const u8* buf, u32 size);
    void loadFromConfigDataBinary(const char* name, const u8* buf, u32 size);
    void setSlot(int slot, bool mario);
    bool mIsPlayerMario = true;
    GameDataHolder* mGameDataHolder;
    // fake state
    char mGameName[16];
    char mConfigName[16];
    u8 game[0xF80];
    u8 config[0x60];
};

class SysConfigFile {
public:
    void makeDataBinary(u8* buf, u32 size) const;
};

class SaveDataHandler {
public:
    u8* _18;
    static void resetSaveData(u8* image);
    static void initializeAllFileInSaveData(u8* image, const SysConfigFile* sys, const UserFile* file);
    static void copySaveDataEachFile(u8* dst, const u8* src);
    void storeUserFile(const UserFile* file);
};

class SaveDataHandleSequence {
public:
    bool isInitializedGameDataHolder() const { return mCurrentUserFile != nullptr; }
    bool isActive() const;
    void restoreSysConfigFile(SysConfigFile*) {}
    void restoreUserFile(UserFile* file, int slot, bool mario);
    SaveDataHandler* mSaveDataHandler;
    UserFile* mCurrentUserFile;
    SysConfigFile* mSysConfigFile;
};

class GameSequenceDirector {
public:
    SaveDataHandleSequence* mSaveDataHandleSequence;
};

class GameSystemSceneController {};

class GameSystem {
public:
    GameSequenceDirector* mSequenceDirector;
    GameSystemSceneController* mSceneController;
};

template < class T > class SingletonHolder {
public:
    static T* get();
};

class ScenarioData {
public:
    JMapInfo* mScenarioData = nullptr;
    bool getValueU32(const char*, int, u32*) const { return false; }
};
class GalaxyStatusAccessor {
public:
    const ScenarioData* mScenarioData = nullptr;
    const char* getName() const { return ""; }
};
class ScenarioDataIter {
public:
    bool isEnd() const { return true; }
    void goNext() {}
    GalaxyStatusAccessor makeAccessor() const { return GalaxyStatusAccessor(); }
};

namespace MR {
    ScenarioDataIter makeBeginScenarioDataIter();
    u32 getHashCode(const char*);
    const char* getCurrentStageName();
    s32 getCurrentScenarioNo();
}  // namespace MR

namespace GameSequenceFunction {}
