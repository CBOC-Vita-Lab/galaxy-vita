// Galaxy 3.0 multiplayer: the puppet demo's switch and its mock roster (owner: demo). See docs: share/heavy/PUPPET-DEMO.md.
//
// SMG_MP_DEMO=1       a ~60 s demo in the first stage that loads: canned remote players, a bubble, name plates, the party strip,
//                     another player's pointer, toasts and a card, then everything goes (decomp/src/Game/MP/MpDemo.cpp).
// SMG_MP_DEMO=menus   from the title screen: the multiplayer screens one after the other (entry screen, character select, the
//                     multiplayer menu, the gathering screen with four mock players, the Connection Test screen), then back to the title.
// SMG_MP_DEMO_SECS=N  length of the puppet demo in seconds (default 60, 20..600) and of the menu tour (default 60).
// Off (unset, empty, 0) by default and in every release default: nothing here is ever set by the game's own env defaults.
//
// The mock roster answers the bridge's roster queries (mp_slot_present / name / look / ping / quiet, mp_local_slot, mp_is_host,
// mp_scene_serial, maxPlayers of mp_settings) while it is on and no real session is active. No socket, no mp_core, no message.
#ifndef PORT_MP_DEMO_H
#define PORT_MP_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MP_DEMO_OFF = 0, MP_DEMO_PUPPETS = 1, MP_DEMO_MENUS = 2 };

// The pure parse of the two variables (host tested): mode text -> MP_DEMO_*, seconds text -> 20..600 (empty / 0 / negative: 60).
void mp_demo_parse(const char* mode, const char* secs, int* outMode, int* outSecs);
int mp_demo_mode(void);   // read once from SMG_MP_DEMO: MP_DEMO_*; anything else is off
int mp_demo_secs(void);   // SMG_MP_DEMO_SECS, default 60, clamped to 20..600

// The mock roster (game thread). Slots 0..3 (MP_MAX_PLAYERS).
void mp_demo_roster_enable(bool on);   // on: the bridge queries answer from the mock; off: forgets everything
void mp_demo_roster_local(int slot);   // the local slot (-1 none)
void mp_demo_roster_host(bool localIsHost);
void mp_demo_roster_set(int slot, bool present, int look, const char* name, int pingMs, bool quiet);
bool mp_demo_roster_present(int slot);
int mp_demo_roster_look(int slot);
const char* mp_demo_roster_name(int slot);
int mp_demo_roster_ping(int slot);
bool mp_demo_roster_quiet(int slot);
int mp_demo_roster_local_slot(void);
bool mp_demo_roster_is_host(void);

extern bool g_mpDemoRoster;   // mirrors mp_demo_roster_enable: the one flag the bridge tests first (false = nothing of the demo runs)

#ifdef __cplusplus
}
#endif

#endif  // PORT_MP_DEMO_H
