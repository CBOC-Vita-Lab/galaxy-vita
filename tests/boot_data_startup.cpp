// Actual port bootstrap, with explicit external boundaries; no game is launched.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../platform/src/port/boot.cpp"

namespace {
struct Fatal {};
std::vector<std::string> events;
bool discPresent = true, visible;
PortDisc disc{};
void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
}
extern "C" void port_log(const char*, ...) {}
extern "C" void port_fatal(const char*, ...) { events.emplace_back("fatal"); throw Fatal{}; }
extern "C" int port_mem_init() { events.emplace_back("memory"); return 1; }
extern "C" void port_os_time_init() { events.emplace_back("time"); }
extern "C" void port_dvd_set_root(const char*) { events.emplace_back("disc-root"); }
extern "C" const PortDisc* port_dvd_disc() { events.emplace_back("identify"); return discPresent ? &disc : nullptr; }
extern "C" int port_language_count() { return 1; }
extern "C" void port_mem_set_disc_id(const char*) { events.emplace_back("disc-id"); }
extern "C" void port_dvd_load_dol_data(const char*) { events.emplace_back("dol-data"); }
extern "C" void port_nand_set_root(const char*) { events.emplace_back("save-root"); }
extern "C" void port_zoom_set_root(const char*) { events.emplace_back("zoom-root"); }
extern "C" void port_gx_init() { events.emplace_back("graphics-init"); }
extern "C" void port_gx_select_backend() { events.emplace_back("graphics-backend"); }
extern "C" void port_os_boot(void (*entry)()) { require(entry != nullptr, "guest entry retained"); events.emplace_back("guest-start"); }
extern "C" int port_boot_data_report(const char* text) {
    require(std::string(text).find("saves unchanged") != std::string::npos, "visible error gives action and save boundary");
    events.emplace_back("report"); return visible;
}
void port_game_main() { std::abort(); }

int main() {
    const std::vector<std::string> normal = {"memory", "time", "disc-root", "identify", "disc-id", "dol-data", "save-root",
                                           "zoom-root", "graphics-init", "graphics-backend", "guest-start"};
    for (bool available : {false, true}) {
        visible = available; events.clear(); port_boot("synthetic-disc", "synthetic-saves");
        require(events == normal, "valid startup order is identical with and without error UI");
    }
    discPresent = false; visible = true; events.clear();
    port_boot("missing-disc", "synthetic-saves");
    require(events == std::vector<std::string>({"memory", "time", "disc-root", "identify", "report"}),
            "visible missing-disc error returns before saves/graphics/guest creation");
    visible = false; events.clear(); bool fatal = false;
    try { port_boot("missing-disc", "synthetic-saves"); } catch (const Fatal&) { fatal = true; }
    require(fatal && events == std::vector<std::string>({"memory", "time", "disc-root", "identify", "report", "fatal"}),
            "disabled or unavailable error UI retains explicit existing failure, never invisible parking");
    std::puts("PASS: actual bootstrap success order and visible/missing-UI failure routes");
}
