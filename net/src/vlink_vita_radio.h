// Vita ad hoc radio for the LAN link (vlink_vita.cpp, vlink.h): brings the PSP-compatible ad hoc network up through
// the system NetCheck dialog and moves whole link frames as PDP datagrams between MAC addresses. The same code runs in
// the recompiled port's Vita platform layer (vita repo, branch mp-one, src/vita_multiplayer.cpp; the radio state
// machine of the multiplayer import reduced to the raw link mode). The dialog is drawn by the renderer's swap: the
// Vita renderer calls np_vita_radio_dialog_render() before every display swap and passes np_vita_radio_dialog_active()
// to vglSwapBuffers (has_commondialog), and calls np_vita_radio_dialog_prepare(true) once its back end is ready.
// Owner thread = the thread that calls np_vita_radio_pump() (once per presented frame).
#ifndef NP_VLINK_VITA_RADIO_H
#define NP_VLINK_VITA_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Galaxy (DESIGN.md section 9): the sceNet pool, sized once for the whole stack. Whoever starts sceNet first (the ad hoc radio, the Same Wi-Fi socket,
// the online socket, the HTTPS client) gives it this pool; the others then get EBUSY and share it. 2 MiB (Strikers 1.3: 1 MiB at each site):
// Halo CE Vita found 1 MiB too small once sockets ask for larger buffers (SO_RCVBUF refused with 0x804101C9 ENOLIBMEM, then ENOBUFS on sends),
// and Galaxy runs the online socket (128 KiB receive buffer), the HTTPS client (TLS records, a TCP socket per request) and the session's
// traffic of up to 3 peers at once. The Same Wi-Fi socket adds SMG_LAN_WIFI_RCVBUF KiB when that is set. A build may raise it with
// -DNP_NET_POOL_BYTES=...; never less than 2 MiB. sceNet keeps its pool for the life of the process. Every setsockopt result is checked and logged.
// Galaxy: 1 = the sceNet / sceNetCtl stack (and the NET module) is never torn down by the ad hoc radio's cleanup (see vlink_vita_radio.cpp).
// Strikers terminated it there; -DNP_NET_KEEP_STACK=0 restores that (net/GALAXY-CHANGES.txt).
#ifndef NP_NET_KEEP_STACK
#define NP_NET_KEEP_STACK 1
#endif
#ifndef NP_NET_POOL_BYTES
#define NP_NET_POOL_BYTES (2u * 1024u * 1024u)
#endif

typedef enum { NP_RADIO_OK, NP_RADIO_AGAIN, NP_RADIO_CLOSED, NP_RADIO_LIMIT, NP_RADIO_INVALID, NP_RADIO_WRONG_PHASE } NpRadioStatus;
typedef struct { uint8_t bytes[6]; } NpRadioAddress;
typedef enum { NP_RADIO_DOWN, NP_RADIO_STARTING, NP_RADIO_DIALOG, NP_RADIO_OPEN, NP_RADIO_CLOSING } NpRadioPhase;
typedef struct {
    NpRadioPhase phase;
    NpRadioStatus status;
    const char *stage;
    uint32_t sce_error, rx_dropped;
    bool cleanup_stalled;
    uint32_t oversize;   // sends refused because the datagram was longer than SCE_NET_ADHOC_PDP_MFS (the link fragments, so this stays 0)
} NpRadioSnapshot;

#ifdef __cplusplus
extern "C" {
#endif
// Owner-thread operations (the game thread, from the presented-frame hook). Close is asynchronous: pump until DOWN.
// Both consoles use the same NetCheck mode (SMG_LAN_DIALOG=conn|create|join, default conn). Pump until OPEN.
NpRadioStatus np_vita_radio_begin_link(void);
// OPEN only. mac == NULL broadcasts (FF:FF:FF:FF:FF:FF plus every known peer).
int np_vita_radio_link_send(const uint8_t *mac, const void *bytes, size_t length);
// Non-blocking: frame length, 0 when nothing is waiting, <0 when the radio is not open.
int np_vita_radio_link_recv(uint8_t mac[6], void *bytes, size_t capacity);
bool np_vita_radio_link_mac(uint8_t mac[6]);
void np_vita_radio_pump(void);
void np_vita_radio_close(void);
void np_vita_radio_suspend(void);
void np_vita_radio_snapshot(NpRadioSnapshot *out);
// Galaxy: another path (Same Wi-Fi, online) is opening: logs one line if the last ad hoc close gave up on a teardown call (cell / Term). No-op otherwise.
void np_vita_radio_note_leftovers(const char *who);
void np_vita_radio_set_dialog_ready(bool ready);
// Renderer startup, only after its surfaces/pools and graphics checks succeeded. Returns 0 (an int, not NpRadioStatus:
// the renderer declares it weakly and has no enum) or a nonzero NpRadioStatus value.
int np_vita_radio_dialog_prepare(bool surface_ready);
// Renderer only: process one operation before its CommonDialog swap.
void np_vita_radio_dialog_render(void);
bool np_vita_radio_dialog_active(void);
#ifdef __cplusplus
}
#endif
#endif
