// Galaxy 3.0 multiplayer: strict validator for user-file blocks (owner: snapval). See mp_savecheck.h.
//
// Every walk below mirrors one deserializer in decomp/src/Game/System (named in each function). The game's reads are noted as
// "game:" where they matter: the validator refuses whatever would make that read leave the block.
#include "mp_savecheck.h"

#include <string.h>

#include <atomic>

namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

// MR::getHashCode / JGadget::getHashCode (hash = c + hash * 31) for the ASCII names below.
constexpr uint32_t hashOf(const char* s) {
    uint32_t h = 0;
    for (; *s; s++) h = (uint32_t)(uint8_t)*s + h * 31u;
    return h;
}
constexpr uint32_t sig4(const char (&s)[5]) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}

enum GameChunk { C_PLAY, C_FLG1, C_PCE1, C_SPN1, C_VLE1, C_GALA, C_GAME_COUNT };
enum ConfigChunk { C_CONF, C_MII, C_MISC, C_CONFIG_COUNT };
struct ChunkSpec {
    uint32_t sig, hash;  // getSignature(), makeHeaderHashCode()
};
// GameDataHolder's chunk holder (GameDataHolder.cpp: PLAY, FLG1, PCE1, SPN1, VLE1, GALA) and ConfigDataHolder's (CONF, MII, MISC).
const ChunkSpec kGameChunks[C_GAME_COUNT] = {
    {sig4("PLAY"), 0x27C90Fu},                             // GameDataPlayerStatus
    {sig4("FLG1"), hashOf("2bytes/flag")},                 // GameEventFlagStorage
    {sig4("PCE1"), hashOf("StarPieceAlmsStorage") << 5},   // StarPieceAlmsStorage
    {sig4("SPN1"), 0x12345679u},                           // SpinDriverPathStorage
    {sig4("VLE1"), sig4("VLE1")},                          // GameEventValueChecker
    {sig4("GALA"), 0xBF0640EEu},                           // GameDataAllGalaxyStorage
};
const ChunkSpec kConfigChunks[C_CONFIG_COUNT] = {
    {sig4("CONF"), 0x2432DAu},  // ConfigDataCreateChunk
    {sig4("MII "), 0x2836E9u},  // ConfigDataMii
    {sig4("MISC"), 0x1u},       // ConfigDataMisc
};

// GameDataAllGalaxyStorage::initHeaderSerializer: the record's attributes (hash = (u16) MR::getHashCode(name)) and their sizes.
struct GalaAttr {
    uint16_t hash;
    uint16_t size;
};
enum { A_NAME, A_STAR, A_FIRST, A_COIN, A_COUNT };
const GalaAttr kGalaAttrs[A_COUNT] = {
    {(uint16_t)hashOf("mGalaxyName"), 2},
    {(uint16_t)hashOf("mPowerStarFlag"), 1},
    {(uint16_t)hashOf("mFirstPlayFlag"), 1},
    {(uint16_t)hashOf("mMaxCoinNum"), 16},
};

constexpr uint32_t kMaxStarPiece = 9999;   // GameDataPlayerStatus::addStockedStarPiece clamp
constexpr uint32_t kMaxLives = 99;         // GameDataPlayerStatus::addPlayerLeft clamp
constexpr uint32_t kMaxCoins = 999;        // GameDataSomeScenarioAccessor::updateMaxCoinNum clamp
constexpr uint32_t kMaxAlms = 9999;        // star pieces given to a hungry Luma (never more than the star piece cap)
constexpr uint8_t kMiiFlags = 0x03;        // ConfigDataMii FLAG_UNK1 | FLAG_UNK2
constexpr uint8_t kMiiMaxIcon = 5;         // ICON_ID_MII .. ICON_ID_PEACH
constexpr uint8_t kMiscFlags = 0x07;       // ConfigDataMisc last loaded Mario | ending Mario | ending Luigi
// ConfigDataMisc::mLastModified: OSTime ticks since 2000 (60.75 MHz). 0 = never; refuse past 2100 (or negative).
constexpr uint64_t kMaxOsTime = 36525ull * 86400ull * 60750000ull;  // 100 years (36525 days) of 60.75 MHz ticks

int fail(uint32_t* at, uint32_t off, int err) {
    if (at) *at = off;
    return err;
}

int findSorted16(const uint16_t* a, int n, uint16_t v) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (a[mid] == v) return mid;
        if (a[mid] < v) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

bool limitsOk(const MpSaveLimits* lim) {
    return !lim || (lim->galaxyCount <= MP_SAVECHECK_MAX_GALAXIES && lim->galaCount <= lim->galaxyCount &&
                    lim->flagCount <= MP_SAVECHECK_MAX_FLAGS && lim->valueCount <= MP_SAVECHECK_MAX_VALUES);
}

int galaxyIndex(const MpSaveLimits* lim, uint16_t hash) {
    if (!lim) return -1;
    int lo = 0, hi = (int)lim->galaxyCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (lim->galaxy[mid].hash == hash) return mid;
        if (lim->galaxy[mid].hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// ---- chunk list: BinaryDataChunkHolder::loadFromFileBinary ---------------------------------------------------------------
// game: calcBinarySize follows every chunk size from offset 4 (reads each header wherever the sizes lead), then the loop hands
// chunk data and (chunk size - 12) to the chunk with that signature. Here: exactly the serializer's chunk set (the version byte 1,
// the count, every signature once, its header hash), every header and chunk inside the block.
struct ChunkRef {
    uint32_t off, size;  // data offset in the block, data size
};
int walkChunks(const uint8_t* b, uint32_t len, const ChunkSpec* spec, int count, ChunkRef* out, uint32_t* at) {
    if (!b || len < 4) return fail(at, 0, MP_SC_BLOCK_LEN);
    if (b[0] != 1 || b[2] != 0 || b[3] != 0) return fail(at, 0, MP_SC_VERSION);
    if (b[1] != count) return fail(at, 1, MP_SC_CHUNK_COUNT);
    bool seen[8] = {};
    uint32_t p = 4;
    for (int c = 0; c < count; c++) {
        if (len - p < 12) return fail(at, p, MP_SC_CHUNK_HEADER);
        const uint32_t sig = rd32(b + p), hash = rd32(b + p + 4), size = rd32(b + p + 8);
        if (size < 12 || size > len - p) return fail(at, p + 8, MP_SC_CHUNK_SIZE);
        int k = -1;
        for (int i = 0; i < count; i++)
            if (spec[i].sig == sig) k = i;
        if (k < 0) return fail(at, p, MP_SC_CHUNK_SIG);
        if (seen[k]) return fail(at, p, MP_SC_CHUNK_DUP);
        seen[k] = true;
        if (hash != spec[k].hash) return fail(at, p + 4, MP_SC_CHUNK_HASH);
        out[k].off = p + 12;
        out[k].size = size - 12;
        p += size;
    }
    return MP_SC_OK;
}

// ---- PLAY: GameDataPlayerStatus::deserialize ----------------------------------------------------------------------------
// game: u8 story progress, u32 stocked star pieces, then (if bytes remain) u16 into mPlayerLeftSupply; bounded stream.
int checkPlay(const uint8_t* d, uint32_t n, uint32_t base, int level, const MpSaveLimits* lim, uint32_t* at) {
    if (n != 7) return fail(at, base, MP_SC_PLAY_SIZE);  // the serializer: 1 + 4 + 2
    if (level < MP_SAVECHECK_FULL) return MP_SC_OK;
    if (lim && lim->storyKnown && d[0] > lim->maxStoryProgress) return fail(at, base, MP_SC_PLAY_VALUE);
    if (rd32(d + 1) > kMaxStarPiece) return fail(at, base + 1, MP_SC_PLAY_VALUE);
    if (rd16(d + 5) > kMaxLives) return fail(at, base + 5, MP_SC_PLAY_VALUE);
    return MP_SC_OK;
}

// ---- FLG1: GameEventFlagStorage::deserialize ----------------------------------------------------------------------------
// game: size / 2 entries of u16 (hash & 0x7FFF | on << 15) through a bounded stream; unknown hashes are skipped.
int checkFlags(const uint8_t* d, uint32_t n, uint32_t base, int level, const MpSaveLimits* lim, uint32_t* at) {
    if (n & 1) return fail(at, base, MP_SC_FLAG_SIZE);
    if (level < MP_SAVECHECK_FULL || !lim) return MP_SC_OK;
    const uint32_t count = n / 2;
    if (count != lim->flagCount) return fail(at, base, MP_SC_FLAG_COUNT);  // the serializer writes every saved flag once
    uint32_t seen[MP_SAVECHECK_MAX_FLAGS / 32] = {};  // (a bit set: this may run on a small host-job thread stack)
    for (uint32_t i = 0; i < count; i++) {
        const int k = findSorted16(lim->flagHash, lim->flagCount, (uint16_t)(rd16(d + 2 * i) & 0x7FFF));
        if (k < 0) return fail(at, base + 2 * i, MP_SC_FLAG_UNKNOWN);
        if (seen[k >> 5] & (1u << (k & 31))) return fail(at, base + 2 * i, MP_SC_FLAG_DUP);
        seen[k >> 5] |= 1u << (k & 31);
    }
    return MP_SC_OK;
}

// ---- PCE1: StarPieceAlmsStorage::deserialize ----------------------------------------------------------------------------
// game: copies 32 bytes (16 u16) whatever the size.
int checkAlms(const uint8_t* d, uint32_t n, uint32_t base, int level, uint32_t* at) {
    if (n != 32) return fail(at, base, MP_SC_ALMS_SIZE);
    if (level < MP_SAVECHECK_FULL) return MP_SC_OK;
    for (uint32_t i = 0; i < 16; i++)
        if (rd16(d + 2 * i) > kMaxAlms) return fail(at, base + 2 * i, MP_SC_ALMS_VALUE);
    return MP_SC_OK;
}

// ---- SPN1: SpinDriverPathStorage / ...Galaxy / ...Scenario / ...One::deserialize -----------------------------------------
// game: u8 galaxy count; per galaxy (raw pointer reads): u16 name hash, u16 block size (the stream seeks by it), u8 scenario
// count, u8 0; when the hash and the count match the game's galaxy, per scenario: a u16 size (never read), then tokens from +2
// until 0xFF: 0b11zzzzzz starts zone z, else a spin driver (id & 0x3F, bit 7: one more byte, the draw range) pushed into a
// vector of 16 (no bounds check: heap overflow past 16), giving up after 100 tokens (returns 0: the next scenario starts at
// the same place) or on a driver before any zone. The walk below runs for every galaxy entry (whether or not the game knows
// the hash: the worst case), stays inside the chunk, and must consume exactly what the serializer wrote.
int checkSpin(const uint8_t* d, uint32_t n, uint32_t base, int level, const MpSaveLimits* lim, uint32_t* at) {
    if (n < 1) return fail(at, base, MP_SC_SPIN_SIZE);
    const bool sets = level >= MP_SAVECHECK_FULL && lim;
    const uint32_t count = d[0];
    if (sets && count != lim->galaxyCount) return fail(at, base, MP_SC_SPIN_COUNT);
    bool seen[MP_SAVECHECK_MAX_GALAXIES] = {};
    uint32_t p = 1;
    for (uint32_t g = 0; g < count; g++) {
        if (n - p < 6) return fail(at, base + p, MP_SC_SPIN_SIZE);
        const uint16_t hash = rd16(d + p), blockSize = rd16(d + p + 2);
        const uint32_t scenarios = d[p + 4];
        if (blockSize < 6 || blockSize > n - p || d[p + 5] != 0) return fail(at, base + p + 2, MP_SC_SPIN_BLOCK);
        if (sets) {
            const int k = galaxyIndex(lim, hash);
            if (k < 0) return fail(at, base + p, MP_SC_SPIN_GALAXY);
            if (seen[k]) return fail(at, base + p, MP_SC_SPIN_DUP);
            seen[k] = true;
            if (scenarios != lim->galaxy[k].spinScenarios) return fail(at, base + p + 4, MP_SC_SPIN_SCENARIOS);
        }
        const uint32_t end = p + blockSize;
        uint32_t q = p + 6;
        for (uint32_t s = 0; s < scenarios; s++) {
            uint32_t r = 0;
            int zone = -1, ones = 0, tokens = 0;
            for (;;) {
                if (++tokens > MP_SAVECHECK_SPIN_TOKENS) return fail(at, base + q + 2 + r, MP_SC_SPIN_WALK);
                if (q + 2 + r >= end) return fail(at, base + q + 2 + r, MP_SC_SPIN_SIZE);
                const uint8_t v = d[q + 2 + r];
                if (v == 0xFF) break;
                if ((v & 0xC0) == 0xC0) {
                    zone = v & 0x3F;
                    r++;
                    continue;
                }
                if (zone < 0) return fail(at, base + q + 2 + r, MP_SC_SPIN_WALK);
                if (++ones > MP_SAVECHECK_SPIN_ONES) return fail(at, base + q + 2 + r, MP_SC_SPIN_ONES);
                if (v & 0x80) {  // interrupted: the draw range byte follows
                    if (q + 3 + r >= end) return fail(at, base + q + 3 + r, MP_SC_SPIN_SIZE);
                    r += 2;
                } else {
                    r += 1;
                }
            }
            const uint32_t consumed = r + 3;  // the game's return value: u16 size + tokens + 0xFF
            if (rd16(d + q) != consumed) return fail(at, base + q, MP_SC_SPIN_BLOCK);
            q += consumed;
        }
        if (q != end) return fail(at, base + p + 2, MP_SC_SPIN_BLOCK);
        p = end;
    }
    if (p != n) return fail(at, base + p, MP_SC_SPIN_SIZE);
    return MP_SC_OK;
}

// ---- VLE1: GameEventValueChecker::deserialize ---------------------------------------------------------------------------
// game: entries of u16 hash + u16 value through a bounded stream (it loops size / 2 times: the second half reads nothing).
int checkValues(const uint8_t* d, uint32_t n, uint32_t base, int level, const MpSaveLimits* lim, uint32_t* at) {
    if (n & 3) return fail(at, base, MP_SC_VALUE_SIZE);
    if (level < MP_SAVECHECK_FULL || !lim) return MP_SC_OK;
    const uint32_t count = n / 4;
    if (count != lim->valueCount) return fail(at, base, MP_SC_VALUE_COUNT);
    uint8_t seen[MP_SAVECHECK_MAX_VALUES] = {};
    for (uint32_t i = 0; i < count; i++) {
        const int k = findSorted16(lim->valueHash, lim->valueCount, rd16(d + 4 * i));
        if (k < 0) return fail(at, base + 4 * i, MP_SC_VALUE_UNKNOWN);
        if (seen[k]) return fail(at, base + 4 * i, MP_SC_VALUE_DUP);
        seen[k] = 1;
    }
    return MP_SC_OK;
}

// ---- GALA: GameDataAllGalaxyStorage / GameDataSomeGalaxyStorage::deserialize -------------------------------------------------
// game: u16 record count; at +2 a BinaryDataContentAccessor header (u16 attribute count, u16 record size, then u16 hash + u16
// offset per attribute); records from +2 + 4 + 4 * attributes, record size each. Per record (raw pointers, no size check):
// *(u16*) at the mGalaxyName offset (a null pointer if the attribute is missing), and for a known galaxy u8 star flags, u8
// visited flags and 8 x u16 coins at their offsets (each optional).
struct GalaLayout {
    uint32_t count, recordSize, records;  // records: offset of the first record in the chunk data
    uint16_t off[A_COUNT];
};
int galaLayout(const uint8_t* d, uint32_t n, uint32_t base, GalaLayout* L, uint32_t* at) {
    if (n < 6) return fail(at, base, MP_SC_GALA_SIZE);
    const uint32_t count = rd16(d), attrs = rd16(d + 2), recordSize = rd16(d + 4);
    if (attrs != A_COUNT) return fail(at, base + 2, MP_SC_GALA_LAYOUT);  // the header serializer's four attributes
    const uint32_t header = 4 + 4 * attrs;
    if (2 + header > n) return fail(at, base + 2, MP_SC_GALA_SIZE);
    bool have[A_COUNT] = {};
    for (uint32_t a = 0; a < attrs; a++) {
        const uint16_t h = rd16(d + 6 + 4 * a), off = rd16(d + 8 + 4 * a);
        int k = -1;
        for (int i = 0; i < A_COUNT; i++)
            if (kGalaAttrs[i].hash == h) k = i;
        if (k < 0 || have[k]) return fail(at, base + 6 + 4 * a, MP_SC_GALA_LAYOUT);
        if ((uint32_t)off + kGalaAttrs[k].size > recordSize) return fail(at, base + 8 + 4 * a, MP_SC_GALA_LAYOUT);
        have[k] = true;
        L->off[k] = off;
    }
    if ((uint64_t)count * recordSize != (uint64_t)(n - 2 - header)) return fail(at, base, MP_SC_GALA_SIZE);
    L->count = count;
    L->recordSize = recordSize;
    L->records = 2 + header;
    return MP_SC_OK;
}

int checkGala(const uint8_t* d, uint32_t n, uint32_t base, int level, const MpSaveLimits* lim, uint32_t* at) {
    GalaLayout L;
    int rc = galaLayout(d, n, base, &L, at);
    if (rc != MP_SC_OK || level < MP_SAVECHECK_FULL) return rc;
    if (lim && L.count != lim->galaCount) return fail(at, base, MP_SC_GALA_COUNT);
    bool seen[MP_SAVECHECK_MAX_GALAXIES] = {};
    for (uint32_t g = 0; g < L.count; g++) {
        const uint32_t ro = L.records + g * L.recordSize;
        const uint8_t* rec = d + ro;
        const MpSaveGalaxyLimit* gl = nullptr;
        if (lim) {
            const int k = galaxyIndex(lim, rd16(rec + L.off[A_NAME]));
            if (k < 0 || !lim->galaxy[k].inGala) return fail(at, base + ro + L.off[A_NAME], MP_SC_GALA_GALAXY);
            if (seen[k]) return fail(at, base + ro + L.off[A_NAME], MP_SC_GALA_DUP);
            seen[k] = true;
            if (lim->galaxy[k].masksKnown) gl = &lim->galaxy[k];
        }
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t co = L.off[A_COIN] + 2 * i;
            const uint16_t coins = rd16(rec + co);
            if (coins > kMaxCoins || (gl && coins != 0 && !((gl->scenarioMask >> i) & 1))) return fail(at, base + ro + co, MP_SC_GALA_COINS);
        }
        if (gl && (rec[L.off[A_STAR]] & ~gl->starMask)) return fail(at, base + ro + L.off[A_STAR], MP_SC_GALA_STARS);
        if (gl && (rec[L.off[A_FIRST]] & ~gl->scenarioMask)) return fail(at, base + ro + L.off[A_FIRST], MP_SC_GALA_VISITED);
    }
    return MP_SC_OK;
}

// ---- config: ConfigDataCreateChunk / ConfigDataMii / ConfigDataMisc::deserialize ----------------------------------------------
// game: bounded streams (u8 created; u8 flag + 8-byte Mii id [+ u8 icon]; u8 flag [+ s64 last modified]).
int checkConf(const uint8_t* d, uint32_t n, uint32_t base, int level, uint32_t* at) {
    if (n != 1) return fail(at, base, MP_SC_CONF_SIZE);
    if (level >= MP_SAVECHECK_FULL && d[0] != 0 && d[0] != 0xFF) return fail(at, base, MP_SC_CONF_VALUE);
    return MP_SC_OK;
}
int checkMii(const uint8_t* d, uint32_t n, uint32_t base, int level, uint32_t* at) {
    if (n != 10) return fail(at, base, MP_SC_MII_SIZE);
    if (level < MP_SAVECHECK_FULL) return MP_SC_OK;
    if (d[0] & ~kMiiFlags) return fail(at, base, MP_SC_MII_VALUE);
    if (d[9] > kMiiMaxIcon) return fail(at, base + 9, MP_SC_MII_VALUE);
    return MP_SC_OK;
}
int checkMisc(const uint8_t* d, uint32_t n, uint32_t base, int level, uint32_t* at) {
    if (n != 9) return fail(at, base, MP_SC_MISC_SIZE);
    if (level < MP_SAVECHECK_FULL) return MP_SC_OK;
    if (d[0] & ~kMiscFlags) return fail(at, base, MP_SC_MISC_VALUE);
    if (rd64(d + 1) > kMaxOsTime) return fail(at, base + 1, MP_SC_MISC_VALUE);  // also every negative OSTime
    return MP_SC_OK;
}

}  // namespace

const char* mp_savecheck_name(int err) {
    static const char* const kNames[MP_SC_COUNT_] = {
        "ok", "args", "block length", "version", "chunk count", "chunk header", "chunk size", "chunk signature", "chunk twice",
        "chunk hash", "PLAY size", "PLAY value", "FLG1 size", "FLG1 unknown flag", "FLG1 flag twice", "FLG1 flag count",
        "PCE1 size", "PCE1 value", "SPN1 size", "SPN1 block size", "SPN1 unknown galaxy", "SPN1 galaxy twice",
        "SPN1 scenario count", "SPN1 galaxy count", "SPN1 walk", "SPN1 too many spin drivers", "VLE1 size", "VLE1 unknown value",
        "VLE1 value twice", "VLE1 value count", "GALA size", "GALA layout", "GALA unknown galaxy", "GALA galaxy twice",
        "GALA galaxy count", "GALA star flags", "GALA visited flags", "GALA coins", "CONF size", "CONF value", "MII size",
        "MII value", "MISC size", "MISC value",
    };
    return err >= 0 && err < MP_SC_COUNT_ ? kNames[err] : "?";
}

int mp_savecheck_game_block(const uint8_t* block, uint32_t len, int level, const MpSaveLimits* lim, uint32_t* at) {
    if (at) *at = 0;
    if (!block || !limitsOk(lim)) return MP_SC_ARGS;
    if (len > MP_SAVECHECK_GAME_BLOCK) return fail(at, 0, MP_SC_BLOCK_LEN);
    ChunkRef c[C_GAME_COUNT];
    int rc = walkChunks(block, len, kGameChunks, C_GAME_COUNT, c, at);
    if (rc != MP_SC_OK) return rc;
    // (the game deserializes in list order; nothing of a refused block is used, so the order here does not matter)
    if ((rc = checkPlay(block + c[C_PLAY].off, c[C_PLAY].size, c[C_PLAY].off, level, lim, at)) != MP_SC_OK) return rc;
    if ((rc = checkFlags(block + c[C_FLG1].off, c[C_FLG1].size, c[C_FLG1].off, level, lim, at)) != MP_SC_OK) return rc;
    if ((rc = checkAlms(block + c[C_PCE1].off, c[C_PCE1].size, c[C_PCE1].off, level, at)) != MP_SC_OK) return rc;
    if ((rc = checkSpin(block + c[C_SPN1].off, c[C_SPN1].size, c[C_SPN1].off, level, lim, at)) != MP_SC_OK) return rc;
    if ((rc = checkValues(block + c[C_VLE1].off, c[C_VLE1].size, c[C_VLE1].off, level, lim, at)) != MP_SC_OK) return rc;
    return checkGala(block + c[C_GALA].off, c[C_GALA].size, c[C_GALA].off, level, lim, at);
}

int mp_savecheck_config_block(const uint8_t* block, uint32_t len, int level, uint32_t* at) {
    if (at) *at = 0;
    if (!block) return MP_SC_ARGS;
    if (len > MP_SAVECHECK_CONFIG_BLOCK) return fail(at, 0, MP_SC_BLOCK_LEN);
    ChunkRef c[C_CONFIG_COUNT];
    int rc = walkChunks(block, len, kConfigChunks, C_CONFIG_COUNT, c, at);
    if (rc != MP_SC_OK) return rc;
    if ((rc = checkConf(block + c[C_CONF].off, c[C_CONF].size, c[C_CONF].off, level, at)) != MP_SC_OK) return rc;
    if ((rc = checkMii(block + c[C_MII].off, c[C_MII].size, c[C_MII].off, level, at)) != MP_SC_OK) return rc;
    return checkMisc(block + c[C_MISC].off, c[C_MISC].size, c[C_MISC].off, level, at);
}

int mp_savecheck_user_file(const uint8_t* const block[3], const uint16_t len[3], int level, const MpSaveLimits* lim,
                           MpSaveCheckResult* res) {
    MpSaveCheckResult r = {MP_SC_ARGS, -1, 0};
    if (block && len) {
        r.err = MP_SC_OK;
        for (int k = 0; k < 3 && r.err == MP_SC_OK; k++) {
            r.block = k;
            r.err = k < 2 ? mp_savecheck_game_block(block[k], len[k], level, lim, &r.at)
                          : mp_savecheck_config_block(block[k], len[k], level, &r.at);
        }
        if (r.err == MP_SC_OK) {
            r.block = -1;
            r.at = 0;
        }
    }
    if (res) *res = r;
    return r.err;
}

uint32_t mp_savecheck_used(const uint8_t* block, uint32_t len) {
    if (!block || len < 4) return 0;
    uint32_t p = 4;
    for (uint32_t c = 0; c < block[1]; c++) {
        if (len - p < 12) return len;
        const uint32_t size = rd32(block + p + 8);
        if (size < 12 || size > len - p) return len;
        p += size;
    }
    return p;
}

// ---- limits ----------------------------------------------------------------------------------------------------------------
namespace {
void sort16(uint16_t* a, int n) {  // insertion sort: <= 1024 entries, once
    for (int i = 1; i < n; i++) {
        const uint16_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}
bool hasDup16(const uint16_t* sorted, int n) {
    for (int i = 1; i < n; i++)
        if (sorted[i] == sorted[i - 1]) return true;
    return false;
}
}  // namespace

bool mp_savecheck_limits_from_block(const uint8_t* block, uint32_t len, MpSaveLimits* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    uint32_t at = 0;
    if (mp_savecheck_game_block(block, len, MP_SAVECHECK_STRUCT, nullptr, &at) != MP_SC_OK) return false;
    ChunkRef c[C_GAME_COUNT];
    if (walkChunks(block, len, kGameChunks, C_GAME_COUNT, c, &at) != MP_SC_OK) return false;
    // SPN1: every galaxy of ScenarioData, with its scenario count.
    const uint8_t* d = block + c[C_SPN1].off;
    const uint32_t galaxies = d[0];
    if (galaxies > MP_SAVECHECK_MAX_GALAXIES) return false;
    uint32_t p = 1;
    for (uint32_t g = 0; g < galaxies; g++) {
        MpSaveGalaxyLimit& L = out->galaxy[g];
        L.hash = rd16(d + p);
        L.spinScenarios = d[p + 4];
        p += rd16(d + p + 2);
    }
    out->galaxyCount = (uint16_t)galaxies;
    // sort by hash (insertion sort on whole records)
    for (int i = 1; i < (int)galaxies; i++) {
        const MpSaveGalaxyLimit v = out->galaxy[i];
        int j = i - 1;
        while (j >= 0 && out->galaxy[j].hash > v.hash) {
            out->galaxy[j + 1] = out->galaxy[j];
            j--;
        }
        out->galaxy[j + 1] = v;
    }
    for (int i = 1; i < (int)galaxies; i++)
        if (out->galaxy[i].hash == out->galaxy[i - 1].hash) return false;
    // GALA: the galaxies with power stars (each must be one of the above).
    GalaLayout G;
    if (galaLayout(block + c[C_GALA].off, c[C_GALA].size, 0, &G, &at) != MP_SC_OK) return false;
    for (uint32_t g = 0; g < G.count; g++) {
        const uint8_t* rec = block + c[C_GALA].off + G.records + g * G.recordSize;
        const int k = galaxyIndex(out, rd16(rec + G.off[A_NAME]));
        if (k < 0 || out->galaxy[k].inGala) return false;
        out->galaxy[k].inGala = 1;
    }
    out->galaCount = (uint16_t)G.count;
    // FLG1 / VLE1: the saved flags and values.
    const uint32_t flags = c[C_FLG1].size / 2, values = c[C_VLE1].size / 4;
    if (flags > MP_SAVECHECK_MAX_FLAGS || values > MP_SAVECHECK_MAX_VALUES) return false;
    for (uint32_t i = 0; i < flags; i++) out->flagHash[i] = (uint16_t)(rd16(block + c[C_FLG1].off + 2 * i) & 0x7FFF);
    for (uint32_t i = 0; i < values; i++) out->valueHash[i] = rd16(block + c[C_VLE1].off + 4 * i);
    out->flagCount = (uint16_t)flags;
    out->valueCount = (uint16_t)values;
    sort16(out->flagHash, (int)flags);
    sort16(out->valueHash, (int)values);
    if (hasDup16(out->flagHash, (int)flags) || hasDup16(out->valueHash, (int)values)) return false;
    return true;
}

bool mp_savecheck_limits_set_galaxy(MpSaveLimits* lim, uint16_t hash, uint8_t starMask, uint8_t scenarioMask) {
    const int k = galaxyIndex(lim, hash);
    if (k < 0) return false;
    lim->galaxy[k].starMask = starMask;
    lim->galaxy[k].scenarioMask = scenarioMask;
    lim->galaxy[k].masksKnown = 1;
    return true;
}

const MpSaveGalaxyLimit* mp_savecheck_limits_find(const MpSaveLimits* lim, uint16_t hash) {
    const int k = galaxyIndex(lim, hash);
    return k < 0 ? nullptr : &lim->galaxy[k];
}

namespace {
MpSaveLimits sInstalled[2];
std::atomic<const MpSaveLimits*> sInstalledPtr{nullptr};
std::atomic<int> sInstallNext{0};
}  // namespace

void mp_savecheck_install(const MpSaveLimits* lim) {
    if (!lim) {
        sInstalledPtr.store(nullptr);
        return;
    }
    // Two buffers: a reader that loaded the previous pointer keeps a stable copy while the next install writes the other one
    // (the game installs once per boot; tests install several times on one thread).
    const int i = sInstallNext.fetch_add(1) & 1;
    sInstalled[i] = *lim;
    sInstalledPtr.store(&sInstalled[i]);
}

const MpSaveLimits* mp_savecheck_installed(void) { return sInstalledPtr.load(); }
