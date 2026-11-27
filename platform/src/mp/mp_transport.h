// Galaxy 3.0 multiplayer (port side, owner: core): the datagram transport under MpCore, and the "connector" that brings one up.
//
// MpTransport = one datagram socket bound to a port on some link: send to a peer address, broadcast to a port, non-blocking
// receive, local address. Three implementations:
//   - loopback (mp_transport_loop.*): N endpoints in one process with impairment (loss, duplicate, reorder, delay, jitter,
//     bandwidth cap) and an injectable clock: unit tests and the in-process mp_bot mode;
//   - VLink (mp_transport_vlink.cpp): a socket on a VLink::Link (port MP_SESSION_PORT; Wi-Fi UDP / ad hoc / POSIX UDP backends);
//   - online (mp_conn_online.cpp): the VLink socket over the NpNet backend (rooms with a code).
// MpConnector = the bring-up of one connection kind (link up, room created / joined) that then hands out the transports.
#ifndef PORT_MP_TRANSPORT_H
#define PORT_MP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "port/mp_bridge.h"

#define MP_SESSION_PORT 0x5347   // "SG": the session socket on the VLink (DESIGN.md section 3)
#define MP_LAN_PORT 0x5348       // discovery beacons (Same Wi-Fi / Nearby)
#define MP_ADDR_BROADCAST 0xFFFFFFFFu
#define MP_DIRECT_PORT 41100     // netx: internet by address: the host's UDP port (41101..41107 when taken)

struct MpAddr {
    uint32_t host;   // VLink virtual IP / loopback endpoint id; MP_ADDR_BROADCAST = every console on the link
    uint16_t port;
    bool operator==(const MpAddr& o) const { return host == o.host && port == o.port; }
    bool operator!=(const MpAddr& o) const { return !(*this == o); }
};

class MpTransport {
public:
    virtual ~MpTransport() {}
    // Bytes sent, or <0. A destination with host == MP_ADDR_BROADCAST is a broadcast to that port.
    virtual int send(const MpAddr& to, const void* data, size_t len) = 0;
    // Non-blocking: datagram length, 0 when nothing is waiting, <0 on a hard error (link gone).
    virtual int recv(MpAddr* from, void* buffer, size_t capacity) = 0;
    virtual MpAddr local() const = 0;
    virtual bool lost() const { return false; }   // the link is gone (Wi-Fi off, sleep): the session ends
    virtual const char* name() const = 0;
};

// Connector states (poll()).
enum { MP_CONNECTOR_WORKING = 1, MP_CONNECTOR_READY = 2 };

struct MpConnectorParams {
    bool host;                    // create (host) or join
    char code[16];                // online: room code to join ("" = create)
    char name[MP_NAME_BYTES + 1]; // player name (online signaling)
    uint8_t visibility;           // online host
    MpSettings settings;          // online host (announced to the service through the weak hooks below)
    uint32_t directIp;            // netx: by address (guest): the host's IPv4 (host order) and UDP port
    uint16_t directPort;
    uint32_t linkId;              // netx: by address: this console's id for the run (its link address is made from it; 0 = random)
};

class MpConnector {
public:
    virtual ~MpConnector() {}
    // Drive the bring-up. Returns MP_CONNECTOR_WORKING, MP_CONNECTOR_READY, or -(MpUiError) on failure.
    virtual int poll(uint64_t nowMs) = 0;
    // Valid once poll() returned READY. The session socket (MP_SESSION_PORT) and the discovery socket (MP_LAN_PORT; may be null:
    // online has no discovery). Owned by the connector.
    virtual MpTransport* session() = 0;
    virtual MpTransport* discovery() = 0;
    // Online: the room code once known ("" otherwise).
    virtual void roomCode(char* out, size_t cap) { if (cap) out[0] = 0; }
    // Players in the online room as the transport sees them (-1 unknown). Information only.
    virtual int roomMembers() { return -1; }
    // core2: the online service's public player code ("XXXX-XXXX", 10 bytes with the NUL) of the room member in `slot` and its virtual
    // address, from this console's own authenticated room view. Only the online connector has one: LAN / ad hoc / by address never.
    virtual bool memberCode(int slot, uint32_t* vip, char out[10]) { (void)slot; (void)vip; out[0] = 0; return false; }
    // svcfix3: the name to show for the room member in `slot` when the service says another member has the same name ("Luma#4DXT"; at most
    // MP_NAME_BYTES), and its virtual address, from the same authenticated view. false = show the name (nobody shares it, an older service).
    virtual bool memberLabel(int slot, uint32_t* vip, char out[17]) { (void)slot; (void)vip; out[0] = 0; return false; }
    // The host's player count changed (online: the service's room listing is updated).
    virtual void playersChanged(int) {}
    // A host migration happened (DESIGN.md section 8): `local` = this console is the host now (online: it announces the room;
    // the room code stays the same and NpNet re-punches to the new host).
    virtual void hostChanged(int, bool) {}
    // netx: by address (host): what the lobby shows. False when this connector has no such address.
    virtual bool addressInfo(MpAddressInfo*) { return false; }
    // fix3: the failure poll() just reported was a drop from the room (online: the service's 403 not_member after this console was
    // silent for a minute: asleep, a long stall): the room may go on, so the core rejoins it as after a sleep (a fresh join)
    // instead of ending the session. poll()'s error is what the player reads if that rejoin cannot happen.
    virtual bool rejoinable() { return false; }
    // mpwifi44: one line for the log about the failure poll() reported ("" = nothing more is known)
    virtual const char* failWhy() const { return ""; }
    virtual const char* name() const = 0;
};

class MpLoopHub;
// Factories (each in its own file). Null when the kind is not available in this build.
MpConnector* mp_connector_loop_create(MpLoopHub* hub, uint32_t endpoint);
MpConnector* mp_connector_loop_direct_create(MpLoopHub* hub, uint32_t endpoint, const MpConnectorParams& p);   // core2 (tests)
MpConnector* mp_connector_vlink_create(MpConn conn);
MpConnector* mp_connector_online_create(const MpConnectorParams& p);
// netx (mp_conn_direct.cpp): internet by address. Weak: the session-layer host tests link without it (null = not available).
MpConnector* mp_connector_direct_create(const MpConnectorParams& p) __attribute__((weak));
bool mp_direct_parse_address(const char* text, uint32_t* ip, uint16_t* port) __attribute__((weak));
uint32_t mp_direct_local_ip(void) __attribute__((weak));   // core2: this console's LAN address (host order), 0 = none

// ---- hooks other modules may provide (weak references: absent = default behaviour) ------------------------------------------
extern "C" {
// net/ (Vita): bring up the Wi-Fi UDP (conn = MP_CONN_WIFI) or ad hoc (MP_CONN_NEARBY) link on a VLink::Link and hand it out.
// request = 1 starts / continues the bring-up, 0 only polls. Returns 2 ready (*linkOut = the started VLink::Link*), 1 working,
// -1 failed (no Wi-Fi / radio), -2 not supported. Absent (the desktop): MpCore makes a POSIX UDP link itself.
int mp_net_link_poll(int conn, int request, void** linkOut) __attribute__((weak));
// net/: the session is over: release the radio / Wi-Fi link brought up by mp_net_link_poll.
void mp_net_link_release(int conn) __attribute__((weak));
// mpwifi44: why the last mp_net_link_poll answered -1. Returns MP_LINK_FAIL_WIFI_DOWN when the console is not connected to an access point
// (or has no address): the one case the player is told to check the Wi-Fi settings; MP_LINK_FAIL_OTHER for anything else (the network
// module, the stack, a socket, a bind: Wi-Fi itself may be fine); MP_LINK_FAIL_UNKNOWN when it cannot tell. *why (when not null) gets one
// line for the log, never null. Absent: unknown.
enum { MP_LINK_FAIL_UNKNOWN = 0, MP_LINK_FAIL_WIFI_DOWN = 1, MP_LINK_FAIL_OTHER = 2 };
int mp_net_link_fail(int conn, const char** why) __attribute__((weak));

// service (MpService): online account and room lookups. Absent: online sign-in counts as done; quick match and join-by-friend
// fail with MP_ERR_SERVICE_DOWN.
//   mp_svc_signin_poll: 1 ready (NpNetConfigureService done), 0 working, <0 = -(MpUiError).
int mp_svc_signin_poll(void) __attribute__((weak));
//   kind 0 = quick match (mode), 1 = a friend's room (friendCode). poll: 0 working; 1 done with a room code in out (out = "" for
//   quick match: no open room, host a new public one); <0 = -(MpUiError).
bool mp_svc_room_lookup_begin(int kind, uint8_t mode, const char* friendCode) __attribute__((weak));
int mp_svc_room_lookup_poll(char* codeOut, int cap) __attribute__((weak));
void mp_svc_room_lookup_cancel(void) __attribute__((weak));
//   The host's room exists (code) / changed (players) / closed: the service lists it (visibility, mode, settings).
void mp_svc_room_hosted(const char* code, const MpSettings* settings, uint8_t visibility, int players) __attribute__((weak));
void mp_svc_room_closed(const char* code) __attribute__((weak));

// netx (mp_portmap.cpp): the router port mapping, pumped by MpCore's helper thread (outside the core lock); weak as above.
void mp_portmap_pump(void) __attribute__((weak));
bool mp_portmap_busy(void) __attribute__((weak));
void mp_portmap_finish(unsigned maxMs) __attribute__((weak));
//   fix3: the online link failed with this service code (NpNetErrorCode): kept as mp_service_last_code() for the menus.
void mp_service_note_code(const char* code) __attribute__((weak));
}

#endif  // PORT_MP_TRANSPORT_H
