// Galaxy 3.0 multiplayer: the session layer's calls for the game side beyond port/mp_bridge.h (integ). Implemented by
// platform/src/mp/mp_core.cpp; used by the flow module (decomp/src/Game/MP/MpGame.cpp, MpFlow.cpp, MpRace.cpp). Plain types only.
#ifndef PORT_MP_CORE_GAME_H
#define PORT_MP_CORE_GAME_H

#include <stdint.h>

#include "port/mp_proto.h"   // MpSettings (bubble: the session menu's "Falls" row)

// Events for the game side (mp_core_poll_event).
enum MpCoreEventKind : uint8_t {
    MP_EV_NONE = 0,
    MP_EV_JOINED = 1,        // slot joined (or rejoined: other = 1)
    MP_EV_LEFT = 2,          // slot left; arg = MpByeReason (or 0 lost)
    MP_EV_HOST_CHANGED = 3,  // slot = new host, other = old host, epoch (on every console that follows the new host)
    MP_EV_BECAME_HOST = 4,   // this console is the new host: re-send WHERE (the core already sends the stored one), SWITCHMAP,
                             //   race state; other = old host, epoch
    MP_EV_RESUMED = 5,       // (new host) slot answered HOST_CHANGE: serial, finished, arg = turns. Send it what it needs.
    MP_EV_STARTED = 6,       // the host started (lobby -> playing)
    MP_EV_SNAPSHOT = 7,      // guest: a new progress snapshot is ready (mp_snapshot_ready)
    MP_EV_LOOK = 8,          // slot's character changed (mp_slot_look); other = the new look
};
struct MpCoreEvent {
    uint8_t kind, slot, other, finished;
    uint16_t epoch, serial;
    uint32_t arg;
};

extern "C" {
// Next session event (game thread). False when none.
bool mp_core_poll_event(MpCoreEvent* out);
// The game's current WHERE payload (galaxy u8, scenario u8, selected u8, startId s16, zone u8, serial u16; 8 bytes): a new host
// re-sends it after a migration. The core also takes it from every WHERE / MOVE the host sends or a guest receives.
void mp_core_set_where(const void* payload, uint16_t len);
// This console's race state, for RESUME after a host migration.
void mp_core_set_race_state(bool finished, uint32_t turns);
// The scene serial this console is in (host: authoritative; guests: from MOVE / WHERE). Older queued STATEs are dropped.
void mp_flow_set_scene_serial(uint16_t serial);
// core2: the friend code of the player in `slot` ("XXXX-XXXX" + NUL into out, cap >= 10): true only in a room joined through the
// online service, for another present player whose code this console's own signed-in room view from the service lists for that slot
// at that player's address (a guest cannot claim someone else's: no console sends codes; the service binds slot, address and account).
// False on LAN / ad hoc / by address, for this console's own slot, an empty slot, or before the service answered. Never log the code.
// Lock-free (the published view): cheap enough for a menu row, but meant for menus, not per frame.
bool mp_core_slot_friend_code(int slot, char* out, size_t cap);
// core2: the player in `slot` for the menus' "played with" list: name, look, friend code and whether the service vouches for it.
// codeVerified: the online service's view (as mp_core_slot_friend_code). Not verified: in a room without the service (Same Wi-Fi,
// Nearby, by address) a console with an online account announces its own code to the room (sealed, after the keys); nobody checks it:
// it lets the others send a friend request and nothing else (the request still needs the other side to accept). False for an empty
// slot, this console's own slot, or outside a session. Never log the code.
// core2: the game side (host) found a message from `slot` implausible or abusive (MpWorldGuard: rates, distances). It counts against
// that player like a malformed message: MP_BAD_KICK (50) in 10 s and the host removes it ("removed by the host"). Ignored on a guest,
// for an empty slot or this console's own. `why`: a short constant text for the log (logged once per window). Any thread.
void mp_core_report_bad(int slot, const char* why);
// sid (smallfix2): a stable id of this player for the session, from the rejoin token's commitment through a keyed, truncated hash: the same after a
// drop and rejoin, a name change or a slot change; different for two players with one name; never reveals the token. 0 = unknown (a peer
// that sent no commitment): key on the name then. Not a secret (never log it next to anything private) and not an account id.
struct MpPlayerInfo { char name[MP_NAME_BYTES + 1]; uint8_t look; char code[10]; bool codeVerified; uint32_t sid; };
bool mp_core_slot_player(int slot, MpPlayerInfo* out);
// core2: this console's own account code ("XXXX-XXXX"; "" or null: no account). The service layer calls it when the account is
// loaded, registered or dropped; any thread (it is handed over, the session announces it at its next pump).
void mp_core_set_own_friend_code(const char* code);
// The profile remembered in mp.cfg (ui: preselect the character / name). Never null; MP_LOOK_* value (MpLook).
const char* mp_core_saved_name(void);
uint8_t mp_core_saved_look(void);
// bubble: Play Together "Falls" (MpSettings.downed, MpDowned). The host's preference in mp.cfg (preselected when hosting) and
// saving a new choice there; the host's change during a session goes to everyone as SETTINGS (false: not the host / no session).
uint8_t mp_core_saved_downed(void);
void mp_core_save_downed(uint8_t downed);
// entry: the entry screen's last choice (Single Player 0 / Multiplayer 1), its default cursor next time (mp.cfg "entry"; 0 on first
// boot). Saving writes mp.cfg only when the value changes.
uint8_t mp_core_saved_entry(void);
void mp_core_save_entry(uint8_t choice);   // (menufix: the entry screen uses mp_entry_remember / mp_entry_flush below, from mp_config.cpp)
// menufix (review2 #11): remember the choice in RAM; persistNow writes mp.cfg at once (Multiplayer: the menu opens next), otherwise (Single
// Player) the write waits for mp_entry_flush at a harmless moment (the file select list is up) or any other save of mp.cfg. Neither can fail the
// caller: a write error is only logged. mp_entry_flush is one bool test when nothing is waiting.
void mp_entry_remember(uint8_t choice, bool persistNow);
void mp_entry_flush(void);
bool mp_core_update_settings(const MpSettings* s);
// touches: the host-rules card's memory in mp.cfg (config only). wanted: the card may be shown for this host (hostKey = a hash of the
// host's name, never 0): "don't show again" is off and this host's card was not shown yet. done: it was shown; never = the player
// chose "don't show again" (for every host).
bool mp_core_rules_wanted(uint32_t hostKey);
bool mp_core_rules_hidden(void);
void mp_core_rules_done(uint32_t hostKey, bool never);
// fix3: the online service's last failure, for the menus' own wording (platform/src/mp/mp_service.cpp; weak here, so tools that
// link the menus without the service still link: null = not there). The service's code ("banned", "friends_full", "not_friends",
// "player_not_found", "own_code", "not_member", "version_mismatch", ...; "" none; cleared when the next request starts, also set by
// the online link's failures), and the account / friend-add request states (0 none, -1 running, 1 done, -2 failed).
const char* mp_service_last_code(void) __attribute__((weak));
int mp_service_register_state(void) __attribute__((weak));
int mp_service_friend_op_state(void) __attribute__((weak));
// Delete My Online Account (Friends): the service removes the account and the friends list; when it has said yes the console's own account file and its
// cached friend list are removed too (nothing else). _delete starts it (false: refused, e.g. no account / one already running); _state: 0 none, -1 running,
// 1 done (the account is gone here too), -2 failed (nothing was changed here).
bool mp_service_account_delete(void) __attribute__((weak));
int mp_service_account_delete_state(void) __attribute__((weak));
}

#endif  // PORT_MP_CORE_GAME_H
