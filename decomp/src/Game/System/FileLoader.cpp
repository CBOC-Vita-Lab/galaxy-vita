#include "Game/System/FileLoader.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>

FileLoader::FileLoader() {
    mLoaderThread = nullptr;
    mRequestFileInfos = nullptr;
    mRequestedFileCount = 0;
    mFileHolder = nullptr;
    mArchiveHolder = nullptr;
    mLoaderThread = new FileLoaderThread(13, 320, MR::getStationedHeapNapa());
    OSResumeThread(mLoaderThread->mThread);
    OSInitMutex(&mMutex);
    mRequestFileInfos = new RequestFileInfo[0x140];
    mFileHolder = new FileHolder();
    mArchiveHolder = new ArchiveHolder();
    mRequestedFileCount = 0;
}

void FileLoader::requestLoadToMainRAM(const char* pName, u8* pData, JKRHeap* pHeap, JKRDvdRipper::EAllocDirection direction, bool jam) {
    if (isNeedToLoadMp(pName)) {
        if (pHeap == nullptr) {
            pHeap = MR::getCurrentHeap();
        }

        RequestFileInfo* info = addRequest(pName);
        info->_0 = 0;
        info->mRequestType = direction;
        info->mFileEntry = mFileHolder->add(pName, pHeap, pData);

        if (jam) {
            OSJamMessage(&mLoaderThread->mQueue, info, OS_MESSAGE_NOBLOCK);
        } else {
            OSSendMessage(&mLoaderThread->mQueue, info, OS_MESSAGE_NOBLOCK);
        }
    }
}

void FileLoader::requestMountArchive(const char* pName, JKRHeap* pHeap, bool jam) {
    if (isNeedToLoadMp(pName)) {
        if (pHeap == nullptr) {
            pHeap = MR::getCurrentHeap();
        }

        RequestFileInfo* info = addRequest(pName);
        info->_0 = 1;
        info->mRequestType = 1;
        info->mFileEntry = mFileHolder->add(pName, pHeap, nullptr);

        if (jam) {
            OSJamMessage(&mLoaderThread->mQueue, info, OS_MESSAGE_NOBLOCK);
        } else {
            OSSendMessage(&mLoaderThread->mQueue, info, OS_MESSAGE_NOBLOCK);
        }
    }
}

bool FileLoader::isLoaded(const char* pName) const {
    return mFileHolder->isExist(pName);
}

bool FileLoader::isMountedArchive(const char* pName) const {
    return mArchiveHolder->getArchive(pName) != nullptr;
}

void* FileLoader::receiveFile(const char* pName) {
    const RequestFileInfo* info = getRequestFileInfoConst(pName);
    if (info != nullptr) {
        info->mFileEntry->waitReadDone();
    }

    return mFileHolder->getContext(pName);
}

JKRMemArchive* FileLoader::receiveArchive(const char* pName) {
    const RequestFileInfo* info = getRequestFileInfoConst(pName);
    if (info != nullptr) {
        info->mFileEntry->waitReadDone();
    }

    return mArchiveHolder->getArchive(pName);
}

void FileLoader::receiveAllRequestedFile() {
    for (u32 i = 0; i < mRequestedFileCount; i++) {
        mRequestFileInfos[i].mFileEntry->waitReadDone();
    }
}

JKRMemArchive* FileLoader::createAndAddArchive(void* pData, JKRHeap* pHeap, const char* pName) {
    return mArchiveHolder->createAndAdd(pData, pHeap, pName)->mArchive;
}

void FileLoader::getMountedArchiveAndHeap(const char* pName, JKRArchive** pArchive, JKRHeap** pHeap) const {
    mArchiveHolder->getArchiveAndHeap(pName, pArchive, pHeap);
}

void FileLoader::clearRequestFileInfo(bool) {
    mRequestedFileCount = 0;
}

void FileLoader::removeHolderIfIsEqualHeap(JKRHeap* pHeap) {
    mFileHolder->removeIfIsEqualHeap(pHeap);
    mArchiveHolder->removeIfIsEqualHeap(pHeap);
}

void FileLoader::removeFile(const char* pName) {
    mFileHolder->removeFile(pName);
}

bool FileLoader::isNeedToLoad(const char* pName) const {
    if (getRequestFileInfoConst(pName) != nullptr) {
        return false;
    }

    return !isLoaded(pName);
}

// MP3 integ (B1): a request made AFTER a scene's init (a menu mounting a layout on the main thread) stays in the list until the NEXT
// scene's init has finished (the list is cleared at the end of initializeScene), but its file died with the first scene's heap.
// The next scene's archive phase then asked for it in vain: nothing was loaded, the archive missing. A finished request whose file
// is no longer held is stale: its name is blanked (the slot stays: the loader thread may still hold the pointer) and it loads again.
// (merge of 3.0 on c181: this is a wrapper in front of isNeedToLoad, which keeps c181's text, control flow and PGO profile; the two callers ask the wrapper.)
bool FileLoader::isNeedToLoadMp(const char* pName) const {
    const RequestFileInfo* prior = getRequestFileInfoConst(pName);
    if (prior != nullptr && prior->_88 == 2 && !isLoaded(pName)) {
        const_cast< RequestFileInfo* >(prior)->mFileName[0] = '\0';
        return true;
    }
    return isNeedToLoad(pName);
}

const RequestFileInfo* FileLoader::getRequestFileInfoConst(const char* pName) const {
    for (u32 i = 0; i < mRequestedFileCount; i++) {
        RequestFileInfo* info = &mRequestFileInfos[i];

        if (MR::isEqualStringCase(info->mFileName, pName)) {
            return info;
        }
    }

    return nullptr;
}

// string load is in the wrong spot for some reason
RequestFileInfo* FileLoader::addRequest(const char* pName) {
    OSMutex* mutex = &mMutex;
    OSLockMutex(mutex);

    RequestFileInfo* info = &mRequestFileInfos[mRequestedFileCount];
    mRequestedFileCount++;
    info->mFileEntry = nullptr;
    info->_88 = 0;
    snprintf(info->mFileName, sizeof(info->mFileName), "%s", pName);

    OSUnlockMutex(mutex);

    return info;
}
