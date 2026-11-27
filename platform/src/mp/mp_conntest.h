// Galaxy 3.0 multiplayer (owner: conntest): the Connection Test engine. Pure code (no sockets, no SDK, no game): it runs the five
// checks of port/mp_conntest.h one after another through MpCtProbes, a table of function pointers the platform fills in
// (mp_conntest_sys.cpp) and the host tests fake (tests/mp/conntest_test.cpp, with a clock they advance). It never blocks: pump() once a
// frame advances whatever is running; every wait has a deadline of its own and the whole run ends within MP_CT_BUDGET_MS.
//
// Everything a probe returns is another machine's word (the service's JSON, a router's answer, a STUN server's address): the probes
// parse it with the hardened readers (np_json, np_upnp, np_stun) and hand over small enums and numbers; this file trusts none of
// them beyond clamping (a delay of -5 ms or 10^9 ms, a quality of 400, an unknown outcome code all end as a plain result).
#ifndef PLATFORM_MP_CONNTEST_H
#define PLATFORM_MP_CONNTEST_H

#include "port/mp_conntest.h"

#include <stdint.h>

// Budgets (milliseconds). The sum of the steps' own limits is a little over the total: the total wins (the step then ends with what it has).
#define MP_CT_BUDGET_MS 8000
#define MP_CT_LOCAL_MS 1500        // the Same Wi-Fi search (ends at once when a room answers)
#define MP_CT_INFO_MS 3000         // the online service's info call
#define MP_CT_HOST_MS 2200         // router port mapping and STUN, together
#define MP_CT_RTT_MS 1200          // one delay sample
#define MP_CT_RTT_SAMPLES 2
// Delay classes (milliseconds): fast below, slow from MP_CT_SLOW_MS (the same steps as the player list's connection bars).
#define MP_CT_FAST_MS 90
#define MP_CT_SLOW_MS 220
// Wi-Fi quality (percent, platforms that tell): weak below.
#define MP_CT_WEAK_PERCENT 35

struct MpCtWifi {
    bool connected;
    int quality;               // 0..100, -1 = the platform does not say
};
enum MpCtInfoOutcome : uint8_t {
    MPCT_INFO_OK = 0,
    MPCT_INFO_DOWN,            // no answer, a bad answer, a captive portal's page: not our service
    MPCT_INFO_OFFLINE,         // the platform says there is no network
    MPCT_INFO_KILLED,          // the service answered and says it is switched off
    MPCT_INFO_UPDATE,          // the service answered and wants a newer game
};
struct MpCtInfo {
    uint8_t outcome;           // MpCtInfoOutcome
    int ms;                    // round trip of that call
};
enum MpCtMapState : uint8_t { MPCT_MAP_WORKING = 0, MPCT_MAP_MAPPED, MPCT_MAP_FAILED };
struct MpCtMap {
    uint8_t state;             // MpCtMapState
    bool cgnat;                // the router's "public" address is private / carrier-grade
    uint32_t extIp;            // host order, 0 unknown
};

// The platform's side. Any pointer may be null (that capability is then absent: the check says so and goes on).
struct MpCtProbes {
    void* ctx;
    uint64_t (*now)(void* ctx);                          // monotonic milliseconds
    bool (*wifi)(void* ctx, MpCtWifi* out);
    // Same Wi-Fi: called every pump while that check runs. Rooms found so far (0 = none yet), -1 the search cannot go out, -2 not
    // possible now (a room is open: the link is up by definition).
    int (*local)(void* ctx);
    bool (*serviceConfigured)(void* ctx);
    bool (*infoBegin)(void* ctx, uint32_t timeoutMs);    // false: the call could not start
    int (*infoPoll)(void* ctx, MpCtInfo* out);           // 0 pending, 1 finished (*out filled)
    void (*infoCancel)(void* ctx);
    int (*mapBegin)(void* ctx);                          // 1 asked the router, 0 port mapping is off in this build / by the player
    void (*mapPoll)(void* ctx, MpCtMap* out);
    void (*mapEnd)(void* ctx);                           // release the mapping (the test never does this inside a room)
    int (*stunBegin)(void* ctx);                         // 1 started, 0 off (mp.cfg "stun = 0") or not available
    int (*stunPoll)(void* ctx, uint32_t* ipOut);         // 0 pending, 1 answered (the public address as the server saw it), -1 no answer
    void (*stunEnd)(void* ctx);
    int (*hostRtt)(void* ctx);                           // inside a room: milliseconds to the room's other end, -1 unknown
    void (*busy)(void* ctx);                             // every pump of a run (keeps the thread that pumps the router mapping alive)
};

struct MpCtOptions {
    bool inRoom;     // started from a session menu: the room's other end is timed, the router mapping is only read (never asked, never released)
    bool isHost;
    bool relayOnly = false;   // the player asked to hide the address (mp.cfg relay_only / SMG_NET_RELAY_ONLY): no router request, no STUN request, hosting is not tested
};

// ---- pure helpers (tested) ------------------------------------------------------------------------------------------------------
// 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier-grade NAT), 169.254/16, 127/8, 0/8: not an address the internet can reach (host order).
bool mp_ct_ip_not_public(uint32_t ip);
uint8_t mp_ct_wifi_result(const MpCtWifi& w);                    // MPCT_R_WIFI_*
uint8_t mp_ct_delay_result(int ms);                              // MPCT_R_DELAY_*; a nonsense number is MPCT_R_NONE
// Hosting from what the router and the STUN server said. mapAsked: a mapping was asked for; map: its state; stunAsked / stunState
// (1 answered, -1 silent, 0 not settled) / stunIp: the public address the STUN server saw.
uint8_t mp_ct_host_result(bool mapAsked, const MpCtMap& map, bool stunAsked, int stunState, uint32_t stunIp);
MpCtVerdict mp_ct_verdict(const uint8_t result[MPCT_CHECKS]);
// "1.2.0" >= "1.10.0"? Dotted numbers compared numerically; false when either is not a version. (the service's app.min / app.latest)
bool mp_ct_version_older(const char* mine, const char* minimum);

class MpConnTest {
public:
    MpConnTest();
    void start(const MpCtProbes& probes, const MpCtOptions& options);   // a running run is cancelled first
    void pump();
    void cancel();                       // stops everything that runs and lets the mapping go (a finished run stays as it is)
    bool running() const { return m_view.running; }
    const MpCtView& view() const { return m_view; }

private:
    void begin(int step);
    void finishStep(uint8_t result, int detail);
    void finishRun();
    void stepWifi(uint64_t now);
    void stepLocal(uint64_t now);
    void stepInternet(uint64_t now);
    void stepHosting(uint64_t now);
    void stepDelay(uint64_t now);
    uint64_t deadline(uint32_t stepMs) const;
    void release();                      // mapping / info / STUN of the running step

    MpCtProbes m_p;
    MpCtOptions m_o;
    MpCtView m_view;
    int m_step;                          // the running check, MPCT_CHECKS when finished
    uint64_t m_start, m_stepStart, m_stepEnd;
    bool m_stepBegun;
    // step state
    bool m_infoRunning, m_mapAsked, m_mapHeld, m_stunAsked;
    int m_infoMs0;                       // the Internet check's own round trip (the delay falls back to it)
    int m_samples, m_sampleMin;
    int m_stunState;
    uint32_t m_stunIp;
};

#endif  // PLATFORM_MP_CONNTEST_H
