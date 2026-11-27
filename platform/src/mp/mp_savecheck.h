// Galaxy 3.0 multiplayer: strict validator for user-file blocks before they reach the game's save parsers (owner: snapval).
// Pure: no game headers, no allocation, no global state except the installed limits. Used by mp_savefile.cpp (snapshots, recovery
// files, import, every block written to disk) and by decomp/src/Game/MP/MpSaveGame.cpp (limits from the live game).
//
// A user file is three blocks: the playing character's game data (mario/luigi, <= 0xF80 bytes), the other character's, and the
// config (<= 0x60). Each block is the game's binary chunk list (BinaryDataChunkHolder::makeFileBinary, little-endian in the port):
//   u8 version (1), u8 chunk count, u16 zero, then per chunk: u32 signature, u32 header hash, u32 chunk size (12 + data), data.
// Game data chunks: PLAY (GameDataPlayerStatus), FLG1 (GameEventFlagStorage), PCE1 (StarPieceAlmsStorage), SPN1
// (SpinDriverPathStorage), VLE1 (GameEventValueChecker), GALA (GameDataAllGalaxyStorage). Config: CONF, MII , MISC.
// The game's deserializers trust every size, count and offset (GALA and SPN1 read raw pointers, SPN1 pushes into a 16-entry
// vector, PCE1 copies 32 bytes whatever the size, the chunk walk follows any chunk size). The validator walks every chunk exactly
// as its deserializer reads it and refuses anything that would read outside the block, write outside a fixed array, or come out
// different from what the game's own serializer writes (sizes, counts, attribute layout, terminators).
//
// Two levels:
//   MP_SAVECHECK_STRUCT  memory safety: everything the parsers read stays inside the block, every fixed array stays in bounds,
//                        every chunk has the size its serializer writes. Game-written data always passes (used for every write).
//   MP_SAVECHECK_FULL    STRUCT + values the game can produce (coins <= 999, star pieces <= 9999, lives <= 99, icon ids, flags)
//                        + with installed limits: the exact galaxy / flag / value sets the game serializes, power star and
//                        scenario bits within the galaxy's ScenarioData, story progress within the story table. For hostile input
//                        (snapshots from the network, recovery files, imported files).
#ifndef PORT_MP_SAVECHECK_H
#define PORT_MP_SAVECHECK_H

#include <stddef.h>
#include <stdint.h>

#define MP_SAVECHECK_GAME_BLOCK 0xF80   // SaveDataHandler::getEnoughtTempBufferSize, mario/luigi block size
#define MP_SAVECHECK_CONFIG_BLOCK 0x60
#define MP_SAVECHECK_MAX_GALAXIES 64    // ScenarioDataParser holds at most 64 (FixedArray< ScenarioData*, 64 >)
#define MP_SAVECHECK_MAX_FLAGS 1024
#define MP_SAVECHECK_MAX_VALUES 128
#define MP_SAVECHECK_SPIN_ONES 16       // SpinDriverPathStorageScenario::mOneStorage.init(16)
#define MP_SAVECHECK_SPIN_TOKENS 100    // SpinDriverPathStorageScenario::deserialize gives up after 100 tokens

enum MpSaveCheckLevel { MP_SAVECHECK_STRUCT = 0, MP_SAVECHECK_FULL = 1 };

enum MpSaveCheckErr {
    MP_SC_OK = 0,
    MP_SC_ARGS,
    MP_SC_BLOCK_LEN,       // block shorter than its chunk list / longer than the block
    MP_SC_VERSION,         // byte 0 != 1 or bytes 2..3 != 0
    MP_SC_CHUNK_COUNT,     // not exactly the game's chunk count
    MP_SC_CHUNK_HEADER,    // a chunk header does not fit
    MP_SC_CHUNK_SIZE,      // chunk size < 12 or past the end of the block
    MP_SC_CHUNK_SIG,       // unknown signature
    MP_SC_CHUNK_DUP,       // a chunk twice
    MP_SC_CHUNK_HASH,      // header hash differs from the game's
    MP_SC_PLAY_SIZE, MP_SC_PLAY_VALUE,
    MP_SC_FLAG_SIZE, MP_SC_FLAG_UNKNOWN, MP_SC_FLAG_DUP, MP_SC_FLAG_COUNT,
    MP_SC_ALMS_SIZE, MP_SC_ALMS_VALUE,
    MP_SC_SPIN_SIZE,       // a read past the chunk / trailing bytes
    MP_SC_SPIN_BLOCK,      // galaxy or scenario block size differs from what the walk consumed
    MP_SC_SPIN_GALAXY, MP_SC_SPIN_DUP, MP_SC_SPIN_SCENARIOS, MP_SC_SPIN_COUNT,
    MP_SC_SPIN_WALK,       // no end marker within 100 tokens, or a spin driver before any zone
    MP_SC_SPIN_ONES,       // more than 16 spin drivers in one scenario (heap overflow in the game)
    MP_SC_VALUE_SIZE, MP_SC_VALUE_UNKNOWN, MP_SC_VALUE_DUP, MP_SC_VALUE_COUNT,
    MP_SC_GALA_SIZE,       // header or records do not fit / trailing bytes
    MP_SC_GALA_LAYOUT,     // attribute table: missing, unknown or duplicate attribute, field outside the record
    MP_SC_GALA_GALAXY, MP_SC_GALA_DUP, MP_SC_GALA_COUNT,
    MP_SC_GALA_STARS,      // power star bits outside the galaxy's stars
    MP_SC_GALA_VISITED,    // visited bits outside the galaxy's scenarios
    MP_SC_GALA_COINS,      // a coin record > 999, or for a scenario the galaxy does not have
    MP_SC_CONF_SIZE, MP_SC_CONF_VALUE, MP_SC_MII_SIZE, MP_SC_MII_VALUE, MP_SC_MISC_SIZE, MP_SC_MISC_VALUE,
    MP_SC_COUNT_
};
const char* mp_savecheck_name(int err);

// ---- limits: what this build's game serializes, and its ScenarioData -------------------------------------------------------
struct MpSaveGalaxyLimit {
    uint16_t hash;           // (u16) MR::getHashCode(galaxy name)
    uint8_t spinScenarios;   // SPN1: SpinDriverPathStorageGalaxy::mNumScenarios
    uint8_t inGala;          // has a GALA record (power stars > 0)
    uint8_t starMask;        // GALA mPowerStarFlag bits the galaxy has (union of ScenarioData PowerStarId)
    uint8_t scenarioMask;    // GALA mFirstPlayFlag bits / mMaxCoinNum slots (scenario 1..N)
    uint8_t masksKnown;      // starMask / scenarioMask are set
    uint8_t reserved;
};
struct MpSaveLimits {
    uint16_t galaxyCount, galaCount, flagCount, valueCount;  // galaxies sorted by hash; flags and values sorted
    uint16_t maxStoryProgress;
    uint8_t storyKnown;
    uint8_t reserved;
    MpSaveGalaxyLimit galaxy[MP_SAVECHECK_MAX_GALAXIES];
    uint16_t flagHash[MP_SAVECHECK_MAX_FLAGS];    // FLG1 15-bit hashes
    uint16_t valueHash[MP_SAVECHECK_MAX_VALUES];  // VLE1 hashes
};
// The sets from a game data block the running game serialized itself (UserFile::makeGameDataBinary of any user file: the sets
// do not depend on progress). Masks unknown, story unknown. False when the block is not structurally valid or too large.
bool mp_savecheck_limits_from_block(const uint8_t* block, uint32_t len, MpSaveLimits* out);
// ScenarioData of one galaxy. False when the hash is not in the limits.
bool mp_savecheck_limits_set_galaxy(MpSaveLimits* lim, uint16_t hash, uint8_t starMask, uint8_t scenarioMask);
const MpSaveGalaxyLimit* mp_savecheck_limits_find(const MpSaveLimits* lim, uint16_t hash);

// Installed limits (the game side installs them once it can build them; null = none: FULL checks values without the sets).
// install copies; safe to read from any thread after install returns (the game installs from the game thread, before use).
void mp_savecheck_install(const MpSaveLimits* lim);
const MpSaveLimits* mp_savecheck_installed(void);

// ---- checks ---------------------------------------------------------------------------------------------------------------
// *at (may be null) gets the byte offset in the block where the check failed.
int mp_savecheck_game_block(const uint8_t* block, uint32_t len, int level, const MpSaveLimits* lim, uint32_t* at);
int mp_savecheck_config_block(const uint8_t* block, uint32_t len, int level, uint32_t* at);
struct MpSaveCheckResult {
    int err;        // MpSaveCheckErr
    int block;      // 0 the mario block, 1 luigi, 2 config (-1 none)
    uint32_t at;    // offset in that block
};
// The three blocks of a user file (mario, luigi, config).
int mp_savecheck_user_file(const uint8_t* const block[3], const uint16_t len[3], int level, const MpSaveLimits* lim,
                           MpSaveCheckResult* res);

// Bytes of the chunk list (4 + chunk sizes) of a block that passed the check.
uint32_t mp_savecheck_used(const uint8_t* block, uint32_t len);

#endif  // PORT_MP_SAVECHECK_H
