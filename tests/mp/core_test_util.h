// Galaxy 3.0 multiplayer: host unit-test helpers for the session layer (virtual time, loopback consoles, log capture).
#pragma once

#include "../../platform/src/mp/mp_core.h"
#include "../../platform/src/mp/mp_transport_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

static uint64_t g_now = 100000;
// (coordinator) g_creep: every clock read is a little later than the last, as on real hardware, where a pump's own time is
// older than the stamps its handlers and connector take while it runs.
static uint64_t g_creep = 0;
static uint64_t VNow(void*) { g_now += g_creep; return g_now; }

static std::vector<std::string> g_log;
static bool g_echo = false;
static void LogSink(const char* line) {
    g_log.push_back(line);
    if (g_echo) fprintf(stderr, "%8llu %s\n", (unsigned long long)g_now, line);
}
static int LogCount(const char* needle) {
    int n = 0;
    for (auto& l : g_log)
        if (l.find(needle) != std::string::npos) n++;
    return n;
}

static int g_fail = 0;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s (t=%llu)\n", __FILE__, __LINE__, #c, \
                    (unsigned long long)g_now);                                           \
            g_fail++;                                                                     \
        }                                                                                 \
    } while (0)

// netpack: MP_SEED=n shifts every console's random seed (and the soak's): the same tests with other timings (default 0 = as always)
static uint32_t SeedAdd() { static const uint32_t v = getenv("MP_SEED") ? (uint32_t)atoi(getenv("MP_SEED")) : 0u; return v; }

static MpCore* MakeCore(MpLoopHub& hub, uint32_t ep, const char* name, uint32_t seed, uint32_t dataHash = 0) {
    MpCoreOptions o;
    memset(&o, 0, sizeof o);
    o.useThread = false;
    o.now = VNow;
    o.seed = seed + SeedAdd() * 7919u;
    MpCore* c = new MpCore(o);
    c->setLoopback(&hub, ep);
    c->setDataHash(dataHash);
    c->setProfile(name, MP_LOOK_MARIO);
    return c;
}

static void Step(std::vector<MpCore*>& cs, int ms, int stepMs = 5) {
    for (int t = 0; t < ms; t += stepMs) {
        g_now += (uint64_t)stepMs;
        for (MpCore* c : cs)
            if (c) c->pump();
    }
}

static MpHostParams HostParams(MpMode mode = MP_MODE_TOGETHER, int maxPlayers = 4) {
    MpHostParams p;
    memset(&p, 0, sizeof p);
    p.conn = MP_CONN_LOOPBACK;
    p.settings.mode = (uint8_t)mode;
    p.settings.maxPlayers = (uint8_t)maxPlayers;
    p.settings.interaction = 1;
    p.settings.sharedKills = 1;
    p.settings.rivals = 1;
    p.settings.raceWaitSec = 18;
    p.settings.nameTags = 1;
    p.settings.flags = 7;
    return p;
}

static MpUiPhase Phase(MpCore* c) {
    MpUiStatus s;
    c->uiStatus(&s);
    return s.phase;
}
[[maybe_unused]] static MpUiError Error(MpCore* c) {
    MpUiStatus s;
    c->uiStatus(&s);
    return s.error;
}

// Scan for a room and join the first one (returns false when it never appears).
static bool ScanJoin(std::vector<MpCore*>& all, MpCore* g, int maxMs = 3000) {
    for (int t = 0; t < maxMs; t += 50) {
        const int n = g->uiLocalScan(MP_CONN_LOOPBACK);
        for (int i = 0; i < n; i++) {
            MpLocalRoom r;
            if (g->uiLocalGet(i, &r) && r.compatible) return g->uiJoinLocal(MP_CONN_LOOPBACK, i);
        }
        Step(all, 50);
    }
    return false;
}

static bool WaitPhase(std::vector<MpCore*>& all, MpCore* c, MpUiPhase p, int maxMs) {
    for (int t = 0; t < maxMs; t += 10) {
        if (Phase(c) == p) return true;
        Step(all, 10);
    }
    return Phase(c) == p;
}

// netpack: wire v3 bundles for the fuzzers. Packets (12-byte header + payload) -> 'G' 'B' proto count {len u16, packet}.
// ForgeBundle then breaks it on purpose in one of the ways a hostile sender can: count off, length zero / short / past the end /
// 65535, trailing bytes, nested bundle, wrong protocol, bad packet magic, truncation.
static std::vector<uint8_t> BundleOf(const std::vector<std::vector<uint8_t>>& pk) {
    std::vector<uint8_t> b = {'G', 'B', (uint8_t)MP_PROTO_VERSION, (uint8_t)pk.size()};
    for (auto& p : pk) { b.push_back((uint8_t)p.size()); b.push_back((uint8_t)(p.size() >> 8)); b.insert(b.end(), p.begin(), p.end()); }
    return b;
}
static void ForgeBundle(std::vector<uint8_t>& b, uint32_t (*rnd)()) {
    if (b.size() < 8) return;
    switch (rnd() % 12) {
    case 0: b[3] = (uint8_t)(b[3] + 1 + rnd() % 3); break;                      // count too large
    case 1: b[3] = (uint8_t)rnd(); break;                                      // any count
    case 2: b[3] = (uint8_t)(rnd() % 2 ? 65 : 255); break;                      // over the cap
    case 3: b[4] = b[5] = 0; break;                                            // zero length
    case 4: b[4] = b[5] = 0xFF; break;                                         // length past the end
    case 5: b[4] = (uint8_t)(b[4] + 1 + rnd() % 200); break;                    // length off
    case 6: b.push_back((uint8_t)rnd()); break;                                // trailing byte
    case 7: b.resize(b.size() - 1 - rnd() % (b.size() - 5)); break;            // truncated
    case 8: b[2] = (uint8_t)rnd(); break;                                      // protocol
    case 9: if (b.size() > 7) b[6] = (uint8_t)rnd(); break;                    // packet magic
    case 10: { std::vector<uint8_t> in = b; b = BundleOf({in, in}); break; }   // nested
    default: if (b.size() > 6) { b[4] = (uint8_t)(rnd() % 14); b[5] = 0; } break;   // a length around the header size
    }
}

// netpack: the packet of a given message type inside a datagram (plain or bundle): pointer and length, or null.
// core2: a sealed datagram is opened first (with the live cores' pair keys: MpCore::debugOpenAny); the pointer is then into a buffer
// that the next call overwrites.
static const uint8_t* FindPacket(const void* data, size_t len, uint8_t type, size_t* outLen) {
    const uint8_t* d = (const uint8_t*)data;
    static uint8_t s_plain[2048];
    if (len >= 2 && d[0] == 'G' && d[1] == 'S') {
        size_t pl = 0;
        if (!MpCore::debugOpenAny(d, len, s_plain, sizeof s_plain, &pl)) return nullptr;
        d = s_plain;
        len = pl;
    }
    if (len >= 4 && d[0] == 'G' && d[1] == 'B') {
        size_t off = 4;
        for (unsigned i = 0; i < d[3] && off + 2 <= len; i++) {
            const size_t n = d[off] | (d[off + 1] << 8);
            off += 2;
            if (n > len - off) return nullptr;
            if (n >= 12 && d[off + 3] == type) { *outLen = n; return d + off; }
            off += n;
        }
        return nullptr;
    }
    if (len >= 12 && d[3] == type) { *outLen = len; return d; }
    return nullptr;
}
