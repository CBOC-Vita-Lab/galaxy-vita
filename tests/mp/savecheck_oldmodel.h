// Galaxy 3.0 multiplayer (tests, owner: snapval): a line-by-line model of the game's ORIGINAL save parsers (before the MP3
// hardening), for ASan: what UserFile::loadFromGameDataBinary / loadFromConfigDataBinary read and write for a block.
//
// Transcribed from decomp/src/Game/System: BinaryDataChunkHolder::loadFromFileBinary + calcBinarySize,
// GameDataPlayerStatus, GameEventFlagStorage, StarPieceAlmsStorage, SpinDriverPathStorage (+Galaxy, +Scenario, +One),
// GameEventValueChecker, GameDataAllGalaxyStorage (+Some, BinaryDataContentAccessor), ConfigDataCreateChunk, ConfigDataMii,
// ConfigDataMisc, and JSUMemoryInputStream (bounded reads). Differences from the game, all on the safe side for the model:
//  - the block is copied to the end of a guarded mapping of exactly its length (the game: the 0xF80 temp buffer, so a read past a
//    0x60 config block or a short block reads stale bytes; here it is an ASan report), and dataSize is 0xF80 like the game;
//  - raw u16/u32 loads go through memcpy (the game dereferences possibly misaligned pointers; the bounds are what matter);
//  - every galaxy hash counts as known, with the stored scenario count (the game skips unknown galaxies: a subset of these reads);
//  - SpinDriverPathStorageScenario's MR::Vector is a heap array of exactly 16 entries, written without a check like push_back;
//  - a null mGalaxyName pointer (the game dereferences it) calls abort().
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

namespace oldmodel {

inline uint16_t ld16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t ld32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// JSUMemoryInputStream (readData clamps to the length it was given; positions are s32).
struct Stream {
    const uint8_t* buf;
    int32_t len, pos;
    bool error;
    Stream(const uint8_t* b, int32_t l) : buf(b), len(l), pos(0), error(false) {}
    int32_t read(void* dst, int32_t n) {
        int32_t want = n;
        if (pos + n > len) n = len - pos;
        if (n > 0) {
            memcpy(dst, buf + pos, (size_t)n);
            pos += n;
        }
        if (n != want) error = true;
        return n;
    }
    uint8_t u8() { uint8_t v = 0; read(&v, 1); return v; }     // (the game's readU8 leaves v uninitialised on a short read)
    uint16_t u16() { uint16_t v = 0; read(&v, 2); return v; }
    int32_t available() const { return len - pos; }
    void seekRel(int32_t off) {
        pos += off;
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
    }
};

inline uint32_t hashOf(const char* s) { uint32_t h = 0; for (; *s; s++) h = (uint32_t)(uint8_t)*s + h * 31u; return h; }

struct SpinOne { int32_t zone, id; float range; };
struct Counters { uint32_t chunks, galaRecords, spinOnes, flags, values; };

// SpinDriverPathStorageScenario::deserialize (+ One::deserialize). `vec` = the 16-entry vector.
inline int32_t spinScenario(const uint8_t* pData, SpinOne* vec, int* vecCount, Counters* c) {
    *vecCount = 0;  // resetAllData
    int32_t readOffset = 0, zoneId = -1;
    for (int32_t idx = 0;;) {
        if (++idx > 100) return 0;
        const uint8_t val = *(pData + readOffset + 2);
        if (val != 0xFF) {
            if ((val & 0xC0) == 0xC0) {
                zoneId = val & 0x3F;
                readOffset++;
                continue;
            }
            if (zoneId < 0) return 0;
            const uint8_t* p = pData + readOffset + 2;
            SpinOne one;
            int32_t readSize = 0;
            if ((p[0] & 0x80) == 0) {
                one.range = (p[0] & 0x40) ? 1.0f : 0.0f;
            } else {
                Stream s(p + 1, 1);
                uint8_t dr = s.u8();
                one.range = dr == 0 ? 1.0f : dr / 256.0f;
                readSize = s.pos;
            }
            one.zone = zoneId;
            one.id = p[0] & 0x3F;
            readOffset += readSize + 1;
            vec[(*vecCount)++] = one;  // MR::Vector::push_back: mArray[mCount++] = item, no capacity check
            c->spinOnes++;
        } else {
            return readOffset + 3;
        }
    }
}

inline int32_t spinChunk(const uint8_t* pData, uint32_t maxBufferSize, Counters* c) {
    SpinOne* vec = (SpinOne*)malloc(16 * sizeof(SpinOne));  // mOneStorage.init(16)
    int vecCount = 0;
    Stream stream(pData, (int32_t)maxBufferSize);
    const uint8_t numEntries = stream.u8();
    for (int32_t idx = 0; idx < numEntries; idx++) {
        const uint8_t* readPtr = stream.buf + stream.pos;
        (void)ld16(readPtr);  // findFromHashCode(*(u16*)readPtr): every hash known (model)
        {   // SpinDriverPathStorageGalaxy::deserialize(readPtr, maxBufferSize - pos)
            const uint8_t* g = readPtr;
            (void)ld16(g + 0);
            const uint8_t numScenarios = g[4];  // == mNumScenarios (model: the stored count)
            int32_t size = 0;
            for (int32_t s = 0; s < numScenarios; s++) size += spinScenario(g + size + 6, vec, &vecCount, c);
            (void)ld16(g + 2);
        }
        stream.seekRel(ld16(readPtr + 2));
    }
    free(vec);
    return 0;
}

// BinaryDataContentAccessor::getPointer: a stream over the header (getHeaderSize bytes), 4 skipped, then hash/offset pairs.
inline const uint8_t* galaPointer(const uint8_t* acc, const char* name, const uint8_t* rec) {
    const int32_t headerSize = ld16(acc) * 4 + 4;
    Stream s(acc, headerSize);
    uint8_t skip[4];
    s.read(skip, 4);
    const uint16_t want = (uint16_t)hashOf(name);
    for (int32_t i = 0; i < ld16(acc); i++) {
        const uint16_t h = s.u16(), pos = s.u16();
        if (h == want) return rec + pos;
    }
    return nullptr;
}

inline int32_t galaChunk(const uint8_t* pData, Counters* c) {
    const int32_t galaxies = ld16(pData);
    const uint8_t* acc = pData + 2;
    uint32_t readOffset = (uint32_t)(ld16(acc) * 4 + 4) + 2;
    for (int32_t idx = 0; idx < galaxies; idx++) {
        const uint8_t* ptr = pData + readOffset;
        const uint8_t* name = galaPointer(acc, "mGalaxyName", ptr);
        if (!name) abort();  // the game: *(u16*)nullptr
        (void)ld16(name);    // findIndex: known (model)
        const uint8_t* star = galaPointer(acc, "mPowerStarFlag", ptr);
        volatile uint8_t sink = star ? *star : 0;
        const uint8_t* first = galaPointer(acc, "mFirstPlayFlag", ptr);
        sink = first ? *first : 0;
        const uint8_t* coins = galaPointer(acc, "mMaxCoinNum", ptr);
        for (int i = 0; i < 8; i++) sink = coins ? (uint8_t)ld16(coins + 2 * i) : 0;
        (void)sink;
        c->galaRecords++;
        readOffset += ld16(acc + 2);
    }
    return 0;
}

// BinaryDataChunkHolder::loadFromFileBinary for a game data (config = false) or config (true) block. Returns its result.
inline bool loadBlock(const uint8_t* block, uint32_t len, bool config, Counters* c) {
    // The copy ends exactly at the end of a mapping that is followed by a 16 MB PROT_NONE guard: any read past the block, however far
    // (a wild offset from a corrupt count), faults in every run. On a malloc'd buffer the report depended on what the heap held beyond
    // the redzone: after the 1M-snapshot fuzz two of the wild-offset demos read other live memory and did not fault (suitefix).
    const size_t kPage = 4096, kGuard = 16u << 20, body = ((len ? len : 1) + kPage - 1) / kPage * kPage;
    uint8_t* area = (uint8_t*)mmap(nullptr, body + kGuard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == (uint8_t*)MAP_FAILED || mprotect(area + body, kGuard, PROT_NONE) != 0) abort();
    uint8_t* pData = area + body - len;
    memcpy(pData, block, len);
    const uint32_t dataSize = 0xF80;  // SaveDataHandler::getEnoughtTempBufferSize()
    // calcBinarySize
    uint32_t size = 4;
    for (int32_t idx = 0; idx < pData[1]; idx++) size += ld32(&pData[size] + 8);
    bool ok = true;
    if (dataSize < size) {
        ok = false;
    } else {
        uint32_t offset = 4;
        bool hashError = false, deserializeError = false;
        static const uint32_t kGame[6][2] = {{0x504C4159u, 0x27C90Fu}, {0x464C4731u, 0}, {0x50434531u, 0}, {0x53504E31u, 0x12345679u},
                                              {0x564C4531u, 0x564C4531u}, {0x47414C41u, 0xBF0640EEu}};
        static const uint32_t kConf[3][2] = {{0x434F4E46u, 0x2432DAu}, {0x4D494920u, 0x2836E9u}, {0x4D495343u, 1u}};
        for (int32_t idx = 0; idx < pData[1]; idx++) {
            const uint8_t* chunk = &pData[offset];
            const uint32_t sig = ld32(chunk), hash = ld32(chunk + 4), chunkSize = ld32(chunk + 8);
            const uint8_t* d = chunk + 12;
            const uint32_t n = chunkSize - 12;
            int k = -1;
            const int count = config ? 3 : 6;
            for (int i = 0; i < count; i++)
                if ((config ? kConf[i][0] : kGame[i][0]) == sig) k = i;
            if (k >= 0) {
                uint32_t want = config ? kConf[k][1] : kGame[k][1];
                if (!config && k == 1) want = hashOf("2bytes/flag");
                if (!config && k == 2) want = hashOf("StarPieceAlmsStorage") << 5;
                if (hash != want) hashError = true;
                c->chunks++;
                int32_t status = 0;
                if (!config) {
                    switch (k) {
                    case 0: {   // GameDataPlayerStatus
                        Stream s(d, (int32_t)n);
                        uint8_t story; uint32_t pieces; uint16_t left;
                        s.read(&story, 1); s.read(&pieces, 4);
                        if (s.available() != 0) s.read(&left, 2);
                        break;
                    }
                    case 1: {   // GameEventFlagStorage
                        Stream s(d, (int32_t)n);
                        const int num = (int)n / 2;
                        for (int i = 0; i < num; i++) { (void)s.u16(); c->flags++; }
                        status = 0;
                        break;
                    }
                    case 2: {   // StarPieceAlmsStorage
                        uint16_t v[16];
                        memcpy(v, d, 32);
                        // the copy is never read by the model: keep it (and so its source read) alive for ASan at -O1 and up
                        __asm__ __volatile__("" : : "m"(v) : "memory");
                        break;
                    }
                    case 3: status = spinChunk(d, n, c); break;
                    case 4: {   // GameEventValueChecker
                        Stream s(d, (int32_t)n);
                        const int32_t num = (int32_t)n / 2;
                        for (int32_t i = 0; i < num; i++) { (void)s.u16(); (void)s.u16(); c->values++; }
                        break;
                    }
                    case 5: status = galaChunk(d, c); break;
                    }
                } else {
                    Stream s(d, (int32_t)n);
                    uint8_t b[16];
                    if (k == 0) s.read(b, 1);
                    if (k == 1) { s.read(b, 1); s.read(b + 1, 8); if (s.available() != 0) s.read(b + 9, 1); }
                    if (k == 2) { s.read(b, 1); if (s.available() != 0) s.read(b + 1, 8); }
                }
                if (status != 0 && status != 1) deserializeError = true;
            }
            offset += chunkSize;
        }
        ok = !hashError && !deserializeError && !(dataSize < offset);
    }
    munmap(area, body + kGuard);
    return ok;
}

}  // namespace oldmodel
