// keepfix 3 (keepsafe review 3 #10): "Bring back settings" (mp.cfg from this console's own copy) while the network helper thread keeps
// recording a port mapping (each record a save of the settings in RAM, under the settings lock). The real mp_config.cpp and the real
// keepsafe: afterwards mp.cfg and the settings in RAM must be the ones brought back, in every round. tests/mp/config_lock_build.sh runs it
// under ThreadSanitizer; KS_CFG_ROUNDS (default 50).
#include "port/mp_keepsafe.h"
#include "../../platform/src/mp/mp_config.h"
#include "../../platform/src/mp/mp_savefile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

extern "C" void mp_log(const char*, ...) {}
extern "C" bool mp_ui_flow_replace_file(const char*, const char*) { return false; }   // (only mp.cfg is brought back here)

static std::string rd(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    std::string s;
    if (!f) return s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static void wr(const std::string& p, const std::string& s) {
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

int main() {
    const int rounds = getenv("KS_CFG_ROUNDS") ? atoi(getenv("KS_CFG_ROUNDS")) : 50;
    int fails = 0;
    for (int round = 0; round < rounds; round++) {
        char dirb[] = "/dev/shm/ks_cfgthread_XXXXXX";
        const std::string root = mkdtemp(dirb);
        const std::string data = root + "/data", mp = data + "/mp";
        mp_save_mkdirs(mp.c_str());
        MpKsPaths p;
        memset(&p, 0, sizeof p);
        snprintf(p.dataDir, sizeof p.dataDir, "%s", data.c_str());
        snprintf(p.mpDir, sizeof p.mpDir, "%s", mp.c_str());
        snprintf(p.onlineDir, sizeof p.onlineDir, "%s/online", data.c_str());
        snprintf(p.teamFile, sizeof p.teamFile, "%s/saves/title/00010000/524d4750/data/GameData.bin", mp.c_str());
        snprintf(p.recoveryDir, sizeof p.recoveryDir, "%s/recovery", mp.c_str());
        mp_keepsafe_set_paths(&p);
        mp_keepsafe_test_reset();
        mp_config_set_dir(mp.c_str());
        wr(mp + "/mp.cfg", "version = 1\nname = NewName\nlook = 2\n");
        mp_keepsafe_boot("3.0.0", "t");   // the automatic copy holds NewName
        MpKsCopy c[4];
        if (mp_keepsafe_list_copies(c, 4) != 1) { printf("FAIL round %d: no copy\n", round); fails++; continue; }
        wr(mp + "/mp.cfg", "version = 1\nname = OldName\nlook = 1\n");
        mp_config_load();
        mp_keepsafe_add_reload_hook([] { usleep(20 * 1000); });   // (the game's Team File reload: a card read, while the lock is held)
        std::atomic<bool> stop{false};
        std::thread helper([&] {
            for (uint16_t port = 40000; !stop; port = (uint16_t)(port == 40000 ? 40001 : 40000)) mp_config_record_upnp_permanent(port);
        });
        usleep(2000);
        char before[96];
        const int rc = mp_keepsafe_bring_back_settings(c[0].name, MP_KS_SET_CONFIG, before, sizeof before);
        usleep(20 * 1000);
        stop = true;
        helper.join();
        const std::string now = rd(mp + "/mp.cfg");
        if (rc != MP_KS_OK || now.find("name = NewName") == std::string::npos || strcmp(mp_config()->name, "NewName") != 0) {
            printf("FAIL round %d: %s, mp.cfg %s, settings in RAM %s\n", round, mp_keepsafe_result_name(rc), now.find("NewName") != std::string::npos ? "new" : "OLD",
                   mp_config()->name);
            fails++;
        }
        const std::string rm = "rm -rf '" + root + "'";
        if (system(rm.c_str()) != 0) {}
    }
    printf("keepsafe_cfg_test: %s (%d rounds)\n", fails ? "FAIL" : "ok", rounds);
    return fails ? 1 : 0;
}
