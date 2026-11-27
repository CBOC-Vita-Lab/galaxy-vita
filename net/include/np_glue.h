// Galaxy 3.0 multiplayer: the glue between the vendored Strikers Charged online stack (net/src) and Galaxy.
//
// In Strikers the stack called into the game's LAN lobby (np_lan.h) and online menu (np_menu_flow.h). Galaxy has neither: these are the
// few functions the stack needs from the game, as plain C. Galaxy's session layer (platform/src/mp) implements the first group; net/src/np_glue.cpp
// has weak defaults for each, so net/ links and behaves sensibly without them. The second group is provided by net/ itself.
// No STL and no other headers here (the game's headers define their own placement new).
#ifndef NP_GLUE_H
#define NP_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

// The connection the player chose (Strikers: np_menu_flow.h): Nearby = PSP-compatible ad hoc (Vita only), Same Wi-Fi = UDP broadcast on the
// network the console is on, Online = the internet transport (NpNetHost / NpNetJoin, np_online_api.h).
enum { kNpMenuNearby = 0, kNpMenuWifi = 1, kNpMenuOnline = 2 };

// ---- implemented by Galaxy (weak defaults in net/src/np_glue.cpp) -------------------------------------------------------------------
// Game thread (the platform's link watch, the online controller's Tick). The link is gone: Wi-Fi lost or its address changed, the ad hoc cell
// closed, the console slept, or no online member is connected any more. Default: VLink::Default().SetLost(true) (every send fails from then on)
// and NpNetOnLinkLost(why) when an online session runs.
void NpLanLinkLost(const char* why);
// Online, game thread: one member's connected path ended (lost, closed, left the room). In the star topology a host gets this for a guest that
// dropped and keeps the others; NpLanLinkLost follows only when no member is connected any more. Default: logs.
void NpLanPeerLost(int slot, const char* why);
// Vita power notices from the link watch (game thread). Default: NpLanLinkLost("the Vita is going to sleep" / "resumed from sleep"):
// DESIGN.md section 3, sleep or resume ends the session for that console.
void NpLanOnSuspend(void);
void NpLanOnResume(void);
// The connection chosen for this session (kNpMenu*). Default: SMG_LAN_MODE = adhoc | wifi | online (default adhoc on the Vita, wifi on the desktop).
int NpMenuLinkMode(void);

// ---- provided by net/ ------------------------------------------------------------------------------------------------------------------
// The player's name for the online room: the Vita's user name (it can be a real name: only ever a suggestion), on the desktop SMG_LINK_NAME or "".
const char* NpLanPlayerName(void);
// Bring the chosen link up (NpMenuLinkMode, or the online session when one was started): -1 failed, 0 idle (request=0 and nothing up),
// 1 working (the NetCheck dialog, punching), 2 ready (VLink::Default() has its backend; start it with VLink::Default().Start()).
int NpLanRadioPoll(int request);
// Galaxy (mpwifi44): why the last NpLanRadioPoll answered -1: 1 = the console is not connected to an access point (or has no address), 2 = anything
// else (the network module, the stack, a socket, a bind: the Wi-Fi itself may be fine), 0 = not known. *why (when not null) gets one line for the
// log, never null.
int NpLanRadioFailKind(const char** why);
// Leave: the online room is left (members told, socket closed), the ad hoc radio closed, the Wi-Fi / UDP socket closed.
void NpLanRadioStop(void);
// Once per frame on the game thread, whatever the connection: on the Vita np_vita_radio_pump() (the ad hoc radio, the Wi-Fi and sleep watch and
// NpNetPoll), on the desktop NpNetPoll(). Cheap when nothing runs.
void NpNetFrame(void);

#ifdef __cplusplus
}
#endif

#endif  // NP_GLUE_H
