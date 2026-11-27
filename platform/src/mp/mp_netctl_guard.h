// mpcrash32: the Vita's SceNet / SceNetCtl calls are imports of the SCE_SYSMODULE_NET module. Until a backend has loaded that module
// (net/src/vlink_vita.cpp WifiOpen, online/np_udp_vita.cpp NetUp, np_https_sys.cpp) the import stub's target is NULL, and calling it jumps to
// address 0: a hard crash (the owner's report: choosing Multiplayer on the entry screen, mp_ui_net_info -> sceNetCtlInetGetState,
// pc=0). `#pragma weak` does not help: the Vita linker gives a weak import a stub address, so `if (sceNetCtlInetGetState)` is always true.
// Every call into SceNet / SceNetCtl made outside a network backend asks this first. Header-only; desktop: always false (no such calls).
#pragma once
#if defined(__vita__)
#include <psp2/sysmodule.h>
static inline bool mp_netctl_live(void) { return sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) == SCE_SYSMODULE_LOADED; }
#else
static inline bool mp_netctl_live(void) { return false; }
#endif
