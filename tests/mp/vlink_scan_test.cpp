// mp74: two (three) consoles on one simulated Wi-Fi whose stack refuses every CHOSEN port (the PS Vita, console log c48: 0x8041010D on
// 41100..41107 whatever the address, option or order; port 0 works and gives a port near 62950). Each console binds with the real ladder
// (net/src/vlink_vita_bind.h), then sends its discovery broadcasts the way WifiSend does: the fixed targets plus, when it ended on a
// system-chosen port, the slice of the port scan (net/src/vlink_vita_scan.h). A datagram arrives where the destination port is a bound port.
// Checked: they find each other (how fast, how many datagrams a second), a console that could bind 41100 is found at once and never scans.
#include "../../net/src/vlink_vita_bind.h"
#include "../../net/src/vlink_vita_scan.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Wifi;
struct Console {
    uint16_t ephemeral;      // the port the stack gives to a bind of port 0
    bool refuseChosen;       // the policy under test
    uint16_t port = 0;       // the port bound (after Open)
    VlinkScan::Cursor scan;
    std::vector<uint16_t> heard;   // ports of the consoles it heard (their datagrams' source)
    bool heardAny = false;
    uint64_t sent = 0;
    Console(uint16_t eph, bool refuse) : ephemeral(eph), refuseChosen(refuse) {}
    // ladder ops
    int next = 10, boundSock = -1;
    uint16_t pending = 0;
    int Socket() { return next++; }
    void Close(int) {}
    int SetBroadcast(int) { return 0; }
    int SetNbio(int) { return 0; }
    int SetReuse(int) { return 0; }
    int Bind(int s, uint32_t, uint16_t p) {
        if (p != 0 && refuseChosen) return int(0x8041010Du);
        pending = p ? p : ephemeral;
        boundSock = s;
        return 0;
    }
    void Log(const char*, ...) {}
    void Open() {
        auto r = VlinkBind::Open(*this, 0xC0A80247u, 41100, 8);
        port = r.socket >= 0 ? pending : 0;
        if (r.socket >= 0 && r.port == 0) scan.Start(port);   // (as WifiOpen does)
    }
};

// The medium: a broadcast from `from` to the subnet on port p reaches every other console bound to p.
struct Lan {
    std::vector<Console*> cs;
    std::vector<uint64_t> perSecond = std::vector<uint64_t>(4096);
    void Broadcast(Console* from, uint16_t destPort, uint64_t nowMs) {
        ++from->sent;
        ++perSecond[(nowMs / 1000) % perSecond.size()];
        for (Console* c : cs)
            if (c != from && c->port == destPort) { c->heard.push_back(from->port); c->heardAny = true; }
    }
};

// WifiSend(broadcast): fixed targets 41100..41107 (+ 255.255.255.255:41100 is the same port) and the scan slice
static void SendBroadcast(Lan& lan, Console* c, uint64_t now) {
    for (int k = 0; k < 8; ++k)
        if (uint16_t(41100 + k) != c->port) lan.Broadcast(c, uint16_t(41100 + k), now);
    uint16_t ports[VlinkScan::kSlicePorts];
    const int n = VlinkScan::NextSlice(c->scan, now, ports);
    for (int i = 0; i < n; ++i) lan.Broadcast(c, ports[i], now);
}

// host: beacon every 500 ms; the other: probe every 1000 ms. Returns the ms until both heard the other (-1: never within limit)
static long Run(Console& a, Console& b, uint64_t limitMs, Lan* keep = nullptr) {
    Lan lan; lan.cs = {&a, &b};
    long both = -1;
    for (uint64_t t = 500; t <= limitMs; t += 50) {
        if (t % 500 == 0) SendBroadcast(lan, &a, t);
        if (t % 1000 == 0) SendBroadcast(lan, &b, t);
        // whoever heard a datagram answers it by unicast to the sender's port, so one direction is enough
        if ((a.heardAny || b.heardAny) && both < 0) both = long(t);
        if (both >= 0) break;
    }
    if (keep) *keep = lan;
    return both;
}

int main() {
    {   // the logged case: both consoles refuse chosen ports, ports 62954 and 62953 (adjacent)
        Console a{62954, true}, b{62953, true}; a.Open(); b.Open();
        CHECK(a.port == 62954 && b.port == 62953 && a.scan.on && b.scan.on);
        Lan lan; const long ms = Run(a, b, 20000, &lan);
        CHECK(ms >= 0 && ms <= 2000);
    }
    {   // 250 ports apart (a different boot state)
        Console a{62950, true}, b{63200, true}; a.Open(); b.Open();
        const long ms = Run(a, b, 30000);
        CHECK(ms >= 0 && ms <= 15000);
    }
    {   // as far apart as the range allows: found in the end (nearest-first spiral, the whole range), within a few minutes
        Console a{49200, true}, b{65500, true}; a.Open(); b.Open();
        const long ms = Run(a, b, 900000);
        CHECK(ms >= 0 && ms <= 400000);
        std::printf("far apart: %ld ms\n", ms);
    }
    {   // one console binds 41100 (a stack that allows it): the other, scanning, is found at once through the fixed targets; the fixed one does not scan
        Console a{0, false}, b{62953, true}; a.Open(); b.Open();
        CHECK(a.port == 41100 && !a.scan.on && b.scan.on);
        const long ms = Run(a, b, 5000);
        CHECK(ms >= 0 && ms <= 1000);
    }
    {   // both bind the fixed range (nothing changes from before): no scan, the first beacon is enough
        Console a{0, false}, b{0, false}; a.Open(); a.port = 41100; b.Open(); b.port = 41101;   // (the fake binds the port asked: both on the range)
        CHECK(!a.scan.on && !b.scan.on);
        const long ms = Run(a, b, 5000);
        CHECK(ms >= 0 && ms <= 1000 && a.sent <= 10 && b.sent <= 10);
    }
    {   // the cost: at most 2 slices... a console sends <= 8 fixed + 32 scan datagrams per beacon, and a slice at most every 250 ms
        Console a{62954, true}; a.Open();
        Lan lan; lan.cs = {&a};
        for (uint64_t t = 500; t <= 60000; t += 500) SendBroadcast(lan, &a, t);
        uint64_t worst = 0; for (uint64_t v : lan.perSecond) if (v > worst) worst = v;
        CHECK(worst <= 2 * 8 + 4 * VlinkScan::kSlicePorts);
        std::printf("worst datagrams/s from one host: %llu\n", (unsigned long long)worst);
    }
    {   // the spiral: nearest first, inside the range, no repeat within one full cycle, never the console's own port
        Console a{62954, true}; a.Open();
        uint16_t p[VlinkScan::kSlicePorts];
        VlinkScan::Cursor c; c.Start(62954);
        const int n = VlinkScan::NextSlice(c, 1000, p);
        CHECK(n == VlinkScan::kSlicePorts && p[0] == 62955 && p[1] == 62953 && p[2] == 62956);
        CHECK(VlinkScan::NextSlice(c, 1100, p) == 0 && VlinkScan::NextSlice(c, 1250, p) == VlinkScan::kSlicePorts);   // (one slice per 250 ms)
        std::vector<char> seen(65536, 0); VlinkScan::Cursor d; d.Start(65535);   // an own port at the top edge: the other side only
        uint64_t t = 0; int total = 0;
        for (int i = 0; i < 700; ++i) { t += 250; const int m = VlinkScan::NextSlice(d, t, p); for (int k = 0; k < m; ++k) { CHECK(p[k] >= VlinkScan::kLo && p[k] <= VlinkScan::kHi && p[k] != 65535); if (!seen[p[k]] ) ++total; seen[p[k]] = 1; } }
        CHECK(total == 65535 - 49152);   // every other port of the range, once seen
        VlinkScan::Cursor off; CHECK(VlinkScan::NextSlice(off, 5, p) == 0);   // off: nothing
    }
    std::printf("vlink_scan_test: %s\n", g_failed ? "FAILED" : "ok");
    return g_failed ? 1 : 0;
}
