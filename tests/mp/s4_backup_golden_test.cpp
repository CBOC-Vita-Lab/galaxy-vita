// savesus scenario 4: "Play on my file" sessions back the single-player file up first (kBackupsKept = 10). After ten more sessions the
// backup made before the FIRST session (the player's own, un-played file) is rotated away: every later backup already holds what earlier
// sessions did to the file. Rule under test: the oldest backup is never rotated out (the folder stays bounded at `keep` files).
#include "../../platform/src/mp/mp_savefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    const char* src = "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    std::vector<uint8_t> orig(MP_SAVE_MAX_FILE + 16);
    uint32_t n = 0;
    if (mp_save_read_file(src, orig.data(), (uint32_t)orig.size(), &n) != 0) return 2;
    orig.resize(n);
    char tmpl[] = "/tmp/s4_golden.XXXXXX";
    const std::string dir = mkdtemp(tmpl), sp = dir + "/GameData.bin", bk = dir + "/backups";
    char first[600], out[600];
    std::vector<uint8_t> cur = orig;
    for (int i = 0; i < 15; i++) {   // fifteen sessions; each changes the file (a guest's star, the host's own play) before the next backup
        FILE* f = fopen(sp.c_str(), "wb"); fwrite(cur.data(), 1, cur.size(), f); fclose(f);
        CHECK(mp_save_backup(sp.c_str(), bk.c_str(), 1762600000 + i, 10, out, sizeof out));
        if (i == 0) snprintf(first, sizeof first, "%s", out);
        // (the next state differs: one more star in slot 1's data is enough, finalized so it stays a valid file)
        if (i < 14) { mp_save_copy_slot(orig.data(), (i % 6) + 1, cur.data(), ((i + 1) % 6) + 1); mp_save_finalize(cur.data(), (uint32_t)cur.size()); }
    }
    struct stat st;
    const bool golden = stat(first, &st) == 0;
    if (!golden) fprintf(stderr, "S4: the backup made before the first session (%s) was rotated away\n", first);
    CHECK(golden);
    MpSaveBackupInfo list[64];
    CHECK(mp_save_backup_list(bk.c_str(), list, 64) <= 10);
    CHECK(stat(out, &st) == 0);   // the newest is there
    std::string c = "rm -rf '" + dir + "'";
    if (system(c.c_str())) {}
    printf("s4_backup_golden_test: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
