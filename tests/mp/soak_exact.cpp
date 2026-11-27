// Galaxy 3.0 multiplayer: the exactly-once soak (netcore). 4 consoles join / leave / vanish / rejoin at random on the loopback hub
// with mild impairment (core_tests' TestSoak, standalone and parametrised), and every reliable game event is checked:
//   invariant (DESIGN.md section 3, "Reliable game events"): for a receiver R and an origin O, while both stay members of one
//   session (a membership = from the WELCOME that admitted the console until it leaves / is removed; host migrations do not end it),
//   O's tracked events reach R's game exactly once and in O's order: no duplicate, no gap, nothing from an older membership of O.
//   (A console that drops out and comes back starts a new membership: what it missed meanwhile is state - SNAPSHOT / SWITCHMAP /
//   WHERE - not events, so a gap across memberships is not a fault. A duplicate never is.)
// At the end everyone must converge into one session (4 active, 1 host, same roster) and every surviving pair must have received
// every event of the final memberships (no tail lost).
//   soak_exact [-seed n] [-turns n] [-creep n] [-shift n] [-loss f] [-jitter ms] [-v] [-echo]   exit 0 = invariant held, converged
// Built by tests/mp/build.sh (soak_exact) and tests/mp/soak_matrix.sh (one line per run: "SOAK seed=.. creep=.. shift=.. ...").
#ifdef SOAK_OLD_TREE   // (netcore's comparison builds against trees without MpCore::debugSessionId)
#define private public
#include "core_test_util.h"
#undef private
static uint32_t SessionOf(MpCore* c) { return c->m_sessionId; }
#else
#include "core_test_util.h"
static uint32_t SessionOf(MpCore* c) { return c->debugSessionId(); }
#endif

#include <deque>
#include <map>
#include <tuple>

static int g_cur = -1;   // the console whose code runs now (log lines are kept per console)
static std::deque<std::string> g_ring[5];
static void RingSink(const char* line) {
    std::deque<std::string>& r = g_ring[g_cur >= 0 && g_cur < 4 ? g_cur : 4];
    char pre[32];
    snprintf(pre, sizeof pre, "%8llu ", (unsigned long long)g_now);
    r.push_back(std::string(pre) + line);
    if (r.size() > 300) r.pop_front();
    if (g_echo) fprintf(stderr, "[%d] %s%s\n", g_cur, pre, line);
}
static void DumpRing(int i, int n) {
    std::deque<std::string>& r = g_ring[i];
    fprintf(stderr, "  --- console %d, last %d log lines:\n", i, n);
    for (size_t k = r.size() > (size_t)n ? r.size() - n : 0; k < r.size(); k++) fprintf(stderr, "    %s\n", r[k].c_str());
}

static void StepN(std::vector<MpCore*>& cs, int ms, int stepMs) {
    for (int t = 0; t < ms; t += stepMs) {
        g_now += (uint64_t)stepMs;
        for (size_t i = 0; i < cs.size(); i++) {
            g_cur = (int)i;
            cs[i]->pump();
        }
        g_cur = -1;
    }
}

struct Node {
    MpCore* c;
    uint32_t ep;
    uint32_t memb;       // membership number: bumps when the console becomes active or is welcomed again
    uint32_t counter;    // events sent in this membership
    uint64_t downUntil;
    bool wasActive;
};

int main(int argc, char** argv) {
    uint32_t seed = 0;
    int turns = 10000, shift = -1, verbose = 0;
    float loss = 0.05f;
    uint32_t jitter = 40;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-seed") && i + 1 < argc) seed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-turns") && i + 1 < argc) turns = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-shift") && i + 1 < argc) shift = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-creep") && i + 1 < argc) {
            const int c = atoi(argv[++i]);
#ifdef SOAK_HAVE_CREEP
            g_creep = (uint64_t)c;
#else
            if (c) { fprintf(stderr, "this tree has no g_creep\n"); return 2; }
#endif
        } else if (!strcmp(argv[i], "-loss") && i + 1 < argc) loss = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-jitter") && i + 1 < argc) jitter = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-echo")) g_echo = true;
        else {   // (harnessfix: a misspelt flag used to run the defaults silently)
            fprintf(stderr, "soak_exact: unknown flag %s\n", argv[i]);
            return 2;
        }
    }
#ifdef SOAK_HAVE_SHIFT
    if (shift >= 0) MpReliable::setMaxBackoffShift(shift);
#else
    if (shift >= 0) { fprintf(stderr, "this tree has no backoff shift\n"); return 2; }
#endif
    mp_log_set_sink(RingSink, true);
    (void)g_fail;   // (core_test_util.h)
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpLoopImpairment imp = {loss, 0.02f, 5, jitter, 0};
    hub.setImpairment(imp, 555 + seed);
    const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
    std::vector<Node> nodes(4);
    std::vector<MpCore*> all;
    for (int i = 0; i < 4; i++) {
        nodes[i].c = MakeCore(hub, (uint32_t)(i + 1), names[i], 1000u + (uint32_t)i + seed * 7919u);
        nodes[i].ep = (uint32_t)(i + 1);
        nodes[i].memb = 0;
        nodes[i].counter = 0;
        nodes[i].downUntil = 0;
        nodes[i].wasActive = false;
        all.push_back(nodes[i].c);
    }
    // (receiver, receiver membership, origin, origin membership) -> last counter delivered
    std::map<std::tuple<int, uint32_t, int, uint32_t>, uint32_t> last;
    uint32_t rng = 0xC0FFEE ^ (seed * 2654435761u);
    auto R = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    // core2 (claims audit): what exactly-once means across a rejoin, checked:
    //   regress  = an event of an origin's EARLIER membership after one of a later membership at the same receiver membership (must be 0:
    //              the generation check drops it). A late tail of an earlier membership BEFORE any event of the next one is allowed
    //              ("stale", counted, not a fault): it is that membership's own events, in order, once.
    //   firstbad = a receiver that was present well before an origin's membership began (>= 2 s) whose first event of that membership is
    //              not number 0 (a hole at the start of a stream; must be 0; its missing events count in lost). A receiver that joined later
    //              (or within 2 s) may first see an event in the middle of the stream: the promise there is "no gap after the first one".
    //   stale    = legitimate by design, but bounded: only before any event of the origin's later membership (else it is a regress) and
    //              only within MP_STALE_MAX_MS of that later membership's start (staleLate: must be 0). The old tail is what the old host
    //              or a follower still re-sends while the host change settles; nothing keeps it longer.
    std::map<std::pair<int, uint32_t>, uint64_t> membStart;   // (node, membership) -> when it began
    std::map<std::tuple<int, uint32_t, int>, uint32_t> highestMemb;   // (receiver, receiver memb, origin) -> latest origin memb seen
    int regress = 0, firstBad = 0, staleLate = 0;
    const uint64_t MP_STALE_MAX_MS = 30000;
    int dups = 0, gaps = 0, lostEvents = 0, stale = 0, joins = 0, leaves = 0, vanishes = 0, migrations = 0, delivered = 0, reports = 0;
    auto newMembership = [&](int i) {
        nodes[i].memb++;
        membStart[{i, nodes[i].memb}] = g_now;
        nodes[i].counter = 0;
        joins++;
    };
    auto drainNode = [&](int i) {
        MpMsg m;
        g_cur = i;
        // membership first (a WELCOME and the first events of the new membership come out of the same pump)
        bool bumped = false;
        if (nodes[i].c->active() && !nodes[i].wasActive) { newMembership(i); nodes[i].wasActive = true; bumped = true; }
        MpCoreEvent ev;
        while (nodes[i].c->pollEvent(&ev)) {
            if (ev.kind == MP_EV_BECAME_HOST) migrations++;
            // a WELCOME (joined / re-admitted) starts a new membership even when the console never went idle (split heal)
            if (ev.kind == MP_EV_JOINED && ev.slot == nodes[i].c->localSlot() && !nodes[i].c->isHost() && !bumped) newMembership(i);
        }
        while (nodes[i].c->poll(&m)) {
            if (m.type != MP_MSG_KILL || m.len != 9) continue;
            uint32_t omemb, ocnt;
            memcpy(&omemb, m.data + 1, 4);
            memcpy(&ocnt, m.data + 5, 4);
            const int on = m.data[0];
            delivered++;
            if (on < 4 && omemb < nodes[on].memb) {   // (an event of an origin's earlier membership)
                stale++;
                const auto ns = membStart.find({on, omemb + 1});
                if (ns != membStart.end() && g_now > ns->second + MP_STALE_MAX_MS) { staleLate++; }
            }
            auto key = std::make_tuple(i, nodes[i].memb, on, omemb);
            auto it = last.find(key);
            const char* what = 0;
            {
                uint32_t& hm = highestMemb[std::make_tuple(i, nodes[i].memb, on)];
                if (omemb < hm) { regress++; what = "REGRESS"; }
                else hm = omemb;
            }
            if (it == last.end() && on < 4 && ocnt != 0) {
                const auto rs = membStart.find({i, nodes[i].memb}), os = membStart.find({on, omemb});
                if (rs != membStart.end() && os != membStart.end() && rs->second + 2000 <= os->second) {
                    firstBad++;
                    lostEvents += (int)ocnt;
                    what = "FIRSTBAD";
                }
            }
            if (it != last.end()) {
                if (ocnt <= it->second) { dups++; what = "DUPLICATE"; }
                else if (ocnt != it->second + 1) { gaps++; lostEvents += (int)(ocnt - it->second - 1); what = "GAP"; }
            }
            if (what && reports < 4) {
                reports++;
                fprintf(stderr, "%s t=%llu receiver=%d(memb %u, slot %d) origin=%d(memb %u) counter=%u last=%u\n", what, (unsigned long long)g_now,
                        i, (unsigned)nodes[i].memb, nodes[i].c->localSlot(), on, (unsigned)omemb, (unsigned)ocnt, it != last.end() ? (unsigned)it->second : 0u);
                if (verbose) for (int k = 0; k < 4; k++) DumpRing(k, 60);
            }
            if (it == last.end() || ocnt > it->second) last[key] = ocnt;
        }
        g_cur = -1;
    };
    for (int turn = 0; turn < turns; turn++) {
        const bool settling = turn > turns - 1500;
        for (int i = 0; i < 4; i++) {
            Node& n = nodes[i];
            g_cur = i;
            if (n.downUntil && g_now >= n.downUntil) {
                hub.setDown(n.ep, false);
                n.downUntil = 0;
            }
            if (n.downUntil) continue;
            const MpUiPhase ph = Phase(n.c);
            if (n.c->active()) {
                if (!n.wasActive) { newMembership(i); n.wasActive = true; }
                if (R() % 4 == 0) {
                    uint8_t p[16];
                    p[0] = (uint8_t)i;
                    memcpy(p + 1, &n.memb, 4);
                    memcpy(p + 5, &n.counter, 4);
                    if (n.c->send(MP_MSG_KILL, p, 9, true)) n.counter++;
                }
                if (!settling) {
                    const uint32_t r = R() % 10000;
                    if (r < 8) { n.c->uiLeave(); leaves++; }
                    else if (r < 12) { hub.setDown(n.ep, true); n.downUntil = g_now + 2000 + R() % 15000; vanishes++; }
                }
            } else {
                n.wasActive = false;
                if (ph == MP_UI_IDLE) {
                    const int cnt = n.c->uiLocalScan(MP_CONN_LOOPBACK);
                    bool joined = false;
                    for (int k = 0; k < cnt && !joined; k++) {
                        MpLocalRoom room;
                        if (n.c->uiLocalGet(k, &room) && room.compatible && room.players < room.maxPlayers)
                            joined = n.c->uiJoinLocal(MP_CONN_LOOPBACK, k);
                    }
                    if (!joined && cnt == 0 && R() % 400 == 0) {
                        MpHostParams hp = HostParams();
                        n.c->uiHost(&hp);
                    }
                }
            }
            g_cur = -1;
        }
        StepN(all, 16, 8);
        for (int i = 0; i < 4; i++) drainNode(i);
    }
    hub.setImpairment(MpLoopImpairment{0, 0, 0, 0, 0}, 1);
    for (int i = 0; i < 4; i++) { hub.setDown(nodes[i].ep, false); nodes[i].downUntil = 0; }
    int settleMs = -1, otherRoomsLeft = 0, bigPlayers = 0;
    // The clean settle may legitimately take up to MP_RECONNECT_MS (30 s) of retries against a session whose host has since left, plus the
    // bring-up timeout of the attempt in flight (MP_LINK_TIMEOUT_MS, 30 s): a console that went for a dead session's rejoin is back in
    // the lobby after ~60 s (seed 7 after the heartbeat change: 61.6 s; before it the slowest was 21.9 s). Bound = that + margin, still
    // a hard limit: no convergence inside it fails the run.
    for (int t = 0; t < 90000; t += 50) {
        int hostsUp = 0;
        for (auto& n : nodes) if (n.c->isHost()) hostsUp++;
        // Two rooms of DIFFERENT sessions are legitimate (two consoles hosted while they could not see a room): as a player would,
        // the host of the smaller one leaves and joins the other. Two hosts of ONE session (a split) must heal by themselves.
        if (hostsUp > 1 && t % 1000 == 0 && t > 0) {
            int big = -1;
            for (int i = 0; i < 4; i++) {
                MpUiStatus s;
                nodes[i].c->uiStatus(&s);
                if (nodes[i].c->isHost() && (big < 0 || s.players > bigPlayers)) { big = i; bigPlayers = s.players; }
            }
            for (int i = 0; i < 4; i++)
                if (i != big && nodes[i].c->isHost() && SessionOf(nodes[i].c) != SessionOf(nodes[big].c)) {
                    g_cur = i;
                    nodes[i].c->uiLeave();
                    otherRoomsLeft++;
                    g_cur = -1;
                }
        }
        for (int i = 0; i < 4; i++) {
            Node& n = nodes[i];
            g_cur = i;
            if (n.c->active() && !n.wasActive) { newMembership(i); n.wasActive = true; }
            if (!n.c->active()) n.wasActive = false;
            if (Phase(n.c) != MP_UI_IDLE) continue;
            const int cnt = n.c->uiLocalScan(MP_CONN_LOOPBACK);
            for (int k = 0; k < cnt; k++) {
                MpLocalRoom room;
                if (n.c->uiLocalGet(k, &room) && room.compatible && n.c->uiJoinLocal(MP_CONN_LOOPBACK, k)) break;
            }
            if (cnt == 0 && hostsUp == 0 && i == 0) { MpHostParams hp = HostParams(); n.c->uiHost(&hp); }
        }
        g_cur = -1;
        StepN(all, 50, 5);
        for (int i = 0; i < 4; i++) drainNode(i);
        int act = 0;
        for (auto& n : nodes) if (n.c->active()) act++;
        int hosts1 = 0;
        for (auto& n : nodes) if (n.c->isHost()) hosts1++;
        if (act == 4 && hosts1 == 1 && t > 5000) { settleMs = t; break; }
    }
    // a clean last stretch: everything sent must arrive
    StepN(all, 5000, 5);
    for (int i = 0; i < 4; i++) drainNode(i);
    int hosts = 0, act = 0;
    for (auto& n : nodes) { if (n.c->isHost()) hosts++; if (n.c->active()) act++; }
    bool conv = act == 4 && hosts == 1;
    for (auto& a : nodes)
        for (auto& b2 : nodes)
            for (int s = 0; s < 4; s++) {
                char n1[17], n2[17];
                a.c->slotName(s, n1);
                b2.c->slotName(s, n2);
                if (a.c->slotPresent(s) != b2.c->slotPresent(s) || strcmp(n1, n2)) conv = false;
            }
    int tailLost = 0;
    if (conv) {
        for (int r = 0; r < 4; r++)
            for (int o = 0; o < 4; o++) {
                if (r == o) continue;
                auto it = last.find(std::make_tuple(r, nodes[r].memb, o, nodes[o].memb));
                if (it != last.end() && it->second + 1 != nodes[o].counter) {
                    tailLost++;
                    fprintf(stderr, "TAIL receiver=%d origin=%d last=%u sent=%u\n", r, o, (unsigned)it->second, (unsigned)nodes[o].counter);
                }
            }
    } else {
        fprintf(stderr, "NOT CONVERGED: active=%d hosts=%d\n", act, hosts);
        for (int i = 0; i < 4; i++) {
            MpUiStatus s;
            nodes[i].c->uiStatus(&s);
            fprintf(stderr, "  console %d: session=%08x phase=%d active=%d host=%d slot=%d hostSlot=%d epoch=%d present=%d%d%d%d\n", i, (unsigned)SessionOf(nodes[i].c), (int)s.phase,
                    nodes[i].c->active(), nodes[i].c->isHost(), nodes[i].c->localSlot(), nodes[i].c->hostSlot(), nodes[i].c->hostEpoch(),
                    nodes[i].c->slotPresent(0), nodes[i].c->slotPresent(1), nodes[i].c->slotPresent(2), nodes[i].c->slotPresent(3));
        }
        if (verbose) for (int k = 0; k < 4; k++) DumpRing(k, 80);
    }
    const bool ok = conv && dups == 0 && gaps == 0 && tailLost == 0 && regress == 0 && firstBad == 0 && staleLate == 0;
    printf("SOAK seed=%u creep=%llu shift=%d loss=%.2f jitter=%u turns=%d dups=%d gaps=%d lost=%d stale=%d stalelate=%d regress=%d firstbad=%d tail=%d conv=%s settle=%d rooms=%d joins=%d leaves=%d vanishes=%d "
           "migrations=%d events=%d %s\n",
           (unsigned)seed, (unsigned long long)
#ifdef SOAK_HAVE_CREEP
           g_creep,
#else
           0ull,
#endif
           shift, loss, (unsigned)jitter, turns, dups, gaps, lostEvents, stale, staleLate, regress, firstBad, tailLost, conv ? "yes" : "NO", settleMs, otherRoomsLeft, joins, leaves, vanishes, migrations, delivered,
           ok ? "PASS" : "FAIL");
    for (MpCore* c : all) delete c;
    return ok ? 0 : 1;
}
