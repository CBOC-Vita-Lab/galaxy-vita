// core2: one door, many seeds. N consoles (2..4) start gathering at random moments within 3 s on the loopback hub with loss and
// jitter; each must end in ONE room with everyone, within a bound, with the same leader on every console, "Press Start" (canStart)
// on exactly one console and never on two at once at any moment; a console arriving after Start is offered the room, not merged.
//   gather_soak [-seeds n] [-loss f] [-v]
#include "core_test_util.h"
#include "../../platform/src/mp/mp_gather.h"
#include "../../platform/src/mp/mp_lan.h"

static int g_forge = 0;   // 1: a stranger beacons a "better" gathering room nobody can join; 2: ... and forged CLOSING / honest ids
static int Run(uint32_t seed, int n, float loss, bool verbose) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setImpairment(MpLoopImpairment{loss, 0.02f, 5, 40, 0}, 77 + seed);
    const char* names[4] = {"Ann", "Bob", "Cid", "Dee"};
    std::vector<MpCore*> cs;
    std::vector<MpGather*> gs;
    uint32_t r = 0x9E3779B9u ^ (seed * 2654435761u);
    auto R = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
    std::vector<uint64_t> startAt;
    const uint64_t t0 = g_now;
    for (int i = 0; i < n; i++) {
        cs.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], 500 + seed * 31 + (uint32_t)i));
        gs.push_back(new MpGather(cs.back()));
        startAt.push_back(t0 + R() % 3000);
    }
    int twoPrompts = 0, settledAt = -1;
    uint32_t fseq = 1;
    for (int ms = 0; ms < 60000; ms += 10) {
        for (int i = 0; i < n; i++)
            if (g_now >= startAt[i] && gs[i]->state() == MP_GATHER_OFF) gs[i]->start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK, g_now);
        if (g_forge && ms % 250 == 0) {   // the stranger at endpoint 99: 3 players, the lowest id: always "ahead"
            MpBeacon b;
            memset(&b, 0, sizeof b);
            b.proto = MP_PROTO_VERSION; b.mode = MP_MODE_TOGETHER; b.players = 1; b.maxPlayers = 4;
            b.flags = MP_BEACON_OPEN | MP_BEACON_GATHERING; b.fingerprint = cs[0]->fingerprint(); b.sessionId = 1; b.seq = fseq++;
            snprintf(b.name, sizeof b.name, "Evil");
            uint8_t raw[MP_BEACON_BYTES];
            const size_t bn = mp_beacon_write(raw, sizeof raw, b);
            for (int i = 0; i < n; i++) hub.inject(MpAddr{99, MP_SESSION_PORT}, MpAddr{(uint32_t)(i + 1), MP_LAN_PORT}, raw, bn);
            if (g_forge == 2) {   // forged CLOSING beacons claiming every honest room's id, from the stranger's address
                for (int i = 0; i < n; i++) {
                    const uint32_t sid = cs[i]->debugSessionId();
                    if (!sid) continue;
                    b.sessionId = sid; b.flags = MP_BEACON_OPEN | MP_BEACON_GATHERING | MP_BEACON_CLOSING; b.closingInto = 1; b.players = 4;
                    const size_t bn2 = mp_beacon_write(raw, sizeof raw, b);
                    for (int k = 0; k < n; k++) hub.inject(MpAddr{99, MP_SESSION_PORT}, MpAddr{(uint32_t)(k + 1), MP_LAN_PORT}, raw, bn2);
                }
            }
        }
        Step(cs, 10, 5);
        int prompts = 0;
        for (int i = 0; i < n; i++) {
            gs[i]->tick(g_now);
            if (gs[i]->canStart(g_now)) prompts++;
        }
        if (prompts > 1) twoPrompts++;
        // settled: everyone active in one room, every console sees n present, one prompt
        int act = 0, hostIdx = -1;
        bool same = true;
        for (int i = 0; i < n; i++) {
            if (cs[i]->active() && gs[i]->state() == MP_GATHER_GATHERING) act++;
            if (cs[i]->isHost()) hostIdx = i;
        }
        if (act == n && hostIdx >= 0) {
            const uint32_t sid = cs[hostIdx]->debugSessionId();
            int present = 0;
            for (int s = 0; s < 4; s++) present += cs[hostIdx]->slotPresent(s) ? 1 : 0;
            for (int i = 0; i < n; i++) same = same && cs[i]->debugSessionId() == sid && cs[i]->hostSlot() == cs[hostIdx]->localSlot();
            if (same && present == n && prompts == 1 && gs[hostIdx]->canStart(g_now)) { settledAt = (int)(g_now - t0); break; }
        }
        if (g_forge == 2 && ms > 20000) {
            // forged CLOSING beacons with the honest rooms' ids (raced to the first sighting) can keep honest rooms apart: the honest
            // limit (DESIGN.md 10.6). What must hold: nobody is lost (every console in a room) and every room can start.
            int inRoom = 0, hosts = 0, startable = 0;
            for (int i = 0; i < n; i++) {
                if (cs[i]->active() && gs[i]->state() == MP_GATHER_GATHERING) inRoom++;
                if (cs[i]->isHost()) { hosts++; if (gs[i]->canStart(g_now)) startable++; }
            }
            if (inRoom == n && hosts >= 1 && startable == hosts) { settledAt = (int)(g_now - t0); twoPrompts = 0; break; }
        }
    }
    bool ok = settledAt >= 0 && twoPrompts == 0;
    if (!ok)
        for (int i = 0; i < n; i++)
        {
            fprintf(stderr, "  console %d: state=%d active=%d host=%d canStart=%d session=%08x slot=%d hostSlot=%d\n", i, (int)gs[i]->state(),
                    cs[i]->active(), cs[i]->isHost(), gs[i]->canStart(g_now), (unsigned)cs[i]->debugSessionId(), cs[i]->localSlot(), cs[i]->hostSlot());
            MpBeacon hb[16];
            MpAddr ha[16];
            uint64_t hl[16];
            const int hn = cs[i]->heardRooms(hb, ha, hl, 16);
            for (int k = 0; k < hn; k++)
                fprintf(stderr, "    hears %08x at %u flags=%x players=%d closingInto=%08x age=%llu\n", (unsigned)hb[k].sessionId, (unsigned)ha[k].host,
                        hb[k].flags, hb[k].players, (unsigned)hb[k].closingInto, (unsigned long long)(g_now - hl[k]));
        }
    // a console arriving after Start is offered the room and is not merged
    bool lateOk = true;
    int hostsNow = 0;
    for (int i = 0; i < n; i++) hostsNow += cs[i]->isHost() ? 1 : 0;
    if (ok && hostsNow == 1) {   // (forge 2 may end with honest rooms apart: then a late console may rightly join a gathering one)
        int h = -1;
        for (int i = 0; i < n; i++) if (cs[i]->isHost()) h = i;
        CHECK(gs[h]->begin());
        Step(cs, 500);
        if (n < 4) {
            cs.push_back(MakeCore(hub, (uint32_t)(n + 1), names[n], 900 + seed));
            gs.push_back(new MpGather(cs.back()));
            gs[n]->start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK, g_now);
            int offered = 0;
            for (int ms = 0; ms < 4000; ms += 10) {
                Step(cs, 10, 5);
                for (MpGather* g : gs) g->tick(g_now);
                MpLocalRoom rooms[4];
                offered = gs[n]->playingRooms(rooms, 4, g_now);
                if (offered) break;
            }
            MpUiStatus st;
            cs[h]->uiStatus(&st);
            lateOk = offered == 1 && st.phase == MP_UI_PLAYING && cs[h]->isHost() && gs[n]->state() != MP_GATHER_STARTED;
            for (int k = 0; k < n; k++) lateOk = lateOk && cs[k]->active();
        }
    }
    printf("GATHER forge=%d seed=%u n=%d loss=%.2f settle=%d two-prompts=%d late=%d %s\n", g_forge, (unsigned)seed, n, loss, settledAt, twoPrompts, lateOk ? 1 : 0,
           ok && lateOk ? "PASS" : "FAIL");
    if (!(ok && lateOk) && verbose)
        for (size_t k = g_log.size() > 120 ? g_log.size() - 120 : 0; k < g_log.size(); k++) fprintf(stderr, "  %s\n", g_log[k].c_str());
    for (MpGather* g : gs) delete g;
    for (MpCore* c : cs) delete c;
    g_log.clear();
    return ok && lateOk ? 0 : 1;
}

int main(int argc, char** argv) {
    mp_log_set_sink(LogSink, true);
    int seeds = 10;
    float loss = 0.15f;
    bool v = false;
    int one = -1, onlyN = 0;   // -seed s -n n: one run
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-seeds") && i + 1 < argc) seeds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-loss") && i + 1 < argc) loss = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-v")) v = true;
        else if (!strcmp(argv[i], "-forge") && i + 1 < argc) g_forge = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-seed") && i + 1 < argc) one = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) onlyN = atoi(argv[++i]);
    }
    int fails = 0;
    for (int n = 2; n <= 4; n++)
        for (int s = 0; s < seeds; s++) {
            if ((one >= 0 && s != one) || (onlyN && n != onlyN)) continue;
            fails += Run((uint32_t)s, n, loss, v);
        }
    printf("gather_soak: %d failures%s\n", fails, g_fail ? " (and failed checks)" : "");
    return fails || g_fail ? 1 : 0;
}
