// Galaxy 3.0 multiplayer: the bridge between the game side (decomp/src/Game/MP, game headers) and the port side (platform/src/mp,
// no game headers). The port side implements every function here; the game side calls them. Plain types only.
// All functions are called on the game thread unless marked "any thread". All are cheap and safe when no session exists
// (mp_active() false): queries return neutral values and actions do nothing.
#ifndef PORT_MP_BRIDGE_H
#define PORT_MP_BRIDGE_H

#include "port/mp_proto.h"

extern "C" {

// ---- session state ---------------------------------------------------------------------------------------------------------
bool mp_active(void);                       // a session is running (connected, joined, playing). One global bool read.
int port_net_session_lockstep(void);                 // MP3 lodgate: mp_active(), or the session is only paused for a rejoin (Wi-Fi lost, sleep: up to 30 s) and this console still owes it
                                            // its world state. Use this, not mp_active(), for every per-console speed feature that changes game state. One global bool read.
MpMode mp_mode(void);
bool mp_is_host(void);
int mp_local_slot(void);                    // -1 when inactive. Slots never change during a session.
int mp_host_slot(void);                     // the current host's slot: 0 at the start; after a host migration the lowest
                                            // remaining slot (DESIGN.md "Host migration"). -1 when inactive.
uint16_t mp_host_epoch(void);               // increments at every host migration (0 at the start)
bool mp_slot_present(int slot);             // a player occupies this slot (includes the local one)
const char* mp_slot_name(int slot);         // UTF-8, never null ("" when empty)
MpLook mp_slot_look(int slot);
int mp_slot_ping_ms(int slot);              // -1 unknown
bool mp_slot_quiet(int slot);               // no packet for 3 s (show a lag icon)
const MpSettings* mp_settings(void);        // never null (defaults when inactive)
uint16_t mp_scene_serial(void);             // host's scene serial (guests: the last one the host announced)

// ---- game messages ---------------------------------------------------------------------------------------------------------
// Send a game message (type 0x20..0x5F). Guests send to the host; the host sends to every guest. The host relays guest messages
// to the other guests per mp_msg_host_relays(). Reliable messages arrive once, in order (per sender). Returns false when it
// could not be queued (inactive, too large, queue full).
bool mp_send(uint8_t type, const void* payload, uint16_t len, bool reliable);
// Host only: send to one guest.
bool mp_send_to(int slot, uint8_t type, const void* payload, uint16_t len, bool reliable);
// Dequeue the next received game message (oldest first). Returns false when none. Messages from a peer that left are dropped.
bool mp_poll(MpMsg* out);
uint32_t mp_poll_many(MpMsg* out, uint32_t max);   // netopt: up to `max` messages under the lightweight receive-queue lock
uint32_t mp_poll_states(MpMsg* out, uint32_t max); // g30 Wave 8: drain only MP_MSG_STATE pre-movement without touching other game messages
// Once per game logic turn (GameScene::update, also during loads from the system loop). Drives timers on the game thread.
void mp_tick_game(uint32_t turn);

// ---- save policy -----------------------------------------------------------------------------------------------------------
bool mp_save_suppressed(void);              // true for guests during a session: the game's own "continue without saving" path
bool mp_save_autoconfirm(void);             // true for the host during a session: save prompts confirm themselves, no A wait
// Host progress snapshot (the user-file blocks). Host: the game side provides it (on join, after each star).
void mp_snapshot_offer(const void* image, uint32_t len);
// Guest: a complete, CRC-checked snapshot is ready (returns its length, 0 = none). Copy it out; returns bytes copied.
uint32_t mp_snapshot_ready(void);
uint32_t mp_snapshot_get(void* out, uint32_t cap);
void mp_snapshot_consumed(void);

// ---- menus (MpMenuSequence calls these; all non-blocking; progress through mp_ui_status) -----------------------------------
enum MpUiPhase : uint8_t {
    MP_UI_IDLE = 0, MP_UI_SIGNING_IN, MP_UI_CREATING, MP_UI_SEARCHING, MP_UI_JOINING, MP_UI_CONNECTING, MP_UI_LOBBY,
    MP_UI_PLAYING, MP_UI_LEAVING, MP_UI_FAILED,
    MP_UI_RECONNECTING     // fix2: the link went (Wi-Fi drop, sleep): the core rejoins the same session on its own for up to 30 s
                           // (mp_active() is false meanwhile); then PLAYING / LOBBY again, or IDLE with an error when it gives up
};
enum MpUiError : uint16_t {
    MP_ERR_NONE = 0, MP_ERR_NO_WIFI, MP_ERR_SERVICE_DOWN, MP_ERR_UPDATE_REQUIRED, MP_ERR_ROOM_NOT_FOUND, MP_ERR_ROOM_FULL,
    MP_ERR_DIFFERENT_VERSION, MP_ERR_TIMEOUT, MP_ERR_HOST_LEFT, MP_ERR_KICKED, MP_ERR_NAT, MP_ERR_NAME_TAKEN, MP_ERR_BAD_NAME,
    MP_ERR_RATE_LIMITED, MP_ERR_INTERNAL,
    MP_ERR_CLOCK   // service review: the console's date and time are wrong (TLS certificate dates): "set the date and time" (appended: no renumbering)
};
struct MpUiStatus {
    MpUiPhase phase;
    MpUiError error;
    uint8_t players;                         // present players including us
    char roomCode[8];                        // "K7QM4D" (online) or "" (local)
    uint8_t progress;                        // 0..100 for long steps (snapshot transfer)
};
struct MpHostParams {
    MpConn conn;
    MpSettings settings;
    uint8_t visibility;                      // 0 code only, 1 friends, 2 public (online)
};
void mp_ui_set_profile(const char* nameUtf8, MpLook look);        // local name and look (saved in mp.cfg)
bool mp_ui_host(const MpHostParams* p);
bool mp_ui_join_code(MpConn conn, const char* code);               // online room code
bool mp_ui_join_friend(const char* friendCode);                    // online, friend's room
bool mp_ui_quick_match(MpMode mode);                               // online public room or a new one
bool mp_ui_join_local(MpConn conn, int listIndex);                 // Same Wi-Fi / Nearby: entry from mp_ui_local_list
bool mp_ui_start(void);                                            // host: lobby -> playing
void mp_ui_leave(void);                                            // any time; always ends in MP_UI_IDLE
void mp_ui_kick(int slot);                                         // host
void mp_ui_status(MpUiStatus* out);

struct MpLocalRoom {
    char host[MP_NAME_BYTES + 1]; uint8_t mode, players, maxPlayers; bool compatible;
    uint32_t id;                                                   // fix2: the room's identity (its session id, never 0)
};
int mp_ui_local_scan(MpConn conn);                                 // start/continue scanning; returns count found so far
bool mp_ui_local_get(int index, MpLocalRoom* out);
// fix2: join the room with this id (MpLocalRoom::id). The list's indexes shift as rooms expire: a press binds to the room the
// player saw, never to whichever room has its row now. False when that room is gone or of another version.
bool mp_ui_join_local_room(MpConn conn, uint32_t roomId);

// netx: internet play by address (no service). The host opens a room with MpHostParams.conn = MP_CONN_DIRECT; its lobby shows
// mp_ui_host_address(). A guest types that address: mp_ui_join_address("203.0.113.7") or ("203.0.113.7:41101"; port 41100 when
// omitted). False when the text is not a unicast IPv4 address (MpUiStatus.error = MP_ERR_ROOM_NOT_FOUND) or a session exists.
enum MpAddrSource : uint8_t { MP_ADDR_SRC_NONE = 0, MP_ADDR_SRC_LAN, MP_ADDR_SRC_STUN, MP_ADDR_SRC_ROUTER };
struct MpAddressInfo {
    char addr[24];       // "203.0.113.7:41100": what the others type; "" until known (a few seconds)
    uint8_t source;      // MpAddrSource: ROUTER = UPnP opened the port; STUN = public address, the port must be forwarded by hand;
                         // LAN = only players on the same network can join
    uint8_t portmap;     // 0 off, 1 asking the router, 2 router port opened, 3 the router did not (no UPnP / refused)
    bool cgnat;          // the router is itself behind another NAT (carrier NAT): not reachable from the internet
    uint16_t port;       // this console's UDP port (the one to forward by hand)
};
bool mp_ui_join_address(const char* textUtf8);
bool mp_ui_host_address(MpAddressInfo* out);                      // false when this console does not host by address

// Service lists (online). Each "request" starts an HTTPS call; "get" returns rows once ready (false while loading or on error).
struct MpFriend { char name[MP_NAME_BYTES + 1]; char code[10]; bool online, joinable; uint8_t mode; };
struct MpBoardRow { uint32_t rank; char name[MP_NAME_BYTES + 1]; uint32_t frames; uint8_t players; bool verified, you; };
struct MpProfile { char name[MP_NAME_BYTES + 1]; char friendCode[10]; bool registered; };
void mp_ui_profile(MpProfile* out);
bool mp_ui_register(const char* nameUtf8);                         // online account (first use)
bool mp_ui_friends_request(void);
int mp_ui_friends_get(MpFriend* rows, int max);                    // -1 loading, -2 error, else count
bool mp_ui_friend_add(const char* code);
bool mp_ui_friend_remove(const char* code);
// MP3: incoming friend requests (players who added us; they become friends when we add them back). Same completion as add / remove.
int mp_ui_incoming_get(MpFriend* rows, int max);                   // of the last mp_ui_friends_request: -1 loading, -2 error, else count (name, code)
bool mp_ui_friend_dismiss(const char* code);                       // "Not Now": drop one request (they may ask again)
bool mp_ui_friend_block(const char* code);                         // no more requests from them; ends a friendship
bool mp_ui_friend_unblock(const char* code);
bool mp_ui_board_request(uint8_t galaxyIndex, uint8_t scenario, uint8_t which /*0 top, 1 near me, 2 friends*/);
int mp_ui_board_get(MpBoardRow* rows, int max);                    // -1 loading, -2 error, else count
// Star Race: submit a finished time (frames at 60/s). Fire and forget; the result shows up in the board.
void mp_race_submit(uint8_t galaxyIndex, uint8_t scenario, uint32_t frames, const uint8_t* poseDigest16);

// ---- input (keyboard dialog) -------------------------------------------------------------------------------------------------
bool mp_keyboard_open(const char* titleUtf8, const char* initialUtf8, int maxChars, bool codeOnly);
int mp_keyboard_poll(char* outUtf8, int cap);                      // 0 running, 1 done (text in out), -1 cancelled
bool mp_dialog_active(void);                                       // any system dialog up: hold game input
bool mp_port_quit(unsigned budgetMs);                               // merge fix 5 (platform/src/mp/mp_boot.cpp): the app is being closed: BYE + wait for a save write, at most budgetMs (<= 300)
int mp_port_entered(void);                                          // mp_entered() as a symbol (platform/src/mp/mp_boot.cpp): the renderer library references it weakly

// ---- logging ---------------------------------------------------------------------------------------------------------------
// "[mp] ..." lines in the port log (ux0:data/smg/log.txt / desktop game.log). Tests assert on these.
void mp_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // extern "C"

#ifdef __cplusplus
// MP3 netpack: "multiplayer was entered in this boot". False until the player opens the Multiplayer menu (or a session / scan / auto
// start begins); set once, never cleared. The per-frame hooks of the game and UI test it FIRST (one inline bool read, no call), so
// single player that never opened multiplayer pays nothing for them and behaves exactly as without them.
// optfix: mp_entered() is also CLEARED again (mp_retire_entered) when the session is over and nothing of multiplayer is on screen, so the
// rest of the boot runs the same early-outs as a boot that never entered. mp_ever_entered() is the sticky one for questions about the whole
// boot (the goodbye and the save-write wait at quit, the renderer's warm-up list), never cleared.
inline bool g_mp_entered = false;
inline bool g_mp_ever_entered = false;
inline bool mp_entered() { return __atomic_load_n(&g_mp_entered, __ATOMIC_RELAXED); }
inline bool mp_ever_entered() { return __atomic_load_n(&g_mp_ever_entered, __ATOMIC_RELAXED); }
inline void mp_mark_entered() {
    if (!mp_ever_entered()) __atomic_store_n(&g_mp_ever_entered, true, __ATOMIC_RELAXED);
    if (!mp_entered()) __atomic_store_n(&g_mp_entered, true, __ATOMIC_RELAXED);
}
inline void mp_retire_entered() { if (mp_entered()) __atomic_store_n(&g_mp_entered, false, __ATOMIC_RELAXED); }
#endif

#endif  // PORT_MP_BRIDGE_H
