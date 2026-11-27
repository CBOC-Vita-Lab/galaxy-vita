// Galaxy 3.0 multiplayer (code5): the far-away invite: a 5-character code (the room-code alphabet: no I L O 0 1) that the online service
// turns into the host's address. The service stores only a SEALED copy of the long code's content (ip, port, secret): the host's console
// seals it with a key made from the five characters, the friend's console opens it with the same five. Plain C API, no game headers.
// Design: share/core/CODE5-DESIGN.md; the service side: server/src/core/invites.js.
//
// Typing: case, spaces, dashes, dots and underscores are forgiven. 0 O 1 I L are not in the alphabet, so no code has one: they are
// refused here (MP_INV_LOOKALIKE) before any request is made, with the same plain "That code didn't work" as any other mistake.
#ifndef PORT_MP_INVITE_H
#define PORT_MP_INVITE_H

#include <stddef.h>
#include <stdint.h>

#define MP_INVITE_LEN 5                 // characters of the code
#define MP_INVITE_BLOB_CHARS 48         // the sealed text the service holds: 36 bytes (8 nonce + 12 payload + 16 tag), base64url, no padding

enum MpInviteTyped : int {
    MP_INV_TYPED_OK = 0,
    MP_INV_TYPED_EMPTY = 1,             // nothing typed
    MP_INV_TYPED_LENGTH = 2,            // not five code characters (15 and 19 are the long form: mp_invite_kind)
    MP_INV_TYPED_LOOKALIKE = 3,         // 0 O 1 I L
    MP_INV_TYPED_CHARACTER = 4,         // another character
};

// Why a lookup or a registration did not give a code / an address (mp_svc_invite_*_poll returns -value).
enum MpInviteError : int {
    MP_INV_NONE = 0,
    MP_INV_WRONG = 1,                   // no such code (typo, never existed, forgotten)
    MP_INV_EXPIRED = 2,                 // it existed and is over (the host left or went on to play, or it ran out)
    MP_INV_FULL = 3,                    // the game has all the friends it takes
    MP_INV_VERSION = 4,                 // the friend plays another version of the game
    MP_INV_BUSY = 5,                    // the service is unreachable, busy, switched off, over its daily budget, or this console is limited
    MP_INV_NO_WIFI = 6,
    MP_INV_CLOCK = 7,                   // the console's date is wrong (TLS)
    MP_INV_UPDATE = 8,                  // this game is too old for the service
    MP_INV_TAKEN = 9,                   // (registering) that code is a live one: pick another
    MP_INV_SEALED_BAD = 10,             // the answer did not open with this code (a damaged or forged answer)
    MP_INV_OFF = 11,                    // this build has no service address
};

extern "C" {
// What was typed -> the five characters in upper case (out >= 6 bytes). MpInviteTyped.
int mp_invite_normalize(const char* typed, char* out);
// How many code characters were typed (separators ignored; 0 if a character that is no code character is in it): 5 = an invite,
// 15 / 19 = the long direct code, anything else = not a code. Case and separators forgiven; look-alikes count (they are rejected later).
int mp_invite_kind(const char* typed);
// Five random characters of the alphabet from `rnd` (the system's secure random). False on bad arguments.
bool mp_invite_make(uint32_t (*rnd)(void), char* out);
// Seals ip (host order), port and the 5-byte secret under `code` (5 upper-case characters). nonce8: 8 random bytes (new for every call).
// out >= MP_INVITE_BLOB_CHARS + 1. Returns the length (48) or 0.
int mp_invite_seal(const char* code, uint32_t ip, uint16_t port, const uint8_t* secret, const uint8_t* nonce8, char* out);
// The friend's side: MP_CODE_OK (port/mp_direct_code.h's MpCodeResult) with ip / port / secret, or MP_CODE_CHECK when the text does not open
// with this code (the wrong key, a damaged copy), MP_CODE_ADDRESS when it opens to an unusable address.
int mp_invite_open(const char* code, const char* blob, uint32_t* ip, uint16_t* port, uint8_t* secret);

// ---- the service side of it (mp_service.cpp; weak in the gather driver: a build or a host test without it has no invites) -----------------------
// Each call starts at most one request and returns; the poll answers 0 while it runs, 1 when it is done, -(MpInviteError) when it failed.
bool mp_svc_invite_register_begin(const char* code, const char* blob);     // POST /v1/invites
int mp_svc_invite_register_poll(int* keepSec);                             // keepSec: when to call mp_svc_invite_keep (half the life)
void mp_svc_invite_keep(const char* code);                                 // POST /v1/invites/:code/keep; the answer is not needed
void mp_svc_invite_close(const char* code);                                // DELETE /v1/invites/:code; always goes out, nothing waits for it
bool mp_svc_invite_resolve_begin(const char* code);                        // POST /v1/invites/resolve
int mp_svc_invite_resolve_poll(char* blobOut, int cap);                    // blobOut: the sealed text (48 characters)
void mp_svc_invite_cancel(void);                                           // drop a running register / resolve
}

#endif
