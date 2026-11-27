// Galaxy 3.0 multiplayer: long chaos soak of the session layer (virtual time, loopback hub, ASan + UBSan + LeakSanitizer).
// 4 consoles (1 host + 3 guests) plus a 5th (always refused: room full) and one with another data fingerprint (always refused:
// version mismatch) run for SOAK_MINUTES of virtual time (default 15) under cycles of
//   CHAOS   (30-80 s): heavy loss / duplication / reordering / delay / bandwidth caps; random leaves, vanishes (link cut), console
//                      kills (no goodbye; back later on ANOTHER endpoint with the same name), host kills (migration), same-name
//                      rejoins, snapshot offers while the net is lossy; every relayed event is checked for duplicates;
//   SETTLE  (<= 120 s): the net is mildly lossy, everyone is revived and rejoins;
//   CALM    : converged? asserts: one host, same epoch / host slot, consistent roster (names, looks, slots), no stuck phase;
//             then 20 s of reliable (relayed) events + STATE flood from every console, one console stalls its poll for 1.5 s:
//             every reliable event from every origin reaches every other console exactly once and in order; a scene-start barrier
//             releases everywhere without the timeout; the host's snapshot (offered while the net is lossy) reaches every guest
//             intact; the 5th console is refused (ROOM_FULL), the mismatching one is refused (DIFFERENT_VERSION), neither joins.
// Memory: RSS must stay flat from the second cycle on (ASAN_OPTIONS=quarantine_size_mb=1 keeps ASan's own quarantine out of it).
//   usage: soak_long [minutes=15] [seed=1]
#include "core_test_util.h"
#include "Game/MP/MpFlowLogic.hpp"

#include <map>
#include <set>
#include <tuple>
#include <unistd.h>

static uint32_t g_rng = 1;
static uint32_t R() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t RR(uint32_t lo, uint32_t hi) { return lo + R() % (hi - lo + 1); }

static long RssKb() {
    FILE* f = fopen("/proc/self/statm", "r");
    long a = 0, b = 0;
    if (f) { if (fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; fclose(f); }
    return b * (sysconf(_SC_PAGESIZE) / 1024);
}

static const char* kNames[4] = {"Alex", "Luigi", "Peach", "Toad"};
enum { kGoodHash = 0xABCD1234, kBadHash = 0x11111111 };

struct Node {
    MpCore* c = nullptr;
    int idx = 0;
    uint32_t ep = 0;
    uint32_t run = 0, counter = 0, calmCounter = 0;
    uint64_t downUntil = 0;     // link cut until
    uint64_t reviveAt = 0;      // killed (no core) until
    bool wasActive = false;
    uint32_t snapCrc = 0;       // last snapshot image received (guest) / offered (host)
    uint64_t stallUntil = 0, aloneSince = 0;
    uint32_t deadEp = 0;
    MpFlowLogic::Barrier bar;
    int64_t barDoneAt = -1;
    bool barByTimeout = false;
};

struct Img { uint32_t seed, len; };
static std::map<uint32_t, Img> g_imgs;   // crc -> how to regenerate
static void MakeImage(uint32_t seed, uint32_t len, uint8_t* out) {
    uint32_t s = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < len; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; out[i] = (uint8_t)s; }
}

static std::vector<Node> N(4);
static int g_cur = -1;                 // which node's code is running (log attribution)
static void SoakSink(const char* line) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s %s", g_cur >= 0 && g_cur < 4 ? kNames[g_cur] : "?", line);
    LogSink(buf);
}
static void SoakStep(std::vector<MpCore*>& cs, int ms, int stepMs = 5) {
    for (int t = 0; t < ms; t += stepMs) {
        g_now += (uint64_t)stepMs;
        for (MpCore* c : cs) {
            g_cur = -1;
            for (auto& n : N) if (n.c == c) g_cur = n.idx;
            if (c) c->pump();
        }
    }
    g_cur = -1;
}
static MpLoopHub* g_hub;
static std::set<uint32_t> g_usedEp;
static int g_chaosDups = 0, g_chaosGaps = 0, g_chaosDelivered = 0, g_joins = 0, g_leaves = 0, g_vanish = 0, g_kills = 0, g_hostKills = 0,
           g_migrations = 0, g_cycles = 0, g_snapOffers = 0, g_snapChecked = 0, g_calmEvents = 0, g_fullRefused = 0, g_verRefused = 0,
           g_barriers = 0, g_stateRx = 0, g_rejoinSame = 0, g_rawFull = 0, g_rawVer = 0, g_strandedLeft = 0, g_manualMerge = 0;
static std::map<std::tuple<int, uint32_t, int, uint32_t>, uint32_t> g_last;               // chaos: (rx idx, rx run, origin idx, origin run)
static std::map<std::tuple<int, int, uint32_t>, uint32_t> g_calmLast;                     // calm: (rx idx, origin idx, window)
static std::map<std::tuple<int, int, uint32_t>, uint32_t> g_calmCount;
struct Dl { uint64_t t; int rx, origin; uint32_t run, cnt; int from; };
static std::vector<Dl> g_dl;
static uint32_t g_window = 0;                                                             // 0 = no calm window open
static std::map<std::pair<int, uint32_t>, uint32_t> g_calmSent;                           // (origin, window) -> sent

static std::vector<MpCore*> AllCores() {
    std::vector<MpCore*> v;
    for (auto& n : N) if (n.c) v.push_back(n.c);
    return v;
}

static uint32_t FreeEp() {
    for (int tries = 0; tries < 100; tries++) {
        const uint32_t e = RR(1, 12);
        if (!g_usedEp.count(e)) return e;
    }
    for (uint32_t e = 1; e < 14; e++) if (!g_usedEp.count(e)) return e;
    return 13;
}

static void Spawn(Node& n) {
    if (n.deadEp) { g_hub->setDown(n.deadEp, false); n.deadEp = 0; }
    n.ep = FreeEp();
    g_usedEp.insert(n.ep);
    n.c = MakeCore(*g_hub, n.ep, kNames[n.idx], 7000 + n.idx * 17 + (R() & 0xFFFF), kGoodHash);
    n.wasActive = false; n.reviveAt = 0; n.downUntil = 0;
    n.snapCrc = 0; n.barDoneAt = -1;
}
static void Kill(Node& n, uint64_t reviveAfterMs) {
    if (!n.c) return;
    g_hub->setDown(n.ep, true);    // no goodbye: the console just vanishes (the destructor's BYE goes nowhere)
    delete n.c;
    n.c = nullptr;
    g_usedEp.erase(n.ep);
    n.deadEp = n.ep;
    n.reviveAt = g_now + reviveAfterMs;
    n.wasActive = false;
}

static MpCore* HostCore() {
    for (auto& n : N) if (n.c && n.c->active() && n.c->isHost()) return n.c;
    return nullptr;
}
static int ActiveCount() { int k = 0; for (auto& n : N) if (n.c && n.c->active()) k++; return k; }

static void Offer(MpCore* host) {
    const uint32_t len = RR(1, MP_SNAPSHOT_MAX), seed = R();
    static uint8_t buf[MP_SNAPSHOT_MAX];
    MakeImage(seed, len, buf);
    host->snapshotOffer(buf, len);
    uint32_t crc = mp_crc32(0, buf, len);
    g_imgs[crc] = Img{seed, len};
    g_snapOffers++;
    for (auto& n : N) if (n.c == host) n.snapCrc = crc;
}

static bool Joinable(Node& n) {
    const int cnt = n.c->uiLocalScan(MP_CONN_LOOPBACK);
    for (int k = 0; k < cnt; k++) {
        MpLocalRoom room;
        if (n.c->uiLocalGet(k, &room) && room.compatible && room.players < room.maxPlayers) return n.c->uiJoinLocal(MP_CONN_LOOPBACK, k);
    }
    return false;
}

static void Receive(Node& n, bool calm) {
    MpMsg m;
    while (n.c && n.c->poll(&m)) {
        if (m.type == MP_MSG_KILL && m.len == 14) {
            const int on = m.data[0];
            const int kind = m.data[1];
            uint32_t w, orun, cnt;
            memcpy(&w, m.data + 2, 4); memcpy(&orun, m.data + 6, 4); memcpy(&cnt, m.data + 10, 4);
            if (on < 0 || on > 3 || on == n.idx) { CHECK(!"event from impossible origin"); continue; }
            if (kind == 0) {
                auto key = std::make_tuple(n.idx, n.run, on, orun);
                auto it = g_last.find(key);
                g_chaosDelivered++;
                g_dl.push_back({g_now, n.idx, on, orun, cnt, m.fromSlot});
                if (it != g_last.end()) {
                    if (cnt <= it->second) { if (!g_chaosDups) { fprintf(stderr, "first dup: log tail\n"); for (size_t i = g_log.size() > 300 ? g_log.size() - 300 : 0; i < g_log.size(); i++) if (!strstr(g_log[i].c_str(), "refused") && !strstr(g_log[i].c_str(), "snapshot") && !strstr(g_log[i].c_str(), "status")) fprintf(stderr, "    %s\n", g_log[i].c_str()); } g_chaosDups++; if (g_chaosDups == 1) { for (auto& d : g_dl) if (d.rx == n.idx && d.origin == on && d.run == orun && d.cnt + 8 >= cnt) fprintf(stderr, "   dl t=%llu rx=%d origin=%d run=%u cnt=%u via slot %d\n", (unsigned long long)d.t, d.rx, d.origin, d.run, d.cnt, d.from); } fprintf(stderr, "DUP chaos rx=%d origin=%d run=%u cnt=%u last=%u t=%llu\n", n.idx, on, orun, cnt, it->second, (unsigned long long)g_now); }
                    else if (cnt != it->second + 1) g_chaosGaps++;
                }
                if (it == g_last.end() || cnt > it->second) g_last[key] = cnt;
            } else {
                auto key = std::make_tuple(n.idx, on, w);
                auto it = g_calmLast.find(key);
                if (w != g_window) { CHECK(!"calm event from another window"); continue; }
                char nm[17];
                n.c->slotName(m.fromSlot, nm);
                if (strcmp(nm, kNames[on]) != 0) { CHECK(!"fromSlot name differs from origin"); fprintf(stderr, "  from slot %d name '%s' origin %s\n", m.fromSlot, nm, kNames[on]); }
                if (it != g_calmLast.end() && cnt != it->second + 1) {
                    CHECK(!"calm event out of order / duplicated / lost");
                    fprintf(stderr, "  calm rx=%d origin=%d cnt=%u after %u\n", n.idx, on, cnt, it->second);
                } else if (it == g_calmLast.end() && cnt != 0) {
                    CHECK(!"calm first event not 0");
                    fprintf(stderr, "  calm rx=%d origin=%d first cnt=%u\n", n.idx, on, cnt);
                }
                g_calmLast[key] = cnt;
                g_calmCount[key]++;
                g_calmEvents++;
            }
        } else if (m.type == MP_MSG_LOADED && m.len == 2) {
            const uint16_t ser = (uint16_t)(m.data[0] | (m.data[1] << 8));
            n.bar.noteLoaded(m.fromSlot, ser);
        } else if (m.type == MP_MSG_STATE) {
            g_stateRx++;
        }
    }
    MpCoreEvent ev;
    while (n.c && n.c->pollEvent(&ev)) {
        if (ev.kind == MP_EV_BECAME_HOST) g_migrations++;
        if (ev.kind == MP_EV_SNAPSHOT) {
            static uint8_t buf[MP_SNAPSHOT_MAX];
            const uint32_t len = n.c->snapshotGet(buf, sizeof buf);
            if (len) {
                const uint32_t crc = mp_crc32(0, buf, len);
                auto it = g_imgs.find(crc);
                if (it == g_imgs.end() || it->second.len != len) CHECK(!"received snapshot is not an offered image");
                else {
                    static uint8_t ref[MP_SNAPSHOT_MAX];
                    MakeImage(it->second.seed, len, ref);
                    if (memcmp(ref, buf, len)) CHECK(!"snapshot bytes differ from the offered image");
                    g_snapChecked++;
                }
                n.snapCrc = crc;
            }
            n.c->snapshotConsumed();
        }
    }
}

static bool Converged(std::string* why) {
    char buf[200];
    int hosts = 0, act = 0;
    uint16_t epoch = 0;
    int hostSlot = -1;
    for (auto& n : N) {
        if (!n.c || !n.c->active()) { *why = "not all active"; return false; }
        act++;
        if (n.c->isHost()) hosts++;
        if (Phase(n.c) != MP_UI_PLAYING && Phase(n.c) != MP_UI_LOBBY) { *why = "phase"; return false; }
    }
    if (hosts != 1) { snprintf(buf, sizeof buf, "%d hosts", hosts); *why = buf; return false; }
    bool first = true;
    for (auto& n : N) {
        if (first) { epoch = n.c->hostEpoch(); hostSlot = n.c->hostSlot(); first = false; }
        else if (n.c->hostEpoch() != epoch || n.c->hostSlot() != hostSlot) { *why = "epoch/hostSlot differ"; return false; }
        if (n.c->isHost() != (n.c->localSlot() == hostSlot)) { *why = "isHost vs hostSlot"; return false; }
    }
    int slotMask = 0;
    for (auto& n : N) { if (slotMask & (1 << n.c->localSlot())) { *why = "duplicate slot"; return false; } slotMask |= 1 << n.c->localSlot(); }
    for (auto& a : N)
        for (auto& b : N)
            for (int s = 0; s < 4; s++) {
                if (a.c->slotPresent(s) != b.c->slotPresent(s)) { *why = "presence differs"; return false; }
                if (!a.c->slotPresent(s)) continue;
                char n1[17], n2[17];
                a.c->slotName(s, n1); b.c->slotName(s, n2);
                if (strcmp(n1, n2)) { *why = "names differ"; return false; }
                if (a.c->slotLook(s) != b.c->slotLook(s)) { *why = "looks differ"; return false; }
            }
    for (auto& n : N) { char nm[17]; n.c->slotName(n.c->localSlot(), nm); if (strcmp(nm, kNames[n.idx])) { *why = "own name"; return false; } }
    std::vector<MpCore*> cs = AllCores();
    int seen = 0;
    for (MpCore* c : cs) for (int s = 0; s < 4; s++) if (c->slotPresent(s)) { const int bit = 1 << c->slotLook(s); (void)bit; }
    for (int s = 0; s < 4; s++) { const int bit = 1 << N[0].c->slotLook(s); if (seen & bit) { *why = "looks not unique"; return false; } seen |= bit; }
    (void)act;
    return true;
}

// per-turn: drive joining / hosting / start
static void Drive(Node& n, bool allowHostNew) {
    if (!n.c) return;
    g_cur = n.idx;
    if (n.downUntil && g_now >= n.downUntil) { g_hub->setDown(n.ep, false); n.downUntil = 0; }
    if (n.downUntil) return;
    const bool act = n.c->active();
    if (act && !n.wasActive) { n.wasActive = true; g_joins++; n.run++; n.counter = 0; }   // (a new membership: events may be caught up again)
    if (!act) {
        n.wasActive = false;
        if (Phase(n.c) == MP_UI_IDLE) {
            if (!Joinable(n)) {
                bool anyHost = false;
                for (auto& o : N) if (&o != &n && o.c && Phase(o.c) != MP_UI_IDLE) anyHost = true;   // (somebody already hosts / joins: a room exists or is coming)
                if (allowHostNew && !anyHost && R() % 300 == 0) { MpHostParams hp = HostParams(); n.c->uiHost(&hp); }
            }
        }
    } else if (n.c->isHost() && Phase(n.c) == MP_UI_LOBBY) {
        n.c->uiStart();
    }
    // A host isolated by a silent partition stays host of an empty room while its former guests run on under a new host (see the
    // report). The player's way out is to leave and rejoin: do that after 20 s alone while another console runs a session.
    if (act && n.c->isHost()) {
        int others = 0;
        for (int s = 0; s < 4; s++) if (s != n.c->localSlot() && n.c->slotPresent(s)) others++;
        if (others == 0) {
            bool elsewhere = false;
            for (auto& o : N) if (&o != &n && o.c && o.c->active() && o.c->isHost()) elsewhere = true;
            if (!n.aloneSince) n.aloneSince = g_now;
            if (elsewhere && g_now - n.aloneSince > 20000) { n.c->uiLeave(); n.aloneSince = 0; g_strandedLeft++; }
        } else n.aloneSince = 0;
    } else n.aloneSince = 0;
}

static void Turn(bool calm) {
    std::vector<MpCore*> cs = AllCores();
    g_now += 0;   // time advances in Step
    for (auto& n : N) Drive(n, true);
    g_cur = -1;
    cs = AllCores();
    SoakStep(cs, 16, 8);
    for (auto& n : N) if (n.c && g_now >= n.stallUntil) Receive(n, calm);
}

static void ChaosEvents() {
    struct Guard { ~Guard() { g_cur = -1; } } guard;
    // random membership chaos, a few per cycle
    for (auto& n : N) {
        g_cur = n.idx;
        if (!n.c || n.downUntil) continue;
        if (n.c->active()) {
            if (R() % 4 == 0) {
                uint8_t p[16];
                p[0] = (uint8_t)n.idx; p[1] = 0;
                const uint32_t z = 0;
                memcpy(p + 2, &z, 4); memcpy(p + 6, &n.run, 4); memcpy(p + 10, &n.counter, 4);
                if (n.c->send(MP_MSG_KILL, p, 14, true)) n.counter++;
            }
            if (R() % 3 == 0) { uint8_t st[51] = {0}; n.c->send(MP_MSG_STATE, st, 51, false); }
            const uint32_t r = R() % 10000;
            if (r < 40) { n.c->uiLeave(); g_leaves++; }
            else if (r < 70) { g_hub->setDown(n.ep, true); n.downUntil = g_now + RR(2000, 20000); g_vanish++; }
            else if (r < 90) { Kill(n, RR(500, 12000)); g_kills++; }
            else if (r < 110 && n.c->isHost()) { Kill(n, RR(500, 20000)); g_hostKills++; }
            else if (r < 130) { Offer(n.c->isHost() ? n.c : n.c); }
        }
    }
}

static void Randomize() {
    MpLoopImpairment imp;
    imp.loss = (float)RR(5, 30) / 100.f;
    imp.dup = (float)RR(0, 10) / 100.f;
    imp.delayMs = RR(0, 150);
    imp.jitterMs = RR(0, 200);
    imp.bytesPerSec = R() % 3 == 0 ? RR(8000, 60000) : 0;
    g_hub->setImpairment(imp, R());
}
static void Mild() {
    MpLoopImpairment imp = {0.04f, 0.02f, 10, 30, 0};
    g_hub->setImpairment(imp, R());
}

// refused joiner: tries to join the full room / the mismatching fingerprint through the normal scan + join path. The client side may
// already refuse (the beacon says full / other version: uiJoinLocal returns false); else the host rejects (error set). Never joins.
static void ExtraJoin(uint32_t hash, const char* name, uint32_t ep, MpUiError want, int* counter) {
    MpCore* x = MakeCore(*g_hub, ep, name, 999 + ep, hash);
    g_usedEp.insert(ep);
    bool tried = false, refusedLocal = false;
    for (int t = 0; t < 4000 && !tried && !refusedLocal; t += 50) {
        std::vector<MpCore*> cs = AllCores(); cs.push_back(x);
        const int cnt = x->uiLocalScan(MP_CONN_LOOPBACK);
        for (int i = 0; i < cnt && !tried && !refusedLocal; i++) {
            MpLocalRoom r;
            if (!x->uiLocalGet(i, &r)) continue;
            if (x->uiJoinLocal(MP_CONN_LOOPBACK, i)) tried = true;
            else refusedLocal = !r.compatible || r.players >= r.maxPlayers;
        }
        Step(cs, 50);
        for (auto& n : N) if (n.c) Receive(n, true);
    }
    CHECK(tried || refusedLocal);
    if (refusedLocal) (*counter)++;
    for (int t = 0; tried && t < 8000; t += 50) {
        std::vector<MpCore*> cs = AllCores(); cs.push_back(x);
        Step(cs, 50);
        for (auto& n : N) if (n.c) Receive(n, true);
        CHECK(!x->active());
        if (Phase(x) == MP_UI_IDLE || Phase(x) == MP_UI_FAILED) break;
    }
    if (tried) {
        const MpUiError e = Error(x);
        CHECK(!x->active());
        if (e != want) fprintf(stderr, "  extra %s: error %d want %d phase %d\n", name, (int)e, (int)want, (int)Phase(x));
        CHECK(e == want);
        if (e == want) (*counter)++;
    }
    for (auto& n : N) CHECK(n.c && n.c->active());
    delete x;
    g_usedEp.erase(ep);
}

// A console that does not scan (no beacon check): a raw HELLO straight at the host, as a stranger's console or a newer build would
// send it. Returns the REJECT reason in the answer, or -1 for no reject.
static int RawHello(uint32_t ep, uint32_t fp, uint8_t protoHdr, uint16_t protoBody, const char* name) {
    MpLoopSocket* sock = g_hub->open(ep, MP_SESSION_PORT);
    CHECK(sock != nullptr);
    if (!sock) return -2;
    MpCore* h = HostCore();
    CHECK(h != nullptr);
    int reason = -1;
    if (h) {
        uint8_t d[64];
        MpWriter w(d, sizeof d);
        w.u8('G'); w.u8('M'); w.u8(protoHdr); w.u8(MP_MSG_HELLO); w.u8(0xFF); w.u8(MP_HDR_RELIABLE); w.u16(100); w.u16(0); w.u16(0);
        w.u32(fp); w.u16(protoBody);
        uint8_t nm[MP_NAME_BYTES] = {0};
        strncpy((char*)nm, name, MP_NAME_BYTES);
        w.bytes(nm, sizeof nm); w.u8(0); w.u8(0); w.u32(0);
        for (int attempt = 0; attempt < 4 && reason < 0; attempt++) {   // (the reject is one unreliable datagram; a client asks again)
        g_hub->inject(MpAddr{ep, MP_SESSION_PORT}, h->debugLocalAddr(), d, w.n);
        for (int t = 0; t < 700 && reason < 0; t += 25) {
            std::vector<MpCore*> cs = AllCores();
            Step(cs, 25);
            for (auto& n : N) if (n.c) Receive(n, true);
            MpAddr from; uint8_t buf[1200];
            int r;
            while ((r = sock->recv(&from, buf, sizeof buf)) > 0)
                if (r >= 13 && buf[3] == MP_MSG_REJECT) reason = buf[12];
        }
        }
    }
    g_hub->closeSocket(sock);
    return reason;
}

int main(int argc, char** argv) {
    const int minutes = argc > 1 ? atoi(argv[1]) : 15;
    g_rng = argc > 2 ? (uint32_t)atoi(argv[2]) * 2654435761u + 12345u : 1;
    if (!g_rng) g_rng = 1;
    mp_log_set_sink(SoakSink, true);
    g_echo = getenv("SOAK_ECHO") != nullptr;
    g_hub = new MpLoopHub();
    g_hub->setClock(VNow, 0);
    for (int i = 0; i < 4; i++) { N[i].idx = i; }
    for (int i = 0; i < 4; i++) Spawn(N[i]);
    MpLoopImpairment imp0 = {0.05f, 0.02f, 5, 40, 0};
    g_hub->setImpairment(imp0, 3);
    const uint64_t t0 = g_now, tEnd = g_now + (uint64_t)minutes * 60000;
    long baseRss = 0, maxRss = 0;
    int logTrim = 0;
    enum Ph { CHAOS, SETTLE, CALM_TRAFFIC, CALM_DRAIN } ph = SETTLE;
    uint64_t phaseEnd = g_now + 120000, phaseStart = g_now;
    uint32_t stalled = 0;
    uint64_t lastMerge = 0, forceHostKillAt = 0;
    int calmSendBudget[4] = {0, 0, 0, 0};
    bool barArmed = false;
    uint32_t lastOffered = 0;
    printf("soak_long: %d min virtual, seed arg %s\n", minutes, argc > 2 ? argv[2] : "1");
    while (g_now < tEnd) {
        const bool calm = ph == CALM_TRAFFIC || ph == CALM_DRAIN;
        // revive killed consoles
        for (auto& n : N) if (!n.c && g_now >= n.reviveAt && (ph != CHAOS || R() % 50 == 0)) { Spawn(n); if (ph == CHAOS && R() % 2 == 0) g_rejoinSame++; }
        if (ph == CHAOS) {
            if (R() % 20 == 0) ChaosEvents();
            if (forceHostKillAt && g_now >= forceHostKillAt) {
                forceHostKillAt = 0;
                for (auto& n : N) if (n.c && n.c->active() && n.c->isHost()) { Kill(n, RR(500, 20000)); g_hostKills++; break; }
            }
            if (R() % 700 == 0) Randomize();
            if (R() % 900 == 0) { MpCore* h = HostCore(); if (h) Offer(h); }
        }
        Turn(calm);
        if (g_log.size() > 20000) { g_log.erase(g_log.begin(), g_log.begin() + 15000); logTrim++; }

        if (ph == CHAOS && g_now >= phaseEnd) {
            Mild();
            for (auto& n : N) if (n.downUntil) { g_hub->setDown(n.ep, false); n.downUntil = 0; }
            ph = SETTLE; phaseStart = g_now; phaseEnd = g_now + 120000;
        } else if (ph == SETTLE) {
            for (auto& n : N) if (n.downUntil && g_now >= n.downUntil) { g_hub->setDown(n.ep, false); n.downUntil = 0; }
            std::string why;
            bool conv = Converged(&why);
            if (conv && g_now - phaseStart > 3000) {
                // stable for a moment?
                static uint64_t convSince = 0;
                static uint64_t lastCycle = ~0ull;
                if (lastCycle != phaseStart) { convSince = g_now; lastCycle = phaseStart; }
                if (g_now - convSince < 4000) goto next;
                // calm window
                g_window++;
                ph = CALM_TRAFFIC; phaseStart = g_now; phaseEnd = g_now + 20000;
                memset(calmSendBudget, 0, sizeof calmSendBudget);
                barArmed = false;
                stalled = 0;
                for (auto& n : N) { n.calmCounter = 0; n.barDoneAt = -1; n.barByTimeout = false; }
                MpCore* h = HostCore();
                CHECK(h != nullptr);
                // the snapshot goes out while the net is lossy
                if (h) { Offer(h); lastOffered = [&]{ for (auto& n : N) if (n.c == h) return n.snapCrc; return 0u; }(); }
                // every console arms the scene-start barrier
                for (auto& n : N) n.bar.arm((uint16_t)g_window, (int64_t)g_now);
                barArmed = true;
                if (R() % 2) { MpLoopImpairment i2 = {0.08f, 0.04f, 20, 60, 0}; g_hub->setImpairment(i2, R()); }
            } else if (!conv && g_now - phaseStart > 45000 && g_now - lastMerge > 20000) {
                // The group fell into separate sessions that no protocol step reunites (see the report). A player would leave the
                // smaller one and join the bigger: do that, and count it.
                lastMerge = g_now;
                Node* win = nullptr;
                int best = -1;
                for (auto& n : N) if (n.c && n.c->active() && n.c->isHost()) {
                    int pc = 0;
                    for (int s2 = 0; s2 < 4; s2++) if (n.c->slotPresent(s2)) pc++;
                    if (pc > best) { best = pc; win = &n; }
                }
                if (win) {
                    for (auto& n : N) {
                        if (&n == win || !n.c || !n.c->active()) continue;
                        const bool member = !n.c->isHost() && n.c->hostSlot() == win->c->localSlot() && n.c->hostEpoch() == win->c->hostEpoch();
                        if (!member) { n.c->uiLeave(); g_manualMerge++; }
                    }
                }
            } else if (g_now >= phaseEnd) {
                fprintf(stderr, "FAIL: no convergence after 120 s settle (t=%llu min): %s\n", (unsigned long long)((g_now - t0) / 60000), why.c_str());
                for (auto& n : N) {
                    MpUiStatus st;
                    if (n.c) { n.c->uiStatus(&st); fprintf(stderr, "  %s active=%d host=%d slot=%d hostSlot=%d epoch=%d phase=%d err=%d\n", kNames[n.idx], n.c->active(), n.c->isHost(), n.c->localSlot(), n.c->hostSlot(), n.c->hostEpoch(), st.phase, st.error); }
                    else fprintf(stderr, "  %s no core\n", kNames[n.idx]);
                    if (n.c && n.c->active()) {
                        fprintf(stderr, "     roster:");
                        for (int s2 = 0; s2 < 4; s2++) { char nm[17]; n.c->slotName(s2, nm); fprintf(stderr, " [%d %s%s look=%d]", s2, n.c->slotPresent(s2) ? nm : "-", "", (int)n.c->slotLook(s2)); }
                        fprintf(stderr, "\n");
                    }
                }
                for (size_t i = 0; i < g_log.size(); i++) if (!strstr(g_log[i].c_str(), "refused")) fprintf(stderr, "    %s\n", g_log[i].c_str());
                g_fail++;
                return 1;
            }
        } else if (ph == CALM_TRAFFIC) {
            // reliable relayed events and STATE flood from everyone; one console stalls its poll for 1.5 s once
            if (stalled == 0 && g_now - phaseStart > 2000) {
                Node& s = N[1 + R() % 3];
                s.stallUntil = g_now + 1500; stalled = 1;
            }
            for (auto& n : N) {
                if (!n.c || !n.c->active()) { CHECK(!"console lost during the calm window"); continue; }
                if (R() % 3 == 0 && g_now - phaseStart < 15000) {
                    uint8_t p[16];
                    p[0] = (uint8_t)n.idx; p[1] = 1;
                    memcpy(p + 2, &g_window, 4); memcpy(p + 6, &n.run, 4); memcpy(p + 10, &n.calmCounter, 4);
                    if (n.c->send(MP_MSG_KILL, p, 14, true)) { g_calmSent[{n.idx, g_window}] = ++n.calmCounter; }
                }
                uint8_t st[68] = {0};
                n.c->send(MP_MSG_STATE, st, 51 + (R() % 2) * 17, false);
                // scene-start barrier: LOADED after a random delay
                if (barArmed && n.barDoneAt < 0 && g_now - phaseStart > 500 + 400u * (uint32_t)n.idx + R() % 400 && !n.bar.slotLoaded(n.c->localSlot())) {
                    const uint16_t ser = (uint16_t)g_window;
                    uint8_t b[2] = {(uint8_t)ser, (uint8_t)(ser >> 8)};
                    if (n.c->send(MP_MSG_LOADED, b, 2, true)) n.bar.noteLoaded(n.c->localSlot(), ser);
                }
            }
            if (g_now >= phaseEnd) { ph = CALM_DRAIN; phaseStart = g_now; phaseEnd = g_now + 25000; Mild(); }
        } else if (ph == CALM_DRAIN) {
            unsigned present = 0;
            for (int s = 0; s < 4; s++) if (N[0].c->slotPresent(s)) present |= 1u << s;
            for (auto& n : N) {
                if (n.barDoneAt >= 0) continue;
                if (n.bar.shouldRelease(present, (int64_t)g_now, 15000)) {
                    n.barDoneAt = (int64_t)g_now;
                    n.barByTimeout = n.bar.loadedCount(present) < MpFlowLogic::popcount4(present);
                }
            }
            if (g_now >= phaseEnd) {
                // assertions
                for (auto& n : N) { CHECK(n.c && n.c->active()); }
                std::string why;
                CHECK(Converged(&why));
                if (!why.empty()) fprintf(stderr, "  not converged at the end of the calm window: %s\n", why.c_str());
                for (auto& a : N)
                    for (auto& o : N) {
                        if (&a == &o) continue;
                        const uint32_t sent = g_calmSent[{o.idx, g_window}];
                        const uint32_t got = g_calmCount[std::make_tuple(a.idx, o.idx, g_window)];
                        if (got != sent) { fprintf(stderr, "  calm: %s got %u of %u events from %s\n", kNames[a.idx], got, sent, kNames[o.idx]); }
                        CHECK(got == sent);
                    }
                for (auto& n : N) {
                    CHECK(n.barDoneAt >= 0);
                    CHECK(!n.barByTimeout);
                    if (n.barDoneAt >= 0 && !n.barByTimeout) g_barriers++;
                    if (lastOffered && n.snapCrc != lastOffered) { fprintf(stderr, "  snapshot: %s has %08x want %08x\n", kNames[n.idx], n.snapCrc, lastOffered); CHECK(!"snapshot not delivered"); }
                }
                // the host knows every guest got it?
                // extras: the room is full; a console with another data fingerprint
                if (ActiveCount() == 4) {
                    ExtraJoin(kGoodHash, "Fifth", 20, MP_ERR_ROOM_FULL, &g_fullRefused);
                    ExtraJoin(kBadHash, "Old", 21, MP_ERR_DIFFERENT_VERSION, &g_verRefused);
                    const int rf = RawHello(22, N[0].c->fingerprint(), MP_PROTO_VERSION, MP_PROTO_VERSION, "Fifth");
                    if (rf != MP_REJECT_FULL) fprintf(stderr, "  raw hello (full room): reject reason %d\n", rf);
                    CHECK(rf == MP_REJECT_FULL);
                    const int rv = RawHello(23, kBadHash, MP_PROTO_VERSION, MP_PROTO_VERSION, "Old");
                    CHECK(rv == MP_REJECT_VERSION);
                    const int rp = RawHello(24, N[0].c->fingerprint(), MP_PROTO_VERSION + 1, MP_PROTO_VERSION + 1, "Newer");
                    CHECK(rp == MP_REJECT_VERSION);
                    if (rf == MP_REJECT_FULL) g_rawFull++;
                    if (rv == MP_REJECT_VERSION && rp == MP_REJECT_VERSION) g_rawVer++;
                    for (int s2 = 0; s2 < 4; s2++) CHECK(N[0].c->slotPresent(s2));
                }
                CHECK(Converged(&why));
                g_cycles++;
                long rss = RssKb();
                if (g_cycles == 2) baseRss = rss;
                if (rss > maxRss) maxRss = rss;
                printf("  cycle %d t=%llus: joins=%d leaves=%d vanish=%d kills=%d (host %d) migr=%d dups=%d gaps=%d calm-events=%d snaps=%d/%d rss=%ldkB hubdrop=%llu\n", g_cycles, (unsigned long long)((g_now - t0) / 1000), g_joins, g_leaves, g_vanish, g_kills, g_hostKills, g_migrations, g_chaosDups, g_chaosGaps, g_calmEvents, g_snapChecked, g_snapOffers, rss, (unsigned long long)g_hub->dropped());
                fflush(stdout);
                ph = CHAOS; phaseStart = g_now; phaseEnd = g_now + RR(30000, 80000);
                if (g_cycles % 2 == 1) forceHostKillAt = g_now + RR(3000, 25000);
                Randomize();
            }
        }
    next:;
        if (ph == SETTLE && g_now - phaseStart > 0 && false) {}
    }
    CHECK(g_chaosDups == 0);
    CHECK(g_strandedLeft == 0 && g_manualMerge == 0);   // the protocol heals split / stranded sessions by itself
    if (baseRss) {
        printf("rss: base %ld kB, max %ld kB\n", baseRss, maxRss);
        CHECK(maxRss - baseRss < 12000);
    }
    CHECK(g_cycles >= 2);
    CHECK(g_hostKills > 0 && g_kills > 0 && g_migrations > 0 && g_fullRefused > 0 && g_verRefused > 0 && g_rawFull > 0 && g_rawVer > 0);
    printf("soak_long: players who had to leave a stranded / split session by hand: stranded old hosts %d, split-session members %d\n", g_strandedLeft, g_manualMerge);
    printf("soak_long: %d cycles; joins=%d leaves=%d vanish=%d kills=%d hostkills=%d migrations=%d  chaos-events=%d dups=%d gaps=%d calm-events=%d state-rx=%d barriers=%d snapshots checked=%d offered=%d  full-refused=%d version-refused=%d\n",
           g_cycles, g_joins, g_leaves, g_vanish, g_kills, g_hostKills, g_migrations, g_chaosDelivered, g_chaosDups, g_chaosGaps, g_calmEvents, g_stateRx, g_barriers,
           g_snapChecked, g_snapOffers, g_fullRefused, g_verRefused);
    for (auto& n : N) { delete n.c; n.c = nullptr; }
    delete g_hub;
    if (g_fail) { printf("SOAK FAILED: %d checks\n", g_fail); return 1; }
    printf("soak passed\n");
    return 0;
}
