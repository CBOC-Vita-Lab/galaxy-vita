#include "JSystem/JAudio2/JASWaveArcLoader.hpp"
#include "JSystem/JAudio2/JASDvdThread.hpp"
#include "JSystem/JAudio2/JASMutex.hpp"
#include "JSystem/JAudio2/JASTaskThread.hpp"
#include "JSystem/JKernel/JKRDvdAramRipper.hpp"
#include <cstring>
#include <stdint.h>

JASHeap* JASWaveArcLoader::sAramHeap;

JASHeap* JASWaveArcLoader::getRootHeap() {
    if (JASWaveArcLoader::sAramHeap) {
        return JASWaveArcLoader::sAramHeap;
    }
    return JASKernel::getAramHeap();
}

char JASWaveArcLoader::sCurrentDir[DIR_MAX] = "/AudioRes/Waves/";

void JASWaveArcLoader::setCurrentDir(char const* dir) {
    strcpy(sCurrentDir, dir);
    u32 len = strlen(sCurrentDir);
    if (sCurrentDir[len - 1] != '/') {
        sCurrentDir[len] = '/';
        sCurrentDir[len + 1] = '\0';
    }
}

char* JASWaveArcLoader::getCurrentDir() {
    return sCurrentDir;
}

JASWaveArc::JASWaveArc() : mHeap(this) {
    _48 = 0;
    mStatus = 0;
    _58 = 0;
    mEntryNum = -1;
    mFileLength = 0;
    _5A = 0;
    OSInitMutex(&mMutex);
}

JASWaveArc::~JASWaveArc() {
}

bool JASWaveArc::loadSetup(u32 param_0) {
    JASMutexLock mutexLock(&mMutex);
    if (_58 != param_0) {
        return false;
    }
    if (mStatus != 1) {
        return false;
    }
    _48 = 1;
    mStatus = 2;
    return true;
}

bool JASWaveArc::eraseSetup() {
    JASMutexLock mutexLock(&mMutex);
    if (mStatus == 0) {
        return false;
    }
    if (mStatus == 1) {
        mStatus = 0;
        return false;
    }
    _48 = 0;
    mStatus = 0;
    return true;
}

void JASWaveArc::loadToAramCallback(void* this_) {
    loadToAramCallbackParams* tmp = (loadToAramCallbackParams*)this_;
    JASWaveArc* wavArc = tmp->mWavArc;
#if defined(__vita__)
    static unsigned traced = 0;
    const bool trace = traced < 4;
    if (trace) {
        ++traced;
        port_log("native-wave phase=begin entry=%d aram=%08x generation=%u", int(tmp->mEntryNum), unsigned(tmp->mBase), unsigned(tmp->_C));
    }
#endif
    if (JKRDvdAramRipper::loadToAram(tmp->mEntryNum, tmp->mBase, EXPAND_SWITCH_UNKNOWN0, 0, 0, nullptr) == nullptr) {
#ifdef TARGET_PC
        port_log("JASWaveArc: loadToAram failed (entry %d, base %#x)", (int)tmp->mEntryNum, (unsigned)tmp->mBase);
#endif
        return;
    }
    wavArc->_5A--;
    if (wavArc->loadSetup(tmp->_C)) {
        wavArc->onLoadDone();
    }
#if defined(__vita__)
    if (trace) port_log("native-wave phase=complete entry=%d status=%u loaded=%u", int(tmp->mEntryNum), unsigned(wavArc->mStatus), unsigned(wavArc->_48));
#endif
}

bool JASWaveArc::sendLoadCmd() {
    JASMutexLock mutexLock(&mMutex);
    _48 = 0;
    mStatus = 1;
    loadToAramCallbackParams commandInfo;
    commandInfo.mWavArc = this;
    commandInfo.mEntryNum = mEntryNum;
    commandInfo.mBase = (uintptr_t)mHeap.getBase();
    commandInfo._C = ++_58;

    _5A++;

    if (JASDvd::getThreadPointer()->sendCmdMsg(loadToAramCallback, &commandInfo, sizeof(commandInfo)) == 0) {
        mHeap.free();
        return false;
    }
    return true;
}

bool JASWaveArc::load(JASHeap* heap) {
    if (mEntryNum < 0) {
#ifdef TARGET_PC
        port_log("JASWaveArc::load: no file entry (%p)", (void*)this);
#endif
        return false;
    }
    JASMutexLock mutexLock(&mMutex);
    if (mStatus != 0) {
        return false;
    }
    if (heap == nullptr) {
        heap = JASWaveArcLoader::getRootHeap();
    }
    if (mHeap.alloc(heap, mFileLength) == false) {
#ifdef TARGET_PC
        port_log("JASWaveArc: ARAM alloc of %#x bytes failed (entry %d)", (unsigned)mFileLength, (int)mEntryNum);
#endif
        return false;
    }
    return sendLoadCmd();
}

bool JASWaveArc::loadTail(JASHeap* heap) {
    if (mEntryNum < 0) {
        return false;
    }
    JASMutexLock mutexLock(&mMutex);
    if (mStatus != 0) {
        return false;
    }
    if (heap == nullptr) {
        heap = JASWaveArcLoader::getRootHeap();
    }
    if (mHeap.allocTail(heap, mFileLength) == false) {
        return false;
    }
    return sendLoadCmd();
}

bool JASWaveArc::erase() {
    return mHeap.free();
}

void JASWaveArc::onDispose() {
    if (eraseSetup()) {
        onEraseDone();
    }
}

void JASWaveArc::setEntryNum(s32 entryNum) {
    DVDFileInfo fileInfo;
    if (entryNum < 0) {
        return;
    }
    if (DVDFastOpen(entryNum, &fileInfo) == FALSE) {
        return;
    }
    mFileLength = fileInfo.length;
    DVDClose(&fileInfo);
    mEntryNum = entryNum;
}

void JASWaveArc::setFileName(char const* fileName) {
    char* currentDir = JASWaveArcLoader::getCurrentDir();
    size_t length = strlen(currentDir);
    length = length + strlen(fileName);
    char* path = new (JASKernel::getSystemHeap(), -4) char[length + 1];
    strcpy(path, currentDir);
    strcat(path, fileName);
    path[length] = '\0';
    int entryNum = DVDConvertPathToEntrynum(path);
    delete[] path;
    if (entryNum < 0) {
        return;
    }
    setEntryNum(entryNum);
}
