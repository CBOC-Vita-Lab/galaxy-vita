// keepfix (keepsafe review 14): mp_config_load and the helper thread's mp_config_save share s_cfg / s_raw / s_loadedFmt. Built with
// ThreadSanitizer by tests/mp/config_lock_build.sh: a data race between a load (keepsafe's reload after an import, game thread) and a save
// (the port mapper's mp_config_record_upnp_permanent, helper thread) makes TSan fail the run. Also checks mp_config_hold blocks a save.
#include "../../platform/src/mp/mp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

extern "C" void mp_log(const char*, ...) {}

int main() {
    char dirb[] = "/dev/shm/ks_cfglock_XXXXXX";
    const std::string d = mkdtemp(dirb);
    mp_config_set_dir(d.c_str());
    FILE* f = fopen((d + "/mp.cfg").c_str(), "wb");
    fputs("version = 1\nname = Alex\nlook = 2\nfuture_key = 7\n", f);
    fclose(f);
    mp_config_load();
    std::atomic<bool> stop{false};
    std::thread helper([&] {
        for (int i = 0; i < 300 && !stop; i++) mp_config_record_upnp_permanent((uint16_t)(40000 + (i % 50)));
    });
    for (int i = 0; i < 300; i++) mp_config_load();
    stop = true;
    helper.join();
    int fails = 0;
    // hold: a save from another thread waits until the hold is released
    mp_config_hold(true);
    std::atomic<bool> saved{false};
    std::thread t([&] { mp_config_record_upnp_permanent(41234); saved = true; });
    usleep(100 * 1000);
    if (saved) { printf("FAIL: a save ran while the settings were held\n"); fails++; }
    mp_config_hold(false);
    t.join();
    if (!saved) { printf("FAIL: the save never ran\n"); fails++; }
    std::string rm = "rm -rf '" + d + "'";
    if (system(rm.c_str()) != 0) {}
    printf("config_lock_test: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
