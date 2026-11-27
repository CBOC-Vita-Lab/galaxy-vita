// savesus S7: platform/src/mp/mp_config.cpp used `static thread_local char text[32*1024+1]` in ParseFile. On the Vita (emulated TLS, __emutls_get_address)
// every thread that touches it allocates 32 KB on first use, never freed. Built with -femulated-tls and --wrap=__emutls_get_address: the test
// drives mp_config from the game thread AND the helper thread (the port mapper's mp_config_record_upnp_permanent runs on the helper) and
// counts the emulated-TLS lookups made. Expected after the fix: none from mp_config (the buffer is one static, guarded by the settings lock).
#include "../../platform/src/mp/mp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <set>
#include <mutex>
#include <string>
#include <thread>

extern "C" void mp_log(const char*, ...) {}

static std::atomic<int> g_lookups{0};
static std::mutex g_mu;
static std::set<std::thread::id> g_threads;
extern "C" void* __real___emutls_get_address(void*);
extern "C" void* __wrap___emutls_get_address(void* c) {
    g_lookups++;
    { std::lock_guard<std::mutex> l(g_mu); g_threads.insert(std::this_thread::get_id()); }
    return __real___emutls_get_address(c);
}

int main() {
    char dirb[] = "/dev/shm/savesus_s7_XXXXXX";
    const std::string d = mkdtemp(dirb);
    mp_config_set_dir(d.c_str());
    FILE* f = fopen((d + "/mp.cfg").c_str(), "wb");
    fputs("version = 1\nname = Alex\nlook = 2\n", f);
    fclose(f);
    mp_config_load();                                         // game thread
    std::thread helper([] { mp_config_record_upnp_permanent(40001); });   // helper thread (save -> ParseFile verify)
    helper.join();
    std::thread t3([] { mp_config_load(); });                 // any other thread (keepsafe import reload, bot)
    t3.join();
    int fails = 0;
    if (g_lookups.load() != 0) {
        printf("FAIL S7: %d emulated-TLS lookups from %zu threads (each thread allocates its own 32 KB buffer on the Vita)\n", g_lookups.load(), g_threads.size());
        fails++;
    }
    std::string rm = "rm -rf '" + d + "'";
    if (system(rm.c_str()) != 0) {}
    printf("s7_tls_test: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
