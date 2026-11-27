// Galaxy 3.0 multiplayer (core2): "one door" gathering, for the menu (flow). Plain C API, game thread, never blocks, safe at any
// time. DESIGN.md section 10.6. A host-test fake with the same calls: tests/mp/mp_gather_fake.cpp (+ mp_gather_fake.h to script it).
//
// mp_gather_start: this console becomes findable AND looks for others at once (listens 1.5 s, joins a gathering room it hears, else
// opens its own not-started room and keeps looking). Rooms that see each other merge by themselves into one (more players wins,
// then the lower room id); the members of a closing room follow it. The leader (the room's host) starts the game with
// mp_gather_begin. A room that has started is never merged; consoles nearby are offered it (mp_gather_playing_rooms).
#ifndef PORT_MP_GATHER_H
#define PORT_MP_GATHER_H

#include "port/mp_bridge.h"
#include "port/mp_direct_code.h"

enum MpGatherState : uint8_t {
    MP_GATHER_OFF = 0,
    MP_GATHER_LISTENING = 1,   // the first 1.5 s: looking before opening a room (show "Looking for players...")
    MP_GATHER_GATHERING = 2,   // in a room (alone or with others); heads[] is the room
    MP_GATHER_MERGING = 3,     // this room is closing into another one / joining it: keep the heads, no Start prompt
    MP_GATHER_STARTED = 4,     // the leader pressed Start: the game takes over (mp_active)
    MP_GATHER_FAILED = 5,      // could not open the transport (mp_gather_last_error / mp_ui_status().error say why)
};
enum MpGatherHeadFlags : uint8_t {
    MP_HEAD_SELF = 1,          // this console
    MP_HEAD_LEADER = 2,        // the room's leader (its host)
    MP_HEAD_ARRIVING = 4,      // still on its way in (it was in the closing room / joining): draw it, maybe dimmed
};
#define MP_GATHER_HEADS 4
struct MpGatherHead {
    uint32_t id;                         // stable per CONSOLE for the whole gathering (survives merges and slot changes): key the
                                         // head's animation on this, never on the slot
    char name[MP_NAME_BYTES + 1];
    uint8_t look;                        // MpLook
    uint8_t flags;                       // MpGatherHeadFlags
};
struct MpGatherStatus {
    MpGatherState state;
    MpConn conn;                         // MP_CONN_WIFI or MP_CONN_NEARBY (MP_CONN_DIRECT once a code was issued)
    bool wifiConnected;                  // the console is connected to a Wi-Fi access point now
    bool nearbyPossible;                 // Nearby (ad hoc) can be used on this console / build
    bool canStart;                       // show "Press Start": this console is the leader, the room is settled (no merge in progress,
                                         // no other gathering room heard for 1.5 s): never true on two consoles that can see each other
    uint8_t headCount;
    MpGatherHead heads[MP_GATHER_HEADS]; // self first, then the others in a stable order (by id)
    uint32_t heard;                      // datagrams of this game heard since the start (0 = nothing at all: "nobody nearby")
    uint8_t roomsSeen;                   // other rooms of this game heard now (gathering or playing, any mode)
    uint8_t playingRooms;                // of those, started rooms this console may join (mp_gather_playing_rooms)
    uint8_t fullRooms;                   // lobby: of those, compatible rooms with no free seat (the beacons the gathering never offers)
    uint8_t otherVersionRooms;           // lobby: of those, rooms of another version of the game (never joinable)
    char code[MP_DIRECT_CODE_MAX];       // the room's spoken code once mp_gather_get_code issued one, else ""
    // code5: the code is 5 characters (the online service holds it) and fits one row. codeLong: it is the long no-service code, shown only
    // because the service could not give a short one right now (the line says so); inviteError: why a code JOIN failed (MpInviteError of
    // port/mp_invite.h: wrong, expired, full, version, busy, ...), 0 when it was not the lookup
    bool codeLong;
    uint8_t inviteError;
};

extern "C" {
bool mp_gather_start(MpMode mode, MpConn conn);   // MP_CONN_NONE = auto: Wi-Fi when connected, else Nearby. False: not possible now
void mp_gather_stop(void);                        // leave (as mp_ui_leave)
// mpwifi44: why the last mp_gather_start answered false, or why the gathering is MP_GATHER_FAILED: the MpUiError to show (0 = nothing failed;
// MP_ERR_NO_WIFI only when the console is not connected to an access point) and, in *reason (may be null), a short word for the log
// ("link-wifi-down", "link-open-failed", "core-busy", ...; never null). The menu refers to it weakly: a test fake may leave it out.
int mp_gather_last_error(const char** reason);
bool mp_gather_switch(MpConn conn);               // Wi-Fi <-> Nearby: gathering restarts there (the heads go, they come back)
void mp_gather_status(MpGatherStatus* out);
int mp_gather_playing_rooms(MpLocalRoom* out, int max);   // started rooms in range ("X is playing: join?")
bool mp_gather_join_playing(uint32_t roomId);
bool mp_gather_begin(void);                       // the leader's Start; false unless status.canStart
// Far away without the service: the leader asks for a code; the room becomes a Play by Address room and its members follow.
// False when not the leader, already started, or the address cannot be found (no Wi-Fi).
bool mp_gather_get_code(char* out, size_t cap);
// May the leader ask for a code now (no side effect): the leader of a not-started Same Wi-Fi / Nearby gathering room, in a build that
// can play by address. (core2) get_code's first call turns the room into a Play by Address room (the members follow it); it returns true
// with the code once the address is known (a few seconds: call again, or read MpGatherStatus.code).
bool mp_gather_code_available(void);
// The friend's side: MpCodeResult (MP_CODE_OK = joining started; anything else: "That code didn't work. Please check it.").
// code5: five characters = an invite: the lookup at the service is part of the joining (status shows MERGING; FAILED with status.inviteError
// when it does not work); 15 / 19 = the long code, as before.
int mp_ui_join_direct_code(const char* code);
}

#endif
