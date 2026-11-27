// Galaxy 3.0 multiplayer: the online service client (accounts, friends, star-time boards) on top of the vendored Strikers HTTPS
// stack (np_https) and JSON reader (np_json). Implements the mp_ui_profile / mp_ui_register / mp_ui_friends_* / mp_ui_friend_* /
// mp_ui_board_* / mp_race_submit functions of port/mp_bridge.h; this header adds what other port modules need around them.
//
// Never blocks: each call starts at most one HTTPS request (it runs on np_https's worker thread) and returns. Answers are collected by
// mp_service_pump(), which every function here also calls first. The service is the GAME=smg deployment (server/SMG-PROTOCOL.txt).
//
// Service URL: env SMG_ONLINE_URL (desktop tests: http://127.0.0.1:PORT of the fake server; http:// is refused on the Vita; env.txt on the Vita),
// else the `server = https://...` key of mp.cfg (a community's own deployment, see mp_config.h), else the build's NP_ONLINE_URL define (default https://galaxy.psvita.dev; no fallback to the old workers.dev address), else
// online is off (every call fails with MP_ERR_SERVICE_DOWN). The account belongs to one address: online.dat for the build's own, online_<hash>.dat
// for any other, so a token is never sent to another server.
// Account: <data>/online/online.dat (ux0:data/smg/online on the Vita; env SMG_ONLINE_DIR overrides): magic "SMGO", CRC32, atomic
// write. Never in GameData.bin.
#ifndef PLATFORM_MP_SERVICE_H
#define PLATFORM_MP_SERVICE_H

#include "port/mp_bridge.h"

extern "C" {

// Collect finished requests and start queued ones (time submissions). Cheap; core may call it once per frame so answers are
// collected (and np_https slots freed) even while no menu asks. Game thread.
void mp_service_pump(void);

bool mp_service_enabled(void);                   // a service URL is configured
MpUiError mp_service_last_error(void);           // the error of the last failed call (MP_ERR_NONE if none)
const char* mp_service_last_code(void);          // the service's error code of that failure ("name_taken", "player_not_found"), or ""
                                                 // (fix3: cleared when the next request starts; also the online link's failures)
// fix3: the online link (mp_conn_online.cpp, NpNet's signaling) failed with this service code ("banned", "version_mismatch",
// "not_member", ...; "" = none): it becomes mp_service_last_code() for the menus' wording.
void mp_service_note_code(const char* code);
// service review: SIGN-UP WITHOUT A SCREEN. Every menu entry that needs the service (Play with Anyone, Friends, a room code, the boards)
// calls this, every frame if it likes, and shows only "Connecting..." while it answers 0:
//    1 ready: an account the service knows (made now, quietly, with the profile's name, when there was none)
//    0 working (the first time: a challenge, a proof of work of well under a second on a Vita, the registration)
//   <0 = -(MpUiError): not possible now; mp_service_last_code() is one of
//        "busy"          the service is full, at a daily limit, switched off, overloaded or unreachable: "Online play is busy. Please try again later."
//        "no_wifi"       no network connection (MP_ERR_NO_WIFI)
//        "clock"         MP_ERR_CLOCK: the console's date and time are wrong (TLS certificates cannot be checked): "Please set the date and time in the system settings."
//        "update"        this game version is too old for the service (MP_ERR_UPDATE_REQUIRED)
//        "offline_build" this build has no service address
//      After a failure the next attempt waits (10 s doubling to 5 min, with jitter): calling again sooner returns the same failure.
// When the service stops knowing the account (it expired after a year unseen, or was removed) the next call registers a new one (a new
// friend code; friends lists do not carry over), at most twice per boot. Idempotent; game thread; never blocks.
int mp_service_ensure_account(void);
// The profile's display name for the service (the console's user name by default; the profile setting when changed). An account that
// exists follows a change quietly at the next mp_service_ensure_account (a name the service refuses keeps the old one there; nothing is
// shown). Without this call the name of mp.cfg (mp_config()->name) is used. Characters the service does not take are dropped; a name
// with nothing left becomes "Player".
void mp_service_set_name(const char* nameUtf8);
int mp_service_register_state(void);             // 0 never/idle, -1 running, 1 done (registered), -2 failed (see last_error)
int mp_service_friend_op_state(void);            // the last friend add/remove/dismiss/block/unblock: 0 none, -1 running, 1 done, -2 failed
// Delete My Online Account (DELETE /v1/me): _delete starts it (false: no account, one running, no service); _state 0 none, -1 running, 1 done (this console's account file and
// cached friend lists are gone too), -2 failed (nothing changed here). port/mp_core_game.h has the weak declarations the menus use.
bool mp_service_account_delete(void);
int mp_service_account_delete_state(void);

// MP3: incoming friend requests (players who added us, not yet our friends). Same completion model as mp_ui_friend_add / _remove
// (poll mp_service_friend_op_state; the list is fetched again after a success). Not in the contract header port/mp_bridge.h yet.
int mp_ui_incoming_get(MpFriend* rows, int max); // from the last mp_ui_friends_request: -1 loading, -2 error, else count (name, code only)
bool mp_ui_friend_dismiss(const char* code);     // drop one request (they may ask again)
bool mp_ui_friend_block(const char* code);       // block: no further requests, no presence; ends an existing friendship
bool mp_ui_friend_unblock(const char* code);

// For the room signaling (agent net): the service base URL and the "Authorization: Bearer <token>" value (token only). False when
// online is off or no account exists. The client must also send "X-Msc-Game: smg" and "X-Msc-App: <MP_SERVICE_APP_VERSION>".
bool mp_service_credentials(char* urlOut, int urlCap, char* tokenOut, int tokenCap);
const char* mp_service_app_version(void);        // "3.0.0" (SMG_APP_VERSION define)

// integ: the session fingerprint (MpCore) as the X-Msc-Build tag of every request and of the online transport's signaling.
void mp_service_set_build(uint32_t fingerprint);
// integ: MpCore's online hooks (declared weak in mp_transport.h): sign-in (configures the online transport with the account),
// quick match / friend room lookups, room announcements. Thread-safe (game thread and the session's helper thread).
int mp_svc_signin_poll(void);
bool mp_svc_room_lookup_begin(int kind, uint8_t mode, const char* friendCode);
int mp_svc_room_lookup_poll(char* codeOut, int cap);
void mp_svc_room_lookup_cancel(void);
void mp_svc_room_hosted(const char* code, const MpSettings* settings, uint8_t visibility, int players);
void mp_svc_room_closed(const char* code);

// MP3 conntest: the Connection Test's info call (GET /v1/info, no account needed). begin: starts it with its own timeout (false: cannot
// start / no service); poll: 0 pending, 1 finished (*out filled), -1 nothing running; cancel drops it. Game thread.
struct MpServiceInfo {
    int transport;        // 0 an HTTP answer arrived, 1 offline, 2 another transport failure, 3 the console clock is wrong (TLS dates)
    int status;           // HTTP status (0 when none)
    int ms;               // round trip as the caller saw it (the poll's granularity)
    bool valid;           // the answer is this service's (a JSON object with "proto"), or 426 (it wants a newer game)
    bool killed;          // the service says "all" or "rooms" is switched off
    bool needsNewer;      // the service's app.min / minProto is newer than this build (or 426)
    MpServiceInfo() : transport(2), status(0), ms(0), valid(false), killed(false), needsNewer(false) {}
};
bool mp_service_info_begin(uint32_t timeoutMs);
int mp_service_info_poll(MpServiceInfo* out);
void mp_service_info_cancel(void);

// beta diagnostics (mp_diag.cpp): one authenticated POST of the player's account (the problem report, the relay check's credentials). begin: false when
// there is no account / no service / cannot start. poll: 0 pending, 1 done (*transport: 0 an HTTP answer, 1 offline, 2 other failure, 3 the console's clock is wrong;
// *status the HTTP status; the body, cut to cap - 1, in out), -1 nothing running. cancel drops it. Game thread.
bool mp_service_post_begin(const char* path, const char* body, size_t len, uint32_t timeoutMs);
int mp_service_post_poll(int* transport, int* status, char* out, int cap);
void mp_service_post_cancel(void);

// keepfix (keepsafe review 10): the previous credentials SaveAccount kept for the account file of the current service address
// (index 0 = the newest: <file>.prev, then .prev.1, .prev.2). The player id and name only; the token never leaves the file. False when
// there is no such valid file.
bool mp_service_account_previous(int index, int* pid, char* name, int nameCap);

// Cancels every request and forgets the cached account (it is read from disk again on the next call). Tests, and shutdown.
void mp_service_shutdown(void);

}  // extern "C"

#endif  // PLATFORM_MP_SERVICE_H
