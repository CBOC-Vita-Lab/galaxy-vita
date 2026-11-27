// Galaxy 3.0 multiplayer: host tests for the user-file block validator (platform/src/mp/mp_savecheck.cpp, owner: snapval) and its
// use in mp_savefile.cpp (snapshots, recovery files, import). Build and run with tests/mp/savecheck_build.sh (g++, ASan + UBSan).
//
//   savecheck_test [GameData.bin] [--fuzz N] [--seed S] [--asan-demo-only]
//
// The save (default /data/saves0/saves/.../GameData.bin) is copied to a temp dir first and only that copy is read.
// Sections: the real save passes (every slot, every block, both levels) and its sets / masks agree with the cooked ScenarioData;
// crafted refusals with their exact reason; every byte of every used chunk mutated; every truncation; oversized counts;
// 100k generated valid user files pass; a structure-aware fuzz of N (default 1M) mutated snapshots through
// mp_save_snapshot_parse; every accepted input is replayed through a model of the ORIGINAL game parsers
// (savecheck_oldmodel.h) under ASan, which must stay silent; crafted refused inputs replayed through the same model in a child
// process make ASan report the out-of-bounds access the old code would have done.
#include "../../platform/src/mp/mp_savecheck.h"
#include "../../platform/src/mp/mp_savefile.h"
#include "savecheck_oldmodel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "port/mp_proto.h"

static int sFails = 0, sChecks = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        sChecks++;                                                       \
        if (!(c)) {                                                      \
            sFails++;                                                    \
            if (sFails < 40) fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

using Bytes = std::vector<uint8_t>;

struct ScenarioRow {
    const char* name;
    uint16_t hash;
    uint8_t starMask, scenarioMask;
    int entries, powerStars, shown;
};
#ifdef SAVECHECK_SCENARIOS
static const ScenarioRow kScenarios[] = {
#include SAVECHECK_SCENARIOS
};
static const int kScenarioCount = (int)(sizeof(kScenarios) / sizeof(kScenarios[0]));
#else
static const ScenarioRow* kScenarios = nullptr;
static const int kScenarioCount = 0;
#endif

// ---- small helpers ----------------------------------------------------------------------------------------------------------
static uint64_t sRng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 7;
    sRng ^= sRng << 17;
    return (uint32_t)(sRng >> 16);
}
static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static Bytes readAll(const std::string& p) {
    Bytes b(MP_SAVE_MAX_FILE + 16);
    uint32_t n = 0;
    if (mp_save_read_file(p.c_str(), b.data(), (uint32_t)b.size(), &n) != 0) return {};
    b.resize(n);
    return b;
}
static void writeAll(const std::string& p, const Bytes& b) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) { perror(p.c_str()); exit(2); }
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}
static Bytes blockOf(const Bytes& img, const char* name) {
    uint32_t off, size;
    if (!mp_save_find(img.data(), name, &off, &size)) return {};
    return Bytes(img.begin() + off, img.begin() + off + size);
}

struct Chunk { uint32_t sig, off, size; };  // off = header offset in the block; size = whole chunk
static std::vector<Chunk> chunksOf(const Bytes& b) {
    std::vector<Chunk> v;
    uint32_t p = 4;
    for (int c = 0; c < b[1]; c++) {
        v.push_back({rd32(&b[p]), p, rd32(&b[p + 8])});
        p += rd32(&b[p + 8]);
    }
    return v;
}
static int chunkIndex(const std::vector<Chunk>& v, const char (&s)[5]) {
    const uint32_t sig = ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].sig == sig) return (int)i;
    return -1;
}

// Validates a whole user file at FULL with the installed limits (the path every hostile input takes).
static int checkFile(const Bytes& m, const Bytes& l, const Bytes& c, MpSaveCheckResult* r = nullptr) {
    const uint8_t* b[3] = {m.data(), l.data(), c.data()};
    const uint16_t n[3] = {(uint16_t)m.size(), (uint16_t)l.size(), (uint16_t)c.size()};
    return mp_savecheck_user_file(b, n, MP_SAVECHECK_FULL, mp_savecheck_installed(), r);
}
static int checkGame(const Bytes& b, int level = MP_SAVECHECK_FULL) {
    return mp_savecheck_game_block(b.data(), (uint32_t)b.size(), level, level == MP_SAVECHECK_FULL ? mp_savecheck_installed() : nullptr,
                                   nullptr);
}
static int checkConfig(const Bytes& b, int level = MP_SAVECHECK_FULL) {
    return mp_savecheck_config_block(b.data(), (uint32_t)b.size(), level, nullptr);
}

static oldmodel::Counters sModel{};
static bool modelLoads(const Bytes& b, bool config) { return oldmodel::loadBlock(b.data(), (uint32_t)b.size(), config, &sModel); }

// ---- the real save -----------------------------------------------------------------------------------------------------------
static Bytes gImg;
static MpSaveLimits gLim;

static void testRealSave() {
    // Limits from the game's own serialisation (here: mario1 as written by the game).
    const Bytes ref = blockOf(gImg, "mario1");
    CHECK(mp_savecheck_limits_from_block(ref.data(), (uint32_t)ref.size(), &gLim));
    printf("limits: galaxies=%u gala=%u flags=%u values=%u\n", gLim.galaxyCount, gLim.galaCount, gLim.flagCount, gLim.valueCount);
    CHECK(gLim.galaxyCount == 48 && gLim.galaCount == 42 && gLim.flagCount == 57 && gLim.valueCount == 25);
    // The cooked ScenarioData agrees with the save: the same galaxies, power-star galaxies = the GALA set, shown scenarios = the
    // SPN1 counts. Then the masks go in.
    if (kScenarioCount > 0) {
        CHECK(kScenarioCount == gLim.galaxyCount);
        int matched = 0;
        for (int i = 0; i < kScenarioCount; i++) {
            const MpSaveGalaxyLimit* g = mp_savecheck_limits_find(&gLim, kScenarios[i].hash);
            CHECK(g != nullptr);
            if (!g) continue;
            CHECK((g->inGala != 0) == (kScenarios[i].powerStars > 0));
            CHECK(g->spinScenarios == kScenarios[i].shown);
            CHECK((kScenarios[i].starMask & ~kScenarios[i].scenarioMask) == 0);
            CHECK(mp_savecheck_limits_set_galaxy(&gLim, kScenarios[i].hash, kScenarios[i].starMask, kScenarios[i].scenarioMask));
            matched++;
        }
        printf("scenario data: %d galaxies matched (star masks from PowerStarId)\n", matched);
    } else {
        printf("scenario data: not generated (masks not checked)\n");
    }
    mp_savecheck_install(&gLim);
    // Every block of every slot passes both levels and parses cleanly through the old parsers.
    for (int s = 1; s <= 6; s++) {
        char n[3][24];
        snprintf(n[0], 24, "mario%d", s);
        snprintf(n[1], 24, "luigi%d", s);
        snprintf(n[2], 24, "config%d", s);
        const Bytes m = blockOf(gImg, n[0]), l = blockOf(gImg, n[1]), c = blockOf(gImg, n[2]);
        MpSaveCheckResult r;
        const int rc = checkFile(m, l, c, &r);
        if (rc != MP_SC_OK) printf("slot %d: %s block=%d at=%u\n", s, mp_savecheck_name(rc), r.block, (unsigned)r.at);
        CHECK(rc == MP_SC_OK);
        CHECK(checkGame(m, MP_SAVECHECK_STRUCT) == MP_SC_OK && checkGame(l, MP_SAVECHECK_STRUCT) == MP_SC_OK);
        CHECK(checkConfig(c, MP_SAVECHECK_STRUCT) == MP_SC_OK);
        CHECK(modelLoads(m, false) && modelLoads(l, false) && modelLoads(c, true));
    }
    printf("stars in the test save: slot 2 mario=%d\n", mp_save_block_stars(blockOf(gImg, "mario2").data(), 0xF80));
    // The snapshot of slot 2 (50 stars) goes through the whole path: pack, parse (validates), apply.
    uint8_t snap[MP_SNAPSHOT_MAX];
    const uint32_t len = mp_save_snapshot_pack_image(gImg.data(), 2, MP_SAVE_SNAP_FLAG_MARIO, snap, sizeof snap);
    MpSaveSnapshotView v;
    MpSaveCheckResult r;
    CHECK(len > 0 && mp_save_snapshot_parse_checked(snap, len, &v, &r) && r.err == MP_SC_OK);
    Bytes img = gImg;
    CHECK(mp_save_snapshot_apply(&v, img.data(), 5) && blockOf(img, "mario5") == blockOf(gImg, "mario2"));
}

// The first galaxy's first scenario of a block's SPN1 chunk replaced by `tokens` + 0xFF, every size fixed up (a well-formed chunk).
static Bytes spliceFirstScenario(const Bytes& blk, const Bytes& tokens) {
    const auto ch = chunksOf(blk);
    const Chunk s = ch[chunkIndex(ch, "SPN1")];
    const uint32_t g = s.off + 12 + 1, sc = g + 6, oldLen = rd16(&blk[sc]);
    const uint32_t used = mp_savecheck_used(blk.data(), (uint32_t)blk.size());
    Bytes scen(2);
    scen.insert(scen.end(), tokens.begin(), tokens.end());
    scen.push_back(0xFF);
    wr16(&scen[0], (uint16_t)scen.size());
    Bytes out(blk.begin(), blk.begin() + sc);
    out.insert(out.end(), scen.begin(), scen.end());
    out.insert(out.end(), blk.begin() + sc + oldLen, blk.begin() + used);
    const int32_t delta = (int32_t)scen.size() - (int32_t)oldLen;
    wr16(&out[g + 2], (uint16_t)(rd16(&out[g + 2]) + delta));
    wr32(&out[s.off + 8], (uint32_t)(rd32(&out[s.off + 8]) + delta));
    return out;
}
static Bytes drivers(int zones, int ones) {
    Bytes t;
    for (int z = 0; z < zones; z++) t.push_back((uint8_t)(0xC0 | (z % 62)));  // (zone 63 would read as the 0xFF end marker)
    for (int i = 0; i < ones; i++) t.push_back((uint8_t)(i & 0x3F));
    return t;
}

// ---- crafted refusals (expected reason) --------------------------------------------------------------------------------------
static Bytes gM2, gL2, gC2;  // slot 2 (progress) blocks

template <class F>
static int mutGame(F f, int level = MP_SAVECHECK_FULL, uint32_t* at = nullptr) {
    Bytes b = gM2;
    f(b);
    return mp_savecheck_game_block(b.data(), (uint32_t)b.size(), level, level == MP_SAVECHECK_FULL ? &gLim : nullptr, at);
}

static void testCrafted() {
    const auto ch = chunksOf(gM2);
    const Chunk play = ch[chunkIndex(ch, "PLAY")], flg = ch[chunkIndex(ch, "FLG1")], pce = ch[chunkIndex(ch, "PCE1")];
    const Chunk spn = ch[chunkIndex(ch, "SPN1")], vle = ch[chunkIndex(ch, "VLE1")], gala = ch[chunkIndex(ch, "GALA")];
    const uint32_t g = gala.off + 12;  // GALA data
    // Block header and chunk list.
    CHECK(mutGame([](Bytes& b) { b[0] = 2; }) == MP_SC_VERSION);
    CHECK(mutGame([](Bytes& b) { b[3] = 1; }) == MP_SC_VERSION);
    CHECK(mutGame([](Bytes& b) { b[1] = 7; }) == MP_SC_CHUNK_COUNT);
    CHECK(mutGame([](Bytes& b) { b[1] = 5; }) == MP_SC_CHUNK_COUNT);
    CHECK(mutGame([](Bytes& b) { b[1] = 0; }) == MP_SC_CHUNK_COUNT);
    CHECK(mutGame([&](Bytes& b) { wr32(&b[gala.off + 8], 0xFFFFFFF0u); }) == MP_SC_CHUNK_SIZE);   // calcBinarySize wraps
    CHECK(mutGame([&](Bytes& b) { wr32(&b[gala.off + 8], 11); }) == MP_SC_CHUNK_SIZE);           // size - 12 underflows
    CHECK(mutGame([&](Bytes& b) { wr32(&b[gala.off + 8], 0xF80 - gala.off + 1); }) == MP_SC_CHUNK_SIZE);
    CHECK(mutGame([&](Bytes& b) { b[spn.off] = 'X'; }) == MP_SC_CHUNK_SIG);
    CHECK(mutGame([&](Bytes& b) { memcpy(&b[spn.off], &b[vle.off], 8); }) == MP_SC_CHUNK_DUP);
    CHECK(mutGame([&](Bytes& b) { memcpy(&b[spn.off], &b[vle.off], 4); }) == MP_SC_CHUNK_HASH);
    CHECK(mutGame([&](Bytes& b) { b[gala.off + 4] ^= 1; }) == MP_SC_CHUNK_HASH);
    CHECK(mutGame([](Bytes& b) { b.resize(3); }) == MP_SC_BLOCK_LEN);
    CHECK(mutGame([](Bytes& b) { b.resize(0xF81, 0); }) == MP_SC_BLOCK_LEN);
    CHECK(mutGame([&](Bytes& b) { b.resize(gala.off + 11); }) == MP_SC_CHUNK_HEADER);
    CHECK(mutGame([&](Bytes& b) { b.resize(gala.off + gala.size - 1); }) == MP_SC_CHUNK_SIZE);
    // PLAY / PCE1 / FLG1 / VLE1.
    CHECK(mutGame([&](Bytes& b) { wr32(&b[play.off + 12 + 1], 10000); }) == MP_SC_PLAY_VALUE);
    CHECK(mutGame([&](Bytes& b) { wr32(&b[play.off + 12 + 1], 10000); }, MP_SAVECHECK_STRUCT) == MP_SC_OK);  // values: FULL only
    CHECK(mutGame([&](Bytes& b) { wr16(&b[play.off + 12 + 5], 100); }) == MP_SC_PLAY_VALUE);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[pce.off + 12 + 6], 10000); }) == MP_SC_ALMS_VALUE);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[flg.off + 12 + 4], rd16(&b[flg.off + 12])); }) == MP_SC_FLAG_DUP);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[flg.off + 12 + 4], (uint16_t)(rd16(&b[flg.off + 12 + 4]) ^ 0x1234)); }) == MP_SC_FLAG_UNKNOWN);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[vle.off + 12 + 8], rd16(&b[vle.off + 12])); }) == MP_SC_VALUE_DUP);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[vle.off + 12 + 8], (uint16_t)(rd16(&b[vle.off + 12 + 8]) ^ 0x4321)); }) == MP_SC_VALUE_UNKNOWN);
    // GALA.
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g], 0xFFFF); }) == MP_SC_GALA_SIZE);                 // galaxy count: records past the block
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g], 41); }) == MP_SC_GALA_SIZE);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g + 2], 0x3FFF); }) == MP_SC_GALA_LAYOUT);           // attribute count
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g + 4], 0xFFFF); }) == MP_SC_GALA_SIZE);             // record size
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g + 4], 19); }) == MP_SC_GALA_LAYOUT);               // coins past the record
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g + 6], 0x1111); }) == MP_SC_GALA_LAYOUT);           // mGalaxyName missing (null deref)
    CHECK(mutGame([&](Bytes& b) { wr16(&b[g + 8 + 12], 0xFFF0); }) == MP_SC_GALA_LAYOUT);      // mMaxCoinNum offset far out
    const uint32_t rec0 = g + 2 + 4 + 16;  // first record (layout: name 0, star 2, first 3, coins 4)
    CHECK(mutGame([&](Bytes& b) { wr16(&b[rec0], 0x0001); }) == MP_SC_GALA_GALAXY);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[rec0 + 20], rd16(&b[rec0])); }) == MP_SC_GALA_DUP);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[rec0 + 4], 1000); }) == MP_SC_GALA_COINS);
    // star bits beyond the galaxy's stars: find a galaxy with fewer than 8 stars
    int recWithFew = -1;
    uint8_t few = 0;
    for (int r = 0; r < 42; r++) {
        const MpSaveGalaxyLimit* gl = mp_savecheck_limits_find(&gLim, rd16(&gM2[rec0 + 20 * r]));
        if (gl && gl->masksKnown && gl->starMask != 0xFF) { recWithFew = r; few = gl->starMask; break; }
    }
    if (kScenarioCount > 0) {
        CHECK(recWithFew >= 0);
        const uint32_t rr = rec0 + 20 * (uint32_t)recWithFew;
        CHECK(mutGame([&](Bytes& b) { b[rr + 2] = (uint8_t)~few; }) == MP_SC_GALA_STARS);
        CHECK(mutGame([&](Bytes& b) { b[rr + 2] = few; }) == MP_SC_OK);                               // every star of it: fine
        CHECK(mutGame([&](Bytes& b) { b[rr + 3] = 0x80; }) == MP_SC_GALA_VISITED || few & 0x80);
        CHECK(mutGame([&](Bytes& b) { wr16(&b[rr + 4 + 14], 5); }) == MP_SC_GALA_COINS || (few & 0x80));  // scenario 8 coins
        CHECK(mutGame([&](Bytes& b) { b[rr + 2] = (uint8_t)~few; }, MP_SAVECHECK_STRUCT) == MP_SC_OK);
    }
    // SPN1: the walk.
    const uint32_t s = spn.off + 12;
    CHECK(mutGame([&](Bytes& b) { b[s] = 0xFF; }) == MP_SC_SPIN_COUNT);
    CHECK(mutGame([&](Bytes& b) { b[s] = 0xFF; }, MP_SAVECHECK_STRUCT) != MP_SC_OK);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[s + 1 + 2], 0xFFFF); }, MP_SAVECHECK_STRUCT) == MP_SC_SPIN_BLOCK);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[s + 1 + 2], 5); }, MP_SAVECHECK_STRUCT) == MP_SC_SPIN_BLOCK);
    CHECK(mutGame([&](Bytes& b) { b[s + 1 + 4] = 200; }) == MP_SC_SPIN_SCENARIOS);
    CHECK(mutGame([&](Bytes& b) { b[s + 1 + 4] = 200; }, MP_SAVECHECK_STRUCT) == MP_SC_SPIN_SIZE);
    CHECK(mutGame([&](Bytes& b) { wr16(&b[s + 1], 0x0001); }) == MP_SC_SPIN_GALAXY);
    // Well-formed scenarios at the game's limits: 16 spin drivers, 100 tokens.
    CHECK(checkGame(spliceFirstScenario(gM2, drivers(1, 16))) == MP_SC_OK);
    CHECK(modelLoads(spliceFirstScenario(gM2, drivers(1, 16)), false));
    CHECK(checkGame(spliceFirstScenario(gM2, drivers(1, 17)), MP_SAVECHECK_STRUCT) == MP_SC_SPIN_ONES);
    CHECK(checkGame(spliceFirstScenario(gM2, drivers(83, 16)), MP_SAVECHECK_STRUCT) == MP_SC_OK);          // 83 + 16 + end = 100 tokens
    CHECK(checkGame(spliceFirstScenario(gM2, drivers(84, 16)), MP_SAVECHECK_STRUCT) == MP_SC_SPIN_WALK);   // 101
    CHECK(checkGame(spliceFirstScenario(gM2, Bytes(99, 0xC1)), MP_SAVECHECK_STRUCT) == MP_SC_OK);         // 99 + end = 100: the last allowed
    CHECK(checkGame(spliceFirstScenario(gM2, Bytes(100, 0xC1)), MP_SAVECHECK_STRUCT) == MP_SC_SPIN_WALK); // the end would be the 101st
    CHECK(checkGame(spliceFirstScenario(gM2, Bytes{0x05}), MP_SAVECHECK_STRUCT) == MP_SC_SPIN_WALK);      // a driver before any zone
    // an interrupted driver without its range byte: the game's walk takes the 0xFF as the range and runs on into the next
    // scenario (the serializer can write this for a range below 1/256); the sizes then disagree: refused
    CHECK(checkGame(spliceFirstScenario(gM2, Bytes{0xC0, 0x85}), MP_SAVECHECK_STRUCT) != MP_SC_OK);
    CHECK(checkGame(spliceFirstScenario(gM2, Bytes{0xC0, 0x85, 0x10}), MP_SAVECHECK_STRUCT) == MP_SC_OK);
    // The first galaxy's first scenario: replace its terminator by drivers (no end) -> walk refused.
    CHECK(mutGame([&](Bytes& b) { b[s + 1 + 6 + 2] = 0x05; }, MP_SAVECHECK_STRUCT) == MP_SC_SPIN_WALK);
    // The PLAY version of an "interrupted" driver at the very end of the chunk: the draw byte would be read past it.
    {
        Bytes b = gM2;
        uint32_t at = 0;
        // last galaxy's last scenario ends with 0xFF at the chunk's last byte: make it a zone start + interrupted driver
        b[spn.off + spn.size - 1] = 0x81;
        const int rc = mp_savecheck_game_block(b.data(), (uint32_t)b.size(), MP_SAVECHECK_STRUCT, nullptr, &at);
        CHECK(rc == MP_SC_SPIN_SIZE || rc == MP_SC_SPIN_WALK);
    }
    // Config.
    {
        const auto cc = chunksOf(gC2);
        const Chunk mii = cc[chunkIndex(cc, "MII ")], misc = cc[chunkIndex(cc, "MISC")], conf = cc[chunkIndex(cc, "CONF")];
        Bytes c = gC2;
        c[mii.off + 12 + 9] = 6;
        CHECK(checkConfig(c) == MP_SC_MII_VALUE && checkConfig(c, MP_SAVECHECK_STRUCT) == MP_SC_OK);
        c = gC2; c[mii.off + 12] = 0x80; CHECK(checkConfig(c) == MP_SC_MII_VALUE);
        c = gC2; c[misc.off + 12 + 8] = 0x80; CHECK(checkConfig(c) == MP_SC_MISC_VALUE);  // negative OSTime
        c = gC2; c[misc.off + 12] = 0x08; CHECK(checkConfig(c) == MP_SC_MISC_VALUE);
        c = gC2; c[conf.off + 12] = 0x01; CHECK(checkConfig(c) == MP_SC_CONF_VALUE);
        c = gC2; wr32(&c[mii.off + 8], 21); CHECK(checkConfig(c, MP_SAVECHECK_STRUCT) != MP_SC_OK);
        c = gC2; c.resize(0x61, 0); CHECK(checkConfig(c, MP_SAVECHECK_STRUCT) == MP_SC_BLOCK_LEN);
        c = gC2; c[1] = 6; CHECK(checkConfig(c, MP_SAVECHECK_STRUCT) == MP_SC_CHUNK_COUNT);
    }
    // Story progress against a known story table.
    {
        MpSaveLimits lim = gLim;
        lim.storyKnown = 1;
        lim.maxStoryProgress = gM2[play.off + 12];
        CHECK(mp_savecheck_game_block(gM2.data(), (uint32_t)gM2.size(), MP_SAVECHECK_FULL, &lim, nullptr) == MP_SC_OK);
        lim.maxStoryProgress = (uint16_t)(gM2[play.off + 12] - 1);
        CHECK(mp_savecheck_game_block(gM2.data(), (uint32_t)gM2.size(), MP_SAVECHECK_FULL, &lim, nullptr) == MP_SC_PLAY_VALUE);
    }
    // Bad limits never crash: counts past the arrays are refused as args.
    {
        MpSaveLimits lim = gLim;
        lim.flagCount = 60000;
        CHECK(mp_savecheck_game_block(gM2.data(), (uint32_t)gM2.size(), MP_SAVECHECK_FULL, &lim, nullptr) == MP_SC_ARGS);
        CHECK(mp_savecheck_game_block(nullptr, 10, MP_SAVECHECK_FULL, nullptr, nullptr) == MP_SC_ARGS);
    }
    // Every reason has a name.
    for (int e = 0; e < MP_SC_COUNT_; e++) CHECK(strcmp(mp_savecheck_name(e), "?") != 0);
}

// ---- every byte of every used chunk, every truncation ----------------------------------------------------------------------
static void exhaustive(const Bytes& base, bool config, uint64_t* accepted, uint64_t* total) {
    const uint32_t used = mp_savecheck_used(base.data(), (uint32_t)base.size());
    static const int kDelta[] = {1, -1, 0x80, 0x7F};
    for (uint32_t i = 0; i < used; i++) {
        uint8_t vals[8];
        int nv = 0;
        vals[nv++] = 0;
        vals[nv++] = 0xFF;
        for (int d : kDelta) vals[nv++] = (uint8_t)(base[i] + d);
        vals[nv++] = (uint8_t)(base[i] ^ 0x40);
        vals[nv++] = (uint8_t)rnd();
        for (int k = 0; k < nv; k++) {
            if (vals[k] == base[i]) continue;
            Bytes b = base;
            b[i] = vals[k];
            const int full = config ? checkConfig(b) : checkGame(b);
            const int st = config ? checkConfig(b, MP_SAVECHECK_STRUCT) : checkGame(b, MP_SAVECHECK_STRUCT);
            CHECK(full == MP_SC_OK ? st == MP_SC_OK : true);  // FULL is stricter than STRUCT
            ++*total;
            if (st == MP_SC_OK) {
                ++*accepted;
                CHECK(modelLoads(b, config));  // ASan: no out-of-bounds in the old parsers
            }
        }
    }
    // Truncations: every length below the used size is refused; everything from the used size up passes (the tail is unused).
    for (uint32_t n = 0; n <= base.size(); n++) {
        Bytes b(base.begin(), base.begin() + n);
        const int st = config ? checkConfig(b, MP_SAVECHECK_STRUCT) : checkGame(b, MP_SAVECHECK_STRUCT);
        CHECK((st == MP_SC_OK) == (n >= used));
        ++*total;
    }
}

static void testExhaustive() {
    uint64_t acc = 0, tot = 0;
    exhaustive(gM2, false, &acc, &tot);
    exhaustive(blockOf(gImg, "luigi1"), false, &acc, &tot);
    exhaustive(gC2, true, &acc, &tot);
    printf("every byte mutated + every truncation: %llu cases, %llu accepted (STRUCT), all replayed through the old parsers\n",
           (unsigned long long)tot, (unsigned long long)acc);
}

// ---- generated valid user files ---------------------------------------------------------------------------------------------
// A small serializer with the game's layout: random progress within the limits. Every one must pass FULL and parse cleanly.
static Bytes chunk(const char (&sig)[5], uint32_t hash, const Bytes& data) {
    Bytes c(12);
    c[0] = (uint8_t)sig[3]; c[1] = (uint8_t)sig[2]; c[2] = (uint8_t)sig[1]; c[3] = (uint8_t)sig[0];
    wr32(&c[4], hash);
    wr32(&c[8], (uint32_t)(12 + data.size()));
    c.insert(c.end(), data.begin(), data.end());
    return c;
}
static Bytes genGame(const Bytes& ref, int spinBudget) {
    const auto rc = chunksOf(ref);
    auto data = [&](int k) { return Bytes(ref.begin() + rc[k].off + 12, ref.begin() + rc[k].off + rc[k].size); };
    auto hash = [&](int k) { return rd32(&ref[rc[k].off + 4]); };
    // PLAY
    Bytes play(7);
    play[0] = (uint8_t)rndn(gLim.storyKnown ? gLim.maxStoryProgress + 1u : 256u);
    wr32(&play[1], rndn(10000));
    wr16(&play[5], (uint16_t)rndn(100));
    // FLG1 (the reference's flags, shuffled, random on/off)
    Bytes flg = data(chunkIndex(rc, "FLG1"));
    const uint32_t nf = (uint32_t)flg.size() / 2;
    for (uint32_t i = nf; i > 1; i--) {
        const uint32_t j = rndn(i);
        uint16_t a = rd16(&flg[2 * (i - 1)]), b = rd16(&flg[2 * j]);
        wr16(&flg[2 * (i - 1)], b);
        wr16(&flg[2 * j], a);
    }
    for (uint32_t i = 0; i < nf; i++) wr16(&flg[2 * i], (uint16_t)((rd16(&flg[2 * i]) & 0x7FFF) | (rnd() & 1 ? 0x8000 : 0)));
    // PCE1
    Bytes pce(32);
    for (int i = 0; i < 16; i++) wr16(&pce[2 * i], (uint16_t)rndn(10000));
    // SPN1: every galaxy with its scenario count, random drivers (<= 16 per scenario), within the budget
    Bytes spn;
    {
        const Bytes ro = data(chunkIndex(rc, "SPN1"));
        spn.push_back(ro[0]);
        uint32_t p = 1;
        for (int g = 0; g < ro[0]; g++) {
            const uint16_t gh = rd16(&ro[p]);
            const int ns = ro[p + 4];
            Bytes gb(6);
            wr16(&gb[0], gh);
            gb[4] = (uint8_t)ns;
            for (int sc = 0; sc < ns; sc++) {
                Bytes sb(2);
                if (spinBudget > 0 && rndn(4) == 0) {
                    int zones = 1 + (int)rndn(3), ones = (int)rndn(17);
                    for (int z = 0; z < zones && ones >= 0; z++) {
                        sb.push_back((uint8_t)(0xC0 | rndn(63)));  // (zone 63 would be 0xFF, the end marker)
                        const int here = z == zones - 1 ? ones : (int)rndn((uint32_t)ones + 1);
                        for (int o = 0; o < here; o++) {
                            const bool inter = rnd() & 1;
                            sb.push_back((uint8_t)((inter ? 0x80 : (rnd() & 1 ? 0x40 : 0)) | rndn(64)));
                            if (inter) sb.push_back((uint8_t)rnd());
                        }
                        ones -= here;
                    }
                }
                sb.push_back(0xFF);
                wr16(&sb[0], (uint16_t)sb.size());  // the serializer's size + 3: 2 size bytes + tokens + 0xFF
                spinBudget -= (int)sb.size();
                gb.insert(gb.end(), sb.begin(), sb.end());
            }
            wr16(&gb[2], (uint16_t)gb.size());
            spn.insert(spn.end(), gb.begin(), gb.end());
            p += rd16(&ro[p + 2]);
        }
    }
    // VLE1: the reference's values, random contents
    Bytes vle = data(chunkIndex(rc, "VLE1"));
    for (size_t i = 0; i + 3 < vle.size(); i += 4) wr16(&vle[i + 2], (uint16_t)rnd());
    // GALA: the reference's header and galaxies, random progress within the masks
    Bytes gala = data(chunkIndex(rc, "GALA"));
    {
        const uint32_t cnt = rd16(&gala[0]), rs = rd16(&gala[4]), recs = 2 + 4 + 4 * rd16(&gala[2]);
        for (uint32_t r = 0; r < cnt; r++) {
            uint8_t* rec = &gala[recs + r * rs];
            const MpSaveGalaxyLimit* gl = mp_savecheck_limits_find(&gLim, rd16(rec));
            const uint8_t sm = gl && gl->masksKnown ? gl->starMask : 0xFF, cm = gl && gl->masksKnown ? gl->scenarioMask : 0xFF;
            rec[2] = (uint8_t)(rnd() & sm);
            rec[3] = (uint8_t)(rnd() & cm);
            for (int i = 0; i < 8; i++) wr16(rec + 4 + 2 * i, (cm >> i) & 1 ? (uint16_t)rndn(1000) : 0);
        }
    }
    Bytes out = {1, 6, 0, 0};
    for (const Bytes& c : {chunk("PLAY", hash(0), play), chunk("FLG1", hash(1), flg), chunk("PCE1", hash(2), pce),
                           chunk("SPN1", hash(3), spn), chunk("VLE1", hash(4), vle), chunk("GALA", hash(5), gala)})
        out.insert(out.end(), c.begin(), c.end());
    return out;
}
static Bytes genConfig() {
    Bytes conf = {(uint8_t)(rnd() & 1 ? 0xFF : 0)};
    Bytes mii(10);
    mii[0] = (uint8_t)(rnd() & 3);
    for (int i = 1; i < 9; i++) mii[i] = (uint8_t)rnd();
    mii[9] = (uint8_t)rndn(6);
    Bytes misc(9);
    misc[0] = (uint8_t)(rnd() & 7);
    const uint64_t t = ((uint64_t)rnd() << 32 | rnd()) % (36525ull * 86400ull * 60750000ull);
    for (int i = 0; i < 8; i++) misc[1 + i] = (uint8_t)(t >> (8 * i));
    Bytes out = {1, 3, 0, 0};
    for (const Bytes& c : {chunk("CONF", 0x2432DA, conf), chunk("MII ", 0x2836E9, mii), chunk("MISC", 1, misc)})
        out.insert(out.end(), c.begin(), c.end());
    return out;
}

static void testGenerated(int n) {
    const Bytes ref = blockOf(gImg, "mario1");
    int pass = 0;
    for (int i = 0; i < n; i++) {
        Bytes m = genGame(ref, 1200), l = genGame(ref, 300), c = genConfig();
        if (m.size() > 0xF80 || l.size() > 0xF80) { i--; continue; }
        if (rnd() & 1) m.resize(0xF80, (uint8_t)rnd());   // a full block with stale bytes after the chunk list
        MpSaveCheckResult r;
        const int rc = checkFile(m, l, c, &r);
        if (rc != MP_SC_OK && sFails < 5) fprintf(stderr, "generated file refused: %s block=%d at=%u\n", mp_savecheck_name(rc), r.block, (unsigned)r.at);
        CHECK(rc == MP_SC_OK);
        pass += rc == MP_SC_OK;
        CHECK(modelLoads(m, false) && modelLoads(l, false) && modelLoads(c, true));
        if (i % 16 == 0) {  // and through the whole snapshot path
            uint8_t snap[MP_SNAPSHOT_MAX];
            const uint32_t len = mp_save_snapshot_pack(m.data(), (uint32_t)m.size(), l.data(), (uint32_t)l.size(), c.data(), (uint32_t)c.size(),
                                                       1 + (int)rndn(6), (uint8_t)(rnd() & 1), snap, sizeof snap);
            MpSaveSnapshotView v;
            CHECK(len > 0 && mp_save_snapshot_parse(snap, len, &v));
        }
    }
    printf("generated valid user files: %d / %d pass FULL and parse cleanly\n", pass, n);
}

// ---- structure-aware fuzz through mp_save_snapshot_parse ----------------------------------------------------------------------
static const uint32_t kInteresting32[] = {0, 1, 11, 12, 13, 0x7F, 0x80, 0xFF, 0x100, 0xF74, 0xF80, 0xF81, 0x7FFF, 0x8000, 0xFFFF,
                                          0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFF4u, 0xFFFFFFFFu};
static uint32_t interesting(uint32_t cur) {
    switch (rndn(4)) {
    case 0: return kInteresting32[rndn(sizeof kInteresting32 / sizeof kInteresting32[0])];
    case 1: return cur + 1 + rndn(16);
    case 2: return cur - 1 - rndn(16);
    default: return rnd();
    }
}

static void mutateBlock(Bytes& b, bool config) {
    const auto ch = chunksOf(b);
    const Chunk& c = ch[rndn((uint32_t)ch.size())];
    const uint32_t d = c.off + 12, n = c.size - 12;
    const int kind = (int)rndn(config ? 6 : 14);
    switch (kind) {
    case 0: b[rndn(4)] = (uint8_t)interesting(b[0]); break;                                    // block header
    case 1: wr32(&b[c.off + 8], interesting(c.size)); break;                                   // chunk size
    case 2: wr32(&b[c.off + 4 * rndn(2)], rnd()); break;                                       // signature / hash
    case 3: if (n) b[d + rndn(n)] = (uint8_t)interesting(b[d]); break;                         // a data byte
    case 4: if (n > 1) { const uint32_t o = d + rndn(n - 1); wr16(&b[o], (uint16_t)interesting(rd16(&b[o]))); } break;
    case 5: b.resize(rndn((uint32_t)b.size() + 1)); break;                                     // truncation
    case 6: {   // GALA header fields
        const auto g = ch[chunkIndex(ch, "GALA")];
        const uint32_t o = g.off + 12 + 2 * rndn(11);
        wr16(&b[o], (uint16_t)interesting(rd16(&b[o])));
        break;
    }
    case 7: {   // GALA record fields: stars, visited, coins
        const auto g = ch[chunkIndex(ch, "GALA")];
        const uint32_t recs = g.off + 12 + 22, r = rndn(42);
        const uint32_t f = rndn(3);
        if (f == 2) wr16(&b[recs + 20 * r + 4 + 2 * rndn(8)], (uint16_t)(rnd() & 1 ? 999 + rndn(3) : rnd()));
        else b[recs + 20 * r + 2 + f] = (uint8_t)rnd();
        break;
    }
    case 8: case 9: {   // SPN1: counts, block sizes, tokens, long driver runs
        const auto s = ch[chunkIndex(ch, "SPN1")];
        const uint32_t sd = s.off + 12, sn = s.size - 12;
        const uint32_t o = sd + rndn(sn);
        switch (rndn(5)) {
        case 0: b[sd] = (uint8_t)interesting(b[sd]); break;
        case 1: b[o] = (uint8_t)(rnd() & 1 ? 0xFF : 0xC0 | rndn(64)); break;
        case 2: if (o + 1 < sd + sn) wr16(&b[o], (uint16_t)interesting(rd16(&b[o]))); break;
        case 3: {   // a run of spin drivers over the next bytes (more than 16)
            const uint32_t run = 1 + rndn(40);
            for (uint32_t i = 0; i < run && o + i < sd + sn; i++) b[o + i] = (uint8_t)(rnd() & 1 ? 0x05 : 0x85);
            break;
        }
        default: b[o] = (uint8_t)rnd(); break;
        }
        break;
    }
    case 10: {  // FLG1 / VLE1 entries
        const auto f = ch[chunkIndex(ch, rnd() & 1 ? "FLG1" : "VLE1")];
        const uint32_t cnt = (f.size - 12) / 2;
        if (cnt) wr16(&b[f.off + 12 + 2 * rndn(cnt)], (uint16_t)(rnd() & 1 ? rd16(&b[f.off + 12]) : rnd()));
        break;
    }
    case 11: {  // swap two chunk headers' sizes / duplicate a header
        const Chunk& c2 = ch[rndn((uint32_t)ch.size())];
        memcpy(&b[c.off], &b[c2.off], rnd() & 1 ? 4 : 12);
        break;
    }
    case 12: b[1] = (uint8_t)interesting(b[1]); break;                                         // chunk count
    default: for (int i = 0, k = 2 + (int)rndn(7); i < k && n; i++) b[d + rndn(n)] = (uint8_t)rnd(); break;
    }
}

static void testFuzz(uint64_t iterations) {
    const Bytes bases[3][3] = {
        {gM2, gL2, gC2},
        {blockOf(gImg, "mario1"), blockOf(gImg, "luigi1"), blockOf(gImg, "config1")},
        {genGame(blockOf(gImg, "mario1"), 1500), genGame(blockOf(gImg, "mario1"), 300), genConfig()},
    };
    uint64_t accepted = 0, apply = 0;
    uint64_t reasons[MP_SC_COUNT_] = {};
    uint8_t snap[MP_SNAPSHOT_MAX];
    Bytes img = gImg;
    const time_t t0 = time(nullptr);
    for (uint64_t it = 0; it < iterations; it++) {
        const Bytes* base = bases[rndn(3)];
        Bytes blk[3] = {base[0], base[1], base[2]};
        const int muts = 1 + (rndn(4) == 0 ? (int)rndn(4) : 0);
        for (int m = 0; m < muts; m++) {
            const int k = rndn(5) == 0 ? 2 : (int)rndn(2);
            if (blk[k].size() >= 4 && blk[k][1] == (k == 2 ? 3 : 6) && mp_savecheck_used(blk[k].data(), (uint32_t)blk[k].size()) <= blk[k].size() &&
                (k == 2 ? checkConfig(blk[k], MP_SAVECHECK_STRUCT) : checkGame(blk[k], MP_SAVECHECK_STRUCT)) == MP_SC_OK)
                mutateBlock(blk[k], k == 2);
            else if (!blk[k].empty())
                blk[k][rndn((uint32_t)blk[k].size())] = (uint8_t)rnd();
        }
        for (Bytes& b : blk)
            if (b.empty()) b.push_back((uint8_t)rnd());   // (the snapshot format refuses empty blocks anyway)
        const uint32_t caps[3] = {0xF80, 0xF80, 0x60};
        bool fits = true;
        for (int k = 0; k < 3; k++) fits &= blk[k].size() <= caps[k];
        if (!fits) { for (int k = 0; k < 3; k++) if (blk[k].size() > caps[k]) blk[k].resize(caps[k]); }
        const uint32_t len = mp_save_snapshot_pack(blk[0].data(), (uint32_t)blk[0].size(), blk[1].data(), (uint32_t)blk[1].size(),
                                                   blk[2].data(), (uint32_t)blk[2].size(), 1 + (int)rndn(6), (uint8_t)(rnd() & 1), snap, sizeof snap);
        CHECK(len > 0);
        MpSaveSnapshotView v;
        MpSaveCheckResult r;
        const bool ok = mp_save_snapshot_parse_checked(snap, len, &v, &r);
        CHECK(ok == (r.err == MP_SC_OK));
        if (r.err >= 0 && r.err < MP_SC_COUNT_) reasons[r.err]++;
        if (ok) {
            accepted++;
            // Accepted: the original parsers stay inside every block (ASan) and load it without an error.
            for (int k = 0; k < 3; k++) {
                const Bytes b(v.block[k], v.block[k] + v.len[k]);
                CHECK(modelLoads(b, k == 2));
            }
            if ((it & 63) == 0) { apply++; CHECK(mp_save_snapshot_apply(&v, img.data(), 1 + (int)rndn(6))); }
        }
        if ((it + 1) % 250000 == 0) printf("  fuzz %llu/%llu (%lds)\n", (unsigned long long)(it + 1), (unsigned long long)iterations, (long)(time(nullptr) - t0));
    }
    printf("fuzz: %llu mutated snapshots, %llu accepted (each replayed through the old parsers), %llu applied, %lds\n",
           (unsigned long long)iterations, (unsigned long long)accepted, (unsigned long long)apply, (long)(time(nullptr) - t0));
    printf("  refusals by reason:");
    for (int e = 1; e < MP_SC_COUNT_; e++)
        if (reasons[e]) printf(" [%s]=%llu", mp_savecheck_name(e), (unsigned long long)reasons[e]);
    printf("\n");
    printf("  old-parser model: %u chunks, %u GALA records, %u spin drivers, %u flags, %u values read\n", sModel.chunks, sModel.galaRecords,
           sModel.spinOnes, sModel.flags, sModel.values);
}

// ---- recovery files and import use the same check -----------------------------------------------------------------------------
static void testRecoveryAndImport(const std::string& dir) {
    MpSaveSnapshotView v;
    CHECK(mp_save_view_slot(gImg.data(), 2, &v));
    MpSaveRecoveryInfo info;
    memset(&info, 0, sizeof info);
    info.sessionId = 7;
    info.slot = 2;
    snprintf(info.galaxy, sizeof info.galaxy, "%s", "AstroGalaxy");
    uint8_t rec[MP_SAVE_REC_MAX];
    const uint32_t len = mp_save_recovery_pack(&info, &v, rec, sizeof rec);
    CHECK(len > 0 && mp_save_recovery_parse(rec, len, nullptr, nullptr));
    // A recovery file whose GALA galaxy count runs past the block (CRC fixed up) is refused.
    Bytes m = gM2;
    const auto ch = chunksOf(m);
    wr16(&m[ch[chunkIndex(ch, "GALA")].off + 12], 0xFFFF);
    MpSaveSnapshotView bad = v;
    bad.block[0] = m.data();
    const uint32_t len2 = mp_save_recovery_pack(&info, &bad, rec, sizeof rec);
    CHECK(len2 > 0 && !mp_save_recovery_parse(rec, len2, nullptr, nullptr));
    // Import: a single-player file whose slot 3 has a broken SPN1 walk (checksum fixed) is refused as invalid source.
    Bytes sp = gImg;
    uint32_t off, size;
    CHECK(mp_save_find(sp.data(), "mario3", &off, &size));
    const auto c3 = chunksOf(Bytes(sp.begin() + off, sp.begin() + off + size));
    sp[off + c3[chunkIndex(c3, "SPN1")].off + 12 + 1 + 6 + 2] = 0x05;  // a driver instead of the end marker, forever
    CHECK(mp_save_finalize(sp.data(), (uint32_t)sp.size()));
    sp.resize(mp_save_disk_length(sp.data()));
    CHECK(mp_save_validate(sp.data(), (uint32_t)sp.size(), true, true) == MP_SAVE_OK);  // a valid GameData.bin: only the slot is bad
    const std::string spPath = dir + "/sp.bin", mpPath = dir + "/mp.bin";
    writeAll(spPath, sp);
    unlink(mpPath.c_str());
    Bytes base = gImg;
    base.resize(MP_SAVE_MAX_FILE, 0);
    CHECK(mp_save_import_file(spPath.c_str(), 3, mpPath.c_str(), 1, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_SRC_INVALID);
    CHECK(access(mpPath.c_str(), F_OK) != 0);  // nothing written
    const int irc = mp_save_import_file(spPath.c_str(), 2, mpPath.c_str(), 1, base.data(), MP_SAVE_MAX_FILE, nullptr);
    if (irc != MP_SAVE_OP_OK) printf("import slot 2: %s\n", mp_save_op_name(irc));
    CHECK(irc == MP_SAVE_OP_OK);
    // put_blocks (every block written to disk) checks STRUCT: a broken block is never written.
    MpSaveSnapshotView w = v;
    w.block[0] = m.data();
    CHECK(mp_save_put_blocks(&w, mpPath.c_str(), 4, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_SRC_INVALID);
    // Apply refuses it too (the in-memory image is never written with it).
    Bytes img = gImg;
    CHECK(!mp_save_snapshot_apply(&w, img.data(), 4) && img == gImg);
    printf("recovery / import / put / apply: broken blocks refused, nothing written\n");
}

// ---- the old code reads out of bounds on refused inputs (ASan, in a child process) ------------------------------------------
static int runOldInChild(const Bytes& b, bool config, const std::string& log) {
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        const int fd = open(log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd >= 0) { dup2(fd, 2); close(fd); }
        oldmodel::Counters c{};
        oldmodel::loadBlock(b.data(), (uint32_t)b.size(), config, &c);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

static void testAsanDemo(const std::string& dir) {
    struct Demo { const char* name; Bytes block; bool config; };
    std::vector<Demo> demos;
    const auto ch = chunksOf(gM2);
    const Chunk gala = ch[chunkIndex(ch, "GALA")], pce = ch[chunkIndex(ch, "PCE1")];
    {   Bytes b = gM2; wr16(&b[gala.off + 12], 0x0800); demos.push_back({"gala-galaxy-count", b, false}); }
    {   Bytes b = gM2; wr16(&b[gala.off + 12 + 8 + 12], 0xFFF0); demos.push_back({"gala-coin-offset", b, false}); }
    {   Bytes b = gM2; wr16(&b[gala.off + 12 + 2], 0x4000); demos.push_back({"gala-attribute-count", b, false}); }
    demos.push_back({"spin-17-drivers", spliceFirstScenario(gM2, drivers(1, 17)), false});   // a well-formed chunk; the vector holds 16
    {   // the chunk list ends 4 bytes before the end of the 0xF80 block, and the count says one more chunk: calcBinarySize reads
        // that chunk's size past the block (the game: past its 0xF80 temp buffer)
        Bytes b = gM2;
        wr32(&b[gala.off + 8], 0xF7C - gala.off);
        b[1] = 7;
        demos.push_back({"chunk-count-past-end", b, false});
    }
    {   // PCE1 as the last chunk of a short block: its 32-byte copy runs past the block
        Bytes b = {1, 1, 0, 0};
        Bytes c(12 + 4, 0);
        memcpy(c.data(), &gM2[pce.off], 8);
        wr32(&c[8], 16);
        b.insert(b.end(), c.begin(), c.end());
        demos.push_back({"alms-short-chunk", b, false});
    }
    {   // config: the first chunk's size skips past the 0x60 block, so the next chunk headers are read beyond it (the game: the
        // temp buffer's stale bytes, i.e. the previous file's game data, parsed as config chunks)
        Bytes c = gC2;
        wr32(&c[4 + 8], 0x60 + 20);
        demos.push_back({"config-chunk-past-block", c, true});
    }
    int caught = 0;
    for (const Demo& d : demos) {
        const int st = d.config ? checkConfig(d.block, MP_SAVECHECK_STRUCT) : checkGame(d.block, MP_SAVECHECK_STRUCT);
        CHECK(st != MP_SC_OK);
        const std::string log = dir + "/asan-" + d.name + ".txt";
        const int status = runOldInChild(d.block, d.config, log);
        const Bytes text = readAll(log);
        const std::string s(text.begin(), text.end());
        const bool asan = s.find("AddressSanitizer") != std::string::npos;
        std::string what;
        const size_t p = s.find("ERROR: AddressSanitizer: ");
        if (p != std::string::npos) what = s.substr(p + 25, s.find(' ', p + 25) - (p + 25));
        const size_t q = s.find("READ of size"), w = s.find("WRITE of size");
        const size_t rw = q != std::string::npos ? q : w;
        if (rw != std::string::npos) what += " " + s.substr(rw, s.find(' ', rw + 14) - rw);
        printf("  old parser on %-22s validator: %-24s child: %s %s\n", d.name, mp_savecheck_name(st),
               asan ? "ASan" : (WIFSIGNALED(status) ? "signal" : "exit"), what.c_str());
        caught += asan || (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
    }
    CHECK(caught == (int)demos.size());
    printf("ASan demo: %d / %zu refused inputs make the old parsers fault (reports in %s/asan-*.txt)\n", caught, demos.size(), dir.c_str());
}

int main(int argc, char** argv) {
    std::string src = "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    uint64_t fuzz = 1000000;
    bool demoOnly = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) fuzz = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) sRng = strtoull(argv[++i], nullptr, 10) | 1;
        else if (!strcmp(argv[i], "--asan-demo-only")) demoOnly = true;
        else src = argv[i];
    }
    char tmpl[] = "/tmp/savecheck-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 2; }
    // Copy the save (read only) into the temp dir; only the copy is used.
    const Bytes orig = readAll(src);
    if (orig.empty()) { fprintf(stderr, "cannot read %s\n", src.c_str()); return 2; }
    const std::string copy = std::string(dir) + "/GameData.bin";
    writeAll(copy, orig);
    gImg = readAll(copy);
    CHECK(mp_save_validate(gImg.data(), (uint32_t)gImg.size(), true, true) == MP_SAVE_OK);
    gImg.resize(MP_SAVE_MAX_FILE, 0);
    gM2 = blockOf(gImg, "mario2");
    gL2 = blockOf(gImg, "luigi2");
    gC2 = blockOf(gImg, "config2");

    testRealSave();
    if (!demoOnly) {
        testCrafted();
        testExhaustive();
        testGenerated(100000);
        testRecoveryAndImport(dir);
        testFuzz(fuzz);
    }
    testAsanDemo(dir);
    // The source save was only read.
    CHECK(readAll(src) == orig);
    printf("%s: %d checks, %d failures\n", sFails ? "FAILED" : "OK", sChecks, sFails);
    return sFails ? 1 : 0;
}
