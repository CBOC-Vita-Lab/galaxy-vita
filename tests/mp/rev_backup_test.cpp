// review (rev-save): the "refuses without a backup" promise of mp_save_writeback_file / saveOnLeave(SINGLE) / recoveryKeep(SINGLE)
// when the console clock is earlier than the newest backups (clock changed by the player, RTC reset, travel across a date
// setting). mp_save_backup writes the backup, then prunes "the oldest by name": the new backup's name sorts first and it is deleted
// at once; the call still reports success and the single-player file is then overwritten with no surviving copy of what it held.
//   g++ -std=c++20 -Iplatform/include -Iplatform/src/mp tests/mp/rev_backup_test.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp -lpthread
#include "../../platform/src/mp/mp_savefile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static int sFails = 0;
#define CHECK(c) do { if (!(c)) { sFails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using Bytes = std::vector<uint8_t>;
static Bytes readFile(const std::string& p) {
    Bytes b(MP_SAVE_MAX_FILE + 16);
    uint32_t n = 0;
    if (mp_save_read_file(p.c_str(), b.data(), (uint32_t)b.size(), &n) != 0) return {};
    b.resize(n);
    return b;
}
static void writeFile(const std::string& p, const Bytes& b) {
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

int main(int argc, char** argv) {
    const Bytes orig = readFile(argc > 1 ? argv[1] : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin");
    if (orig.empty()) return 2;
    char tmpl[] = "/tmp/rev_backup.XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string sp = dir + "/GameData.bin", mp = dir + "/coop.bin", backups = dir + "/backups";
    writeFile(sp, orig);
    Bytes coop = orig;
    mp_save_copy_slot(orig.data(), 1, coop.data(), 2);   // co-op slot 2 = an (almost) empty file
    mp_save_finalize(coop.data(), (uint32_t)coop.size());
    writeFile(mp, coop);
    // ten earlier backups made while the clock said 2026-11
    for (int i = 0; i < 10; i++) {
        char out[600];
        CHECK(mp_save_backup(sp.c_str(), backups.c_str(), 1762600000 + i, 10, out, sizeof out));
    }
    // the clock now says 2000-01-01 (Vita date reset / changed); the player says Yes to writing co-op file 2 over file 2
    char backup[600];
    const int rc = mp_save_writeback_file(mp.c_str(), 2, sp.c_str(), 2, backups.c_str(), 946684800, 10, backup, sizeof backup, nullptr);
    printf("writeback rc=%s backup=%s exists=%d\n", mp_save_op_name(rc), backup, exists(backup) ? 1 : 0);
    CHECK(rc == MP_SAVE_OP_OK);
    CHECK(readFile(sp) != orig);              // the single-player file was changed
    CHECK(exists(backup));                    // FAILS: the backup the call reported is already gone
    // and no other backup holds the pre-write state either? (here the older ones do, because the file never changed between them;
    // in real use the file changes between sessions and the older backups hold older progress)
    std::string c = "rm -rf '" + dir + "'";
    if (system(c.c_str()) != 0) {}
    printf("rev_backup_test: %d failures\n", sFails);
    return sFails ? 1 : 0;
}
