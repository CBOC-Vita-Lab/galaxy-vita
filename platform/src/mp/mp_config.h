// Galaxy 3.0 multiplayer (port side, owner: core): mp.cfg, the player's multiplayer settings (DESIGN.md section 6).
// File: <data>/mp/mp.cfg = ux0:data/smg/mp/mp.cfg on the Vita; $PETARI_DATA_DIR/mp/mp.cfg (or ./mp/mp.cfg) on the desktop.
// Text "key = value" lines; unknown keys are ignored; written atomically (temporary file, verified, previous kept as .bak).
// Key `server` (see MP_SERVER_URL_MAX). Environment overrides for test processes: SMG_MP_NAME, SMG_MP_LOOK, SMG_MP_DOWNED (0 bubbles / 1 infinite lives).
#ifndef PORT_MP_CONFIG_H
#define PORT_MP_CONFIG_H

#include "port/mp_bridge.h"

// serverfix: the service address a player may set in mp.cfg (`server = https://my-galaxy.example.org`): the run-time override of the build's
// NP_ONLINE_URL, for a community that runs its own deployment of server/. HTTPS only, host[:port] only (no path, query, user info or
// spaces), at most 120 characters; anything else is ignored (the build's address stays). The same pinned TLS roots apply (np_https_roots.h).
// Accounts are per server: mp_service.cpp keeps a separate account file for each address, so a token is never sent to another host.
#define MP_SERVER_URL_MAX 120
bool mp_server_url_clean(const char* in, char* out, size_t cap);   // false: not acceptable (out = ""); true: out = normalised, no trailing slash

struct MpConfig {
    char name[MP_NAME_BYTES + 1];   // UTF-8, cleaned (mp_name_clean). nameTyped == 0: always the chosen character's name (follows `look`)
    uint8_t nameTyped;              // name_typed: 1 = the player typed (or set in mp.cfg) a name of their own: kept; 0 = the character's name
    uint8_t look;                   // MpLook
    uint8_t conn;                   // default connection (MpConn)
    uint8_t mode;                   // last mode hosted (MpMode)
    uint8_t maxPlayers;             // 2..4
    uint8_t interaction;            // 0 off, 1 friendly, 2 rough
    uint8_t sharedKills;            // 0/1
    uint8_t nameTags;               // 0 off, 1 on, 2 on + edge arrows
    uint8_t rivals;                 // 0 solid, 1 ghost, 2 hidden
    uint8_t raceWaitSec;            // / 10, 0 = no limit
    uint8_t visibility;             // 0 code only, 1 friends, 2 public
    uint8_t relayOnly;              // Wi-Fi privacy: online through the relay only
    uint8_t dropIn;                 // 0/1
    uint8_t powerSaveTipShown;      // the one-time Wi-Fi power-save tip (DESIGN.md section 9)
    uint8_t entry;                  // entry: the entry screen's last choice (its default cursor): 0 Single Player (default), 1 Multiplayer
    uint8_t downed;                 // Play Together falls: 0 bubbles (default), 1 infinite lives (MpDowned)
    uint8_t rulesHide;              // touches: "don't show again" on the host-rules card (0/1)
    uint32_t rulesSeen[4];          // touches: the hosts whose rules card was shown (hash of the host's name, newest first, 0 = empty)
    char server[MP_SERVER_URL_MAX + 1];   // serverfix: "server" = a community's own online service, "https://host[:port]" (mp_server_url_clean); "" = the build's
    uint8_t consoleKey[16];         // corefix 6: "console_key" (32 hex digits): this console's secret, random, made once on first use, never from
                                    // personal data and NEVER sent: peers only see mp_session_wire_id(key, session id). All zero = none yet.
    uint16_t upnpPermanent;         // corefix 8: "upnp_permanent": the router only took a mapping with no lease (this external port); deleted at exit, and at the
                                    // next start if the game was killed first. 0 = none.
    uint8_t stun;                   // corefix 6: 1 (default) = Play by Address asks a public STUN server (stun.cloudflare.com) for the public
                                    // address; 0 = never (the router's UPnP answer or the LAN address is shown instead). "stun" in mp.cfg.
};

const MpConfig* mp_config(void);             // never null; defaults until loaded
void mp_config_set(const MpConfig* c);       // validates and cleans; does not save
bool mp_config_record_upnp_permanent(uint16_t port);   // the port mapper's record (helper thread); set + save under the config lock
bool mp_config_load(void);                   // false: no file (defaults kept) or unreadable
bool mp_config_save(void);                   // atomic write; false on an I/O error (logged)
// keepfix: take (true) / release (false) the lock every load / save / set of mp.cfg holds (keepsafe, around its file replacements).
void mp_config_hold(bool hold);
bool mp_config_load_held(void);              // keepfix 2: mp_config_load for a caller between mp_config_hold(true) and (false)
bool mp_config_replace_held(const char* text, size_t len);   // keepfix 3: mp.cfg's text from keepsafe's copy, this writer's steps, then loaded
// savefix: whether this run may write mp.cfg. MP_CONFIG_NEWER: the file is from a newer game (version > 1); MP_CONFIG_UNREADABLE:
// it is there but could not be read (another layout or damaged; a copy is kept as mp.cfg.unreadable). In both cases settings
// changes stay for this run only (mp_config_save returns false) until the file is restored (keepsafe) or removed.
enum { MP_CONFIG_WRITABLE = 0, MP_CONFIG_NEWER = 1, MP_CONFIG_UNREADABLE = 2 };
int mp_config_read_only(void);
const char* mp_config_dir(void);             // "<data>/mp"
void mp_config_set_dir(const char* dir);     // tests: another folder
MpSettings mp_config_host_settings(MpMode mode);   // MpSettings for hosting from the saved preferences
// corefix 6: this console's persistent 128-bit secret (HELLO / RESUME carry only a keyed hash of it and the session id, never the secret
// or a fixed id). Made the first time it is asked for (secure random) and saved in mp.cfg at once, so a restarted console is still
// the same console to a session it was in. (An old mp.cfg's 32-bit "console_id" is ignored: it was sent in clear to every host.)
// smallfix: false (out zeroed) when there is no key yet and the secure random source failed: nothing is made or saved, the session is refused.
bool mp_config_console_key(uint8_t out[16]);

// Clean a wire / user name: stops at NUL or `len`, invalid UTF-8 and control characters become '?', trimmed, at most
// MP_NAME_BYTES bytes (never splits a character); empty -> "Player". out has MP_NAME_BYTES + 1 bytes.
void mp_name_clean(const char* in, size_t len, char* out);   // (core2: "" when nothing usable is left)
// core2: an empty name becomes the character's ("Luigi"): the fallback everywhere a name is shown (never "Player")
void mp_name_or_character(char* name, int look);
// The name a player has before typing one of their own: the CHARACTER's name ("Luigi"). Never the console's user name (it can be a
// real name: owner's privacy rule, 13 Nov) and never "Player". out: MP_NAME_BYTES + 1 bytes.
void mp_default_name(int look, char* out);

#endif  // PORT_MP_CONFIG_H
