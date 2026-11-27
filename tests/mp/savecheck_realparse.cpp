// Galaxy 3.0 multiplayer (tests, owner: snapval): the game's REAL save parsers on hostile blocks, with guard pages.
// Compiled with the game's desktop flags and linked with the real BinaryDataChunkHolder, GameDataAllGalaxyStorage,
// SpinDriverPathStorage and StarPieceAlmsStorage objects: either the hardened ones of this worktree, or the original ones of the
// baseline desktop build (/home/claude/g3/desk/build). Build and run: tests/mp/savecheck_realparse.sh.
//
// ScenarioData is stood in for by the cooked game data's table (savecheck_scenarios.inc: names, scenario counts, power stars),
// so the storages are built exactly as the game builds them. PLAY / FLG1 / VLE1 are stand-in chunks that only take their data
// (their real parsers use bounded streams). Each block is placed at the end of a page followed by an inaccessible page and the
// parser is given the block's length: any read past the block faults.
//   savecheck_realparse <GameData.bin> [iterations]
// The new objects: the real save loads and re-serializes byte for byte (valid data unchanged), then a fuzz of mutated blocks
// must not fault and must never push more than 16 spin drivers into a scenario. The original objects: the same mutations make
// it fault (counted in a child process; the first fault is reported).
#include "Game/System/BinaryDataChunkHolder.hpp"
#include "Game/System/BinaryDataContentAccessor.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/System/GameDataGalaxyStorage.hpp"
#include "Game/System/ScenarioDataParser.hpp"
#include "Game/System/SpinDriverPathStorage.hpp"
#include "Game/System/StarPieceAlmsStorage.hpp"
#include "Game/Util/JMapInfo.hpp"
#include <JSystem/JSupport/JSUInputStream.hpp>
#include <JSystem/JSupport/JSUOutputStream.hpp>

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// ---- ScenarioData stand-in ------------------------------------------------------------------------------------------------
namespace {
struct Row {
    const char* name;
    unsigned short hash;
    unsigned char starMask, scenarioMask;
    int entries, powerStars, shown;
};
const Row kRows[] = {
#include SAVECHECK_SCENARIOS
};
const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));
ScenarioData* sData[64];  // in the game's ScenarioDataParser order (GalaxyNameSortTable): taken from the save's SPN1 chunk
const Row* rowOf(const ScenarioData* d) {
    for (int i = 0; i < kRowCount; i++)
        if (!strcmp(kRows[i].name, d->mGalaxyName)) return &kRows[i];
    abort();
}
}  // namespace

ScenarioData::ScenarioData(const char* pName) : mScenarioData(), mGalaxyName(), mZoneList() {
    mGalaxyName = strdup(pName);
    int entries = 0;
    for (int i = 0; i < kRowCount; i++)
        if (!strcmp(kRows[i].name, pName)) entries = kRows[i].entries;
    JMapData* data = (JMapData*)calloc(1, sizeof(JMapData) + 16);
    data->mNumEntries = entries;
    mScenarioData = (JMapInfo*)calloc(1, sizeof(JMapInfo));
    mScenarioData->mData = data;
}
ScenarioDataIter MR::makeBeginScenarioDataIter() { return ScenarioDataIter(nullptr, 0); }
ScenarioDataIter::ScenarioDataIter(const ScenarioDataParser* pParser, int cur) : mParser(pParser), mCur(cur) {}
bool ScenarioDataIter::isEnd() const { return mCur >= kRowCount; }
void ScenarioDataIter::goNext() { if (!isEnd()) mCur++; }
GalaxyStatusAccessor ScenarioDataIter::makeAccessor() const { return GalaxyStatusAccessor(sData[mCur]); }
GalaxyStatusAccessor::GalaxyStatusAccessor(const ScenarioData* pData) : mScenarioData(pData) {}
const char* GalaxyStatusAccessor::getName() const { return mScenarioData->mGalaxyName; }
s32 GalaxyStatusAccessor::getScenarioNum() const { return rowOf(mScenarioData)->shown; }
s32 GalaxyStatusAccessor::getPowerStarNum() const { return rowOf(mScenarioData)->powerStars; }

// (the game's own heap allocators are not linked: plain malloc, defined here so the large-model absolute calls resolve)
void* operator new(size_t n) { return malloc(n ? n : 1); }
void* operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// (MR::zeroMemory's cache-line path; the port's version lives in an object this program does not link)
extern "C" void DCZeroRange(void* p, u32 n) { memset(p, 0, n); }

// (weak in the JSystem headers; defined by objects this program does not link)
JSUInputStream::~JSUInputStream() {}
JSUOutputStream::~JSUOutputStream() {}

// ---- stand-in chunks (bounded-stream parsers in the game) ---------------------------------------------------------------------
namespace {
struct TakeChunk : BinaryDataChunkBase {
    u32 mSig, mHash;
    TakeChunk(u32 sig, u32 hash) : mSig(sig), mHash(hash) {}
    virtual u32 makeHeaderHashCode() const { return mHash; }
    virtual u32 getSignature() const { return mSig; }
    virtual s32 serialize(u8*, u32) const { return 0; }
    virtual s32 deserialize(const u8*, u32) { return 0; }
    virtual void initializeData() {}
};

u32 hashOf(const char* s) { u32 h = 0; for (; *s; s++) h = (u32)(u8)*s + h * 31u; return h; }
u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

struct Parsers {
    BinaryDataChunkHolder* holder;
    GameDataAllGalaxyStorage* gala;
    SpinDriverPathStorage* spin;
    StarPieceAlmsStorage* alms;
};
Parsers makeParsers() {
    Parsers p;
    p.holder = new BinaryDataChunkHolder(4096, 6);   // GameDataHolder's holder, same chunk order
    p.gala = new GameDataAllGalaxyStorage();
    p.spin = new SpinDriverPathStorage();
    p.alms = new StarPieceAlmsStorage();
    p.holder->addChunk(new TakeChunk('PLAY', 0x27C90F));
    p.holder->addChunk(new TakeChunk('FLG1', hashOf("2bytes/flag")));
    p.holder->addChunk(p.alms);
    p.holder->addChunk(p.spin);
    p.holder->addChunk(new TakeChunk('VLE1', 'VLE1'));
    p.holder->addChunk(p.gala);
    return p;
}

// A block at the end of a page, an inaccessible page after it.
u8* sArena = nullptr;
const size_t kPage = 4096, kArena = 2 * kPage;
u8* placeAtEnd(const u8* data, u32 len) {
    if (!sArena) {
        sArena = (u8*)mmap(nullptr, kArena + kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(sArena + kArena, kPage, PROT_NONE);
    }
    u8* p = sArena + kArena - len;
    memcpy(p, data, len);
    return p;
}

int maxSpinDrivers(const Parsers& p) {
    int m = 0;
    for (int g = 0; g < p.spin->mGalaxyStorage.size(); g++) {
        const SpinDriverPathStorageGalaxy* gs = p.spin->mGalaxyStorage[g];
        for (int s = 0; s < gs->mNumEntries; s++)
            if (gs->mScenarioStorage[s].mOneStorage.size() > m) m = gs->mScenarioStorage[s].mOneStorage.size();
    }
    return m;
}

unsigned sRng = 12345;
unsigned rnd() { sRng = sRng * 1103515245u + 12345u; return sRng >> 8; }

// One mutation of a valid game block (chunk-aware: sizes, GALA header, SPN1 tokens, truncation, random bytes).
u32 mutate(u8* b, u32 len) {
    u32 off[6], size[6];
    u32 p = 4;
    for (int c = 0; c < 6; c++) {
        off[c] = p;
        size[c] = rd32(b + p + 8);
        p += size[c];
    }
    const int c = rnd() % 6;
    const u32 d = off[c] + 12, n = size[c] - 12;
    static const u32 kVals[] = {0, 1, 11, 12, 0xFF, 0xFFFF, 0x8000, 0xF80, 0x7FFFFFFF, 0xFFFFFFFF};
    switch (rnd() % 8) {
    case 0: { const u32 v = kVals[rnd() % 10]; memcpy(b + off[c] + 8, &v, 4); break; }
    case 1: { const u16 v = (u16)(rnd() & 1 ? kVals[rnd() % 10] : rnd()); memcpy(b + off[5] + 12 + 2 * (rnd() % 11), &v, 2); break; }
    case 2: { const u32 o = off[3] + 12 + rnd() % (size[3] - 12); const u32 run = 1 + rnd() % 40;
              for (u32 i = 0; i < run && o + i < off[3] + size[3]; i++) b[o + i] = (u8)(rnd() & 1 ? 0x05 : 0x85); break; }
    case 3: { const u32 o = off[3] + 12 + rnd() % (size[3] - 12); b[o] = (u8)rnd(); break; }
    case 4: len = rnd() % (len + 1); if (len < 4) len = 4; break;
    case 5: b[1] = (u8)(rnd() % 9); break;
    case 6: if (n > 1) { const u16 v = (u16)rnd(); memcpy(b + d + rnd() % (n - 1), &v, 2); } break;
    default: for (int i = 0; i < 4 && n; i++) b[d + rnd() % n] = (u8)rnd(); break;
    }
    return len;
}

sigjmp_buf sJump;
volatile sig_atomic_t sFaults = 0;
void onFault(int) { sFaults = sFaults + 1; siglongjmp(sJump, 1); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s GameData.bin [iterations]\n", argv[0]); return 2; }
    const long iterations = argc > 2 ? atol(argv[2]) : 200000;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    static u8 img[0x10000];
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    const size_t imgLen = fread(img, 1, sizeof(img), f);
    fclose(f);
    if (imgLen < 0x140) return 2;
    {   // the galaxies in the game's order: the order SpinDriverPathStorage serialized them in (mario1's SPN1 chunk)
        const u8* b = img + 0x140;
        u32 p = 4;
        while (rd32(b + p) != 'SPN1') p += rd32(b + p + 8);
        const u8* d = b + p + 12;
        u32 q = 1;
        for (int g = 0; g < d[0] && g < kRowCount; g++) {
            const u16 h = rd16(d + q);
            for (int i = 0; i < kRowCount; i++)
                if (kRows[i].hash == h) sData[g] = new ScenarioData(kRows[i].name);
            if (!sData[g]) { fprintf(stderr, "galaxy %04x not in the scenario data\n", h); return 2; }
            q += rd16(d + q + 2);
        }
        if (d[0] != kRowCount) { fprintf(stderr, "galaxy count %d != %d\n", d[0], kRowCount); return 2; }
    }
    // mario1..luigi6 offsets from the file table
    u32 blocks[12];
    int nb = 0;
    for (u32 i = 0; i < rd32(img + 8) && nb < 12; i++) {
        const char* name = (const char*)img + 0x10 + i * 0x10;
        if (!strncmp(name, "mario", 5) || !strncmp(name, "luigi", 5)) blocks[nb++] = rd32(img + 0x10 + i * 0x10 + 12);
    }
    int fails = 0;
    // Valid data: every game block loads; the real GALA / SPN1 / PCE1 serializers write back what was loaded. The "state" line
    // (a checksum of the re-serialized chunks and the star count) must be the same for the hardened and the original parsers
    // (savecheck_realparse.sh compares them). (SPN1 differs from the file where the file has a zone with no spin driver left:
    // the serializer drops it; the original does the same.)
    for (int k = 0; k < nb; k++) {
        Parsers p = makeParsers();
        const u8* blk = img + blocks[k];
        const bool ok = p.holder->loadFromFileBinary(placeAtEnd(blk, 0xF80), 0xF80);
        u32 off = 4, same = 0, sum = 0;
        for (int c = 0; c < blk[1]; c++) {
            const u32 sig = rd32(blk + off), size = rd32(blk + off + 8);
            static u8 out[4096];
            memset(out, 0, sizeof(out));
            s32 n = -1;
            if (sig == 'GALA') n = p.gala->serialize(out, sizeof(out));
            if (sig == 'SPN1') n = p.spin->serialize(out, sizeof(out));
            if (sig == 'PCE1') n = p.alms->serialize(out, sizeof(out));
            if (n >= 0) {
                same += (u32)n == size - 12 && memcmp(out, blk + off + 12, size - 12) == 0;
                for (s32 i = 0; i < n; i++) sum = sum * 31u + out[i];
            }
            off += size;
        }
        int stars = 0;
        for (int g = 0; g < p.gala->getGalaxyNum(); g++) stars += p.gala->getGalaxyStorage(g)->getPowerStarNumOwned();
        if (!ok) fails++;
        printf("state block %d: load=%d stars=%d reserialized=%08x (same bytes as the file: %u of GALA/SPN1/PCE1)\n", k, ok, stars, sum, same);
    }
    // Mutated blocks against the guard page.
#ifndef SAVECHECK_NEW
    // (the original parsers corrupt the heap: the fuzz runs in a child; its death is a result too)
    const pid_t child = fork();
    if (child != 0) {
        int st = 0;
        waitpid(child, &st, 0);
        if (WIFSIGNALED(st)) printf("original parsers: the fuzz process died (signal %d) of a corrupted heap\n", WTERMSIG(st));
        else if (WEXITSTATUS(st) != 0) printf("original parsers: the fuzz process died (exit %d) of a corrupted heap\n", WEXITSTATUS(st));
        return 0;
    }
#endif
    Parsers p = makeParsers();
    long loaded = 0, refused = 0, done = 0;
    int worst = 0;
#ifndef SAVECHECK_NEW
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onFault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    long firstFault = -1;
#endif
    static u8 work[0xF80];
    for (long it = 0; it < iterations; it++) {
        done = it + 1;
        const u8* src = img + blocks[rnd() % nb];
        memcpy(work, src, 0xF80);
        u32 len = 0xF80;
        const int muts = 1 + rnd() % 3;
        for (int m = 0; m < muts; m++) {
            // keep mutating while the chunk list is still walkable by the mutator itself
            u32 q = 4;
            bool walkable = work[1] == 6 && len >= 4;
            for (int c = 0; c < 6 && walkable; c++) {
                if (len - q < 12) { walkable = false; break; }
                const u32 s = rd32(work + q + 8);
                if (s < 13 || s > len - q) walkable = false;
                else q += s;
            }
            if (walkable) len = mutate(work, len);
        }
#ifndef SAVECHECK_NEW
        if (sigsetjmp(sJump, 1)) {
            if (firstFault < 0) firstFault = it;
            p = makeParsers();  // (the faulting parse left its objects half-written)
            continue;
        }
#endif
        const bool ok = p.holder->loadFromFileBinary(placeAtEnd(work, len), len);
        (ok ? loaded : refused)++;
        const int m = maxSpinDrivers(p);
        if (m > worst) worst = m;
#ifndef SAVECHECK_NEW
        if ((it & 4095) == 0 || m > 16)
            printf("original parsers: after %ld blocks: %d faults (first at %ld), most spin drivers in a scenario %d%s\n", it + 1,
                   (int)sFaults, firstFault, worst, worst > 16 ? " (written past the 16-entry vector)" : "");
        if (worst > 16 && sFaults > 0) break;  // both kinds shown; the heap is not to be trusted any more
#endif
    }
#ifdef SAVECHECK_NEW
    printf("hardened parsers: %ld mutated blocks, %ld loaded, %ld refused, no fault; most spin drivers in a scenario: %d\n", done,
           loaded, refused, worst);
    if (worst > 16) fails++;
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
#else
    printf("original parsers: %ld mutated blocks, %d faults (first at %ld), %ld loaded, %ld refused; most spin drivers in a scenario: %d "
           "(> 16 = written past the vector)\n", done, (int)sFaults, firstFault, loaded, refused, worst);
    return 0;
#endif
}
