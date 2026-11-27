#include "Game/AudioLib/AudSoundNameConverter.hpp"
#include <JSystem/JAudio2/JAUSoundTable.hpp>
#include <JSystem/JGadget/hashcode.hpp>
#include <cstring>
#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/host_vector.h"
#include "port/logic_prof.h"
#include "port/name_index.h"
// GXV_UPD_SNDNAME (update25, 11 Nov; exact; default 0; platform/src/port/perf.cpp port_upd_sndname_mode): getSoundID's scan of the name table
// from the category's first entry, as a hash index (port/name_index.h) over the same entries: the first entry at or after the start whose hash
// and name match, which is what the scan returns. Built from the table the first time it is asked once the table is complete (every entry
// written: mNumNameDatas == mNumItems) and again if the table's storage or size changes. 1: on. 2: verify (both ways, the scan's answer is used;
// "snd-name:" lines count the calls, the entries the scans looked at, and any difference). Every mode counts for GXV_LOGIC_PROF's counts line
// (the lookups of a sampled turn and the entries the scan would look at: that is what the index saves).
extern "C" int port_upd_sndname_mode(void);
extern "C" int port_on_frame_thread(void);  // (platform/src/port/perf.cpp: the index and the counters are the game thread's; others scan as before)
namespace {
port::NameIndex< PortHostVector< uint16_t > > sSndIdx;
const AudSoundNameData* sSndIdxData = nullptr;
s32 sSndIdxN = -1;
unsigned sSndAsks = 0, sSndBad = 0;
}  // namespace
#endif

namespace {
    static const char* sCategoryNamePrefix[] = {
        "SY",   "PV", "PM", "BV", "BM", "OJ", "AT", "DM", "EV", "EM", "SV", "SM", "RS",  //
        "HO",   "BG", "MB", "ST",                                                        //
        "\0\0",                                                                          //
    };

    // static const u8 sGroupNoTable[] = { };
};  // namespace

AudSoundNameConverter::AudSoundNameConverter()
    : mNumItems(), mNumNameDatas(), mGroupItemOffsets(), mNumItemsSection0(), mNumItemsSection1(), mNumItemsSection2(), mSoundNameData() {
    init();
}

JAISoundID AudSoundNameConverter::getSoundID(const char* pName) const {
    return getSoundID(pName, JGadget::getHashCode(pName));
}

inline s32 AudSoundNameConverter::getSeSoundCategory(const char* pName) const {
    for (s32 i = 0; i < 13; i++) {
        if (pName[3] == ::sCategoryNamePrefix[i][0] && pName[4] == ::sCategoryNamePrefix[i][1]) {
            return i;
        }
    }
    return -1;
}

inline s32 AudSoundNameConverter::getOtherSoundCategory(const char* pName) const {
    for (s32 i = 13; i < 17; i++) {
        if (pName[0] == ::sCategoryNamePrefix[i][0] && pName[1] == ::sCategoryNamePrefix[i][1]) {
            return i;
        }
    }
    return -1;
}

JAISoundID AudSoundNameConverter::getSoundID(const char* pName, u32 hash) const {
    bool isSE;
    if (pName[0] == 'S' && pName[1] == 'E') {
        isSE = true;
    } else {
        isSE = false;
    }

    s32 startingOffset;
#ifdef TARGET_PC
    // A name outside every category read the word before the table (a heap
    // block header) as the first index to search from.
    s32 category = isSE ? getSeSoundCategory(pName) : getOtherSoundCategory(pName);
    if (category < 0) {
        return -1;
    }
    startingOffset = mGroupItemOffsets[category];
#else
    if (isSE) {
        startingOffset = mGroupItemOffsets[getSeSoundCategory(pName)];
    } else {
        startingOffset = mGroupItemOffsets[getOtherSoundCategory(pName)];
    }
#endif

#ifdef TARGET_PC
    const int mode = port_upd_sndname_mode();
    if ((mode != 0 || ::port::LogicProfOn()) && port_on_frame_thread()) {
        s32 found = -1;
        s32 scanned = 0;
        if (mode != 2 && mode != 1) {
            // (census only: the scan, counted)
            s32 i = startingOffset;
            for (; i < mNumItems; i++) {
                const AudSoundNameData& data = mSoundNameData[i];
                if (hash == data.mHash && strcmp(pName, data.mName) == 0) {
                    found = i;
                    break;
                }
            }
            scanned = (found >= 0 ? found + 1 : mNumItems) - startingOffset;
            PORT_LOGIC_COUNT(PORT_LC_SND_CALLS, 1);
            PORT_LOGIC_COUNT(PORT_LC_SND_SCAN, scanned > 0 ? static_cast< unsigned >(scanned) : 0);
            return found >= 0 ? mSoundNameData[found].mID : JAISoundID(-1);
        }
        const bool whole = mNumNameDatas == mNumItems && mSoundNameData != nullptr;
        if (whole && (!sSndIdx.built || sSndIdxData != mSoundNameData || sSndIdxN != mNumItems)) {
            const AudSoundNameData* d = mSoundNameData;
            sSndIdx.Build(mNumItems, [d](int i) { return static_cast< uint32_t >(d[i].mHash); });
            sSndIdxData = mSoundNameData;
            sSndIdxN = mNumItems;
        }
        const bool useIdx = whole && sSndIdx.built;
        s32 viaIdx = -2;
        if (useIdx) {
            const AudSoundNameData* d = mSoundNameData;
            viaIdx = sSndIdx.Find(hash, startingOffset, [d](int i) { return static_cast< uint32_t >(d[i].mHash); },
                                  [d, pName](int i) { return strcmp(pName, d[i].mName) == 0; });
        }
        if (!useIdx || mode == 2) {
            s32 i = startingOffset;
            for (; i < mNumItems; i++) {
                const AudSoundNameData& data = mSoundNameData[i];
                if (hash == data.mHash && strcmp(pName, data.mName) == 0) {
                    found = i;
                    break;
                }
            }
            scanned = (found >= 0 ? found + 1 : mNumItems) - startingOffset;
            if (useIdx) {
                ++sSndAsks;
                if (viaIdx != found) {
                    ++sSndBad;
                    if (sSndBad <= 8) {
                        port_log_summary("snd-name: MISMATCH \"%s\" from entry %d: scan %d, index %d", pName, static_cast< int >(startingOffset),
                                         static_cast< int >(found), static_cast< int >(viaIdx));
                    }
                }
                if ((sSndAsks & 0x3FFFu) == 0) {
                    port_log_summary("snd-name: %u asks answered both ways, %u mismatches (GXV_UPD_SNDNAME=2)", sSndAsks, sSndBad);
                }
            }
        } else {
            found = viaIdx;
            // (what the scan would have looked at, for the census)
            scanned = (found >= 0 ? found + 1 : mNumItems) - startingOffset;
        }
        PORT_LOGIC_COUNT(PORT_LC_SND_CALLS, 1);
        PORT_LOGIC_COUNT(PORT_LC_SND_SCAN, scanned > 0 ? static_cast< unsigned >(scanned) : 0);
        return found >= 0 ? mSoundNameData[found].mID : JAISoundID(-1);
    }
#endif
    for (s32 i = startingOffset; i < mNumItems; i++) {
        const AudSoundNameData& data = mSoundNameData[i];
        if (hash == data.mHash && strcmp(pName, data.mName) == 0) {
            return data.mID;
        }
    }
    return -1;
}

void AudSoundNameConverter::init() {
    const JAUSoundNameTable* table = JAUSoundNameTable::getInstance();
    initDataTable(table);
    addSectionInfo(table, 0);
    addSectionInfo(table, 1);
    addSectionInfo(table, 2);
}

void AudSoundNameConverter::initDataTable(const JAUSoundNameTable* pTable) {
    mGroupItemOffsets = new u32[17];
    mNumItems = 0;
    mNumItemsSection0 = getSoundNameTableNumItem_inSection(pTable, 0);
    mNumItemsSection1 = getSoundNameTableNumItem_inSection(pTable, 1);
    mNumItemsSection2 = getSoundNameTableNumItem_inSection(pTable, 2);

    mNumItems += mNumItemsSection0;
    mNumItems += mNumItemsSection1;
    mNumItems += mNumItemsSection2;

    mSoundNameData = new AudSoundNameData[mNumItems];

    for (s32 i = 0; i < mNumItems; i++) {
        mSoundNameData[i].mName = nullptr;
        mSoundNameData[i].mID.setAnonymous();
        mSoundNameData[i].mHash = 0;
    }
}

AudSoundNameData::AudSoundNameData() {
}

void AudSoundNameConverter::addGroupInfo(const JAUSoundNameTable* pTable, u8 sectionID, u8 groupID) {
    for (u16 i = 0; i < pTable->getNumItems_inGroup(sectionID, groupID); i++) {
        AudSoundNameData& data = mSoundNameData[mNumNameDatas];
        data.mID.set(sectionID, groupID, i);
        data.mName = pTable->getName(data.mID);
        data.mHash = JGadget::getHashCode(data.mName);
        mNumNameDatas++;
    }
}

void AudSoundNameConverter::addSectionInfo(const JAUSoundNameTable* pTable, u8 sectionID) {
    for (u8 i = 0; i < pTable->getNumGroups_inSection(sectionID); i++) {
        addGroupInfo(pTable, sectionID, i);
    }
}

u32 AudSoundNameConverter::getSoundNameTableNumItem_inSection(const JAUSoundNameTable* pTable, u8 sectionID) {
    u32 numItems = 0;
    for (u8 i = 0; i < pTable->getNumGroups_inSection(sectionID); i++) {
        switch (sectionID) {
        case 0:
            mGroupItemOffsets[i + 0] = numItems;
            break;
        case 1:
            mGroupItemOffsets[i + 14] = numItems + mNumItemsSection0;
            break;
        case 2:
            mGroupItemOffsets[i + 16] = numItems + mNumItemsSection0 + mNumItemsSection1;
            break;
        }

        numItems += pTable->getNumItems_inGroup(sectionID, i);
    }

    return numItems;
}
