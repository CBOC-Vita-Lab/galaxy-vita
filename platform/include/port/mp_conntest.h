// Galaxy 3.0 multiplayer: the Connection Test (owner: conntest). What the menus see of it: plain types and a small C API, so the game
// side (decomp/src/Game/MP/MpMenuSequence.cpp) needs nothing else. The engine is platform/src/mp/mp_conntest.{h,cpp} (pure, host
// tested: tests/mp/conntest_test.cpp), the real probes platform/src/mp/mp_conntest_sys.cpp.
//
// Five checks, one after another (total budget about 8 s), each reported as it finishes:
//   Wi-Fi, Same Wi-Fi (can other consoles be found), Internet (is the online service reachable), Hosting (router port mapping,
//   carrier-grade NAT), Delay (round trip to the service, and to the room's host from inside a room).
// A verdict function turns the results into ONE headline and up to three short lines; the game words them (MpMessages MP_T_CT_*).
#ifndef PORT_MP_CONNTEST_H
#define PORT_MP_CONNTEST_H

// Weak for the callers (tools and tests that link the menus without the port still link). The units that DEFINE them define MP_CONNTEST_DEFINES_STRONG before this header: their
// definitions are strong, so the ELF shows "T" for the real function (a weak definition shows "W", which the post-link check cannot tell from a stub).
#ifdef MP_CONNTEST_DEFINES_STRONG
#define MP_CONNTEST_WEAK
#else
#define MP_CONNTEST_WEAK __attribute__((weak))
#endif

#include <stdint.h>

enum MpCtCheck : uint8_t { MPCT_WIFI = 0, MPCT_LOCAL, MPCT_INTERNET, MPCT_HOSTING, MPCT_DELAY, MPCT_CHECKS };

enum MpCtState : uint8_t { MPCT_ST_WAITING = 0, MPCT_ST_RUNNING, MPCT_ST_DONE };

// What one check found (the row's words). A check that was not run (nothing to test: no Wi-Fi, no service, in a room, out of time)
// is MPCT_R_SKIPPED. The grouping below is by check; MPCT_R_NONE is only the value of a check that has not finished.
enum MpCtResult : uint8_t {
    MPCT_R_NONE = 0,
    MPCT_R_SKIPPED,
    // Wi-Fi
    MPCT_R_WIFI_GOOD, MPCT_R_WIFI_WEAK, MPCT_R_WIFI_NONE,
    MPCT_R_WIFI_UNKNOWN,      // connected, the platform does not say how well (desktop)
    // Same Wi-Fi: other consoles on this network
    MPCT_R_LOCAL_FOUND,       // a room on this Wi-Fi answered (detail = rooms)
    MPCT_R_LOCAL_OK,          // the search went out and nothing is wrong; no room is open right now
    MPCT_R_LOCAL_BLOCKED,     // the search could not be sent (no link, no broadcast)
    // Internet: the online service
    MPCT_R_NET_OK, MPCT_R_NET_DOWN, MPCT_R_NET_NOT_SET_UP, MPCT_R_NET_UPDATE, MPCT_R_NET_OFF,
    // Hosting over the internet
    MPCT_R_HOST_OPEN,         // the router opened a port (UPnP / NAT-PMP / PCP) and the address is public
    MPCT_R_HOST_MAYBE,        // no port opened, but the console has a public address: friends may get through
    MPCT_R_HOST_CGNAT,        // the provider's own NAT sits in front of the router: nobody can reach a host here
    MPCT_R_HOST_UNKNOWN,      // no port, and no public address could be learned (STUN off or silent)
    // Delay
    MPCT_R_DELAY_FAST, MPCT_R_DELAY_OK, MPCT_R_DELAY_SLOW,
    MPCT_R_COUNT
};

// The verdict's wording ids (the game maps them to MpText). One headline, up to MPCT_MAX_LINES short lines.
enum MpCtHeadline : uint8_t {
    MPCT_H_NONE = 0,
    MPCT_H_ALL_GOOD,          // "Everything looks good."
    MPCT_H_JOIN_ONLY,         // "You can join friends."
    MPCT_H_ONLINE_ONLY,       // "Online play works." (but Same Wi-Fi is blocked here)
    MPCT_H_SLOW,              // "The connection is slow."
    MPCT_H_WIFI_WEAK,         // "Your Wi-Fi is weak."
    MPCT_H_NO_WIFI,           // "Not connected to Wi-Fi."
    MPCT_H_NO_INTERNET,       // "Can't go online right now."
    MPCT_H_NOT_SET_UP,        // "Online play isn't set up."
    MPCT_H_UPDATE,            // "The game needs an update."
    MPCT_H_PAUSED,            // "Online play is paused."
    MPCT_H_COUNT
};
enum MpCtLine : uint8_t {
    MPCT_L_NONE = 0,
    MPCT_L_HOST_AND_JOIN,     // "You can host and join."
    MPCT_L_HOST_MAYBE,        // "Hosting may not work from here."
    MPCT_L_HOST_CGNAT,        // "Your provider blocks hosting."
    MPCT_L_LET_FRIEND_HOST,   // "Let a friend host the room."
    MPCT_L_MOVE_CLOSER,       // "Move closer to your router."
    MPCT_L_SLOW_LAG,          // "Online games may lag."
    MPCT_L_TRY_LOCAL,         // "Same Wi-Fi or Nearby may be smoother."
    MPCT_L_LOCAL_STILL,       // "Same Wi-Fi and Nearby still work."
    MPCT_L_NEARBY_STILL,      // "Nearby play still works."
    MPCT_L_CONNECT_WIFI,      // "Connect to Wi-Fi in the Vita's Settings."
    MPCT_L_CHECK_WIFI,        // "Check that your Wi-Fi has internet."
    MPCT_L_NOT_SET_UP,        // "This version has no online service."
    MPCT_L_UPDATE,            // "Update the game to play online."
    MPCT_L_TRY_LATER,         // "Please try again later."
    MPCT_L_LOCAL_BLOCKED,     // "This Wi-Fi may block other consoles."
    MPCT_L_COUNT
};
constexpr int MPCT_MAX_LINES = 3;

struct MpCtVerdict {
    uint8_t headline;         // MpCtHeadline
    uint8_t lineCount;
    uint8_t lines[MPCT_MAX_LINES];   // MpCtLine
    bool weakLink;            // the Wi-Fi is weak or the delay is slow: the Wi-Fi power-save tip is worth showing
    bool good;                // everything works: the headline is the good one
};

struct MpCtView {
    uint8_t state[MPCT_CHECKS];      // MpCtState
    uint8_t result[MPCT_CHECKS];     // MpCtResult (MPCT_R_NONE until the check is DONE)
    int16_t detail[MPCT_CHECKS];     // Wi-Fi: quality 0..100 (-1 unknown); Same Wi-Fi: rooms found; Delay: milliseconds (-1 none)
    bool running;                    // a run is going
    bool done;                       // the last run finished (every check DONE) and `verdict` is valid
    bool inRoom;                     // the run was started from inside a room
    MpCtVerdict verdict;
    uint32_t serial;                 // changes whenever anything above changes
};

extern "C" {
// Weak: tools / tests that link the menus without the port still link (null = no Connection Test in this program).
// Start a run (a running one is replaced). Game thread. `inRoom`: started from a session menu (the room's host is timed too, the router
// is left alone). False when it cannot start.
bool mp_conntest_start(bool inRoom) MP_CONNTEST_WEAK;
void mp_conntest_pump(void) MP_CONNTEST_WEAK;               // game thread, once a frame while the screen is up
void mp_conntest_view(MpCtView* out) MP_CONNTEST_WEAK;
void mp_conntest_cancel(void) MP_CONNTEST_WEAK;             // stops the run and lets the router mapping go
// The last finished run found a weak Wi-Fi or a slow link (the Wi-Fi power-save tip).
bool mp_conntest_weak_link(void) MP_CONNTEST_WEAK;
// release3: "Save a report": writes <data dir>/report.txt (mp_report_save: scrubbed log tail, build, settings and the last test's lines).
bool mp_conntest_save_report(void) MP_CONNTEST_WEAK;
// The Wi-Fi privacy setting "relay only" is on (shown in the Details window).
bool mp_conntest_relay_only(void) MP_CONNTEST_WEAK;
// linkfix: the last HTTPS handshake to the service failed only because of the certificate dates (the console's date and time are wrong),
// until the next successful handshake (net/src/np_https.cpp np_https_last_clock_fault).
bool mp_conntest_clock_wrong(void) MP_CONNTEST_WEAK;
}

#endif  // PORT_MP_CONNTEST_H
