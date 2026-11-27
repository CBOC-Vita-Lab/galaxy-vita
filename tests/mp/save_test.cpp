// Galaxy 3.0 multiplayer: host tests for platform/src/mp/mp_savefile.cpp (owner: save).
// Build and run (ASan/UBSan), from the worktree root:
//   g++ -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -Iplatform/include tests/mp/save_test.cpp platform/src/mp/mp_savefile.cpp -pthread -o /tmp/save_test && /tmp/save_test [GameData.bin]
// (clang++ works the same where its ASan runtime is installed; this container has only GCC's libasan.)
// The default save is /data/saves0/saves/title/00010000/524d4750/data/GameData.bin; it is only ever read (copied to a temp dir).
#include "../../platform/src/mp/mp_savefile.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "port/mp_proto.h"

static int sFails = 0, sChecks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        sChecks++;                                                            \
        if (!(c)) {                                                           \
            sFails++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                     \
    } while (0)

using Bytes = std::vector<uint8_t>;
static std::string gDir;

static Bytes readFile(const std::string& p) {
    Bytes b(MP_SAVE_MAX_FILE + 16);
    uint32_t n = 0;
    if (mp_save_read_file(p.c_str(), b.data(), (uint32_t)b.size(), &n) != 0) return {};
    b.resize(n);
    return b;
}
static void writeFile(const std::string& p, const Bytes& b) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) { perror(p.c_str()); exit(2); }
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static uint32_t rd32(const Bytes& b, uint32_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); }
static void wr32(Bytes& b, uint32_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (uint8_t)(v >> (8 * i)); }
static Bytes block(const Bytes& img, const char* name) {
    uint32_t o, s;
    if (img.size() < 0x140 || mp_save_validate(img.data(), (uint32_t)img.size(), false, false) != MP_SAVE_OK) return {};
    if (!mp_save_find(img.data(), name, &o, &s)) return {};
    return Bytes(img.begin() + o, img.begin() + o + s);
}
static Bytes fullBuf(const Bytes& img) { Bytes b(MP_SAVE_MAX_FILE, 0); memcpy(b.data(), img.data(), img.size()); return b; }
static void rmrf(const std::string& d) { std::string c = "rm -rf '" + d + "'"; if (system(c.c_str()) != 0) {} }

static void testChecksumAndValidate(const Bytes& orig) {
    CHECK(orig.size() == 0xBE00);
    CHECK(rd32(orig, 0) == 0xB1D5EF2Du);
    CHECK(mp_save_checksum(orig.data(), rd32(orig, 12)) == rd32(orig, 0));
    CHECK(mp_save_validate(orig.data(), (uint32_t)orig.size(), true, true) == MP_SAVE_OK);
    uint32_t o, s;
    CHECK(mp_save_find(orig.data(), "mario1", &o, &s) && o == 0x140 && s == 0xF80);
    CHECK(mp_save_find(orig.data(), "config6", &o, &s) && s == 0x60);
    CHECK(mp_save_find(orig.data(), "sysconf", &o, &s) && s == 0x80);
    CHECK(!mp_save_find(orig.data(), "mario7", &o, &s));
}

static void testCorrupt(const Bytes& orig) {
    const uint32_t n = (uint32_t)orig.size();
    Bytes b = orig; b[0x500] ^= 1;  // payload byte: checksum
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_CHECKSUM);
    CHECK(mp_save_validate(b.data(), n, true, false) == MP_SAVE_OK);  // in-memory image check ignores the checksum
    CHECK(mp_save_validate(orig.data(), n - 32, true, true) == MP_SAVE_ERR_SHORT);
    CHECK(mp_save_validate(orig.data(), 8, true, true) == MP_SAVE_ERR_SHORT);
    CHECK(mp_save_validate(orig.data(), 0, true, true) == MP_SAVE_ERR_SHORT);
    b = orig; b.resize(n + 32);
    CHECK(mp_save_validate(b.data(), n + 32, true, true) == MP_SAVE_ERR_LENGTH);
    b = orig; wr32(b, 12, 0x20000);  // huge size
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_SIZE);
    b = orig; wr32(b, 12, 0xFFFFFFFFu);
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_SIZE);
    b = orig; wr32(b, 4, 3);
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_VERSION);
    b = orig; wr32(b, 8, 24);
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_COUNT);
    b = orig; wr32(b, 8, 0);
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_COUNT);
    b = orig; wr32(b, 0x10 + 5 * 16 + 12, 0xFFFF0);  // offset out of bounds
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_TABLE);
    b = orig; wr32(b, 0x10 + 5 * 16 + 12, 0x20);  // offset into the table / not ascending
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_TABLE);
    b = orig; memset(b.data() + 0x10 + 2 * 16, 'x', 12);  // name without NUL
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_TABLE);
    b = orig; memcpy(b.data() + 0x10 + 3 * 16, "mario9\0\0\0\0\0", 12);  // mario2 renamed: a slot block missing
    CHECK(mp_save_validate(b.data(), n, true, true) == MP_SAVE_ERR_MISSING);
    // Random damage to the header and table never crashes (ASan) and never yields MP_SAVE_OK with a broken checksum.
    srand(1234);
    for (int i = 0; i < 20000; i++) {
        b = orig;
        const int flips = 1 + rand() % 4;
        for (int k = 0; k < flips; k++) b[rand() % 0x140] = (uint8_t)rand();
        const uint32_t len = (rand() % 8 == 0) ? (uint32_t)(rand() % n) : n;
        const int r = mp_save_validate(b.data(), len, true, true);
        if (r == MP_SAVE_OK) CHECK(mp_save_checksum(b.data(), rd32(b, 12)) == rd32(b, 0));
        uint32_t o, s;
        if (mp_save_find(b.data(), "mario3", &o, &s) && r == MP_SAVE_OK) CHECK(o + s <= len);
    }
}

static void testImport(const Bytes& orig) {
    const std::string sp = gDir + "/sp_GameData.bin", mp = gDir + "/mp/GameData.bin";
    writeFile(sp, orig);
    chmod(sp.c_str(), 0444);
    struct stat before;
    stat(sp.c_str(), &before);
    mkdir((gDir + "/mp").c_str(), 0755);
    // No co-op file and no base: refused, nothing created.
    CHECK(mp_save_import_file(sp.c_str(), 2, mp.c_str(), 4, nullptr, 0, nullptr) == MP_SAVE_OP_DST_MISSING);
    CHECK(!exists(mp));
    // Fresh co-op file from a base image (the game passes its in-memory image: here a copy of slot 1 everywhere, checksum stale).
    Bytes base = fullBuf(orig);
    for (int s = 2; s <= 6; s++) mp_save_copy_slot(orig.data(), 1, base.data(), s);
    wr32(base, 0, 0);
    Bytes out(MP_SAVE_MAX_FILE);
    CHECK(mp_save_import_file(sp.c_str(), 2, mp.c_str(), 4, base.data(), MP_SAVE_MAX_FILE, out.data()) == MP_SAVE_OP_OK);
    Bytes got = readFile(mp);
    CHECK(got.size() == orig.size());
    CHECK(mp_save_validate(got.data(), (uint32_t)got.size(), true, true) == MP_SAVE_OK);
    CHECK(memcmp(out.data(), got.data(), got.size()) == 0);
    CHECK(block(got, "mario4") == block(orig, "mario2"));
    CHECK(block(got, "luigi4") == block(orig, "luigi2"));
    CHECK(block(got, "config4") == block(orig, "config2"));
    CHECK(block(got, "sysconf") == block(orig, "sysconf"));
    CHECK(block(got, "mario2") == block(orig, "mario1"));  // other slots: the base's
    // Second import into the existing co-op file: slot 4 kept, slot 1 replaced by sp slot 2.
    CHECK(mp_save_import_file(sp.c_str(), 2, mp.c_str(), 1, nullptr, 0, nullptr) == MP_SAVE_OP_OK);
    got = readFile(mp);
    CHECK(block(got, "mario1") == block(orig, "mario2") && block(got, "mario4") == block(orig, "mario2"));
    CHECK(mp_save_validate(got.data(), (uint32_t)got.size(), true, true) == MP_SAVE_OK);
    // The single-player file: identical bytes, untouched (opened read-only only; mode 0444 kept; mtime unchanged).
    struct stat after;
    stat(sp.c_str(), &after);
    CHECK(readFile(sp) == orig);
    CHECK(after.st_mtime == before.st_mtime && (after.st_mode & 0777) == 0444);
    CHECK(!exists(sp + ".mptmp") && !exists(sp + ".mpbak"));
    // Same file as source and destination: refused.
    CHECK(mp_save_import_file(sp.c_str(), 1, sp.c_str(), 2, nullptr, 0, nullptr) == MP_SAVE_OP_SAME_FILE);
    symlink(sp.c_str(), (gDir + "/link.bin").c_str());
    CHECK(mp_save_import_file(sp.c_str(), 1, (gDir + "/link.bin").c_str(), 2, nullptr, 0, nullptr) == MP_SAVE_OP_SAME_FILE);
    // Corrupt sources are refused and the co-op file is not touched.
    const Bytes coop = readFile(mp);
    const std::string bad = gDir + "/bad.bin";
    Bytes b = orig; b[0x3000] ^= 0x40; writeFile(bad, b);
    CHECK(mp_save_import_file(bad.c_str(), 2, mp.c_str(), 3, nullptr, 0, nullptr) == MP_SAVE_OP_SRC_INVALID);
    b = orig; b.resize(0x1000); writeFile(bad, b);
    CHECK(mp_save_import_file(bad.c_str(), 2, mp.c_str(), 3, nullptr, 0, nullptr) == MP_SAVE_OP_SRC_INVALID);
    b = orig; wr32(b, 0x10 + 16 * 4 + 12, 0xF000); b.resize(orig.size()); writeFile(bad, b);
    CHECK(mp_save_import_file(bad.c_str(), 2, mp.c_str(), 3, nullptr, 0, nullptr) == MP_SAVE_OP_SRC_INVALID);
    CHECK(mp_save_import_file((gDir + "/none.bin").c_str(), 2, mp.c_str(), 3, nullptr, 0, nullptr) == MP_SAVE_OP_SRC_MISSING);
    CHECK(mp_save_import_file(sp.c_str(), 0, mp.c_str(), 3, nullptr, 0, nullptr) == MP_SAVE_OP_ARGS);
    CHECK(mp_save_import_file(sp.c_str(), 2, mp.c_str(), 7, nullptr, 0, nullptr) == MP_SAVE_OP_ARGS);
    CHECK(readFile(mp) == coop);
    chmod(sp.c_str(), 0644);
}

static void testWriteBack(const Bytes& orig) {
    const std::string dir = gDir + "/wb";
    mkdir(dir.c_str(), 0755);
    const std::string sp = dir + "/GameData.bin", mp = dir + "/coop.bin", backups = dir + "/backups";
    writeFile(sp, orig);
    Bytes coop = orig;
    mp_save_copy_slot(orig.data(), 2, coop.data(), 5);  // co-op slot 5 = sp slot 2's progress
    mp_save_finalize(coop.data(), (uint32_t)coop.size());
    writeFile(mp, coop);
    char backup[600];
    CHECK(mp_save_writeback_file(mp.c_str(), 5, sp.c_str(), 1, backups.c_str(), 1762600000, 10, backup, sizeof backup, nullptr) == MP_SAVE_OP_OK);
    CHECK(strstr(backup, "/backups/GameData-00000001-20251108-") != nullptr);   // savefix: sequence first, then the date
    CHECK(readFile(backup) == orig);  // the backup is the whole old single-player file
    const Bytes got = readFile(sp);
    CHECK(mp_save_validate(got.data(), (uint32_t)got.size(), true, true) == MP_SAVE_OK);
    CHECK(block(got, "mario1") == block(orig, "mario2") && block(got, "config1") == block(orig, "config2"));
    CHECK(block(got, "mario3") == block(orig, "mario3") && block(got, "sysconf") == block(orig, "sysconf"));
    // Keep the newest 10 backups.
    for (int i = 0; i < 12; i++)
        CHECK(mp_save_writeback_file(mp.c_str(), 5, sp.c_str(), 1, backups.c_str(), 1762600100 + i, 10, backup, sizeof backup, nullptr) == MP_SAVE_OP_OK);
    int count = 0;
    std::string c = "ls '" + backups + "' | grep -c '^GameData-.*\\.bin$' > " + dir + "/count.txt";
    if (system(c.c_str()) == 0) { FILE* f = fopen((dir + "/count.txt").c_str(), "r"); if (f) { if (fscanf(f, "%d", &count) != 1) count = -1; fclose(f); } }
    CHECK(count == 10);
    CHECK(exists(backup));
    // Same-second backups get distinct names.
    char b1[600], b2[600];
    CHECK(mp_save_backup(sp.c_str(), backups.c_str(), 1762700000, 50, b1, sizeof b1) && mp_save_backup(sp.c_str(), backups.c_str(), 1762700000, 50, b2, sizeof b2));
    CHECK(strcmp(b1, b2) != 0);
    // Refusals: no single-player file (nothing created), invalid single-player file (unchanged, no backup), backup impossible.
    const std::string sp2 = dir + "/missing.bin";
    CHECK(mp_save_writeback_file(mp.c_str(), 5, sp2.c_str(), 1, backups.c_str(), 1, 10, backup, sizeof backup, nullptr) == MP_SAVE_OP_DST_MISSING);
    CHECK(!exists(sp2));
    Bytes broken = orig; broken[0x2100] ^= 0xFF; writeFile(sp2, broken);
    CHECK(mp_save_writeback_file(mp.c_str(), 5, sp2.c_str(), 1, backups.c_str(), 1, 10, backup, sizeof backup, nullptr) == MP_SAVE_OP_DST_INVALID);
    CHECK(readFile(sp2) == broken);
    writeFile(dir + "/notadir", orig);
    const Bytes spBefore = readFile(sp);
    CHECK(mp_save_writeback_file(mp.c_str(), 5, sp.c_str(), 2, (dir + "/notadir/x").c_str(), 1, 10, backup, sizeof backup, nullptr) == MP_SAVE_OP_BACKUP);
    CHECK(readFile(sp) == spBefore);
}

static void testSnapshot(const Bytes& orig) {
    uint8_t snap[MP_SNAPSHOT_MAX];
    const uint32_t len = mp_save_snapshot_pack_image(orig.data(), 2, MP_SAVE_SNAP_FLAG_MARIO, snap, sizeof snap);
    CHECK(len == MP_SAVE_SNAP_HEADER + 0xF80 + 0xF80 + 0x60 && len <= MP_SNAPSHOT_MAX);
    MpSaveSnapshotView v;
    CHECK(mp_save_snapshot_parse(snap, len, &v) && v.slot == 2 && v.flags == MP_SAVE_SNAP_FLAG_MARIO);
    Bytes img = orig;
    CHECK(mp_save_snapshot_apply(&v, img.data(), 6));
    CHECK(block(img, "mario6") == block(orig, "mario2") && block(img, "luigi6") == block(orig, "luigi2") &&
          block(img, "config6") == block(orig, "config2"));
    CHECK(block(img, "mario5") == block(orig, "mario5") && block(img, "sysconf") == block(orig, "sysconf"));
    // Shorter blocks (just the chunk lists): the rest of the destination block is zeroed.
    // (snapval: blocks are validated now (mp_savecheck), so the short blocks are real ones cut to their used length)
    const Bytes sm = block(orig, "mario2"), sl = block(orig, "luigi2"), sc = block(orig, "config2");
    const uint32_t um = mp_save_block_used(sm.data(), 0xF80), ul = mp_save_block_used(sl.data(), 0xF80), uc = mp_save_block_used(sc.data(), 0x60);
    CHECK(um < 0xF80 && ul < 0xF80 && uc < 0x60);
    const uint32_t l2 = mp_save_snapshot_pack(sm.data(), um, sl.data(), ul, sc.data(), uc, 1, 0, snap, sizeof snap);
    CHECK(l2 == MP_SAVE_SNAP_HEADER + um + ul + uc && mp_save_snapshot_parse(snap, l2, &v) && mp_save_snapshot_apply(&v, img.data(), 3));
    Bytes m3 = block(img, "mario3");
    CHECK(memcmp(m3.data(), sm.data(), um) == 0 && m3[um] == 0 && m3[0xF7F] == 0);
    // Blocks the game's parsers would read out of bounds are refused (and nothing is applied).
    Bytes small(100, 0xAB);
    const uint32_t l4 = mp_save_snapshot_pack(small.data(), 100, small.data(), 50, small.data(), 10, 1, 0, snap, sizeof snap);
    CHECK(l4 == MP_SAVE_SNAP_HEADER + 160 && !mp_save_snapshot_parse(snap, l4, &v));
    // Hostile payloads.
    CHECK(mp_save_snapshot_pack(small.data(), 0xF81, small.data(), 1, small.data(), 1, 1, 0, snap, sizeof snap) == 0);
    CHECK(mp_save_snapshot_pack(small.data(), 1, small.data(), 1, small.data(), 1, 7, 0, snap, sizeof snap) == 0);
    const uint32_t l3 = mp_save_snapshot_pack_image(orig.data(), 2, 1, snap, sizeof snap);
    uint8_t t[MP_SNAPSHOT_MAX];
    memcpy(t, snap, l3); t[100] ^= 1; CHECK(!mp_save_snapshot_parse(t, l3, &v));            // crc
    memcpy(t, snap, l3); CHECK(!mp_save_snapshot_parse(t, l3 - 1, &v));                       // short
    memcpy(t, snap, l3); t[6] = 0; CHECK(!mp_save_snapshot_parse(t, l3, &v));                 // slot 0
    memcpy(t, snap, l3); t[6] = 9; CHECK(!mp_save_snapshot_parse(t, l3, &v));                 // slot 9
    memcpy(t, snap, l3); t[7] = 0x80; CHECK(!mp_save_snapshot_parse(t, l3, &v));             // unknown flag
    memcpy(t, snap, l3); t[0] = 'X'; CHECK(!mp_save_snapshot_parse(t, l3, &v));               // magic
    memcpy(t, snap, l3); t[12] = 0x61; CHECK(!mp_save_snapshot_parse(t, l3, &v));            // config len 0x61 > 0x60
    memcpy(t, snap, l3); t[8] = 0; t[9] = 0; CHECK(!mp_save_snapshot_parse(t, l3, &v));      // zero length
    CHECK(!mp_save_snapshot_parse(nullptr, l3, &v) && !mp_save_snapshot_parse(snap, MP_SNAPSHOT_MAX + 1, &v));
    srand(99);
    for (int i = 0; i < 20000; i++) {
        memcpy(t, snap, l3);
        uint32_t l = l3;
        for (int k = 0; k < 1 + rand() % 3; k++) t[rand() % 24] = (uint8_t)rand();
        if (rand() % 4 == 0) l = (uint32_t)(rand() % (l3 + 1));
        if (mp_save_snapshot_parse(t, l, &v)) {
            Bytes x = orig;
            CHECK((uint32_t)(v.len[0] + v.len[1] + v.len[2] + MP_SAVE_SNAP_HEADER) == l);
            mp_save_snapshot_apply(&v, x.data(), 1 + rand() % 6);
        }
    }
    // Apply into an image without that slot is refused.
    Bytes noSlot = orig; memcpy(noSlot.data() + 0x10 + 15 * 16, "zzz\0", 4);  // mario6 renamed
    CHECK(mp_save_snapshot_parse(snap, l3, &v) && !mp_save_snapshot_apply(&v, noSlot.data(), 6));
}

static void testAtomic(const Bytes& orig) {
    const std::string dir = gDir + "/atomic";
    mkdir(dir.c_str(), 0755);
    const std::string p = dir + "/GameData.bin";
    Bytes newer = orig;
    mp_save_copy_slot(orig.data(), 2, newer.data(), 1);
    mp_save_finalize(newer.data(), (uint32_t)newer.size());
    CHECK(newer != orig);
    for (int withOld = 0; withOld <= 1; withOld++) {
        for (int step = 1; step <= 6; step++) {
            for (int truncTmp = 0; truncTmp <= 1; truncTmp++) {
                rmrf(dir); mkdir(dir.c_str(), 0755);
                if (withOld) writeFile(p, orig);
                mp_save_test_crash_after(step);
                mp_save_write_atomic(p.c_str(), newer.data(), (uint32_t)newer.size());
                mp_save_test_crash_after(0);
                if (truncTmp && exists(p + ".mptmp")) truncate((p + ".mptmp").c_str(), 0x100);  // a torn temporary
                bool changed = false;
                const bool ok = mp_save_recover(p.c_str(), &changed);
                const Bytes got = readFile(p);
                const bool isOld = withOld && got == orig, isNew = got == newer;
                if (withOld || exists(p)) {
                    CHECK(ok && (isOld || isNew));
                    if (!(isOld || isNew)) fprintf(stderr, "  withOld=%d step=%d trunc=%d size=%zu\n", withOld, step, truncTmp, got.size());
                } else {
                    CHECK(ok && !exists(p));  // crashed before anything valid existed: nothing (a fresh start)
                }
                if (step >= 3 && !truncTmp) CHECK(isNew || (withOld && step < 5 && isOld));
                CHECK(!exists(p + ".mptmp"));
            }
        }
    }
    rmrf(dir); mkdir(dir.c_str(), 0755);
    writeFile(p, orig);
    CHECK(mp_save_write_atomic(p.c_str(), newer.data(), (uint32_t)newer.size()));
    CHECK(readFile(p) == newer && readFile(p + ".mpbak") == orig && !exists(p + ".mptmp"));
    // An invalid primary with a valid .mpbak: recovered from the backup, the broken file kept as .mpbad.
    Bytes junk(0x200, 0x55); writeFile(p, junk);
    bool changed = false;
    CHECK(mp_save_recover(p.c_str(), &changed) && changed && readFile(p) == orig && readFile(p + ".mpbad") == junk);
}

// smallfix: the session's .mpbak is removed only when GameData.bin validates and no .mptmp is beside it
static void testDropSessionBackup(const Bytes& orig) {
    const std::string dir = gDir + "/dropbak";
    mkdir(dir.c_str(), 0755);
    const std::string p = dir + "/GameData.bin";
    const char* why = "";
    auto clean = [&] { for (const char* s : {"", ".mpbak", ".mptmp"}) unlink((p + s).c_str()); };
    clean();
    writeFile(p, orig); writeFile(p + ".mpbak", orig);
    CHECK(mp_save_drop_session_backup(p.c_str(), &why) && !exists(p + ".mpbak") && exists(p));          // good primary: removed
    clean();
    Bytes torn(orig.begin(), orig.begin() + orig.size() / 2);
    writeFile(p, torn); writeFile(p + ".mpbak", orig);
    CHECK(!mp_save_drop_session_backup(p.c_str(), &why) && exists(p + ".mpbak") && why[0]);              // bad primary (failed read-back): kept
    clean();
    writeFile(p + ".mpbak", orig); writeFile(p + ".mptmp", orig);
    CHECK(!mp_save_drop_session_backup(p.c_str(), &why) && exists(p + ".mpbak") && exists(p + ".mptmp")); // both renames refused: no primary: kept
    clean();
    writeFile(p, orig); writeFile(p + ".mpbak", orig); writeFile(p + ".mptmp", orig);
    CHECK(!mp_save_drop_session_backup(p.c_str(), &why) && exists(p + ".mpbak"));                        // good primary but a .mptmp: kept
    clean();
    writeFile(p + ".mpbak", orig);
    CHECK(!mp_save_drop_session_backup(p.c_str(), &why) && exists(p + ".mpbak"));                        // missing primary, lone .mpbak: kept
    clean();
}

// scenarios-final: SMG_MP_TEST_SAVE_FAIL (mp_save_test_fail_from): from the n-th GameData.bin commit on mp_save_commit_tmp fails and the .mptmp stays
static void testSaveFailHook(const Bytes& orig) {
    const std::string dir = gDir + "/failhook";
    mkdir(dir.c_str(), 0755);
    const std::string p = dir + "/GameData.bin";
    auto clean = [&] { for (const char* s : {"", ".mpbak", ".mptmp"}) unlink((p + s).c_str()); };
    clean();
    mp_save_test_fail_from(2);
    writeFile(p, orig); writeFile(p + ".mptmp", orig);
    CHECK(mp_save_commit_tmp(p.c_str(), nullptr) && exists(p + ".mpbak") && !exists(p + ".mptmp"));      // 1st commit passes (makes the .mpbak)
    writeFile(p + ".mptmp", orig);
    CHECK(!mp_save_commit_tmp(p.c_str(), nullptr) && exists(p + ".mptmp") && exists(p + ".mpbak"));      // 2nd fails: the .mptmp stays
    const char* why = "";
    CHECK(!mp_save_drop_session_backup(p.c_str(), &why) && exists(p + ".mpbak") && strstr(why, ".mptmp"));   // ... so the session keeps its .mpbak
    mp_save_test_fail_from(0);
    CHECK(mp_save_commit_tmp(p.c_str(), nullptr) && !exists(p + ".mptmp"));                               // off: commits again
    clean();
    mp_save_test_fail_from(1);
    writeFile(p + ".mptmp", orig);
    const std::string other = dir + "/other.bin";
    writeFile(other + ".mptmp", orig);
    CHECK(mp_save_commit_tmp(other.c_str(), nullptr));                                                   // only GameData.bin is hit
    mp_save_test_fail_from(0);
    clean(); unlink(other.c_str()); unlink((other + ".mpbak").c_str());
}

static void testPolicy() {
    CHECK(mp_save_policy(false, -1, -1, 0, false) == MP_SAVE_POLICY_NORMAL);
    CHECK(mp_save_policy(false, 0, 0, 0, true) == MP_SAVE_POLICY_NORMAL);   // no session: never touch the game's saves
    CHECK(mp_save_policy(true, 0, 0, 0, false) == MP_SAVE_POLICY_AUTOCONFIRM);  // original host
    CHECK(mp_save_policy(true, 1, 0, 0, false) == MP_SAVE_POLICY_SUPPRESS);     // guest
    CHECK(mp_save_policy(true, 3, 0, 0, false) == MP_SAVE_POLICY_SUPPRESS);
    CHECK(mp_save_policy(true, 1, 1, 1, false) == MP_SAVE_POLICY_SUPPRESS);     // host by migration
    CHECK(mp_save_policy(true, 2, 1, 1, false) == MP_SAVE_POLICY_SUPPRESS);     // guest after a migration
    CHECK(mp_save_policy(true, 0, 1, 1, false) == MP_SAVE_POLICY_SUPPRESS);     // original host back (rejoin) after a migration
    CHECK(mp_save_policy(true, 0, 0, 2, false) == MP_SAVE_POLICY_SUPPRESS);     // any migration: nobody saves until the end
    CHECK(mp_save_policy(true, 0, 0, 0, true) == MP_SAVE_POLICY_SUPPRESS);      // core says suppress
    CHECK(mp_save_policy(true, 0, -1, 0, false) == MP_SAVE_POLICY_SUPPRESS);    // host unknown (weak bridge): safe side
}

static void testStars(const Bytes& orig) {
    int m = -1, l = -1;
    CHECK(mp_save_slot_stars(orig.data(), 2, &m, &l) == 50 && m == 50 && l == 0);
    CHECK(mp_save_slot_stars(orig.data(), 1, &m, &l) == 0);
    CHECK(mp_save_slot_stars(orig.data(), 7, &m, &l) == -1);
    CHECK(mp_save_compare_progress(orig.data(), 1, orig.data(), 2) < 0);  // importing file 1 over a co-op slot like file 2: ask
    CHECK(mp_save_compare_progress(orig.data(), 2, orig.data(), 1) > 0);
    CHECK(mp_save_compare_progress(orig.data(), 3, orig.data(), 4) == 0);
    Bytes zero(0xF80, 0);
    CHECK(mp_save_block_stars(zero.data(), (uint32_t)zero.size()) == 0);
    uint32_t o, s;
    mp_save_find(orig.data(), "mario2", &o, &s);
    Bytes b(orig.begin() + o, orig.begin() + o + s);
    srand(7);
    for (int i = 0; i < 20000; i++) {  // hostile blocks never read out of bounds
        Bytes x = b;
        for (int k = 0; k < 1 + rand() % 4; k++) x[rand() % 0x400] = (uint8_t)rand();
        const uint32_t len = rand() % 3 == 0 ? (uint32_t)(rand() % x.size()) : (uint32_t)x.size();
        Bytes y(x.begin(), x.begin() + len);
        mp_save_block_stars(y.data(), len);
    }
}

static MpSaveRecoveryInfo recInfo(uint32_t seq) {
    MpSaveRecoveryInfo i;
    memset(&i, 0, sizeof i);
    i.sessionId = 0xC0FFEE;
    i.seq = seq;
    i.unixTime = 1762600000 + seq;
    i.flags = MP_SAVE_REC_FLAG_MARIO;
    i.slot = 2;
    i.scenario = 3;
    i.stars = 50;
    snprintf(i.galaxy, sizeof i.galaxy, "EggStarGalaxy");
    return i;
}

static void testRecovery(const Bytes& orig) {
    const std::string dir = gDir + "/mp/recovery";
    MpSaveSnapshotView v, w;
    CHECK(mp_save_view_slot(orig.data(), 2, &v));
    uint8_t rec[MP_SAVE_REC_MAX], rec2[MP_SAVE_REC_MAX];
    MpSaveRecoveryInfo info = recInfo(1), got;
    const uint32_t len = mp_save_recovery_pack(&info, &v, rec, sizeof rec);
    CHECK(len == MP_SAVE_REC_MAX);
    CHECK(mp_save_recovery_parse(rec, len, &got, &w) && got.seq == 1 && got.slot == 2 && got.stars == 50 && got.unixTime == info.unixTime &&
          strcmp(got.galaxy, "EggStarGalaxy") == 0 && (w.flags & MP_SAVE_SNAP_FLAG_MARIO));
    CHECK(mp_save_slot_matches(orig.data(), 2, &w) && !mp_save_slot_matches(orig.data(), 1, &w));
    // Hostile records.
    uint8_t t[MP_SAVE_REC_MAX];
    memcpy(t, rec, len); t[MP_SAVE_REC_HEADER + 5] ^= 1; CHECK(!mp_save_recovery_parse(t, len, &got, &w));
    memcpy(t, rec, len); t[25] = 7; CHECK(!mp_save_recovery_parse(t, len, &got, &w));   // slot 7
    memcpy(t, rec, len); t[26] = 1; t[27] = 0; CHECK(!mp_save_recovery_parse(t, len, &got, &w));  // co-op source without a slot
    memcpy(t, rec, len); t[32] = 1; CHECK(!mp_save_recovery_parse(t, len, &got, &w));   // control char in the galaxy name
    CHECK(!mp_save_recovery_parse(rec, len - 1, &got, &w) && !mp_save_recovery_parse(rec, 10, &got, &w));
    srand(5);
    for (int i = 0; i < 20000; i++) {
        memcpy(t, rec, len);
        uint32_t l = len;
        for (int k = 0; k < 1 + rand() % 3; k++) t[rand() % MP_SAVE_REC_HEADER] = (uint8_t)rand();
        if (rand() % 4 == 0) l = (uint32_t)(rand() % (len + 1));
        if (mp_save_recovery_parse(t, l, &got, &w)) CHECK((uint32_t)(w.len[0] + w.len[1] + w.len[2] + MP_SAVE_REC_HEADER) == l);
    }
    // Write, keep newest 3, list ignores other names, next index looks into discarded too.
    rmrf(dir);
    for (int n = 1; n <= 5; n++) {
        MpSaveRecoveryInfo in = recInfo((uint32_t)n);
        const uint32_t l = mp_save_recovery_pack(&in, &v, rec, sizeof rec);
        CHECK(mp_save_recovery_write(dir.c_str(), n, rec, l, MP_SAVE_RECOVERY_KEEP));
    }
    writeFile(dir + "/notes.txt", Bytes(3, 'x'));
    int idx[16];
    int n = mp_save_recovery_list(dir.c_str(), idx, 16);
    CHECK(n == 3 && idx[0] == 3 && idx[2] == 5);
    CHECK(mp_save_recovery_next_index(dir.c_str()) == 6);
    uint32_t rl = 0;
    CHECK(mp_save_recovery_read(dir.c_str(), 4, rec2, sizeof rec2, &rl) && mp_save_recovery_parse(rec2, rl, &got, nullptr) && got.seq == 4);
    CHECK(mp_save_recovery_discard(dir.c_str(), 5, 10));
    CHECK(exists(dir + "/discarded/session-00000005.bin") && !exists(dir + "/session-00000005.bin"));
    CHECK(mp_save_recovery_next_index(dir.c_str()) == 6);
    CHECK(mp_save_recovery_delete(dir.c_str(), 4) && !exists(dir + "/session-00000004.bin"));
    {   // keepfix (keepsafe review 2 #3): at the largest number the next index is never one in use and never wraps under a live file
        const std::string wd = dir + "_wrap";
        rmrf(wd);
        MpSaveRecoveryInfo in = recInfo(1);
        const uint32_t l = mp_save_recovery_pack(&in, &v, rec, sizeof rec);
        CHECK(mp_save_recovery_write(wd.c_str(), 1, rec, l, 0) && mp_save_recovery_write(wd.c_str(), 99999999, rec, l, 0));
        const int next = mp_save_recovery_next_index(wd.c_str());
        char p1[600];
        mp_save_recovery_path(wd.c_str(), next, p1, sizeof p1);
        CHECK(next < 0 || (next > 0 && !exists(p1)));
        CHECK(next != 1);   // (a lower number than live ones is the first the keep-3 prune deletes)
        rmrf(wd);
    }
    CHECK(!mp_save_recovery_write(dir.c_str(), 9, orig.data(), 0x100, 3));  // only recovery records are written there
    // Crash in the middle of a checkpoint rewrite (and of the first write): the checkpoint is recovered, old or new.
    MpSaveRecoveryInfo i1 = recInfo(10), i2 = recInfo(11);
    MpSaveSnapshotView v1;
    mp_save_view_slot(orig.data(), 1, &v1);
    uint8_t r1[MP_SAVE_REC_MAX], r2[MP_SAVE_REC_MAX];
    const uint32_t l1 = mp_save_recovery_pack(&i1, &v1, r1, sizeof r1), l2 = mp_save_recovery_pack(&i2, &v, r2, sizeof r2);
    for (int first = 0; first <= 1; first++) {
        for (int step = 1; step <= 6; step++) {
            for (int torn = 0; torn <= 1; torn++) {
                rmrf(dir);
                if (!first) CHECK(mp_save_recovery_write(dir.c_str(), 7, r1, l1, 3));
                mp_save_test_crash_after(step);
                mp_save_recovery_write(dir.c_str(), 7, r2, l2, 3);
                mp_save_test_crash_after(0);
                const std::string tmp = dir + "/session-00000007.bin.mptmp";
                if (torn && exists(tmp)) truncate(tmp.c_str(), 40);
                n = mp_save_recovery_list(dir.c_str(), idx, 16);
                const bool ok = mp_save_recovery_read(dir.c_str(), 7, rec2, sizeof rec2, &rl) && mp_save_recovery_parse(rec2, rl, &got, nullptr);
                if (first) {
                    // Nothing older exists: either the new checkpoint or nothing (a torn first write), never garbage.
                    CHECK(!ok || got.seq == 11);
                    if (step >= 3 && !torn) CHECK(ok && got.seq == 11 && n == 1);
                } else {
                    CHECK(ok && (got.seq == 10 || got.seq == 11));
                    if (step >= 5 && !torn) CHECK(got.seq == 11);
                }
            }
        }
    }
    // Background writer: the bytes land, the result reports the time; a crash inside it is recovered on the next read.
    rmrf(dir);
    CHECK(mp_save_async_submit(dir.c_str(), 12, r1, l1, 3));
    mp_save_async_wait();
    MpSaveAsyncResult ar;
    CHECK(mp_save_async_take_result(&ar) && ar.ok && ar.index == 12 && ar.bytes == l1);
    CHECK(!mp_save_async_take_result(&ar));
    CHECK(mp_save_recovery_read(dir.c_str(), 12, rec2, sizeof rec2, &rl) && rl == l1 && memcmp(rec2, r1, l1) == 0);
    mp_save_test_crash_after(5);
    CHECK(mp_save_async_submit(dir.c_str(), 12, r2, l2, 3));
    mp_save_async_wait();
    mp_save_test_crash_after(0);
    CHECK(mp_save_async_take_result(&ar) && !ar.ok);
    CHECK(!exists(dir + "/session-00000012.bin") && exists(dir + "/session-00000012.bin.mptmp"));  // "power cut" mid-write
    CHECK(mp_save_recovery_read(dir.c_str(), 12, rec2, sizeof rec2, &rl) && mp_save_recovery_parse(rec2, rl, &got, nullptr) && got.seq == 11);
    for (int k = 0; k < 50; k++) mp_save_async_submit(dir.c_str(), 12, (k & 1) ? r1 : r2, (k & 1) ? l1 : l2, 3);  // latest wins
    mp_save_async_wait();
    CHECK(mp_save_recovery_read(dir.c_str(), 12, rec2, sizeof rec2, &rl) && rl == l1 && memcmp(rec2, r1, l1) == 0);
    // fix2 E: a writer that never finishes (hung card): the wait ends at its timeout instead of blocking the game thread forever
    mp_save_test_hang_writer(1);
    mp_save_test_async_timeout_ms(150);
    CHECK(mp_save_async_submit(dir.c_str(), 12, r2, l2, 3));
    const struct timespec t0 = [] { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t; }();
    CHECK(!mp_save_async_wait());                  // times out: false = "the file can't be used right now"
    CHECK(!mp_save_async_wait_ms(20));
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    const long waitedMs = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(waitedMs >= 140 && waitedMs < 2000);
    CHECK(!mp_save_async_idle());
    mp_save_test_hang_writer(0);                   // the card answers late: idle again, the wait succeeds
    CHECK(mp_save_async_wait());
    CHECK(mp_save_async_idle());
    mp_save_test_async_timeout_ms(0);
    // Keeping a recovery file in a co-op slot: a fresh co-op file from a blank base, then into an existing one.
    const std::string coop = gDir + "/rec_coop/GameData.bin";
    mkdir((gDir + "/rec_coop").c_str(), 0755);
    Bytes base = fullBuf(orig);
    MpSaveSnapshotView blank;
    mp_save_view_slot(orig.data(), 3, &blank);  // slot 3 of the test save was never played: it stands in for the blank blocks
    CHECK(mp_save_fill_blank_base(base.data(), MP_SAVE_MAX_FILE, &blank));
    CHECK(mp_save_slot_matches(base.data(), 2, &blank) && mp_save_slot_matches(base.data(), 6, &blank));
    CHECK(mp_save_recovery_parse(r2, l2, &got, &w));
    CHECK(mp_save_put_blocks(&w, coop.c_str(), 5, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_OK);
    Bytes c = readFile(coop);
    CHECK(mp_save_validate(c.data(), (uint32_t)c.size(), true, true) == MP_SAVE_OK);
    CHECK(block(c, "mario5") == block(orig, "mario2") && block(c, "config5") == block(orig, "config2"));
    CHECK(block(c, "mario2") == block(orig, "mario3"));  // other slots blank, never the source's other files
    CHECK(mp_save_slot_stars(c.data(), 5, nullptr, nullptr) == 50 && mp_save_slot_stars(c.data(), 2, nullptr, nullptr) == 0);
    CHECK(mp_save_put_blocks(&w, coop.c_str(), 1, nullptr, 0, nullptr) == MP_SAVE_OP_OK);
    c = readFile(coop);
    CHECK(block(c, "mario1") == block(orig, "mario2") && block(c, "mario5") == block(orig, "mario2"));
    CHECK(mp_save_put_blocks(&w, (gDir + "/none/GameData.bin").c_str(), 1, nullptr, 0, nullptr) == MP_SAVE_OP_DST_MISSING);
    // gamefix 6: the blank base replaces the destination only when it is MISSING. An existing co-op file that fails validation is
    // kept aside (.mpbad) and nothing is written; one that cannot be read is left alone; a single-player destination (no fallback)
    // is never touched.
    {
        const std::string dir2 = gDir + "/rec_bad";
        mkdir(dir2.c_str(), 0755);
        const std::string bad = dir2 + "/GameData.bin";
        Bytes good = readFile(coop);   // a co-op file with several played slots
        Bytes broken = good;
        broken[0x3000] ^= 0x40;        // checksum mismatch
        writeFile(bad, broken);
        CHECK(mp_save_validate(broken.data(), (uint32_t)broken.size(), true, true) != MP_SAVE_OK);
        CHECK(mp_save_put_blocks(&w, bad.c_str(), 3, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_DST_ASIDE);
        CHECK(!exists(bad) && exists(bad + ".mpbad") && readFile(bad + ".mpbad") == broken);   // kept, byte for byte; not blanked
        CHECK(mp_save_put_blocks(&w, bad.c_str(), 3, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_OK);   // retry: missing -> fresh
        CHECK(exists(bad) && mp_save_slot_stars(readFile(bad).data(), 3, nullptr, nullptr) == 50);
        // the same bad file as a single-player destination (no fallback): untouched
        const std::string sp = dir2 + "/sp/GameData.bin";
        mkdir((dir2 + "/sp").c_str(), 0755);
        writeFile(sp, broken);
        CHECK(mp_save_put_blocks(&w, sp.c_str(), 3, nullptr, 0, nullptr) == MP_SAVE_OP_DST_INVALID);
        CHECK(exists(sp) && !exists(sp + ".mpbad") && readFile(sp) == broken);
        // a file that fails to read (a directory in its place: EISDIR): left exactly as it is, nothing written, no .mpbad
        const std::string unreadable = dir2 + "/dirfile/GameData.bin";
        mkdir((dir2 + "/dirfile").c_str(), 0755);
        mkdir(unreadable.c_str(), 0755);
        CHECK(mp_save_put_blocks(&w, unreadable.c_str(), 3, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_DST_INVALID);
        CHECK(!exists(unreadable + ".mpbad"));
        // a missing file still gets the blank base (a new co-op file; the caller makes the folder)
        mkdir((dir2 + "/fresh").c_str(), 0755);
        CHECK(mp_save_put_blocks(&w, (dir2 + "/fresh/GameData.bin").c_str(), 3, base.data(), MP_SAVE_MAX_FILE, nullptr) == MP_SAVE_OP_OK);
    }
    // block_used: the stale tail of a block does not count.
    Bytes m = block(orig, "mario2");
    const uint32_t used = mp_save_block_used(m.data(), (uint32_t)m.size());
    CHECK(used > 4 && used <= m.size());
    Bytes img = orig;
    uint32_t mo, ms;
    mp_save_find(img.data(), "mario2", &mo, &ms);
    if (used < ms) img[mo + ms - 1] ^= 0x5A;  // garbage after the chunk list
    CHECK(mp_save_slot_matches(img.data(), 2, &v));
}

// savefix (m-files): slotState's three states from the file on disk
static void testSlotState(const Bytes& orig) {
    const std::string dir = gDir + "/slotstate";
    mkdir(dir.c_str(), 0755);
    const std::string p = dir + "/GameData.bin";
    int stars = 7;
    CHECK(mp_save_slot_state_file(p.c_str(), false, 2, &stars) == 0 && stars == -1);   // no file: new
    writeFile(p + ".mptmp", orig);
    CHECK(mp_save_slot_state_file(p.c_str(), true, 2, &stars) == -1);                    // an interrupted replace (.mptmp) beside it
    unlink((p + ".mptmp").c_str());
    writeFile(p + ".mpbak", orig);
    CHECK(mp_save_slot_state_file(p.c_str(), true, 2, &stars) == 0);                     // only the previous version: a deleted save (smallfix)
    unlink((p + ".mpbak").c_str());
    writeFile(p, orig);
    CHECK(mp_save_slot_state_file(p.c_str(), false, 2, &stars) == 1 && stars == 50);    // the test save's file 2: 50 stars
    const int s1 = mp_save_slot_state_file(p.c_str(), false, 1, &stars);
    CHECK(s1 == 0 || s1 == 1);
    if (s1 == 0) CHECK(stars == -1);
    int coopStars = -1;
    CHECK(mp_save_slot_state_file(p.c_str(), true, 1, &coopStars) == 1 && coopStars >= 0);   // co-op: regardless of the created flag
    CHECK(mp_save_slot_state_file(p.c_str(), false, 0, &stars) == -1 && mp_save_slot_state_file(p.c_str(), false, 7, &stars) == -1);
    Bytes bad = orig;
    bad[0x200] ^= 1;   // checksum no longer matches
    writeFile(p, bad);
    CHECK(mp_save_slot_state_file(p.c_str(), true, 2, &stars) == -1 && stars == -1);     // there but invalid: unreadable, never "new"
    chmod(p.c_str(), 0);
    if (geteuid() != 0) CHECK(mp_save_slot_state_file(p.c_str(), true, 2, &stars) == -1);   // cannot be opened: unreadable
    chmod(p.c_str(), 0644);
    Bytes slotBad = orig;
    uint32_t off = 0, size = 0;
    mp_save_find(slotBad.data(), "mario2", &off, &size);
    slotBad[off] = 9;   // the block's format byte: does not parse
    mp_save_finalize(slotBad.data(), (uint32_t)slotBad.size());
    writeFile(p, slotBad);
    CHECK(mp_save_slot_state_file(p.c_str(), true, 2, &stars) == -1);
    CHECK(mp_save_slot_state_file(p.c_str(), true, 3, &stars) == 1);                     // the other slots still read
}

// A4: the torn-write loop. A 4 KiB synthetic image (magic, id, pseudo-random body, FNV-1a checksum) is replaced by another one with the
// power cut at EVERY byte boundary of write(tmp), and at each of the rename steps (3 = tmp complete, 4 = before rename 1 (path -> .mpbak),
// 5 = between the renames, 6 = after rename 2 (tmp -> path)); each with a zero-filled-tail variant (the file that holds the new bytes is
// present at the right length but zero from the cut on: a medium that kept the size and lost the data). Recovery runs, and what is in
// place must be exactly the old image or the new one (or nothing, when no old one existed): never anything else.
static uint32_t fnv(const uint8_t* d, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; } return h; }
static Bytes synthImage(uint32_t id, uint32_t n = 4096) {
    Bytes b(n);
    uint32_t x = id * 2654435761u + 1;
    for (uint32_t i = 8; i + 4 < n; i++) { x = x * 1664525u + 1013904223u; b[i] = (uint8_t)(x >> 24); }
    memcpy(b.data(), "TORN", 4);
    wr32(b, 4, id);
    wr32(b, n - 4, fnv(b.data(), n - 4));
    return b;
}
static bool synthValid(const uint8_t* d, uint32_t n) {
    if (n != 4096 || memcmp(d, "TORN", 4) != 0) return false;
    const uint32_t want = d[n - 4] | (d[n - 3] << 8) | (d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
    return fnv(d, n - 4) == want;
}
static void zeroTail(const std::string& f, uint32_t cut, uint32_t len) {   // present, right length, zero from `cut` on
    FILE* fp = fopen(f.c_str(), "r+b");
    if (!fp) return;
    fseek(fp, (long)cut, SEEK_SET);
    Bytes z(len - cut, 0);
    fwrite(z.data(), 1, z.size(), fp);
    fclose(fp);
}
static void cleanDir(const std::string& d) {   // (rmrf forks a shell: far too slow for tens of thousands of cells)
    mkdir(d.c_str(), 0755);
    if (DIR* dp = opendir(d.c_str())) {
        while (struct dirent* e = readdir(dp)) if (e->d_name[0] != '.') unlink((d + "/" + e->d_name).c_str());
        closedir(dp);
    }
}
static std::vector<uint32_t> cutsFor(uint32_t len, bool all) {
    std::vector<uint32_t> c;
    for (uint32_t i = 0; i <= len; i++) if (all || i < 64 || i + 64 >= len || i % 37 == 0) c.push_back(i);
    return c;
}
// One cell of the grid. `writer` writes `neu` over `old` (or over nothing); `read` runs the matching recovery and returns what is in place.
template <class Write, class Read>
static void tornGrid(const char* what, const std::string& dir, const std::string& primary, const Bytes& old, const Bytes& neu, bool allCuts, Write writer, Read reader) {
    const uint32_t len = (uint32_t)neu.size();
    int cells = 0, holes = 0;
    for (int withOld = 0; withOld <= 1; withOld++) {
        for (int stage = 0; stage <= 4; stage++) {          // 0 = cut inside write(tmp); 1..4 = crash after step 3, 4, 5, 6
            for (int zero = 0; zero <= 1; zero++) {
                for (uint32_t cut : cutsFor(len, allCuts)) {
                    if (stage > 0 && cut == len && zero) continue;
                    if (stage > 0 && !zero && cut != 0 && cut != len) continue;   // (a torn file at a rename step only exists in the zero-tail variant)
                    if (stage == 0 && zero && cut == len) continue;
                    cleanDir(dir);
                    if (withOld) writeFile(primary, old);
                    if (stage == 0) mp_save_test_cut_write((int)cut); else mp_save_test_crash_after(stage + 2);
                    writer();
                    mp_save_test_cut_write(-1);
                    mp_save_test_crash_after(0);
                    if (zero) {
                        const std::string tmp = primary + ".mptmp";
                        zeroTail(stage == 4 ? primary : tmp, cut, len);    // the file that holds the new bytes (stage 4: already renamed in)
                        if (stage == 0 && exists(tmp)) truncate(tmp.c_str(), len), zeroTail(tmp, cut, len);
                    }
                    const Bytes got = reader();
                    cells++;
                    const bool isOld = withOld && got == old, isNew = got == neu, isNone = got.empty() && !withOld;
                    if (!(isOld || isNew || isNone)) {
                        holes++;
                        if (holes <= 5) fprintf(stderr, "  HOLE %s: withOld=%d stage=%d zero=%d cut=%u got %zu bytes\n", what, withOld, stage, zero, cut, got.size());
                    }
                    CHECK(isOld || isNew || isNone);
                }
            }
        }
    }
    printf("  torn-write grid %s: %d cells, %d holes\n", what, cells, holes);
}

static void testTornLoop(const Bytes& orig) {
    const std::string dir = gDir + "/torn";
    const std::string p = dir + "/GameData.bin";
    const Bytes a = synthImage(1), b = synthImage(2);
    CHECK(synthValid(a.data(), 4096) && synthValid(b.data(), 4096) && a != b);
    tornGrid("generic 4 KiB", dir, p, a, b, true,
        [&] { mp_save_write_atomic(p.c_str(), b.data(), (uint32_t)b.size()); },
        [&] { bool ch = false; mp_save_recover_with(p.c_str(), synthValid, &ch); return readFile(p); });
    // the recovery-checkpoint writer (mp_save_recovery_write -> mp_save_write_atomic; read = mp_save_recovery_read with its recovery)
    MpSaveSnapshotView v1, v2;
    CHECK(mp_save_view_slot(orig.data(), 1, &v1) && mp_save_view_slot(orig.data(), 2, &v2));
    MpSaveRecoveryInfo i1 = recInfo(10), i2 = recInfo(11);
    uint8_t r1[MP_SAVE_REC_MAX], r2[MP_SAVE_REC_MAX];
    const uint32_t l1 = mp_save_recovery_pack(&i1, &v1, r1, sizeof r1), l2 = mp_save_recovery_pack(&i2, &v2, r2, sizeof r2);
    CHECK(l1 && l2);
    const Bytes ro(r1, r1 + l1), rn(r2, r2 + l2);
    CHECK(ro != rn);
    const std::string rp = dir + "/session-00000007.bin";
    tornGrid("recovery checkpoint", dir, rp, ro, rn, false,
        [&] { mp_save_recovery_write(dir.c_str(), 7, r2, l2, 3); },
        [&] {
            uint8_t buf[MP_SAVE_REC_MAX]; uint32_t n = 0;
            if (!mp_save_recovery_read(dir.c_str(), 7, buf, sizeof buf, &n)) return Bytes();
            return Bytes(buf, buf + n);
        });
    rmrf(dir);
}

int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    const Bytes orig = readFile(src);
    if (orig.empty()) { fprintf(stderr, "cannot read %s\n", src); return 2; }
    char tmpl[] = "/tmp/mp_save_test.XXXXXX";
    gDir = mkdtemp(tmpl);
    testChecksumAndValidate(orig);
    testCorrupt(orig);
    testImport(orig);
    testWriteBack(orig);
    testSnapshot(orig);
    testAtomic(orig);
    testTornLoop(orig);
    testPolicy();
    testStars(orig);
    testRecovery(orig);
    testSlotState(orig);
    testDropSessionBackup(orig);
    testSaveFailHook(orig);
    CHECK(readFile(src) == orig);  // the original save was only read
    rmrf(gDir);
    printf("save_test: %d checks, %d failed\n", sChecks, sFails);
    return sFails ? 1 : 0;
}
