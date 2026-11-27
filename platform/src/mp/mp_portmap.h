// Galaxy 3.0 multiplayer (port side, owner: netx): the router port mapping (UPnP IGD, net/src/online/np_upnp.*) of a host that plays
// over the internet: by address (mp_conn_direct.cpp) or through the online service (mp_conn_online.cpp: the mapped port becomes one
// more NpNet candidate, so fewer pairs fall back to the relay).
//
// One mapping at a time. The connector (under the core lock) requests and releases; the work happens in mp_portmap_pump(), called by
// MpCore's helper thread outside the core lock (never the game thread). Nothing blocks: the UPnP client's every step is non-blocking
// with its own deadlines. A release keeps the helper thread alive until the DeletePortMapping went out (mp_portmap_busy()).
//   SMG_MP_UPNP=0            no port mapping (the host shows its STUN / LAN address and asks for a manual forward)
//   SMG_MP_UPNP_SSDP=ip:port tests: search there (a fake gateway) instead of the multicast group
//   SMG_MP_UPNP_LEASE=<s>    lease asked for (default 3600; renewed at half)
#ifndef PORT_MP_PORTMAP_H
#define PORT_MP_PORTMAP_H

#include <stddef.h>
#include <stdint.h>

enum MpPortmapState : uint8_t {
    MP_PORTMAP_OFF = 0,       // nothing asked (or disabled)
    MP_PORTMAP_WORKING,       // searching the router, asking
    MP_PORTMAP_MAPPED,        // the router forwards extPort to us
    MP_PORTMAP_FAILED         // no UPnP router / refused (error says why); the address may still be known (extIp)
};

struct MpPortmapInfo {
    uint8_t state;            // MpPortmapState
    bool cgnat;               // the router's "public" address is private / CGNAT: not reachable from the internet
    uint16_t intPort, extPort;
    uint32_t extIp;           // host order; 0 unknown
    uint32_t serial;          // changes whenever anything above changes
    char error[96];
};

extern "C" {
// Ask for a mapping of UDP internalPort (localIp 0 = the console's own address). The same port again keeps the running mapping.
// Owners: the mapping lives as long as ANY owner wants it, and an owner only ever lets go of its own claim (review2 #10). A session (a room
// hosted by address or through the service) and the Connection Test are different owners: a test that ends never deletes the mapping a room
// uses, and a test that starts while a room holds the mapping just reads it. The plain functions below act as MP_PORTMAP_SESSION.
enum MpPortmapOwner : uint8_t { MP_PORTMAP_SESSION = 0, MP_PORTMAP_TEST = 1 };
// false: not accepted (a test asking for another port than the one a session holds: one mapping at a time, the session wins). A session's
// request for another port replaces the test's mapping.
bool mp_portmap_request_as(uint8_t owner, uint16_t internalPort, uint32_t localIp, const char* who);
void mp_portmap_release_as(uint8_t owner);     // this owner's claim ends; the mapping is deleted (in the background) when no owner is left
void mp_portmap_request(uint16_t internalPort, uint32_t localIp, const char* who);   // = mp_portmap_request_as(MP_PORTMAP_SESSION, ...)
void mp_portmap_release(void);                 // = mp_portmap_release_as(MP_PORTMAP_SESSION); the state goes back to OFF when nobody else holds it
void mp_portmap_info(MpPortmapInfo* out);      // any thread
void mp_portmap_pump(void);                    // MpCore's helper thread, outside the core lock
bool mp_portmap_busy(void);                    // something to pump (a mapping is held or being made or removed)
void mp_portmap_finish(unsigned maxMs);        // shutdown: remove the mapping now, waiting at most maxMs
}

#endif  // PORT_MP_PORTMAP_H
