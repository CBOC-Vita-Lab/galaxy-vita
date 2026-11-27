// Galaxy 3.0 multiplayer (port side, owner: core): MpTransport over a VLink socket (net/ vlink.h). Shared by the Same Wi-Fi /
// Nearby connector (mp_transport_vlink.cpp) and the online connector (mp_conn_online.cpp). Without vlink.h in the build the
// helpers compile to stubs that fail cleanly (MP_HAVE_VLINK 0).
#ifndef PORT_MP_TRANSPORT_VLINK_H
#define PORT_MP_TRANSPORT_VLINK_H

#include "mp_transport.h"

#if defined(__has_include)
#if __has_include("vlink.h")
#define MP_HAVE_VLINK 1
#endif
#endif
#ifndef MP_HAVE_VLINK
#define MP_HAVE_VLINK 0
#endif

// The VLink frame fingerprint used by every multiplayer link. Deliberately constant: the session fingerprint (build, protocol,
// data) is checked by MpCore in the beacon and HELLO, so that a different version is answered with REJECT(VERSION) instead of
// being dropped silently by the link.
#define MP_LINK_FINGERPRINT 0x534D4750u   // "SMGP"

// Open a socket bound to `port` on a started link (a VLink::Link*). Null on failure. delete it to close it.
MpTransport* mp_vlink_socket_open(void* link, uint16_t port, const char* name);

#endif  // PORT_MP_TRANSPORT_VLINK_H
