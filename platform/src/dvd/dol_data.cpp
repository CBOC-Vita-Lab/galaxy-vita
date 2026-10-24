// Data the game's executable (main.dol) holds rather than its files: the
// error message archive (the disc error screens' texts, font and window) and
// the StoryEvent and GalaxyID tables.  The port's executable is built from
// the decompilation, which leaves them out: tools/cook/cook.py takes them
// from the player's own main.dol into the converted sys/ folder, and
// port_boot loads them here, into the symbols the game's code names
// (ErrorArchive.hpp, BCSVInclude.s), before the game starts.
#include <stdio.h>

#include <string>

#include "port/heap_routing.h"
#include "port/port.h"

namespace {
const size_t kArchiveMax = 1 << 20;  // the converted archive is about 115 KB (PAL), 375 KB (Korean)
const size_t kTableMax = 1 << 14;
}  // namespace

extern "C" {
// Writable: JKR writes into the archives it mounts.
alignas(32) unsigned char cErrorArchive[kArchiveMax];
alignas(32) unsigned char StoryEventBCSV[kTableMax];
alignas(32) unsigned char GalaxyIDBCSV[kTableMax];
}

static void load(const std::string& root, const char* name, unsigned char* dst, size_t capacity) {
    std::string path = root + "/sys/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        port_fatal("%s is missing: convert the game's files again with tools/cook/cook.py, which now also takes data from the "
                   "disc's sys/main.dol",
                   path.c_str());
    }
    size_t size = fread(dst, 1, capacity, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (size == 0 || more) {
        port_fatal("%s: unexpected size", path.c_str());
    }
}

extern "C" void port_dvd_load_dol_data(const char* root) {
    PortHostAllocScope scope;
    load(root, "ErrorMessageArchive.arc", cErrorArchive, sizeof(cErrorArchive));
    load(root, "StoryEvent.bcsv", StoryEventBCSV, sizeof(StoryEventBCSV));
    load(root, "GalaxyID.bcsv", GalaxyIDBCSV, sizeof(GalaxyIDBCSV));
}
