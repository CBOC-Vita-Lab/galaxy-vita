// C API of the online transport for the game side (the menu, np_lan.cpp). No STL and no other headers: the game's own headers
// (np_lan.cpp) define a placement operator new that clashes with <new>. The full controller is in np_online.h.
#ifndef NP_NET_ONLINE_API_H
#define NP_NET_ONLINE_API_H

// ---- C API for the menu (np_online_api.cpp) -----------------------------------------------------------------------------------
// The menu worker's entry points. All calls are from the game thread; none blocks.
//   NpNetHost(name)        create a room; the invite code appears in the info once the server answered
//   NpNetJoin(code, name)  join the room with that invite code
//   NpNetPoll()            drive the transport (also done by the VLink backend while a game session exists)
//   NpNetGetInfo(&info)    progress for the menu: phase, invite code, members, errors
//   NpNetLeave()           leave the room and close everything
// Signaling and the HTTPS requests run on worker threads; a failure (no service, wrong code, version mismatch, nobody joined in time) ends in phase 8
// with a sentence in NpNetError() that can be shown as it is.
// The game's own lobby (Create Game / Join Game) runs over the link once NpNetReady() says a member is connected; the existing
// Multiplayer entry (NpLanMenuSelect) waits for that through NpLanRadioPoll when an online session was started.
extern "C" {
// The online service account (the menu's sign-in step stores the token): base URL(s) "https://host[,https://host2]", the bearer token, the game
// version "1.2.0" and the build fingerprint hash (hex, may be ""). NpNetHost / NpNetJoin use it for signaling. Strings are copied.
void NpNetConfigureService(const char* baseUrl, const char* token, const char* appVersion, const char* buildHash);
int NpNetHost(const char* playerName);
int NpNetJoin(const char* inviteCode, const char* playerName);
void NpNetPoll(void);
void NpNetLeave(void);
// beta diagnostics: a JSON object (the session's numbers: no name, id or address) the NEXT NpNetLeave sends with the room's leave request as
// {"summary":<json>}. Copied; "" / null clears.
void NpNetSetLeaveSummary(const char* json);
int NpNetActive(void);    // 1 while an online session (any phase) exists
int NpNetReady(void);     // 1 when at least one member is connected
// Progress for the menu without any C++ types. Phase: 0 off, 1 resolving, 2 STUN, 3 room, 4 waiting for a friend, 5 connecting, 6 ready,
// 7 relay needed (direct connection failed), 8 failed (NpNetError says why), 9 link lost.
int NpNetPhase(void);
const char* NpNetInviteCode(void);   // "" until the server created / found the room
int NpNetMemberCount(void);          // other members in the room
int NpNetConnectedCount(void);       // of those, the ones with a working path
// Galaxy (core2): the service's public player code of the member in `slot` and its virtual address (see vlink_online.cpp); 1 / 0
int NpNetMemberCode(int slot, unsigned int* vip, char* out, int cap);
// Galaxy (svcfix3): the name to show for the member in `slot` when the service says another member has the same name (members[].label: "Luma#4DXT"),
// and its virtual address; 1 / 0 (0 = show the name: not in a room, no such member, nobody shares it, or an older service without labels)
int NpNetMemberLabel(int slot, unsigned int* vip, char* out, int cap);
const char* NpNetError(void);
// Galaxy (fix3): the online service's error code behind phase 8 ("not_member": the service dropped this console from the room after
// a long silence; "version_mismatch", "room_full", "banned", ...), "" when the failure has none. For the game's own wording.
const char* NpNetErrorCode(void);
const char* NpNetWarning(void);      // the session goes on but degraded ("" if not)
// Why the last link ended, as a sentence for the toast when the lobby comes back ("The online link ended because the Vita went to sleep."), "" if none.
const char* NpNetEndReason(void);
// Link quality of the worst connected member, for the lockstep's input delay (A7) and the debug line; -1 until measured.
int NpNetRttMs(void);
int NpNetJitterMs(void);
int NpNetLossPermille(void);
// "Connection: Auto / Relay only" (privacy): 1 relay only, 0 Auto, -1 follow SMG_NET_RELAY_ONLY (np-player.txt key, 0/1). Applies to the next session.
void NpNetSetRelayOnly(int on);
// The relay (TURN): 0 not used, 1 being set up, 2 allocated, 3 failed (NpNetRelayText is a sentence for the player); NpNetUsingRelay 1 when a
// connected member is reached through it.
int NpNetRelayState(void);
const char* NpNetRelayText(void);
int NpNetUsingRelay(void);
int NpNetReconnect(void);            // phase 9 (link lost): rebuild the link in the same room; 1 when it started
void NpNetOnLinkLost(const char* why);   // the platform lost the network / resumed from sleep (np_lan.cpp NpLanLinkLost calls this)
// For NpLanRadioPoll: -2 online play was not selected, 0 idle, 1 connecting, 2 ready, -1 failed. request=1 starts a session from the
// SMG_NET_MODE environment on the desktop (host | join, SMG_NET_INVITE) when none was started through the API.
int NpNetRadioPoll(int request);
void NpNetRadioStop(void);
// Galaxy (netx): UPnP. The socket's local IPv4 address and UDP port (host byte order): 1 when known (the socket is open), else 0.
int NpNetLocalEndpoint(unsigned int* ip, unsigned short* port);
// The router forwards public ip:port (host byte order) to that socket: published as one more candidate; 0, 0 withdraws it.
void NpNetSetMappedCandidate(unsigned int ip, unsigned short port);
}

#endif  // NP_NET_ONLINE_API_H
