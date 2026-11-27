// Common helpers of the online transport. See np_net.h.
#include "np_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(NP_TARGET_VITA)
#include <psp2/kernel/processmgr.h>
#endif

namespace NpNet {

const char* AddrStr(const Addr& a) {
    static char ring[8][28];
    static unsigned next = 0;
    char* out = ring[next++ & 7];
    snprintf(out, 28, "%u.%u.%u.%u:%u", a.ip >> 24, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, a.port);
    return out;
}

const char* AddrLog(const Addr& a) {
#if defined(NP_TARGET_VITA)
    static const bool full = [] { const char* v = getenv("SMG_NET_LOG_ADDRS"); return v && v[0] == '1'; }();
#else
    static const bool full = true;
#endif
    if (full || IsPrivateIp(a.ip) || a.ip == 0) return AddrStr(a);
    static char ring[8][28];
    static unsigned next = 0;
    char* out = ring[next++ & 7];
    snprintf(out, 28, "%u.%u.x.x:%u", a.ip >> 24, (a.ip >> 16) & 255, a.port);
    return out;
}

bool ParseAddr(const char* text, uint16_t defaultPort, Addr* out) {
    if (!text || !out) return false;
    unsigned p[4];
    int used = 0;
    if (sscanf(text, "%u.%u.%u.%u%n", &p[0], &p[1], &p[2], &p[3], &used) != 4) return false;
    for (unsigned v : p)
        if (v > 255) return false;
    unsigned port = defaultPort;
    if (text[used] == ':') {
        char* end = 0;
        const unsigned long v = strtoul(text + used + 1, &end, 10);
        if (end == text + used + 1 || *end || v == 0 || v > 65535) return false;
        port = unsigned(v);
    } else if (text[used]) {
        return false;
    }
    out->ip = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    out->port = uint16_t(port);
    return true;
}

bool IsPrivateIp(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 255;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127) ||
           a == 127;
}

PacketClass Classify(const uint8_t* d, size_t len) {
    if (len < 1) return PKT_UNKNOWN;
    switch (d[0]) {
    case kTypePunch: return PKT_PUNCH;
    case kTypeData: return PKT_DATA;
    case kTypeCtrl: return PKT_CTRL;
    default: break;
    }
    if ((d[0] & 0xC0) == 0x00) {
        // RFC 5389: the top two bits are zero and the magic cookie sits at bytes 4..7
        if (len >= 20 && d[4] == 0x21 && d[5] == 0x12 && d[6] == 0xA4 && d[7] == 0x42) return PKT_STUN;
        return PKT_UNKNOWN;
    }
    if ((d[0] & 0xC0) == 0x40) return PKT_CHANNEL;
    return PKT_UNKNOWN;
}

const char* NatClassName(int nat) {
    switch (nat) {
    case NAT_OPEN: return "open";
    case NAT_CONE: return "cone";
    case NAT_SYMMETRIC: return "symmetric";
    default: return "unknown";
    }
}

bool PunchPossible(int a, int b) { return !(a == NAT_SYMMETRIC && b == NAT_SYMMETRIC); }

uint64_t SteadyNowMs(void*) {
#if defined(NP_TARGET_VITA)
    return sceKernelGetProcessTimeWide() / 1000u;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec / 1000000);
#endif
}

}  // namespace NpNet
