// Galaxy 3.0 multiplayer (beta diagnostics): what a player's console can send us, with consent, to fix bugs.
//
//   1. The session summary: a few numbers about the last online / Nearby session (no name, no id, no address). Sent with the online room's leave
//      request ({"summary":...}) and kept for the next problem report's "stats".
//   2. "Check Connection": the router's behaviour (STUN) and, with an online account, a relay loopback. Its result is the report's "check".
//   3. "Send a Problem Report": POST /v1/report with the consent screen's promise: the online name (the account's), the game's log from recent
//      play (filtered and scrubbed by mp_report.cpp: no addresses, e-mail addresses or long secrets), the last check, the last session's numbers.
//      Never saves, never settings, never a password.
//
// The menu (decomp/src/Game/MP/MpMenuReport.cpp) talks to the port only through the extern "C" functions below (weak declarations: the host
// tests of the menu link none of this). The pure text functions (JSON, summary, body) are in mp_diag_text.cpp, no network, tested by
// tests/mp/diag_test.cpp.
#ifndef PORT_MP_DIAG_H
#define PORT_MP_DIAG_H

// The menu and the session code call these weakly (a build without them still runs). The units that DEFINE them (mp_diag.cpp, mp_netcheck.cpp) define
// MP_DIAG_DEFINES_STRONG before this header, so that their definitions are strong: a definition that inherits the weak declaration shows as "W" in the ELF,
// and the post-link check (T only) cannot tell it from a weak default with no real body.
#ifdef MP_DIAG_DEFINES_STRONG
#define MP_DIAG_WEAK
#else
#define MP_DIAG_WEAK __attribute__((weak))
#endif

#include <stddef.h>
#include <stdint.h>

// ---- the data -------------------------------------------------------------------------------------------------------------------------
struct MpDiagSummary {
    bool valid;
    char transport[8];       // "udp" | "relay"
    uint32_t durS;           // seconds the session ran
    uint32_t members;        // most players present at once
    char ended[40];          // "leave" | "host_lost" | "kicked" | "error:<short code>"
    float rxRate;            // datagrams received per second (session mean)
    float gapMeanMs, gapMaxMs;   // time between two pose arrivals of one remote player (mean over the session / longest)
    float bufMeanMs, bufMaxMs;   // how far behind the newest pose the puppets were drawn (mean / most)
    uint32_t extrap, hold, snap, late, stall100;   // puppet frames guessed ahead / held still; buffer snaps; poses that arrived late; gaps over 100 ms
    uint32_t txFail;         // datagrams the transport refused with an error
    bool relayUsed;          // the relay carried the traffic at some point
    char app[16];            // "3.0.0"
};

struct MpDiagCheck {
    bool valid;
    char nat[12];            // "open" | "cone" | "symmetric" | "blocked" | "unknown"
    int stunMs;              // STUN round trip, -1 none
    char relay[8];           // "ok" | "fail" | "skipped"
    int relayRttMs;          // relay loopback round trip, -1 none
    char err[40];            // short code of what failed ("" none)
};

// Pose-stream totals of one session, summed over the remote players (decomp/src/Game/MP/MpPuppets.cpp pushes them about once a second).
struct MpDiagPose {
    uint32_t arrivals;       // pose messages taken in
    uint32_t gaps;           // arrivals that had a predecessor (the count behind gapSumMs)
    float gapSumMs, gapMaxMs;
    uint32_t samples;        // frames a puppet was drawn
    float depthSumMs, depthMaxMs;
    uint32_t extrap, hold, snap, late, stall100;
};

// What the report is made of (mp_diag_build_body).
struct MpDiagBodyIn {
    const char* kind;        // "user" | "after_error"
    const char* where;       // "mp_menu" | "settings" | "post_session"
    const char* note;        // UTF-8, at most 200 characters (longer is cut); null / "" = none
    const MpDiagCheck* check;      // null = none
    const MpDiagSummary* stats;    // null = none
    const char* logPrev; size_t logPrevLen; bool logPrevIsTail;   // the rotated log (null: none)
    const char* logCur; size_t logCurLen; bool logCurIsTail;       // the current log
};

#define MP_DIAG_LOG_MAX 48000          // bytes of log in a report (UTF-8, after masking), the newest lines kept
#define MP_DIAG_NOTE_MAX 200           // characters of the player's note
#define MP_DIAG_BODY_MAX 60000         // bytes of the whole POST body (np_https takes 64 KiB): the log is cut further when its escaping needs more

enum MpDiagResult {   // mp_diag_report_poll / mp_diag_check_poll
    MP_DIAG_PENDING = 0,
    MP_DIAG_OK = 1,
    MP_DIAG_NO_ACCOUNT = 2,
    MP_DIAG_OFFLINE = 3,      // no network
    MP_DIAG_TIMEOUT = 4,      // the service did not answer in time
    MP_DIAG_BAD_REQUEST = 5,  // 400
    MP_DIAG_TOO_LARGE = 6,    // 413
    MP_DIAG_RATE_LIMITED = 7, // 429 (try later)
    MP_DIAG_BUSY = 8,         // 503 and other server trouble (try later)
    MP_DIAG_FAILED = 9,       // anything else
    MP_DIAG_CLOCK = 10        // the console's date and time are wrong
};

// ---- pure text (mp_diag_text.cpp) -----------------------------------------------------------------------------------------------------
extern "C" {
// JSON string content (no surrounding quotes) of UTF-8 text: " \ and control characters escaped; a byte that is not valid UTF-8 becomes '?'.
// Writes at most cap - 1 bytes and a NUL; returns the length the whole text needs (like snprintf): the caller sees a cut when it is >= cap.
size_t mp_diag_json_escape(const char* in, size_t n, char* out, size_t cap);
// {"transport":"udp","dur_s":..,...} / {"nat":..}: valid JSON objects. Returns the length (0 when cap is too small or s is not valid).
size_t mp_diag_summary_json(const MpDiagSummary* s, char* out, size_t cap);
size_t mp_diag_check_json(const MpDiagCheck* c, char* out, size_t cap);
// The POST body of /v1/report: {"v":1,"kind":..,"where":..,"note":..,"check":{..}|null,"stats":{..}|null,"log":".."}. The log is the filtered, scrubbed
// text (mp_report_log_payload) of at most MP_DIAG_LOG_MAX bytes, cut further (oldest lines first) until the whole body fits `limit` bytes.
// Returns the body length (< cap), 0 on failure. *logBytes (optional): the bytes of log in it.
int mp_diag_probe_source(uint32_t aIp, uint32_t bIp, bool bAnswered);   // 0 unknown, 1 same public IP, 2 different (the relay loopback's second socket)
bool mp_diag_error_offers_report(int err);   // MpUiError: true = a session that ended this way may offer a problem report (unexpected faults only)
size_t mp_diag_build_body(const MpDiagBodyIn* in, char* out, size_t cap, size_t limit, size_t* logBytes);
}

// ---- for the port (core, puppets) ----------------------------------------------------------------------------------------------------
extern "C" {
// MpCore: a session went live / ended. `kind` 0 = Nearby / Same Wi-Fi / loopback, 1 = online / by address. end: the reason the core reports
// (MpUiError as int, 0 = left by us); relay: the transport used the relay. Builds the summary (and keeps it for the next report), remembers an
// error for the after-error offer, and for an online session hands the summary to the room's leave request.
void mp_diag_session_begin(int kind) MP_DIAG_WEAK;
void mp_diag_session_count(uint32_t rx, uint32_t txFail, uint32_t members, int relay) MP_DIAG_WEAK;   // running totals, any time
void mp_diag_session_end(int err, const char* why, int online) MP_DIAG_WEAK;
// MpPuppets (game thread, about once a second while a session runs): the pose totals so far (replace, not add).
void mp_diag_note_pose(const MpDiagPose* totals) MP_DIAG_WEAK;
// The last session's numbers; false when none.
bool mp_diag_last_summary(MpDiagSummary* out) MP_DIAG_WEAK;

// ---- for the menu ---------------------------------------------------------------------------------------------------------------------
bool mp_diag_has_account(void) MP_DIAG_WEAK;        // the online account (player token) exists
bool mp_diag_service_enabled(void) MP_DIAG_WEAK;    // this build has a service address
// "Something went wrong last time": an online or Nearby session ended with an error since the last offer. Reading it does not clear it.
bool mp_diag_error_pending(void) MP_DIAG_WEAK;
void mp_diag_error_clear(void) MP_DIAG_WEAK;        // the offer was shown (once)
// Send a Problem Report. kind 0 = user, 1 = after_error; where 0 = mp_menu, 1 = settings, 2 = post_session. note may be null. false: cannot start.
bool mp_diag_report_begin(int kind, int where, const char* note) MP_DIAG_WEAK;
int mp_diag_report_poll(char* idOut, int cap) MP_DIAG_WEAK;   // MpDiagResult; on MP_DIAG_OK the report's id ("XXXXXXXX") is in idOut
void mp_diag_report_cancel(void) MP_DIAG_WEAK;
// Check Connection: STUN (the NAT, the round trip) and, with an account, the relay loopback. Poll every frame.
bool mp_diag_check_begin(void) MP_DIAG_WEAK;
int mp_diag_check_poll(MpDiagCheck* out) MP_DIAG_WEAK;        // MP_DIAG_PENDING while running; then MP_DIAG_OK (out filled: relay may be "skipped" / "fail")
void mp_diag_check_cancel(void) MP_DIAG_WEAK;
bool mp_diag_last_check(MpDiagCheck* out) MP_DIAG_WEAK;       // the last finished check, false when none
}

#endif  // PORT_MP_DIAG_H
