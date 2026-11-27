// Vita UDP socket of the online transport (sceNet), and DNS. Same network bring-up as the Wi-Fi LAN backend (vlink_vita.cpp WifiOpen: the
// net module, sceNetInit with a pool, sceNetCtl, a connected check), then one non-blocking datagram socket bound to the VLink port.
// Every call checks its result (the order and checks follow vitaQuake's net_udp_psp2.c); the Wi-Fi watch in vlink_vita.cpp (np_vita_lan_frame)
// closes the link through OnlineNet::OnLinkLost when the access point or the address changes.
#if defined(NP_TARGET_VITA)

#include "np_net.h"
#include "../vlink_vita_radio.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace NpNet {
namespace {

struct NetState {
    bool moduleLoaded, netOwned, netctlOwned;
    void* pool;
    uint32_t ip;   // our address when the socket was opened
    NetState() : moduleLoaded(false), netOwned(false), netctlOwned(false), pool(0), ip(0) {}
} g_state;

uint32_t ParseDotted(const char* text) {
    unsigned p[4];
    if (sscanf(text, "%u.%u.%u.%u", &p[0], &p[1], &p[2], &p[3]) != 4) return 0;
    for (unsigned v : p)
        if (v > 255) return 0;
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// The network stack up (idempotent, shares what the Wi-Fi LAN backend started). The console's own address, or 0 when Wi-Fi is not connected.
uint32_t NetUp() {
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        const int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (rc < 0 && sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) { NPN_LOG("net module not loaded (0x%08X)\n", unsigned(rc)); return 0; }   // rev32 F3: re-test: another thread may have loaded it meanwhile
        if (rc >= 0) g_state.moduleLoaded = true;
    }
    if (!g_state.netOwned) {
        const int kPool = int(NP_NET_POOL_BYTES);   // C2: 1 MiB (Galaxy: the stack-wide size, vlink_vita_radio.h) (the Wi-Fi LAN backend uses the same when it started the stack first: EBUSY below is then fine)
        void* pool = malloc(kPool);
        if (!pool) return 0;
        SceNetInitParam param;
        memset(&param, 0, sizeof param);
        param.memory = pool;
        param.size = kPool;
        const int rc = sceNetInit(&param);
        NPN_LOG("sceNetInit (online, pool %d KiB): 0x%08X\n", kPool / 1024, unsigned(rc));
        if (rc >= 0) {
            g_state.netOwned = true;
            g_state.pool = pool;
        } else {
            free(pool);
            if (unsigned(rc) != unsigned(SCE_NET_ERROR_EBUSY)) { NPN_LOG("sceNetInit failed 0x%08X\n", unsigned(rc)); return 0; }
        }
    }
    if (!g_state.netctlOwned) {
        const int rc = sceNetCtlInit();
        if (rc >= 0) g_state.netctlOwned = true;
        else if (unsigned(rc) != 0x80412102u) { NPN_LOG("sceNetCtlInit failed 0x%08X\n", unsigned(rc)); return 0; }
    }
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        NPN_LOG("Wi-Fi is not connected (state %d)\n", state);
        return 0;
    }
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return 0;
    return ParseDotted(info.ip_address);
}

class VitaUdp : public Udp {
public:
    VitaUdp() : sock_(-1), port_(0) {}
    ~VitaUdp() override { Close(); }

    bool Open(uint16_t port) override {
        if (sock_ >= 0) return true;
        // C1: infrastructure Wi-Fi and the ad hoc radio cannot run at once; whoever got the radio first keeps it
        NpRadioSnapshot snap;
        memset(&snap, 0, sizeof snap);
        np_vita_radio_snapshot(&snap);
        if (snap.phase != NP_RADIO_DOWN) {
            err_ = "The ad hoc radio is in use. Leave the ad hoc game first, then try again.";
            NPN_LOG("refused: the ad hoc radio is active (phase %d)\n", int(snap.phase));
            return false;
        }
        np_vita_radio_note_leftovers("Online play");
        err_ = "";
        // Open() runs on the game thread, on purpose and briefly: the heavy part of NetUp (loading SCE_SYSMODULE_NET, sceNetInit with its 1 MiB pool,
        // sceNetCtlInit) happens once per run, and only when nothing else did it first (the Wi-Fi LAN backend and the ad hoc code share the stack:
        // IsLoaded / EBUSY make those steps no-ops); every later Open (a second room, a link rebuild) only reads the connection state, a few
        // syscalls. Moving the socket setup to the resolver thread would add a hand-over of a half-open socket for a stall nobody has measured.
        // The time is logged when it is not tiny, so a console log shows whether that assumption holds (if it does not, this is the place to move).
        const uint64_t t0 = SteadyNowMs(0);
        const uint32_t ip = NetUp();
        const uint64_t took = SteadyNowMs(0) - t0;
        if (took >= 20) NPN_LOG("network bring-up on the game thread took %u ms\n", unsigned(took));
        if (!ip) {
            err_ = "Wi-Fi is not connected. Connect to a network and try again.";
            return false;
        }
        g_state.ip = ip;
        sock_ = sceNetSocket("smg_online", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
        if (sock_ < 0) {
            NPN_LOG("socket failed 0x%08X\n", unsigned(sock_));
            sock_ = -1;
            return false;
        }
        int one = 1;
        if (sceNetSetsockopt(sock_, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one) < 0) {
            NPN_LOG("cannot make the socket non-blocking\n");   // (checked in Strikers already)
            Close();
            return false;
        }
        int rcv = 128 * 1024;   // the game drains the socket once a frame: room for a long frame's worth of datagrams
        if (const int rc = sceNetSetsockopt(sock_, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcv, sizeof rcv); rc < 0)   // Galaxy: checked and logged
            NPN_LOG("SO_RCVBUF %d KiB refused 0x%08X (sceNet pool too small?): the system default is used\n", rcv / 1024, unsigned(rc));
        SceNetSockaddrIn local;
        bool bound = false;
        for (int t = 0; t < kPortTries && !bound; ++t) {   // C3
            memset(&local, 0, sizeof local);
            local.sin_len = sizeof local;
            local.sin_family = SCE_NET_AF_INET;
            local.sin_port = sceNetHtons(uint16_t(port + t));
            local.sin_addr.s_addr = 0;
            const int rc = sceNetBind(sock_, reinterpret_cast<SceNetSockaddr*>(&local), sizeof local);
            if (rc >= 0) {
                bound = true;
                port = uint16_t(port + t);
            } else {
                NPN_LOG("bind to UDP port %u failed (0x%08X)\n", unsigned(port + t), unsigned(rc));
            }
        }
        // Galaxy (mp73): the console refuses a bind to a chosen port with 0x8041010D (EACCES) while a bind to port 0 may work (the Same Wi-Fi socket
        // was refused on all of 41100..41107). Next: SO_REUSEADDR on the chosen ports, then port 0 and let the system choose: the room's address
        // carries the port actually bound (LocalPort), the router mapping and STUN use it too. Every result is logged.
        if (!bound) {
            int reuse = 1;
            const int rrc = sceNetSetsockopt(sock_, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &reuse, sizeof reuse);
            NPN_LOG("bind fallback: SO_REUSEADDR 0x%08X\n", unsigned(rrc));
            for (int t = 0; t < kPortTries + 1 && !bound; ++t) {
                const uint16_t want = t < kPortTries ? uint16_t(port + t) : uint16_t(0);
                memset(&local, 0, sizeof local);
                local.sin_len = sizeof local;
                local.sin_family = SCE_NET_AF_INET;
                local.sin_port = sceNetHtons(want);
                const int rc = sceNetBind(sock_, reinterpret_cast<SceNetSockaddr*>(&local), sizeof local);
                if (rc < 0) {
                    NPN_LOG("bind fallback: port %u (SO_REUSEADDR) failed 0x%08X\n", unsigned(want), unsigned(rc));
                    continue;
                }
                bound = true;
                port = want;
                if (!want) {
                    SceNetSockaddrIn got;
                    memset(&got, 0, sizeof got);
                    unsigned int len = sizeof got;
                    const int grc = sceNetGetsockname(sock_, reinterpret_cast<SceNetSockaddr*>(&got), &len);
                    port = grc >= 0 ? __builtin_bswap16(got.sin_port) : uint16_t(0);
                    NPN_LOG("bind fallback: the system chose UDP port %u (getsockname 0x%08X)\n", unsigned(port), unsigned(grc));
                    if (!port) { bound = false; break; }
                } else {
                    NPN_LOG("bind fallback: UDP port %u bound with SO_REUSEADDR\n", unsigned(port));
                }
            }
        }
        if (!bound) {
            err_ = "The network port is busy. Restart the game and try again.";
            Close();
            return false;
        }
        port_ = port;
        NPN_LOG("UDP socket bound to port %u\n", unsigned(port));   // mp74: the log never showed which port a successful online bind got
        return true;
    }
    void Close() override {
        if (sock_ >= 0) sceNetSocketClose(sock_);
        sock_ = -1;
    }
    bool IsOpen() const override { return sock_ >= 0; }

    int SendTo(const Addr& to, const void* data, size_t len) override {
        if (sock_ < 0) return -1;
        SceNetSockaddrIn sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_len = sizeof sa;
        sa.sin_family = SCE_NET_AF_INET;
        sa.sin_port = sceNetHtons(to.port);
        sa.sin_addr.s_addr = __builtin_bswap32(to.ip);
        const int n = sceNetSendto(sock_, data, unsigned(len), 0, reinterpret_cast<SceNetSockaddr*>(&sa), sizeof sa);
        if (n >= 0) return n;
        // a full send buffer or a transient route error is a lost datagram, not a dead socket
        return (unsigned(n) == unsigned(SCE_NET_ERROR_EAGAIN) || unsigned(n) == unsigned(SCE_NET_ERROR_ENOBUFS) || unsigned(n) == unsigned(SCE_NET_ERROR_EHOSTUNREACH) ||
                unsigned(n) == unsigned(SCE_NET_ERROR_ENETUNREACH))
                   ? 0
                   : -1;
    }

    int RecvFrom(Addr* from, void* buffer, size_t capacity) override {
        if (sock_ < 0) return -1;
        SceNetSockaddrIn src;
        memset(&src, 0, sizeof src);
        unsigned int srcLen = sizeof src;
        const int n = sceNetRecvfrom(sock_, buffer, unsigned(capacity), SCE_NET_MSG_DONTWAIT, reinterpret_cast<SceNetSockaddr*>(&src), &srcLen);
        if (n < 0) return (unsigned(n) == unsigned(SCE_NET_ERROR_EAGAIN) || unsigned(n) == 0x80410104u) ? 0 : -1;
        from->ip = __builtin_bswap32(src.sin_addr.s_addr);
        from->port = __builtin_bswap16(src.sin_port);
        return n;
    }

    const char* Error() const override { return err_; }
    uint32_t LocalIp() override { return g_state.ip; }
    uint32_t GatewayIp() override {
        SceNetCtlInfo info;
        memset(&info, 0, sizeof info);
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE, &info) < 0) return 0;
        return ParseDotted(info.default_route);
    }
    uint16_t LocalPort() override { return port_; }

private:
    int sock_;
    uint16_t port_;
    const char* err_ = "";
};

}  // namespace

Udp* CreatePlatformUdp() { return new VitaUdp; }

int ResolveIpv4(const char* host, uint32_t* out, int max) {
    if (max < 1 || NetUp() == 0) return 0;
    const int rid = sceNetResolverCreate("smg_dns", 0, 0);
    if (rid < 0) return 0;
    SceNetInAddr addr;
    memset(&addr, 0, sizeof addr);
    const int rc = sceNetResolverStartNtoa(rid, host, &addr, 2 * 1000 * 1000, 3, 0);   // 2 s per try, 3 tries
    sceNetResolverDestroy(rid);
    if (rc < 0) return 0;
    out[0] = __builtin_bswap32(addr.s_addr);
    return out[0] ? 1 : 0;
}

}  // namespace NpNet

// The Wi-Fi watch (vlink_vita.cpp): did the console's address change since the socket was opened?
extern "C" int np_vita_online_ip_changed(void) {
    if (!NpNet::g_state.ip) return 0;
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return 0;
    const uint32_t now = NpNet::ParseDotted(info.ip_address);
    return now != NpNet::g_state.ip ? 1 : 0;
}

#endif  // NP_TARGET_VITA
