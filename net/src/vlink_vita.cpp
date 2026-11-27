// Vita backends of VLink for the native port (vlink.h) and the platform hooks of np_lan.h (NpLanRadioPoll/Stop,
// NpLanPlayerName). The same code, with the renamed link, runs in the recompiled port's Vita platform layer (vita
// repo, branch mp-one, src/vita_lan.cpp).
//
// Two ways for consoles to reach each other:
//   * Nearby (default, SMG_LAN_MODE=adhoc): PSP-compatible ad hoc Wi-Fi, no router. The radio is
//     brought up by the state machine in vlink_vita_radio.cpp, which shows the system NetCheck dialog
//     through the renderer. Frames travel as PDP datagrams
//     between MAC addresses.
//   * Same Wi-Fi (SMG_LAN_MODE=wifi): plain UDP on the access point the Vita is already connected
//     to (also what Vita3K offers). Broadcast finds the other console.
// Only datagrams cross here. The game's own LAN protocol (lobby, reliability, lockstep) runs above.
#if defined(NP_TARGET_VITA)  // the Vita target only (the np Makefile compiles every platform/ source for every target)

#include "vlink.h"
#include "../include/np_glue.h"   // Galaxy: NpLanLinkLost / OnSuspend / OnResume / NpMenuLinkMode from the game (Strikers: np_lan.h, np_menu_flow.h)
#include "vlink_vita_radio.h"
#include "vlink_vita_bind.h"
#include "vlink_vita_scan.h"
#include "online/np_online_api.h"

#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

#include "np_log.h"
#define VLV_LOG(...) NpLogf("vlink-vita", __VA_ARGS__)   // Galaxy: also the Vita log.txt (np_log.h)

namespace {

enum class Mode { Adhoc, Wifi };

Mode ChosenMode()
{
    return NpMenuLinkMode() == kNpMenuWifi ? Mode::Wifi : Mode::Adhoc;
}

// ---- ad hoc PDP backend ----------------------------------------------------------------------
uint32_t AdhocVip(void*)
{
    // The address outlives the radio. When the cell goes during a match the game restarts its LAN session on the way back to the lobby (np_lan.cpp EnsureLanSession), and
    // that is how the player gets out: the lobby stays empty on a dead link. With no address that restart failed, and the fallback from there is the main-menu transition
    // that crashed in SHNavigation::HideButtons. Every send and receive of a closed radio simply fails.
    static uint32_t last = 0;
    uint8_t mac[6];
    if (!np_vita_radio_link_mac(mac)) return last;
    // 10.x.y.z from the low MAC bytes: stable per console, unique between two nearby Vitas.
    uint32_t vip = 0x0A000000u | uint32_t(mac[3]) << 16 | uint32_t(mac[4]) << 8 | mac[5];
    if ((vip & 0xFF) == 0 || (vip & 0xFF) == 0xFF) vip ^= 0x80;
    last = vip;
    return vip;
}

int AdhocSend(void*, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len)
{
    return np_vita_radio_link_send(broadcast || !to ? nullptr : to->bytes, data, len);
}

int AdhocRecv(void*, VLink::PeerAddr* from, void* buffer, size_t capacity)
{
    std::memset(from, 0, sizeof *from);
    return np_vita_radio_link_recv(from->bytes, buffer, capacity);
}

void AdhocClose(void*) {}  // the radio is closed by MscLanRadioStop

// PDP carries at most SCE_NET_ADHOC_PDP_MFS = 1444 bytes per datagram; the game's datagrams (up to 1,415) plus the link's 24-byte frame header
// (up to 1,496 with the largest the link accepts) are split into two PDP datagrams by the link when they do not fit (vlink.cpp SendFrame).
const VLink::Backend kAdhoc = {"adhoc", 0, AdhocVip, AdhocSend, AdhocRecv, AdhocClose, SCE_NET_ADHOC_PDP_MFS};

// ---- Wi-Fi UDP backend -----------------------------------------------------------------------
struct Wifi {
    bool moduleLoaded = false, netOwned = false, netctlOwned = false;
    void* pool = nullptr;
    int socket = -1;
    uint16_t port = 24100;            // mp75: 24100..24107. The console refuses chosen UDP ports from about 32768 up (the system's own range): survey 23 Nov, 1024-30000 bind, 40000+ and 9300 refused
    uint32_t vip = 0;                 // this console's IPv4 (host order)
    uint32_t targets[16] = {};        // broadcast addresses (host order)
    uint16_t targetPorts[16] = {};
    int targetCount = 0;
    uint32_t subnetBcast = 0;         // mp74: the subnet broadcast address (host order), where the port scan goes
    VlinkScan::Cursor scan;           // mp74: on when this console is on a system-chosen port (no chosen port could be bound)
} g_wifi;

uint32_t ParseIp(const char* text)
{
    uint32_t parts[4];
    if (std::sscanf(text, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4) return 0;
    for (uint32_t p : parts)
        if (p > 255) return 0;
    return parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
}

// Galaxy (mpwifi44): why the last Same Wi-Fi / Nearby bring-up answered -1, for the game's log and its message. kind: 1 = the console is not
// connected to an access point (or has no address): the only case in which the player is told to check the Wi-Fi settings; 2 = anything else
// (the network module, the stack, a socket, a bind): the Wi-Fi itself may be fine; 0 = nothing failed / not known.
int g_failKind = 0;
char g_failWhy[96] = "";
bool WifiFail(int kind, const char* format, uint32_t a = 0, uint32_t b = 0)
{
    g_failKind = kind;
    std::snprintf(g_failWhy, sizeof g_failWhy, format, a, b);
    VLV_LOG("Same Wi-Fi could not open: %s\n", g_failWhy);
    return false;
}

constexpr int kWifiPortTries = 8;   // Galaxy (mpwifi44): the Same Wi-Fi port and the seven after it (WifiOpen)
uint16_t g_wifiBasePort = 0;        // the configured port, while a later one of the range is in use

void WifiClose(void*)
{
    if (g_wifi.socket >= 0) sceNetSocketClose(g_wifi.socket);
    g_wifi.socket = -1;
    g_wifi.scan = VlinkScan::Cursor();
    if (g_wifiBasePort) g_wifi.port = g_wifiBasePort;   // (the next open starts at the configured port again)
    g_wifiBasePort = 0;
}

// mp74: a one-time survey for the console log (never used by the game): which binds does this console refuse? Run once per process, only when every
// chosen port of the Same Wi-Fi range was refused. Console log c48: every chosen port 0x8041010D, port 0 fine. The survey separates the hypotheses:
// a range (a fixed list of ports from 1024 up to 65535), the ports next to the one the system gave, the socket's protocol / name / type (UDP proto 17,
// empty name, P2P type), SO_REUSEPORT, a TCP socket, the interface address. One line per try: "survey: <what> -> 0x........".
template <class Ops>
void WifiPortSurvey(Ops& ops, uint16_t ownPort)
{
    static bool done = false;
    if (done) return;
    done = true;
    struct Try { const char* what; const char* name; int type; int proto; uint16_t port; bool reusePort; bool iface; };
    static const uint16_t kPorts[] = {1024, 1025, 2000, 5000, 8080, 9300, 20000, 30000, 40000, 41000, 49151, 49152, 50000, 55000, 60000, 62000, 65000, 65535};
    auto run = [&](const Try& t) {
        const int s = sceNetSocket(t.name, SCE_NET_AF_INET, t.type, t.proto);
        if (s < 0) { VLV_LOG("survey: %s -> socket failed 0x%08X\n", t.what, uint32_t(s)); return; }
        if (t.reusePort) {
            int one = 1;
            const int rrc = sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEPORT, &one, sizeof one);
            VLV_LOG("survey: %s: SO_REUSEPORT 0x%08X\n", t.what, uint32_t(rrc));
        }
        SceNetSockaddrIn local{};
        local.sin_len = sizeof local;
        local.sin_family = SCE_NET_AF_INET;
        local.sin_port = sceNetHtons(t.port);
        local.sin_addr.s_addr = t.iface ? __builtin_bswap32(g_wifi.vip) : 0;
        const int rc = sceNetBind(s, reinterpret_cast<SceNetSockaddr*>(&local), sizeof local);
        VLV_LOG("survey: %s (port %u) -> 0x%08X\n", t.what, unsigned(t.port), uint32_t(rc));
        sceNetSocketClose(s);
    };
    for (uint16_t port : kPorts) run({"udp", "smg_survey", SCE_NET_SOCK_DGRAM, 0, port, false, false});
    for (int d = 1; d <= 3; ++d) {
        run({"udp next to the system's port (+)", "smg_survey", SCE_NET_SOCK_DGRAM, 0, uint16_t(ownPort + d), false, false});
        run({"udp next to the system's port (-)", "smg_survey", SCE_NET_SOCK_DGRAM, 0, uint16_t(ownPort - d), false, false});
    }
    run({"udp proto 17", "smg_survey", SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP, 41100, false, false});
    run({"udp empty name", "", SCE_NET_SOCK_DGRAM, 0, 41100, false, false});
    run({"udp p2p type", "smg_survey", SCE_NET_SOCK_DGRAM_P2P, 0, 41100, false, false});
    run({"udp SO_REUSEPORT", "smg_survey", SCE_NET_SOCK_DGRAM, 0, 41100, true, false});
    run({"udp interface address", "smg_survey", SCE_NET_SOCK_DGRAM, 0, 9300, false, true});
    run({"tcp", "smg_survey", SCE_NET_SOCK_STREAM, 0, 41100, false, false});
    run({"tcp", "smg_survey", SCE_NET_SOCK_STREAM, 0, 1339, false, false});
    run({"tcp", "smg_survey", SCE_NET_SOCK_STREAM, 0, 9300, false, false});
    (void)ops;
}

bool WifiOpen()
{
    if (g_wifi.socket >= 0) return true;
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
            if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) return WifiFail(2, "the network module did not load");   // rev32 F3: a refusal while another thread's load completed is no failure
        } else {
            g_wifi.moduleLoaded = true;
        }
    }
    // SMG_LAN_WIFI_RCVBUF=<KiB> (wave 5 NET, default 0 = the system's default): a larger receive buffer on the Wi-Fi socket, as VitAdhoc's
    // transport_lan sets (256 KiB), so no datagram is dropped by the stack while the game thread is inside a long frame (the socket is drained
    // once a frame). The net pool grows by the same amount so the buffer cannot starve the stack. Not measured on a console: off by default.
    const int rcvKiB = [] { const char* v = std::getenv("SMG_LAN_WIFI_RCVBUF"); const int k = v ? std::atoi(v) : 0; return k < 0 ? 0 : (k > 1024 ? 1024 : k); }();
    if (!g_wifi.netOwned) {
        // The stack lives until the game ends and the online transport shares it (sceNetInit then answers EBUSY there): the same 1 MiB as its own start, not less, or
        // a Same Wi-Fi visit leaves online play's TLS and 128 KiB receive buffer a quarter of the pool.
        const int kPool = int(NP_NET_POOL_BYTES) + rcvKiB * 1024;   // Galaxy: the stack-wide size (vlink_vita_radio.h)
        g_wifi.pool = std::malloc(kPool);
        if (!g_wifi.pool) {   // lanfix: a refused pool is a failed start, not a null memory block handed to the network stack
            VLV_LOG("sceNetInit: no memory for the %d KiB net pool\n", kPool / 1024);
            return WifiFail(2, "no memory for the %u KiB net pool", uint32_t(kPool / 1024));
        }
        SceNetInitParam param{};
        param.memory = g_wifi.pool;
        param.size = kPool;
        const int rc = sceNetInit(&param);
        VLV_LOG("sceNetInit (Same Wi-Fi, pool %d KiB): 0x%08X\n", kPool / 1024, uint32_t(rc));
        if (rc >= 0) {
            g_wifi.netOwned = true;
        } else {
            std::free(g_wifi.pool);
            g_wifi.pool = nullptr;
            if (uint32_t(rc) != uint32_t(SCE_NET_ERROR_EBUSY)) {
                VLV_LOG("sceNetInit failed 0x%08X\n", rc);
                return WifiFail(2, "sceNetInit failed 0x%08X", uint32_t(rc));
            }
        }
    }
    if (!g_wifi.netctlOwned) {
        const int rc = sceNetCtlInit();
        if (rc >= 0) g_wifi.netctlOwned = true;
        else if (uint32_t(rc) != 0x80412102u) {
            VLV_LOG("sceNetCtlInit failed 0x%08X\n", rc);
            return WifiFail(2, "sceNetCtlInit failed 0x%08X", uint32_t(rc));
        }
    }
    int state = 0;
    if (const int rc = sceNetCtlInetGetState(&state); rc < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        VLV_LOG("Wi-Fi is not connected (state %d)\n", state);
        // (a refused call is not "Wi-Fi off": only a state that was read and is not CONNECTED is)
        return rc < 0 ? WifiFail(2, "sceNetCtlInetGetState failed 0x%08X", uint32_t(rc)) : WifiFail(1, "Wi-Fi is not connected (state %u)", uint32_t(state));
    }
    SceNetCtlInfo ip{}, mask{};
    if (const int rc = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &ip); rc < 0) return WifiFail(1, "Wi-Fi has no address yet (0x%08X)", uint32_t(rc));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &mask) < 0) std::strcpy(mask.netmask, "255.255.255.0");
    g_wifi.vip = ParseIp(ip.ip_address);
    // Two emulators on one computer report the same IP: SMG_LINK_VIP gives each its own virtual address (as the desktop UDP backend).
    if (const char* forced = std::getenv("SMG_LINK_VIP"); forced && *forced) {
        if (const uint32_t v = ParseIp(forced)) g_wifi.vip = v;
    }
    const uint32_t netmask = ParseIp(mask.netmask);
    g_wifi.subnetBcast = g_wifi.vip | ~netmask;
    if (!g_wifi.vip) return WifiFail(1, "Wi-Fi has no usable address");
    if (const char* port = std::getenv("SMG_LINK_PORT")) g_wifi.port = uint16_t(std::atoi(port));
    // Broadcast targets: the subnet broadcast and 255.255.255.255 on our port, plus any extra
    // "ip:port" list in SMG_LINK_BCAST (two emulator instances on one computer use two ports).
    g_wifi.targetCount = 0;
    const auto add = [](uint32_t ip, uint16_t port) {
        if (g_wifi.targetCount < 16) {
            g_wifi.targets[g_wifi.targetCount] = ip;
            g_wifi.targetPorts[g_wifi.targetCount++] = port;
        }
    };
    if (const char* list = std::getenv("SMG_LINK_BCAST"); list && *list) {
        char copy[256];
        std::snprintf(copy, sizeof copy, "%s", list);
        for (char* item = std::strtok(copy, ","); item; item = std::strtok(nullptr, ",")) {
            char* colon = std::strrchr(item, ':');
            uint16_t port = g_wifi.port;
            if (colon) {
                *colon = 0;
                port = uint16_t(std::atoi(colon + 1));
            }
            if (const uint32_t target = ParseIp(item)) add(target, port);
        }
    } else {
        // Galaxy (mpwifi44): a console whose first port was refused binds one of the next seven (below), so discovery broadcasts go to all eight
        // ports on the subnet's broadcast address (answers come back unicast to whatever port the asker has), and to the first on 255.255.255.255.
        const uint16_t base = g_wifi.port;
        for (int k = 0; k < kWifiPortTries; ++k) add(g_wifi.vip | ~netmask, uint16_t(base + k));
        add(0xFFFFFFFFu, base);
    }
    // Galaxy (mp73): the bind is a ladder (vlink_vita_bind.h): the console refused every port with 0x8041010D (EACCES) in the old order
    // (SO_BROADCAST, SO_NBIO, bind); the first rung is now the online socket's order (bind first, SO_BROADCAST after). Each rung logs every call.
    struct VitaOps {
        const char* ipText;
        int Socket()
        {
            const int s = sceNetSocket("smg_lan", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
            VLV_LOG("Same Wi-Fi socket: sceNetSocket 0x%08X (address %s)\n", uint32_t(s), ipText);
            return s;
        }
        void Close(int s) { sceNetSocketClose(s); }
        int Opt(int s, int name, const char* what)
        {
            int one = 1;
            const int rc = sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, name, &one, sizeof one);
            int now = -1;
            unsigned int len = sizeof now;
            const int grc = sceNetGetsockopt(s, SCE_NET_SOL_SOCKET, name, &now, &len);
            VLV_LOG("  %s: set 0x%08X, read back 0x%08X value %d\n", what, uint32_t(rc), uint32_t(grc), now);
            return rc;
        }
        int SetBroadcast(int s) { return Opt(s, SCE_NET_SO_BROADCAST, "SO_BROADCAST"); }
        int SetNbio(int s) { return Opt(s, SCE_NET_SO_NBIO, "SO_NBIO"); }
        int SetReuse(int s) { return Opt(s, SCE_NET_SO_REUSEADDR, "SO_REUSEADDR"); }
        int Bind(int s, uint32_t addr, uint16_t port)
        {
            SceNetSockaddrIn local{};
            local.sin_len = sizeof local;
            local.sin_family = SCE_NET_AF_INET;
            local.sin_port = sceNetHtons(port);
            local.sin_addr.s_addr = addr ? __builtin_bswap32(addr) : 0;
            VLV_LOG("  sceNetBind(socket %d, family %u, len %u, port %u (wire 0x%04X), addr 0x%08X, vport %u)\n", s, unsigned(local.sin_family),
                    unsigned(local.sin_len), unsigned(port), unsigned(local.sin_port), unsigned(local.sin_addr.s_addr), unsigned(local.sin_vport));
            return sceNetBind(s, reinterpret_cast<SceNetSockaddr*>(&local), sizeof local);
        }
        void Log(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
        {
            char line[200];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(line, sizeof line, fmt, ap);
            va_end(ap);
            VLV_LOG("%s", line);
        }
    } ops{ip.ip_address};
    const uint16_t basePort = g_wifi.port;
    const VlinkBind::Result opened = VlinkBind::Open(ops, g_wifi.vip, basePort, kWifiPortTries);
    g_wifi.socket = opened.socket;
    const uint32_t firstRefusal = opened.firstRefusal;
    const int bindRc = opened.socket >= 0 ? 0 : -1;
    if (opened.socket >= 0) {
        g_wifi.port = opened.port;
        if (!opened.port) {   // the last rung: the system chose
            SceNetSockaddrIn got{};
            unsigned int len = sizeof got;
            const int grc = sceNetGetsockname(g_wifi.socket, reinterpret_cast<SceNetSockaddr*>(&got), &len);
            g_wifi.port = grc >= 0 ? sceNetNtohs(got.sin_port) : uint16_t(basePort);
            VLV_LOG("Same Wi-Fi: every port of %u..+7 was refused; the system chose UDP port %u (getsockname 0x%08X): found by the port scan (vlink_vita_scan.h)\n",
                    unsigned(basePort), unsigned(g_wifi.port), uint32_t(grc));
            g_wifi.scan.Start(g_wifi.port);   // mp74: its broadcasts sweep the ephemeral ports too
            WifiPortSurvey(ops, g_wifi.port);
        }
        VLV_LOG("Same Wi-Fi bound by rung %d (%s), port %u, SO_BROADCAST 0x%08X, %d bind tries (netmask %s)\n", int(opened.rung), VlinkBind::RungName(opened.rung),
                unsigned(opened.port), uint32_t(opened.broadcastRc), opened.attempts, mask.netmask);
    }
    if (opened.socket >= 0 && rcvKiB > 0) {
        int bytes = rcvKiB * 1024;
        const int rc = sceNetSetsockopt(g_wifi.socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &bytes, sizeof bytes);
        int now = 0;
        unsigned int len = sizeof now;
        sceNetGetsockopt(g_wifi.socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &now, &len);
        VLV_LOG("Wi-Fi receive buffer %d KiB asked: %s (0x%08X), now %d bytes\n", rcvKiB, rc >= 0 ? "set" : "refused", uint32_t(rc), now);
    }
    if (bindRc < 0) {
        // Diagnostic only (never used): does any bind work, port 0 (the system picks)? If this is refused too, the refusal is not about the port.
        if (const int probe = sceNetSocket("smg_lan_probe", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0); probe >= 0) {
            const int rc0 = ops.Bind(probe, 0, 0);
            VLV_LOG("probe: bind to port 0 (any free port): 0x%08X\n", uint32_t(rc0));
            sceNetSocketClose(probe);
        }
        WifiClose(nullptr);
        return WifiFail(2, "bind to UDP ports %u..+7 failed, first 0x%08X", uint32_t(basePort), firstRefusal);
    }
    if (g_wifi.port != basePort) VLV_LOG("Same Wi-Fi uses UDP port %u (port %u was refused 0x%08X)\n", g_wifi.port, basePort, firstRefusal);
    g_wifiBasePort = basePort;
    g_failKind = 0;
    g_failWhy[0] = 0;
    VLV_LOG("Wi-Fi link on %s port %u (%d broadcast targets)\n", ip.ip_address, g_wifi.port, g_wifi.targetCount);
    return true;
}

uint32_t WifiVip(void*) { return g_wifi.vip; }

int WifiSendOne(uint32_t ip, uint16_t port, const void* data, size_t len)
{
    SceNetSockaddrIn to{};
    to.sin_len = sizeof to;
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(port);
    to.sin_addr.s_addr = __builtin_bswap32(ip);
    return sceNetSendto(g_wifi.socket, data, unsigned(len), 0, reinterpret_cast<SceNetSockaddr*>(&to), sizeof to);
}

int WifiSend(void*, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len)
{
    if (g_wifi.socket < 0) return -1;
    if (!broadcast && to) {
        uint32_t ip;
        uint16_t port;
        std::memcpy(&ip, to->bytes, 4);
        std::memcpy(&port, to->bytes + 4, 2);
        return WifiSendOne(ip, port, data, len);
    }
    int best = -1;
    for (int i = 0; i < g_wifi.targetCount; ++i) {
        if (g_wifi.targets[i] == g_wifi.vip && g_wifi.targetPorts[i] == g_wifi.port) continue;
        const int n = WifiSendOne(g_wifi.targets[i], g_wifi.targetPorts[i], data, len);
        if (n >= 0) best = n;
    }
    if (g_wifi.scan.on && g_wifi.subnetBcast) {   // mp74: a console on a system-chosen port sweeps the ephemeral ports (vlink_vita_scan.h)
        uint16_t ports[VlinkScan::kSlicePorts];
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const int n = VlinkScan::NextSlice(g_wifi.scan, now, ports);
        for (int i = 0; i < n; ++i)
            if (WifiSendOne(g_wifi.subnetBcast, ports[i], data, len) >= 0) best = 1;
    }
    return best;
}

int WifiRecv(void*, VLink::PeerAddr* from, void* buffer, size_t capacity)
{
    if (g_wifi.socket < 0) return 0;
    SceNetSockaddrIn src{};
    unsigned int srcLen = sizeof src;
    const int n = sceNetRecvfrom(g_wifi.socket, buffer, unsigned(capacity), SCE_NET_MSG_DONTWAIT,
                                 reinterpret_cast<SceNetSockaddr*>(&src), &srcLen);
    if (n < 0) return uint32_t(n) == uint32_t(SCE_NET_ERROR_EAGAIN) ? 0 : (uint32_t(n) == 0x80410104u ? 0 : -1);
    std::memset(from, 0, sizeof *from);
    const uint32_t ip = __builtin_bswap32(src.sin_addr.s_addr);
    const uint16_t port = __builtin_bswap16(src.sin_port);
    std::memcpy(from->bytes, &ip, 4);
    std::memcpy(from->bytes + 4, &port, 2);
    return n;
}

const VLink::Backend kWifi = {"wifi", 0, WifiVip, WifiSend, WifiRecv, WifiClose};

Mode g_active = Mode::Adhoc;
bool g_failedReported = false;
bool g_stopping = false;   // NpLanRadioStop is closing the radio: not a loss

// ---- link watch and resume (wave 5 NET, SMG_LAN_LINKWATCH, default 1) -----------------------------------------------------------
// Once per presented frame on the game thread (np_vita_radio_pump calls np_vita_lan_frame first):
//   * Wi-Fi: sceNetCtlInetGetState once a second; two looks in a row that are not CONNECTED end the link (VitAdhoc's 2-strike watch).
//   * Nearby: the radio closes itself when its cell is gone (vlink_vita_radio.cpp, the same 2-strike rule); that is reported here.
//   * Resume: Galaxy (fix3): not here. Strikers registered a power callback on whichever thread first ran this function; in Galaxy
//     that is the game thread or MpCore's helper thread (both pump), and a callback only fires on the thread that made it, so one
//     made on the helper never fired once that thread had ended. MpCore's own watch (platform/src/mp/mp_core_vita.cpp: a power
//     callback made and delivered on its helper thread, which runs whenever a session or a scan exists, plus the process clock
//     against the real-time clock) sees every resume and ends / rejoins the session, which closes this link with it.
// Each loss goes to NpLanLinkLost: every link send fails from then on and the game ends the session through its own socket-error path,
// with its own "connection lost" popup, instead of a frozen match. The 20 s keep-alive limit is unchanged (the partner's side).
bool LinkWatchOn() {
    static const int on = [] { const char* v = std::getenv("SMG_LAN_LINKWATCH"); return v && v[0] == '0' ? 0 : 1; }();
    return on != 0;
}
uint64_t g_nextWifiLookUs = 0;
int g_wifiStrikes = 0;
int g_lastRssi = -1;
bool g_lossReported = false;
NpRadioPhase g_lastPhase = NP_RADIO_DOWN;

}  // namespace

extern "C" int np_vita_online_ip_changed(void);   // online/np_udp_vita.cpp

extern "C" void np_vita_lan_frame(void) {
    // Internet play (online/): the transport is driven from here once a frame (STUN, signaling, punching also run before a game session
    // exists), and its Wi-Fi watch is the Wi-Fi branch below, extended by an address-change check.
    const bool onlineUp = NpNetActive() != 0;
    NpNetPoll();   // also pumps the HTTPS requests that outlive a session (the leave message)
    if (!LinkWatchOn()) return;
    const bool wifiUp = onlineUp || (g_active == Mode::Wifi && g_wifi.socket >= 0);
    NpRadioSnapshot snap{};
    np_vita_radio_snapshot(&snap);
    const bool radioUp = !onlineUp && g_active == Mode::Adhoc && snap.phase != NP_RADIO_DOWN;
    if (onlineUp) {
        // a lost online link was rebuilt (phase left LOST = 9): the watch is armed again
        if (NpNetPhase() != 9) { g_lossReported = false; }
        else g_lossReported = true;
    }
    if (!wifiUp && !radioUp) {
        g_lastPhase = snap.phase;
        return;
    }
    // (Galaxy fix3: no power callback here: sleep / resume is MpCore's watch, see above)
    if (radioUp) {
        // The radio closed by itself (cell gone, receive error) while nobody asked it to.
        if (g_lastPhase == NP_RADIO_OPEN && snap.phase != NP_RADIO_OPEN && !g_stopping && !g_lossReported) {
            g_lossReported = true;
            NpLanLinkLost("the ad hoc radio closed: the cell is gone");
        }
        g_lastPhase = snap.phase;
        return;
    }
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now < g_nextWifiLookUs) return;
    g_nextWifiLookUs = now + 1000000;
    int state = 0;
    const int rc = sceNetCtlInetGetState(&state);
    if (rc >= 0 && state == SCE_NETCTL_STATE_CONNECTED) {
        g_wifiStrikes = 0;
        if (onlineUp) {   // A10: signal strength once a second, kept for the log line of a loss (no network name is read or logged)
            SceNetCtlInfo ri;
            std::memset(&ri, 0, sizeof ri);
            g_lastRssi = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &ri) >= 0 ? int(ri.rssi_percentage) : -1;
        }
        if (onlineUp && !g_lossReported && np_vita_online_ip_changed()) {
            // roamed to another access point / DHCP renewal gave a new address: the socket's NAT mapping and our candidates are stale
            g_lossReported = true;
            VLV_LOG("the Wi-Fi address changed during an online session\n");
            NpLanLinkLost("Wi-Fi address changed");
        }
        return;
    }
    if (++g_wifiStrikes == 2 && !g_lossReported) {
        g_lossReported = true;
        VLV_LOG("Wi-Fi is not connected (state %d, 0x%08X) at two looks a second apart; last signal strength %d%%\n", state, uint32_t(rc), g_lastRssi);
        NpLanLinkLost("Wi-Fi lost");
    }
}

int NpLanRadioPoll(int request)
{
    const int online = NpNetRadioPoll(request);   // internet play: an online session was started through the menu API (or SMG_NET_MODE)
    if (online != -2) {
        if (online == -1) {   // (mpwifi44: an online session that failed answers for whichever link was asked for: say so)
            g_failKind = 2;
            std::snprintf(g_failWhy, sizeof g_failWhy, "an online session exists and failed (phase %d)", NpNetPhase());
        }
        return online;
    }
    // Online play was chosen but no online session exists (the menu flow was cancelled or failed, or the room is gone): never fall back to the radio.
    if (NpMenuLinkMode() == kNpMenuOnline) {
        if (request) WifiFail(2, "online play is the chosen link and has no session");
        return request ? -1 : 0;
    }
    if (request) {
        g_active = ChosenMode();
        g_stopping = false;
        g_lossReported = false;
        g_wifiStrikes = 0;
    }
    // C1: the radio is claimed by one mode at a time. A Same-Wi-Fi session does not start while the ad hoc radio of an earlier Nearby session is
    // still up or closing (infrastructure Wi-Fi and ad hoc cannot run together), and a Nearby session does not start while the Wi-Fi socket of an
    // earlier session is open. SMG_LAN_EXCLUSIVE=0 restores the old behaviour.
    static const bool exclusive = [] { const char* v = std::getenv("SMG_LAN_EXCLUSIVE"); return !(v && v[0] == '0'); }();
    if (request && exclusive) {
        if (g_active == Mode::Wifi) {
            NpRadioSnapshot other{};
            np_vita_radio_snapshot(&other);
            if (other.phase != NP_RADIO_DOWN) {
                if (other.phase != NP_RADIO_CLOSING) np_vita_radio_close();   // any phase; a close already under way is waited for
                static int logged = -1;
                if (logged != int(other.phase)) {
                    logged = int(other.phase);
                    VLV_LOG("Same Wi-Fi requested while the ad hoc radio is not down (phase %d): closing it, waiting\n", logged);
                }
                return 1;
            }
        } else if (g_wifi.socket >= 0) {
            VLV_LOG("Nearby requested while the Wi-Fi socket of an earlier session is open: closing it\n");
            WifiClose(nullptr);
        }
    }
    if (g_active == Mode::Wifi) {
        if (g_wifi.socket >= 0) return 2;
        if (!request) return 0;
        np_vita_radio_note_leftovers("Same Wi-Fi");   // (the ad hoc radio is DOWN here; logs only when its last close gave up on a teardown call)
        if (!WifiOpen()) return -1;
        VLink::Default().SetBackend(&kWifi);
        return 2;
    }
    NpRadioSnapshot snap{};
    np_vita_radio_snapshot(&snap);
    switch (snap.phase) {
    case NP_RADIO_OPEN:
        VLink::Default().SetBackend(&kAdhoc);
        g_failedReported = false;
        return 2;
    case NP_RADIO_STARTING:
    case NP_RADIO_DIALOG:
        return 1;
    case NP_RADIO_CLOSING:
        return request ? 1 : 0;  // a previous session is still releasing the radio
    case NP_RADIO_DOWN:
    default:
        if (!request) return 0;
        if (snap.status != NP_RADIO_OK && snap.status != NP_RADIO_AGAIN && !g_failedReported && snap.stage) {
            // The previous attempt ended (cancelled dialog, no partner radio, error).
            g_failedReported = true;
            VLV_LOG("ad hoc start ended at %s (status %d, error 0x%08X)\n", snap.stage, int(snap.status), snap.sce_error);
            g_failKind = 0;   // (mpwifi44: Nearby keeps its own message; the line above has the stage)
            std::snprintf(g_failWhy, sizeof g_failWhy, "the ad hoc start ended at %s (0x%08X)", snap.stage, unsigned(snap.sce_error));
            return -1;
        }
        g_failedReported = false;
        const NpRadioStatus status = np_vita_radio_begin_link();
        if (status == NP_RADIO_AGAIN) return 1;
        VLV_LOG("ad hoc start refused (%d)\n", int(status));
        g_failKind = 0;
        std::snprintf(g_failWhy, sizeof g_failWhy, "the ad hoc start was refused (%d)", int(status));
        return -1;
    }
}

// Galaxy (mpwifi44): see g_failKind. *why is never null.
int NpLanRadioFailKind(const char** why)
{
    if (why) *why = g_failWhy;
    return g_failKind;
}

void NpLanRadioStop()
{
    if (NpNetActive()) {
        NpNetRadioStop();   // leaving multiplayer: the room is left, the members told, the socket closed
        return;
    }
    g_stopping = true;
    if (g_active == Mode::Wifi) {
        WifiClose(nullptr);
        return;
    }
    NpRadioSnapshot snap{};
    np_vita_radio_snapshot(&snap);
    VLV_LOG("ad hoc closing: %u received datagram(s) dropped by the radio ring, %u send(s) refused for exceeding the PDP maximum of %d bytes\n",
            snap.rx_dropped, snap.oversize, int(SCE_NET_ADHOC_PDP_MFS));
    np_vita_radio_close();
    g_failedReported = true;  // a later DOWN after this close is not a new failure
}

const char* NpLanPlayerName()
{
    static char name[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE] = {};
    if (!name[0] && sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, reinterpret_cast<SceChar8*>(name), sizeof name) < 0)
        name[0] = 0;
    return name;
}

#endif  // NP_TARGET_VITA
