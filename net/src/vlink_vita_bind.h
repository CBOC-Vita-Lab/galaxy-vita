// Galaxy (mp73): the Same Wi-Fi socket's bind, as a pure ladder over a small operations table so a host test can script the Vita's behaviour.
//
// Console log (c46/sc70, 192.168.2.71 and an iPhone hotspot): sceNetBind of UDP 41100..41107 on the Same Wi-Fi socket returns 0x8041010D
// (SCE_NET_ERROR_EACCES) for every port. (mp74: the claim that "the online socket binds 41100 on the same console" was never in a log: the only online
// binds the console logs show are Open(0), the connection check's. c48 console log: every bind of a chosen port refused on every rung, port 0 fine.)
// The cause is not known from the code; the
// two things the failing socket does that the working online one does not are: SO_BROADCAST set BEFORE the bind, and a bind of the whole
// range with nothing else tried. So: the first rung is the online socket's own order (SO_NBIO, bind, only then SO_BROADCAST); later rungs
// vary one thing each (SO_REUSEADDR, the interface address instead of INADDR_ANY, the old order) and each rung runs on a fresh socket.
// Every setsockopt, bind and result is logged, so the console log names the rung that works (or shows that none does).
#pragma once
#include <cstdint>

namespace VlinkBind {

enum class Rung : int {
    BindThenBroadcast = 0,   // SO_NBIO, bind(ANY), SO_BROADCAST (the order of the online socket)
    ReuseAddr,               // + SO_REUSEADDR before the bind
    InterfaceAddress,        // bind(interface address) instead of INADDR_ANY
    BroadcastFirst,          // the old order: SO_BROADCAST, SO_NBIO, bind(ANY)
    AnyPort,                 // last resort: bind(ANY, port 0), the system chooses (result.port = 0: ask getsockname). Others cannot find this
                             // console by broadcast, but it can find theirs (answers are unicast to its port) and the link opens at all
    Count
};

inline const char* RungName(Rung r)
{
    switch (r) {
        case Rung::BindThenBroadcast: return "bind first, SO_BROADCAST after";
        case Rung::ReuseAddr: return "SO_REUSEADDR, bind first, SO_BROADCAST after";
        case Rung::InterfaceAddress: return "bind to the interface address, SO_BROADCAST after";
        case Rung::BroadcastFirst: return "SO_BROADCAST before the bind (the old order)";
        case Rung::AnyPort: return "any free port (port 0)";
        default: return "?";
    }
}

// Ops: int Socket(); void Close(int); int SetBroadcast(int); int SetNbio(int); int SetReuse(int);
//      int Bind(int sock, uint32_t addr /*host order, 0 = any*/, uint16_t port); void Log(const char* fmt, ...)
// Result of Open: the bound socket (>= 0) or a negative code; port/rung/refusal filled in.
struct Result {
    int socket = -1;
    uint16_t port = 0;
    Rung rung = Rung::BindThenBroadcast;
    uint32_t firstRefusal = 0;   // the first bind error code
    int broadcastRc = 0;         // result of the SO_BROADCAST call that ended up on the bound socket
    int attempts = 0;
};

template <class Ops>
Result Open(Ops& ops, uint32_t ifaceAddr, uint16_t basePort, int portTries)
{
    Result res;
    for (int r = 0; r < int(Rung::Count); ++r) {
        const Rung rung = Rung(r);
        if (rung == Rung::InterfaceAddress && !ifaceAddr) continue;
        const int s = ops.Socket();
        if (s < 0) {
            ops.Log("rung %d (%s): socket failed 0x%08X\n", r, RungName(rung), uint32_t(s));
            if (!res.firstRefusal) res.firstRefusal = uint32_t(s);
            continue;
        }
        int bcRc = 0;
        if (rung == Rung::BroadcastFirst) {
            bcRc = ops.SetBroadcast(s);
            ops.Log("rung %d: SO_BROADCAST 0x%08X (before bind)\n", r, uint32_t(bcRc));
        }
        const int nbRc = ops.SetNbio(s);
        ops.Log("rung %d (%s): socket %d SO_NBIO 0x%08X\n", r, RungName(rung), s, uint32_t(nbRc));
        if (nbRc < 0) { ops.Close(s); continue; }
        if (rung == Rung::ReuseAddr) {
            const int ruRc = ops.SetReuse(s);
            ops.Log("rung %d: SO_REUSEADDR 0x%08X\n", r, uint32_t(ruRc));
        }
        const uint32_t addr = rung == Rung::InterfaceAddress ? ifaceAddr : 0u;
        for (int k = 0; k < (rung == Rung::AnyPort ? 1 : portTries); ++k) {
            const uint16_t port = rung == Rung::AnyPort ? uint16_t(0) : uint16_t(basePort + k);
            const int rc = ops.Bind(s, addr, port);
            ++res.attempts;
            if (rc >= 0) {
                if (rung != Rung::BroadcastFirst) {
                    bcRc = ops.SetBroadcast(s);
                    ops.Log("rung %d: SO_BROADCAST 0x%08X (after bind)\n", r, uint32_t(bcRc));
                }
                ops.Log("rung %d (%s): bound %s:%u ok\n", r, RungName(rung), addr ? "interface" : "any", unsigned(port));
                res.socket = s; res.port = port; res.rung = rung; res.broadcastRc = bcRc;
                return res;
            }
            if (!res.firstRefusal) res.firstRefusal = uint32_t(rc);
            ops.Log("rung %d (%s): bind %s:%u failed 0x%08X\n", r, RungName(rung), addr ? "interface" : "any", unsigned(port), uint32_t(rc));
        }
        ops.Close(s);
    }
    return res;   // socket stays -1
}

}  // namespace VlinkBind
