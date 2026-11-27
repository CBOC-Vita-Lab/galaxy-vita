// Galaxy 3.0 multiplayer (port side, owner: core): the Vita network watch for the session's network thread (see mp_core_sys.h),
// and the read-only Wi-Fi Turbo detection. Desktop: no-ops (the Wi-Fi is always "up").
//   - keep-awake: sceKernelPowerTick(DEFAULT) at 1 Hz in a lobby / session (the "disable auto suspend" flag alone left the screen free to dim and
//     switch off, and a player who waits in a bubble, spectates or sits in a lobby touches nothing); desktop: a stub that only logs it;
//   - resume from sleep: power callback resume bits (the game gets no warning before a suspend) or a gap between the process
//     clock (stands still while asleep) and the real-time clock;
//   - Wi-Fi watch: net-control callback (checked every tick), state at 1 Hz with two strikes, IP / access point changes;
//   - every 10 s in a session: the network library's lowest free memory (sceNetGetStatisticsInfo).
// SceNet / SceNetCtl are referenced weakly: the Vita link (vita/vgx-game/CMakeLists.txt) gets their stubs from net/; without
// them the watch only uses the power and clock parts.
#include "mp_core_sys.h"
#include "mp_posix_io.h"   // netfix B8b

#include "port/mp_bridge.h"
#include "mp_netctl_guard.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// netlog: keep the screen lit and the console awake while a lobby or session exists (the helper thread's `inSession`: any phase but idle, so the
// lobby, "waiting for players", a bubble, spectating all count). One tick a second; "[mp] watch: keep-awake on" when it begins and "off" when
// the session is over (the helper's first idle pass, or the thread's end). Outside a session nothing is called: the console's own timers
// run as they always did. Desktop: no power manager: the same two log lines, no call (the harness asserts them).
static void KeepAwake(bool inSession, uint64_t nowMs);
static void KeepAwakeEnd(void);
extern "C" void mp_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));   // (host tests that link only this file have no log: the keep-awake lines are skipped there)

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

#pragma weak sceNetCtlInetGetState
#pragma weak sceNetCtlInetGetInfo
#pragma weak sceNetCtlInetRegisterCallback
#pragma weak sceNetCtlInetUnregisterCallback
#pragma weak sceNetCtlCheckCallback
#pragma weak sceNetGetStatisticsInfo
extern "C" unsigned np_vita_radio_rx_late_take_max(void) __attribute__((weak));   // MP3 nocap (net/src/vlink_vita_radio.cpp)
extern "C" unsigned np_vita_radio_ring_take_peak(void) __attribute__((weak));
extern "C" void np_vita_radio_send_totals(unsigned* blocked, unsigned* err) __attribute__((weak));   // netlog
extern "C" void np_vita_radio_send_take(unsigned* ok, unsigned* blocked, unsigned* err) __attribute__((weak));   // netlog (net/src/vlink_vita_radio.cpp)

namespace {
volatile int s_resumeFlag = 0;
volatile int s_netEvent = 0;
SceUID s_powerCb = -1;
int s_netCid = -1;
bool s_netCbOn = false;
uint64_t s_lastTick1Hz = 0, s_lastStats = 0, s_lastClock = 0;
uint64_t s_procUs = 0, s_rtcUs = 0;
int s_strikes = 0;
bool s_wifiUp = true;
char s_ip[16] = {0};
char s_bssid[8] = {0};
bool s_haveId = false;
bool s_adHoc = false;

int PowerCb(int, int, int arg, void*) {
    const unsigned resumed = SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_AFTER_SYSTEM_RESUME;
    if ((unsigned)arg & resumed) s_resumeFlag = 1;
    return 0;
}
void NetCb(int event, void*) { s_netEvent = event ? event : 1; }

uint64_t RtcUs() {
    SceRtcTick t;
    if (sceRtcGetCurrentTick(&t) < 0) return 0;
    return t.tick;
}

// current IP / access point; false when not connected
bool ReadId(char* ip, char* bssid) {
    if (!mp_netctl_live() || !sceNetCtlInetGetInfo) return false;
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return false;
    memcpy(ip, info.ip_address, 16);
    ip[15] = 0;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_BSSID, &info) >= 0) memcpy(bssid, &info, 6);
    return ip[0] != 0;
}
}  // namespace

void mp_watch_begin(void) {
    s_resumeFlag = 0;
    s_netEvent = 0;
    s_strikes = 0;
    // fix3: the first 1 Hz look (Wi-Fi state, keep-awake) and the first 10 s statistics line come one interval after the thread
    // starts, not on its first pump (s_lastTick1Hz / s_lastStats were 0, or a previous thread's values)
    s_lastTick1Hz = s_lastStats = mp_sys_now_ms();
    s_procUs = sceKernelGetProcessTimeWide();
    s_rtcUs = RtcUs();
    s_powerCb = sceKernelCreateCallback("smg_mp_power", 0, PowerCb, 0);
    if (s_powerCb >= 0) scePowerRegisterCallback(s_powerCb);
    if (mp_netctl_live() && sceNetCtlInetRegisterCallback && sceNetCtlInetRegisterCallback(NetCb, 0, &s_netCid) >= 0) s_netCbOn = true;
    char ip[16] = {0}, bssid[8] = {0};
    s_haveId = ReadId(ip, bssid);
    if (s_haveId) {
        memcpy(s_ip, ip, sizeof s_ip);
        memcpy(s_bssid, bssid, sizeof s_bssid);
    }
}

void mp_watch_end(void) {
    KeepAwakeEnd();
    if (s_powerCb >= 0) {
        scePowerUnregisterCallback(s_powerCb);
        sceKernelDeleteCallback(s_powerCb);
        s_powerCb = -1;
    }
    if (s_netCbOn && mp_netctl_live() && sceNetCtlInetUnregisterCallback) sceNetCtlInetUnregisterCallback(s_netCid);
    s_netCbOn = false;
}

void mp_watch_set_ad_hoc(bool adHoc) {
    if (adHoc == s_adHoc) return;
    s_adHoc = adHoc;
    // a new baseline: the infrastructure state of before the ad hoc cell (or of after it) says nothing about the other mode
    s_strikes = 0;
    s_wifiUp = true;
    s_haveId = false;
    s_netEvent = 0;
}

int mp_watch_tick(bool inSession, uint64_t nowMs) {
    int out = 0;
    if (s_netCbOn && mp_netctl_live() && sceNetCtlCheckCallback) sceNetCtlCheckCallback();
    // sleep: resume bits, or the process clock stood still while the real-time clock ran (checked every 250 ms)
    if (nowMs - s_lastClock >= 250) {
        s_lastClock = nowMs;
        const uint64_t p = sceKernelGetProcessTimeWide();
        const uint64_t r = RtcUs();
        if (s_rtcUs && r > s_rtcUs && p >= s_procUs) {
            const uint64_t dr = r - s_rtcUs, dp = p - s_procUs;
            if (dr > dp + 2000000u) {
                mp_log("watch: the console slept (%u s of real time passed)", (unsigned)((dr - dp) / 1000000u));
                out |= MP_WATCH_RESUMED;
            }
        }
        s_procUs = p;
        s_rtcUs = r;
    }
    if (s_resumeFlag) {
        s_resumeFlag = 0;
        mp_log("watch: resume reported by the system");
        out |= MP_WATCH_RESUMED;
    }
    KeepAwake(inSession, nowMs);   // netlog: 1 Hz in a lobby / session, off outside one
    if (nowMs - s_lastTick1Hz >= 1000 || s_netEvent) {
        const bool tick1 = nowMs - s_lastTick1Hz >= 1000;
        if (tick1) s_lastTick1Hz = nowMs;
        s_netEvent = 0;
        int stateRc = 0;
        int state = 0;
        // (an error from the call itself, e.g. the network control layer is not started yet, says nothing about the Wi-Fi: no strike)
        if (!s_adHoc && mp_netctl_live() && sceNetCtlInetGetState && (stateRc = sceNetCtlInetGetState(&state)) >= 0) {
            const bool connected = state == SCE_NETCTL_STATE_CONNECTED;
            char ip[16] = {0}, bssid[8] = {0};
            const bool haveId = connected && ReadId(ip, bssid);
            if (!connected || !haveId) {
                if (tick1 && ++s_strikes >= 2 && s_wifiUp) {
                    s_wifiUp = false;
                    mp_log("watch: Wi-Fi lost (state %d)", state);
                    out |= MP_WATCH_WIFI_LOST;
                }
            } else {
                s_strikes = 0;
                if (s_haveId && (strcmp(ip, s_ip) != 0 || memcmp(bssid, s_bssid, 6) != 0)) {
                    mp_log("watch: the network changed (IP %s -> %s)", s_ip, ip);
                    out |= MP_WATCH_WIFI_LOST;
                }
                if (!s_wifiUp) mp_log("watch: Wi-Fi back (IP %s)", ip);
                s_wifiUp = true;
                memcpy(s_ip, ip, sizeof s_ip);
                memcpy(s_bssid, bssid, sizeof s_bssid);
                s_haveId = true;
            }
        }
    }
    if (inSession && mp_netctl_live() && sceNetGetStatisticsInfo && nowMs - s_lastStats >= 10000) {
        s_lastStats = nowMs;
        SceNetStatisticsInfo st;
        memset(&st, 0, sizeof st);
        if (sceNetGetStatisticsInfo(&st, 0) >= 0)
            mp_log("net memory: free min %d, free now %d", st.libnet_mem_free_min, st.libnet_mem_free_size);
        // MP3 nocap: Nearby: the ad hoc receive worker (core 2, default priority) - its longest wait for the core in the last 10 s, and the
        // most datagrams it had waiting for the helper's pump (net/src/vlink_vita_radio.cpp)
        if (s_adHoc && np_vita_radio_rx_late_take_max && np_vita_radio_ring_take_peak)
        {
            unsigned blocked = 0, err = 0;   // netlog: sends the radio turned away (would block / time-out: counted as sent, not retried) and failed, since it opened
            if (np_vita_radio_send_totals) np_vita_radio_send_totals(&blocked, &err);
            mp_log("net ad hoc: rx late max %.1f ms, ring peak %u, sends blocked %u (dropped, not retried), send errors %u", np_vita_radio_rx_late_take_max() / 1000.0,
                   np_vita_radio_ring_take_peak(), blocked, err);
        }
    }
    return out;
}

bool mp_watch_wifi_up(void) { return s_adHoc || s_wifiUp; }

void mp_watch_sleep_ms(unsigned ms) { sceKernelDelayThreadCB(ms * 1000u); }

// MP3 nocap: the thread census (see mp_core_sys.h). The same scan as the renderer's census (vgx_host_galaxy_vita.cpp PinLibraryThreads):
// thread ids from 0x40010001 until 500 in a row are not threads. Masks: 0x10000 core 0, 0x20000 core 1, 0x40000 core 2, 0x60000 cores
// 1-2, 0x80000 the fourth core, 0 no restriction.
extern "C" int port_vita_core3_available(void) __attribute__((weak));   // vita/runtime/memory.cpp (decided once, at start-up)
void mp_sys_thread_census(void) {
    const int core3 = port_vita_core3_available ? port_vita_core3_available() : -1;
    int seen = 0, logged = 0, missesInRow = 0;
    int onMask[6] = {0, 0, 0, 0, 0, 0};   // core 0 only, 1, 2, 1-2, 3, other
    const SceUID me = sceKernelGetThreadId();
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        ++seen;
        const unsigned mask = (unsigned)info.currentCpuAffinityMask;
        onMask[mask == 0x10000u ? 0 : mask == 0x20000u ? 1 : mask == 0x40000u ? 2 : mask == 0x60000u ? 3 : mask == 0x80000u ? 4 : 5]++;
        if (logged < 48) {
            ++logged;
            mp_log("threads: \"%s\"%s priority 0x%X, core mask 0x%X (made with 0x%X)", info.name, uid == me ? " (this helper)" : "",
                   (unsigned)info.currentPriority, mask, (unsigned)info.initCpuAffinityMask);
        }
    }
    mp_log("cores: fourth core %s; %d threads (masks: core 0 only %d, core 1 %d, core 2 %d, cores 1-2 %d, fourth core %d, other %d)",
           core3 > 0 ? "available (CapUnlocker)" : core3 == 0 ? "not available (no CapUnlocker, or GXV_CORE3=0)" : "unknown", seen, onMask[0],
           onMask[1], onMask[2], onMask[3], onMask[4], onMask[5]);
}

bool mp_sys_radio_send_take(uint32_t* ok, uint32_t* blocked, uint32_t* err) {
    unsigned a = 0, b = 0, c = 0;
    if (!np_vita_radio_send_take) { *ok = *blocked = *err = 0; return false; }
    np_vita_radio_send_take(&a, &b, &c);
    *ok = a; *blocked = b; *err = c;
    return true;
}

#else  // desktop

void mp_sys_thread_census(void) {}

bool mp_sys_radio_send_take(uint32_t* ok, uint32_t* blocked, uint32_t* err) { *ok = *blocked = *err = 0; return false; }

void mp_watch_begin(void) {}
void mp_watch_end(void) { KeepAwakeEnd(); }
void mp_watch_set_ad_hoc(bool) {}
int mp_watch_tick(bool inSession, uint64_t nowMs) {
    KeepAwake(inSession, nowMs);
    return 0;
}
bool mp_watch_wifi_up(void) { return true; }
void mp_watch_sleep_ms(unsigned ms) { mp_sys_sleep_ms(ms); }

#endif

static bool s_awakeOn = false;
static uint64_t s_awakeAt = 0;
static void KeepAwake(bool inSession, uint64_t nowMs) {
    if (!inSession) {
        KeepAwakeEnd();
        return;
    }
    if (!s_awakeOn) {
        s_awakeOn = true;
        s_awakeAt = 0;
        if (mp_log) mp_log("watch: keep-awake on (the screen stays lit and the console does not auto-suspend while the session lasts)");
    }
    if (s_awakeAt && nowMs - s_awakeAt < 1000) return;
    s_awakeAt = nowMs ? nowMs : 1;
#ifdef __vita__
    // SMG_MP_KEEP_SCREEN (env.txt, read once, default 1): the screen stays lit in a session (a bubbled, spectating or waiting player presses
    // nothing). 0: only auto-suspend is held off (the screen may dim).
    static const bool keepScreen = [] {
        const char* e = getenv("SMG_MP_KEEP_SCREEN");
        const bool on = !(e && e[0] == '0' && e[1] == 0);
        if (!on && mp_log) mp_log("watch: SMG_MP_KEEP_SCREEN=0: the screen may dim in a session");
        return on;
    }();
    sceKernelPowerTick(keepScreen ? SCE_KERNEL_POWER_TICK_DEFAULT : SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);   // (DEFAULT restarts every idle timer: dimming and screen-off too)
#endif
}
static void KeepAwakeEnd(void) {
    if (!s_awakeOn) return;
    s_awakeOn = false;
    if (mp_log) mp_log("watch: keep-awake off");
}

// ---- Wi-Fi Turbo (read-only; for the UI's tip) --------------------------------------------------------------------------
// optfix item 10: read in 1 KB chunks (the last needle-1 bytes carried over), so no 32 KB static buffer sits in bss; same answer as the
// old whole-prefix read: only the first 32 KB are searched, case-insensitive.
bool mp_file_mentions(const char* path, const char* needle) {
    const size_t n = strlen(needle);
    if (n == 0 || n > 64) return false;
    const long kMax = 32 * 1024;
    char win[1024 + 64 + 1];
    size_t keep = 0;   // bytes carried over at the start of win
    long at = 0;
    for (;;) {
        long room = kMax - at;
        if (room <= 0) return false;
        if (room > 1024) room = 1024;
        const long got = mp_io_read(path, win + keep, (size_t)room, at);
        if (got <= 0) return false;   // (also: an unreadable file is "not mentioned")
        at += got;
        const size_t total = keep + (size_t)got;
        for (size_t i = keep; i < total; i++) win[i] = (char)tolower((unsigned char)win[i]);
        win[total] = 0;
        for (size_t i = 0; i + n <= total; i++)
            if (memcmp(win + i, needle, n) == 0) return true;
        keep = total >= n - 1 ? n - 1 : total;
        memmove(win, win + total - keep, keep);
        if (got < room) return false;
    }
}

static bool FileMentions(const char* path, const char* needle) { return mp_file_mentions(path, needle); }

extern "C" bool mp_wifi_turbo_installed(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* path = mp_io_can_open("ux0:tai/config.txt") ? "ux0:tai/config.txt" : "ur0:tai/config.txt";
        cached = FileMentions(path, "wifiturbo_kernel.skprx") ? 1 : 0;
    }
    return cached == 1;
}
