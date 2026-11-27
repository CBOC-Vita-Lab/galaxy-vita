// savesus S8: can the keep-3 prune of recovery checkpoints delete an UNRESOLVED crash recovery (one the player was never asked about,
// or never answered)? Drives the real mp_savefile.cpp: 3 unresolved crash files (1..3), then two more sessions each write a checkpoint
// exactly as MpSaveGame does (mp_save_recovery_write(.., MP_SAVE_RECOVERY_KEEP)). Rule under test: a checkpoint nobody answered is never
// unlinked by a prune; it may only be set aside in discarded/ (where the player's own "No" puts one).
#include "../../platform/src/mp/mp_savefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "port/mp_proto.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    std::vector<uint8_t> orig(MP_SAVE_MAX_FILE + 16);
    uint32_t n = 0;
    if (mp_save_read_file(src, orig.data(), (uint32_t)orig.size(), &n) != 0) { fprintf(stderr, "cannot read %s\n", src); return 2; }
    orig.resize(n);
    char tmpl[] = "/tmp/s8_prune.XXXXXX";
    const std::string dir = std::string(mkdtemp(tmpl)) + "/recovery";
    MpSaveSnapshotView v;
    CHECK(mp_save_view_slot(orig.data(), 2, &v));
    uint8_t rec[MP_SAVE_REC_MAX];
    for (int s = 1; s <= 5; s++) {
        MpSaveRecoveryInfo i;
        memset(&i, 0, sizeof i);
        i.sessionId = 0xC0FFEE + s; i.seq = (uint32_t)s; i.unixTime = 1762600000 + s; i.flags = MP_SAVE_REC_FLAG_MARIO; i.slot = 2;
        i.scenario = 3; i.stars = 40 + (uint8_t)s;
        snprintf(i.galaxy, sizeof i.galaxy, "EggStarGalaxy");
        const uint32_t l = mp_save_recovery_pack(&i, &v, rec, sizeof rec);
        CHECK(mp_save_recovery_write(dir.c_str(), s, rec, l, MP_SAVE_RECOVERY_KEEP));   // 1..3 are crashes; 4 and 5 are later sessions
    }
    // nobody answered 1 and 2: they must still be somewhere the player can get them back (live, or set aside in discarded/)
    for (int s = 1; s <= 2; s++) {
        char name[64];
        snprintf(name, sizeof name, "/session-%08d.bin", s);
        const bool live = exists(dir + name), aside = exists(dir + "/discarded" + name);
        if (!live && !aside) fprintf(stderr, "S8: unresolved crash recovery %d was deleted by the keep-%d prune\n", s, MP_SAVE_RECOVERY_KEEP);
        CHECK(live || aside);
    }
    // the live folder still holds at most KEEP files (the disk bound the prune exists for)
    int idx[16];
    CHECK(mp_save_recovery_list(dir.c_str(), idx, 16) <= MP_SAVE_RECOVERY_KEEP);
    // the player's own "No" still prunes discarded/ (declined files are not progress)
    for (int s = 6; s <= 20; s++) {
        MpSaveRecoveryInfo i;
        memset(&i, 0, sizeof i);
        i.sessionId = 1; i.seq = (uint32_t)s; i.unixTime = 1762600100 + s; i.flags = MP_SAVE_REC_FLAG_MARIO; i.slot = 2; i.scenario = 3; i.stars = 41;
        snprintf(i.galaxy, sizeof i.galaxy, "EggStarGalaxy");
        const uint32_t l = mp_save_recovery_pack(&i, &v, rec, sizeof rec);
        CHECK(mp_save_recovery_write(dir.c_str(), s, rec, l, 0));
        CHECK(mp_save_recovery_discard(dir.c_str(), s, 10));
    }
    int cnt = 0;
    for (int s = 1; s <= 20; s++) { char nm[64]; snprintf(nm, sizeof nm, "/discarded/session-%08d.bin", s); cnt += exists(dir + nm); }
    CHECK(cnt <= 10);
    std::string rm = "rm -rf '" + dir.substr(0, dir.size() - 9) + "'";
    if (system(rm.c_str())) {}
    printf("s8_recovery_prune_test: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
