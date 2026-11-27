// keepsafe audit: FAILING tests that show what the multiplayer settings and save layer do with files of another version or damaged ones.
// They document findings for the owner of mp_config.cpp / mp_savefile.cpp (see /home/claude/g3/share/keepsafe-requests.md). Expected today: the
// "FINDING" lines fail; when savefix has fixed a case its check passes. Exit status 1 while any finding is open (not part of allsuites).
//   tests/mp/keepsafe_findings.sh
#include "../../platform/src/mp/mp_config.h"
#include "../../platform/src/mp/mp_savefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>
static int open_ = 0;
#define FINDING(id, cond, what) do { if (!(cond)) { open_++; printf("FINDING %s OPEN: %s\n", id, what); } else printf("finding %s fixed: %s\n", id, what); } while (0)
static std::string rd(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); std::string s; if (!f) return "<missing>"; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
static void wr(const std::string& p, const std::string& s) { FILE* f = fopen(p.c_str(), "wb"); fwrite(s.data(), 1, s.size(), f); fclose(f); }
extern "C" void mp_log(const char*, ...) {}
int main() {
    char dirb[] = "/dev/shm/ks_findings_XXXXXX";
    std::string d = mkdtemp(dirb);
    mp_config_set_dir(d.c_str());
    // K1: mp.cfg from a newer build (a different first line layout, no "version = " key we know) and an empty .bak: both fail to load -> defaults,
    //     then the very next save (the console key is made at boot) replaces the file; the old bytes survive only as .bak until the next save.
    const std::string newer = "# written by 3.1\nformat: 2\nname: Alex\nlook: 3\nfuture = 1\n";
    wr(d + "/mp.cfg", newer);
    mp_config_load();
    uint8_t key[16];
    mp_config_console_key(key);   // what mp_core_boot does: makes a key and saves
    FINDING("K1 mp.cfg-unparseable-overwritten", rd(d + "/mp.cfg") == newer, "an mp.cfg that does not parse is replaced by defaults at the first save (mp_config.cpp:324 + :377)");
    // K2: a second save destroys the only copy of it
    mp_config_save();
    FINDING("K2 mp.cfg-bak-rotates", rd(d + "/mp.cfg.bak") == newer || rd(d + "/mp.cfg.unreadable") == newer, "the unreadable file is gone after two saves (.bak is overwritten; mp_config.cpp:377-378)");
    // K3: unknown keys of a newer build are dropped on write
    wr(d + "/mp.cfg", "version = 1\nname = Alex\nfuture_option = 7\n");
    mp_config_load();
    mp_config_save();
    FINDING("K3 mp.cfg-unknown-keys", rd(d + "/mp.cfg").find("future_option = 7") != std::string::npos, "an unknown key is dropped on write (mp_config.cpp:350)");
    // K4: version = 2 is read as if it were 1 and written back as version = 1
    wr(d + "/mp.cfg", "version = 2\nname = Alex\nlook = 9\n");
    mp_config_load();
    mp_config_save();
    FINDING("K4 mp.cfg-newer-version", rd(d + "/mp.cfg").find("version = 2") != std::string::npos, "a file with version = 2 is rewritten as version = 1 (mp_config.cpp:122, :350)");
    // S1: a recovery write prunes files it does not understand (a newer recovery format), oldest first
    std::string rec = d + "/recovery";
    mp_save_mkdirs(rec.c_str());
    char p[300];
    for (int i = 1; i <= 4; i++) { mp_save_recovery_path(rec.c_str(), i, p, sizeof p); wr(p, i == 1 ? "newer-format-recovery-file" : "x"); }
    // (pruneDir keeps the newest `keep` by number; we call it through the public writer with an invalid blob, which refuses, so use delete semantics)
    mp_save_recovery_path(rec.c_str(), 1, p, sizeof p);
    int idx[8];
    const int n = mp_save_recovery_list(rec.c_str(), idx, 8);
    printf("note S1 (read from the code, not demonstrated): mp_save_recovery_write(keep=3) prunes the oldest session files by number, whatever their format (mp_savefile.cpp:865-868); %d listed\n", n); (void)p;
    // S2: mp_save_recover_with replaces .mpbad on every call
    std::string gd = d + "/GameData.bin";
    wr(gd, "newer-format-1");
    wr(gd + ".mpbak", "x");
    bool ch = false;
    mp_save_recover_with(gd.c_str(), [](const uint8_t* b, uint32_t n) { return n == 1 && b[0] == 'x'; }, &ch);
    const std::string first = rd(gd + ".mpbad");
    wr(gd + ".mpbak", "x");
    wr(gd, "newer-format-2");
    mp_save_recover_with(gd.c_str(), [](const uint8_t* b, uint32_t n) { return n == 1 && b[0] == 'x'; }, &ch);
    FINDING("S2 mpbad-overwritten", rd(gd + ".mpbad") != "newer-format-2" || first == "newer-format-1" && rd(gd + ".mpbad.1") == "newer-format-1" || false, "a second recovery overwrites the first file kept aside as .mpbad: the earlier bytes are lost (mp_savefile.cpp:408-409, :571)");
    std::string cmd = "rm -rf " + d;
    if (system(cmd.c_str())) {}
    printf("%d findings open\n", open_);
    return open_ ? 1 : 0;
}
