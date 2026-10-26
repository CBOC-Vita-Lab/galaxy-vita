// Brings up the emulated Wii environment and starts the game's main().
#include "port/port.h"

extern "C" void port_os_time_init(void);
extern "C" void port_gx_init(void);
extern "C" void port_gx_select_backend(void);
extern "C" void port_dvd_set_root(const char* root);
extern "C" void port_dvd_load_dol_data(const char* root);
extern "C" void port_nand_set_root(const char* root);
extern "C" void port_zoom_set_root(const char* saveRoot);  // perf.cpp: where the camera zoom setting is kept
extern "C" int port_boot_data_report(const char* message);

// GameSystem.cpp's main(), renamed for the port.
void port_game_main(void);

static void gameEntry() { port_game_main(); }

extern "C" void port_boot(const char* dataRoot, const char* saveRoot) {
    port_log("port_boot: data=%s save=%s", dataRoot, saveRoot);
    port_mem_init();
    port_os_time_init();
    port_dvd_set_root(dataRoot);
    const PortDisc* disc = port_dvd_disc();
    if (!disc) {
        if (port_boot_data_report("Game file list or language data is missing or unreadable. Restore converted game data and relaunch; saves unchanged."))
            return;  // The direct splash remains live; no guest was started.
        port_fatal("%s: not the files of a Super Mario Galaxy disc the port knows (no EuEnglish, UsEnglish, JpJapanese or KrKorean "
                   "folder with the game's texts in sys/fst.bin and files/)",
                   dataRoot);
    }
    port_log("disc: %s (%s), %d languages", disc->id, disc->region, port_language_count());
    port_mem_set_disc_id(disc->id);
    port_dvd_load_dol_data(dataRoot);
    port_nand_set_root(saveRoot);
    port_zoom_set_root(saveRoot);
    port_gx_init();
    port_gx_select_backend();
    port_os_boot(gameEntry);
}
