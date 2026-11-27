// Galaxy 3.0 multiplayer (flow, wave 2b): what the simplified menus need from the port beyond port/mp_bridge.h. Plain types only.
// Implemented by platform/src/mp/mp_ui_flow.cpp. Every function is declared weak, so tools and tests that link the menus without
// that file still link: the menus check for null and then behave as with no service, no rejoin, no stored data.
#ifndef PORT_MP_UI_FLOW_H
#define PORT_MP_UI_FLOW_H

// Weak for the callers (tools and tests that link the menus without the port still link). The units that DEFINE them define MP_UI_FLOW_DEFINES_STRONG before this header: their
// definitions are strong, so the ELF shows "T" for the real function (a weak definition shows "W", which the post-link check cannot tell from a stub).
#ifdef MP_UI_FLOW_DEFINES_STRONG
#define MP_UI_FLOW_WEAK
#else
#define MP_UI_FLOW_WEAK __attribute__((weak))
#endif

#include <stdint.h>

#include "port/mp_proto.h"   // MP_NAME_BYTES, MpLook

extern "C" {

// ---- what this console can reach right now (cheap; once a frame is fine) ----------------------------------------------------
struct MpNetInfo {
    bool wifiUp;       // connected to an access point with an address (desktop: always). Ad hoc (Nearby) and Wi-Fi rooms exclude
                       // each other on the Vita: the Join list scans Wi-Fi rooms when this is true, consoles nearby when false.
    bool service;      // an online service is configured (mp_service_enabled): Quick Match, friends' rooms, room codes, Leaderboards
    bool registered;   // the online account exists (no keyboard needed before an online room)
};
void mp_ui_net_info(MpNetInfo* out) MP_UI_FLOW_WEAK;

// ---- rejoin (mp_core_resume): back into the session this console dropped out of, within the core's window (120 s) ----------
// False when there is nothing to rejoin (left on purpose, kicked, too late) or a session exists; the menus then say so.
bool mp_ui_rejoin(void) MP_UI_FLOW_WEAK;

// ---- the friend code of a player in this room (Add Friend / Block on a player) -----------------------------------------------
// CORE REQUEST (netcore / service): the roster has names only. Until an online room member's friend code is known to the core,
// this answers false and the menus offer Add Friend / Block only where a code is known. out: 10 bytes ("1234-5678" + NUL).
bool mp_ui_slot_friend_code(int slot, char* out) MP_UI_FLOW_WEAK;

// ---- small things the menus remember (<data>/mp/flow.cfg; written atomically; never a save file) -----------------------------
#define MP_PLAYED_WITH 20
enum MpReach : uint8_t {   // "Who can join" (the host's last choice is the next room's)
    MP_REACH_LOCAL = 0,    // Same Wi-Fi (or Nearby with no Wi-Fi)
    MP_REACH_FRIENDS = 1,  // online, friends (and the room code)
    MP_REACH_ANYONE = 2,   // online, public (Quick Match finds it)
    MP_REACH_CODE = 3,     // online, room code only
    MP_REACH_ADDRESS = 4,  // internet by address (netx; no service)
    MP_REACH_COUNT = 5
};
struct MpFlowStore {
    uint8_t charPicked;    // the character select was shown once (first play): later the last character is taken at once
    uint8_t reach;         // MpReach
    uint8_t lastCoop;      // the co-op file the host played last (1..6, 0 none): the "Which file?" cursor
    uint8_t fallsAsked;    // flow (one door): the falls question was answered once (the answer itself: mp.cfg "downed")
    uint8_t playedCount;   // names in playedWith (newest first)
    char playedWith[MP_PLAYED_WITH][MP_NAME_BYTES + 1];   // names (UTF-8, cleaned), newest first
    char playedCode[MP_PLAYED_WITH][10];                  // flow: each one's friend code when it was known ("" none): Add Friend
};
bool mp_flow_store_get(MpFlowStore* out) MP_UI_FLOW_WEAK;       // false: none yet (defaults in out)
void mp_flow_store_set(const MpFlowStore* in) MP_UI_FLOW_WEAK;  // saves at once (rare: a pick, a new name, a host)

// ---- co-op file side data (<data>/mp/coop.cfg): who played on each co-op file and where, never inside the save itself ----------
struct MpCoopMeta {
    uint8_t count;                              // names (0 = none known: the menu says "Co-op File N")
    char names[4][MP_NAME_BYTES + 1];           // the players, the host first
    uint8_t looks[4];                           // their characters (MpLook): the row shows the host's head
    int8_t galaxy;                              // the last galaxy (MpMsgText galaxy index), -1 unknown
    uint8_t used;                               // a session played on it / progress was kept in it (review 2 #3: "in use", not "New")
};
bool mp_coop_meta_get(int slot, MpCoopMeta* out) MP_UI_FLOW_WEAK;        // slot 1..6; false: nothing stored
void mp_coop_meta_set(int slot, const MpCoopMeta* in) MP_UI_FLOW_WEAK;   // count 0 clears it

// MP3 keepfix: drop the cached flow.cfg / coop.cfg (keepsafe replaced them); the next read loads them from the card.
void mp_ui_flow_forget(void) MP_UI_FLOW_WEAK;
// MP3 keepfix 3: replace flow.cfg or coop.cfg (name) with text through this file's writer (keepsafe's "Bring back settings")
bool mp_ui_flow_replace_file(const char* name, const char* text) MP_UI_FLOW_WEAK;

}  // extern "C"

#endif  // PORT_MP_UI_FLOW_H
