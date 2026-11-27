// Galaxy 3.0 multiplayer (release3): the "A newer version of the game is available." notice.
//
// The online service says which game version is the newest in two places (server/src/core/app.js): the info answer
// (GET /v1/info -> "app": {"min", "latest"}) and every error answer ("error": {..., "latestVersion"}; a failed room join because of a
// different version is one of those). The service client hands each answer body to mp_update_scan_body(); if the version found is
// newer than this build's (SMG_APP_VERSION), a notice becomes pending. The UI asks mp_update_take_notice() from its per-frame update:
// it returns true ONCE per boot, then the UI shows the toast MpMsgText MP_T_UPDATE_AVAILABLE.
//
// Everything here is cheap and bounded (no heap, no locks beyond one atomic, any thread). A hostile or broken answer never crashes it:
// anything that is not a plain dotted version is ignored.
#ifndef PORT_MP_UPDATE_H
#define PORT_MP_UPDATE_H

#include <stddef.h>

extern "C" {

// Dotted numeric versions: "3.0.0", "3.1", "10.2.0.1", optionally "-rc1" (ignored for the order). 1 to 4 numbers of 1 to 4 digits;
// a missing number counts as 0 ("3.1" == "3.1.0"). Returns -1 if a < b, 0 if equal, 1 if a > b, 2 if either is not a valid version.
int mp_version_compare(const char* a, const char* b);

// The version of this build (the one the service client sends as X-Msc-App) is `current`; tests set another with mp_update_set_current.
// Records `latest` (a version string from the service). Returns true if it is newer than this build (a notice is now pending, or was
// shown already this boot). Invalid or not-newer strings change nothing.
bool mp_update_note_latest(const char* latest);

// Looks for "latestVersion" (error answers) or "latest" inside "app" (info answers) in a JSON body of `len` bytes and notes it.
// Not a JSON parser: a bounded scan for those two keys; bodies over 64 KiB are ignored. Returns true if a newer version was found.
bool mp_update_scan_body(const char* body, size_t len);

bool mp_update_available(void);     // a newer version than this build is known (stays true)
const char* mp_update_latest(void); // that version ("" if none), for logs
bool mp_update_take_notice(void);   // true exactly once per boot, after a newer version became known; then false forever

// Tests only.
void mp_update_set_current(const char* version);   // null: back to mp_service_app_version()
void mp_update_reset(void);

}  // extern "C"

#endif  // PORT_MP_UPDATE_H
