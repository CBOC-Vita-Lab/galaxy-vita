// mp73: the Same Wi-Fi bind ladder (net/src/vlink_vita_bind.h) against a scripted console.
#include "../../net/src/vlink_vita_bind.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Fake {
    // behaviour switches
    bool refuseIfBroadcastBeforeBind = false;   // the hypothesis: EACCES when SO_BROADCAST is already set
    bool refuseAny = false;                     // EACCES unless a later rung's feature is used
    bool needReuse = false, needIface = false;
    bool refuseAll = false;
    bool onlyPortZero = false;                  // chosen ports refused, port 0 fine (the by-address room's failure)
    uint32_t refusedPorts = 0;                  // bit k: port base+k refused (EADDRINUSE-like)
    uint16_t base = 41100;
    int nextSock = 7;
    struct S { bool bc = false, nb = false, ru = false; bool open = true; };
    std::vector<S> socks = std::vector<S>(64);
    std::vector<std::string> log;
    std::vector<uint16_t> bound;
    int closed = 0;
    int Socket() { return nextSock++; }
    void Close(int s) { socks[s].open = false; ++closed; }
    int SetBroadcast(int s) { socks[s].bc = true; return 0; }
    int SetNbio(int s) { socks[s].nb = true; return 0; }
    int SetReuse(int s) { socks[s].ru = true; return 0; }
    int Bind(int s, uint32_t addr, uint16_t port) {
        const uint32_t EACCES_ = 0x8041010Du;
        if (refuseAll) return int(EACCES_);
        if (onlyPortZero && port != 0) return int(EACCES_);
        if (refuseIfBroadcastBeforeBind && socks[s].bc) return int(EACCES_);
        if (needReuse && !socks[s].ru) return int(EACCES_);
        if (needIface && !addr) return int(EACCES_);
        if (port >= base && port < base + 8 && ((refusedPorts >> (port - base)) & 1)) return int(EACCES_);
        bound.push_back(port);
        return 0;
    }
    void Log(const char* f, ...) { char b[300]; va_list ap; va_start(ap, f); std::vsnprintf(b, sizeof b, f, ap); va_end(ap); log.push_back(b); }
};

int main()
{
    using namespace VlinkBind;
    {   // a healthy console: the first rung, first port, SO_BROADCAST set after the bind
        Fake f; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket >= 0 && r.port == 41100 && r.rung == Rung::BindThenBroadcast);
        CHECK(f.socks[r.socket].bc && f.socks[r.socket].nb && !f.socks[r.socket].ru);
        CHECK(r.broadcastRc == 0 && r.attempts == 1);
    }
    {   // broadcast-before-bind refused: the old order would fail on every port, the first rung does not care
        Fake f; f.refuseIfBroadcastBeforeBind = true; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket >= 0 && r.rung == Rung::BindThenBroadcast && f.socks[r.socket].bc);
    }
    {   // first port taken: the next one, same rung
        Fake f; f.refusedPorts = 0x3; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket >= 0 && r.port == 41102 && r.rung == Rung::BindThenBroadcast && r.firstRefusal == 0x8041010Du && r.attempts == 3);
    }
    {   // needs SO_REUSEADDR: found on the second rung, on a fresh socket; the failed socket was closed
        Fake f; f.needReuse = true; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket >= 0 && r.rung == Rung::ReuseAddr && f.socks[r.socket].ru && f.closed == 1 && r.attempts == 9);
    }
    {   // needs the interface address
        Fake f; f.needIface = true; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket >= 0 && r.rung == Rung::InterfaceAddress && f.closed == 2);
        Fake g; g.needIface = true; auto r2 = Open(g, 0, 41100, 8);   // no address known: that rung is skipped, nothing opens
        CHECK(r2.socket < 0);
    }
    {   // nothing works: every rung tried, every socket closed, the first code kept, every attempt logged
        Fake f; f.refuseAll = true; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket < 0 && r.firstRefusal == 0x8041010Du && r.attempts == 33 && f.closed == 5);
        int fails = 0; for (auto& l : f.log) if (l.find("failed 0x8041010D") != std::string::npos) ++fails;
        CHECK(fails == 33);
    }
    {   // every chosen port refused, port 0 works: the last rung opens a socket and says port 0
        Fake f; f.onlyPortZero = true; auto r = Open(f, 0xC0A80247u, 41100, 8);
        CHECK(r.socket >= 0 && r.rung == Rung::AnyPort && r.port == 0 && r.attempts == 33 && f.socks[r.socket].bc);
    }
    std::printf(g_failed ? "vlink_bind_ladder_test: %d FAILED\n" : "vlink_bind_ladder_test: all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
