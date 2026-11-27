// Galaxy 3.0 multiplayer: the puppet demo's switch and mock roster (owner: demo). See port/mp_demo.h.
#include "port/mp_demo.h"

#include <stdlib.h>
#include <string.h>

#ifndef MP_MAX_PLAYERS
#define MP_MAX_PLAYERS 4
#endif
#ifndef MP_NAME_BYTES
#define MP_NAME_BYTES 16
#endif

bool g_mpDemoRoster = false;

namespace {
    struct Slot {
        bool present;
        int look;
        char name[24];
        int ping;
        bool quiet;
    };
    Slot sSlot[MP_MAX_PLAYERS];
    int sLocal = -1;
    bool sHost = false;
    int sMode = -1;
    int sSecs = 0;

    bool valid(int s) { return s >= 0 && s < MP_MAX_PLAYERS; }
}  // namespace

extern "C" {

void mp_demo_parse(const char* mode, const char* secs, int* outMode, int* outSecs) {
    int m = MP_DEMO_OFF;
    if (mode != nullptr && *mode != '\0') {
        if (strcmp(mode, "1") == 0 || strcmp(mode, "puppets") == 0) m = MP_DEMO_PUPPETS;
        else if (strcmp(mode, "menus") == 0) m = MP_DEMO_MENUS;
    }
    int v = (secs != nullptr && *secs != '\0') ? atoi(secs) : 60;
    if (outMode != nullptr) *outMode = m;
    if (outSecs != nullptr) *outSecs = v <= 0 ? 60 : (v < 20 ? 20 : (v > 600 ? 600 : v));
}

int mp_demo_mode(void) {
    if (sMode < 0) {
        mp_demo_parse(getenv("SMG_MP_DEMO"), getenv("SMG_MP_DEMO_SECS"), &sMode, &sSecs);
    }
    return sMode;
}

int mp_demo_secs(void) {
    (void)mp_demo_mode();
    return sSecs;
}

void mp_demo_roster_enable(bool on) {
    g_mpDemoRoster = on;
    if (!on) {
        memset(sSlot, 0, sizeof sSlot);
        sLocal = -1;
        sHost = false;
    }
}

void mp_demo_roster_local(int slot) { sLocal = valid(slot) ? slot : -1; }
void mp_demo_roster_host(bool localIsHost) { sHost = localIsHost; }

void mp_demo_roster_set(int slot, bool present, int look, const char* name, int pingMs, bool quiet) {
    if (!valid(slot)) return;
    Slot& s = sSlot[slot];
    s.present = present;
    s.look = look < 0 ? 0 : look;
    s.name[0] = '\0';
    if (name != nullptr) {
        strncpy(s.name, name, sizeof s.name - 1);
        s.name[MP_NAME_BYTES < (int)sizeof s.name - 1 ? MP_NAME_BYTES : (int)sizeof s.name - 1] = '\0';
    }
    s.ping = pingMs;
    s.quiet = quiet;
}

bool mp_demo_roster_present(int slot) { return valid(slot) && sSlot[slot].present; }
int mp_demo_roster_look(int slot) { return valid(slot) ? sSlot[slot].look : 0; }
const char* mp_demo_roster_name(int slot) { return valid(slot) ? sSlot[slot].name : ""; }
int mp_demo_roster_ping(int slot) { return valid(slot) && sSlot[slot].present ? sSlot[slot].ping : -1; }
bool mp_demo_roster_quiet(int slot) { return valid(slot) && sSlot[slot].quiet; }
int mp_demo_roster_local_slot(void) { return sLocal; }
bool mp_demo_roster_is_host(void) { return sHost; }

}  // extern "C"
