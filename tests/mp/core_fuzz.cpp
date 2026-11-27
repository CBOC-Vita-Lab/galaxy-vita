// Galaxy 3.0 multiplayer: fuzz test of every receive path of the session layer (ASan + UBSan + LeakSanitizer).
// A real session (host + 2 guests, loopback) is recorded; then 1,000,000 mutated datagrams (bit flips, truncations, random
// bytes, random types, extensions, sequence numbers stamped so reliable ones reach the handlers) go into the host's and the
// guests' receive paths, from the real peers' addresses and from strangers. Beacons, snapshot messages, poses, settings and
// keys are fuzzed through their parsers too. Guests that get kicked or end are replaced. No crash, no leak, no stuck state.
#include "core_test_util.h"

struct Pkt {
    MpAddr from, to;
    std::vector<uint8_t> d;
};
static std::vector<Pkt> g_corpus;
static void Tap(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    static uint8_t s_open[2048];   // core2: sealed datagrams go into the corpus opened (the fuzzer delivers them as the member's)
    if (len >= 2 && ((const uint8_t*)data)[0] == 'G' && ((const uint8_t*)data)[1] == 'S') {
        size_t pl = 0;
        if (!MpCore::debugOpenAny(data, len, s_open, sizeof s_open, &pl)) return;
        data = s_open;
        len = pl;
    }
    if (g_corpus.size() < 20000) g_corpus.push_back(Pkt{from, to, std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + len)});
}

static uint32_t g_rng = 0x9E3779B9u;
static uint32_t R() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void Mutate(std::vector<uint8_t>& d) {
    if (R() % 7 == 0 && g_corpus.size() > 4) {   // netpack: a bundle of 2..4 recorded packets (unbundled), often broken on purpose
        std::vector<std::vector<uint8_t>> pk;
        const int n = 2 + (int)(R() % 3);
        for (int i = 0; i < n; i++) {
            const std::vector<uint8_t>& c = g_corpus[R() % g_corpus.size()].d;
            if (c.size() >= 12 && c[0] == 'G' && c[1] == 'M') pk.push_back(c);
        }
        if (pk.size() >= 2) {
            d = BundleOf(pk);
            if (R() % 4) ForgeBundle(d, R);
        }
        return;
    }
    if (d.size() >= 4 && d[0] == 'G' && d[1] == 'B' && R() % 3 == 0) { ForgeBundle(d, R); return; }
    switch (R() % 8) {
    case 0: {   // bit flips
        const int n = 1 + (int)(R() % 8);
        for (int i = 0; i < n && !d.empty(); i++) d[R() % d.size()] ^= (uint8_t)(1u << (R() % 8));
        break;
    }
    case 1:   // truncation
        if (!d.empty()) d.resize(R() % d.size());
        break;
    case 2: {   // random bytes, random length
        d.resize(R() % 1200);
        for (auto& b : d) b = (uint8_t)R();
        if (d.size() >= 2 && R() % 2) { d[0] = 'G'; d[1] = 'M'; }
        break;
    }
    case 3:   // header kept, payload random
        for (size_t i = 12; i < d.size(); i++) d[i] = (uint8_t)R();
        break;
    case 4:   // random type
        if (d.size() > 3) d[3] = (uint8_t)R();
        break;
    case 5: {   // extend
        const size_t n = R() % 64;
        for (size_t i = 0; i < n; i++) d.push_back((uint8_t)R());
        break;
    }
    case 6:   // random from-slot / flags
        if (d.size() > 5) { d[4] = (uint8_t)(R() % 6); d[5] = (uint8_t)(R() % 8); }
        break;
    default: {   // one random field byte set to an edge value
        static const uint8_t edge[] = {0, 1, 0x7F, 0x80, 0xFE, 0xFF};
        if (!d.empty()) d[R() % d.size()] = edge[R() % sizeof edge];
        break;
    }
    }
}

int main(int argc, char** argv) {
    mp_log_set_sink(LogSink, true);
    const long iters = argc > 1 ? atol(argv[1]) : 1000000;
    MpLoopHub* hub = new MpLoopHub();
    hub->setClock(VNow, 0);
    hub->setTap(Tap, 0);
    std::vector<MpCore*> all;
    MpCore* host = MakeCore(*hub, 1, "Host", 7);
    all.push_back(host);
    MpHostParams hp = HostParams();
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    static uint8_t img[5000];
    for (int i = 0; i < 5000; i++) img[i] = (uint8_t)i;
    host->snapshotOffer(img, sizeof img);
    uint32_t nextEp = 2;
    int replaced = 0;
    auto addGuest = [&]() {
        char name[17];
        snprintf(name, sizeof name, "Fz%u", nextEp);
        MpCore* g = MakeCore(*hub, nextEp, name, 100 + nextEp);
        nextEp++;
        all.push_back(g);
        for (int attempt = 0; attempt < 2; attempt++) {
            if (ScanJoin(all, g) && (WaitPhase(all, g, MP_UI_PLAYING, 3000) || g->active())) return;
            // the room is full of guests that ended without a goodbye: the host drops them after MP_PEER_LOST_MS
            g->uiLeave();
            Step(all, MP_PEER_LOST_MS + 1000);
        }
        fprintf(stderr, "guest %s could not join\n", name);
        for (size_t i = g_log.size() > 60 ? g_log.size() - 60 : 0; i < g_log.size(); i++) fprintf(stderr, "  %s\n", g_log[i].c_str());
        CHECK(!"guest could not join");
    };
    addGuest();
    addGuest();
    host->uiStart();
    // record traffic of every kind
    uint8_t pose[68] = {0}, ev[8] = {0}, pointer[15] = {0}, where[8] = {1, 2, 3, 0, 0, 0, 1, 0}, sw[400] = {0};
    uint8_t bubble[6] = {1, 0, 1, 3, MP_BUBBLE_BY_NONE, 0};   // serial 1, on, 3 lives
    const uint8_t chatPhrase[2] = {MP_CHAT_PHRASE, 7};   // (chat) a phrase and a typed text
    const uint8_t chatText[14] = {MP_CHAT_TEXT, 12, 'H', 'e', 'l', 'l', 'o', ' ', 't', 'h', 'e', 'r', 'e', '!'};
    for (int t = 0; t < 200; t++) {
        for (size_t i = 0; i < all.size(); i++) {
            all[i]->send(MP_MSG_STATE, pose, (uint16_t)(51 + (t % 2) * 17), false);
            if (t % 5 == 0) all[i]->send(MP_MSG_STAR, ev, 8, true);
            if (t % 7 == 0) all[i]->send(MP_MSG_SWITCH, ev, 4, true);
            if (t % 9 == 0) all[i]->send(MP_MSG_TALLY, ev, 8, true);   // (the team star's counters: relayed by the host)
            if (t % 4 == 0) all[i]->send(MP_MSG_POINTER, pointer, 15, false);   // (coop) the pointer ping
            if (t % 25 == 0) all[i]->send(MP_MSG_EMOTE, ev, 2, true);           // (coop) the greeting
            if (t % 11 == 0) all[i]->send(MP_MSG_BUBBLE, bubble, 6, true);   // bubble: any -> host relays
            if (t % 13 == 0) all[i]->send(MP_MSG_CHAT, (t % 26) ? chatPhrase : chatText, (t % 26) ? 2 : 14, true);   // (chat)
        }
        if (t % 30 == 0) host->send(MP_MSG_RESTART, ev, 2, true);         // bubble: host only
        if (t % 20 == 0) { host->send(MP_MSG_WHERE, where, 8, true); host->send(MP_MSG_SWITCHMAP, sw, 340, true); }
        if (t == 50) { MpSettings s = hp.settings; s.interaction = 2; host->updateSettings(&s); }
        if (t == 60) { MpSettings s = hp.settings; s.interaction = 2; s.downed = MP_DOWNED_INFINITE; host->updateSettings(&s); }
        if (t == 80) host->snapshotOffer(img, 3000);
        Step(all, 16);
        MpMsg m;
        for (MpCore* c : all) while (c->poll(&m)) {}
    }
    // beacons too
    {
        MpBeacon b;
        memset(&b, 0, sizeof b);
        b.proto = MP_PROTO_VERSION; b.mode = 1; b.players = 2; b.maxPlayers = 4; b.sessionId = 5;
        uint8_t buf[64];
        const size_t n = mp_beacon_write(buf, sizeof buf, b);
        g_corpus.push_back(Pkt{MpAddr{1, MP_SESSION_PORT}, MpAddr{MP_ADDR_BROADCAST, MP_LAN_PORT}, std::vector<uint8_t>(buf, buf + n)});
    }
    hub->setTap(0, 0);
    printf("corpus: %zu datagrams\n", g_corpus.size());

    MpLanScan scan;
    MpSnapshotRx* rx = new MpSnapshotRx();
    rx->reset();
    long toHost = 0, toGuest = 0, stranger = 0;
    for (long it = 0; it < iters; it++) {
        const Pkt& base = g_corpus[R() % g_corpus.size()];
        std::vector<uint8_t> d = base.d;
        Mutate(d);
        if (R() % 3 == 0) Mutate(d);
        const uint32_t pick = R() % 100;
        // a live target: the host, or one of the guests in the session
        MpCore* target = host;
        MpAddr from = base.from;
        if (pick < 45) {
            std::vector<MpCore*> gs;
            for (size_t i = 1; i < all.size(); i++) if (all[i]->active()) gs.push_back(all[i]);
            if (!gs.empty()) {
                target = gs[R() % gs.size()];
                from = host->debugLocalAddr();   // as if from the host
                toGuest++;
                if (d.size() >= 12 && (d[5] & MP_HDR_RELIABLE) && R() % 2) {
                    const uint16_t s = target->debugExpectedSeq(target->hostSlot());
                    d[6] = (uint8_t)s; d[7] = (uint8_t)(s >> 8);
                    d[4] = (uint8_t)target->hostSlot();
                }
            }
        } else if (pick < 85) {
            from.host = 1000 + R() % 50;   // strangers in radio range
            from.port = (uint16_t)(R() % 3 ? MP_SESSION_PORT : R());
            stranger++;
        } else {
            // from a guest's address (counts as its bad messages: it gets kicked and is replaced)
            std::vector<MpCore*> gs;
            for (size_t i = 1; i < all.size(); i++) if (all[i]->active()) gs.push_back(all[i]);
            if (!gs.empty()) {
                MpCore* g = gs[R() % gs.size()];
                from = g->debugLocalAddr();
                if (d.size() >= 12 && (d[5] & MP_HDR_RELIABLE) && R() % 2) {
                    const int slot = g->localSlot();
                    const uint16_t s = host->debugExpectedSeq(slot);
                    d[6] = (uint8_t)s; d[7] = (uint8_t)(s >> 8);
                    d[4] = (uint8_t)slot;
                }
            }
            toHost++;
        }
        {   // core2: from a member (an address the target has a peer at): as sealed by it; else plain (a stranger: only a join may pass)
            const int as = target->debugSlotOf(from);
            if (as >= 0) target->debugReceiveAuthed(as, d.data(), d.size());
            else target->debugReceive(from, d.data(), d.size());
        }
        // parsers
        MpBeacon b;
        if (mp_beacon_read(d.data(), d.size(), &b)) scan.add(from, b, g_now, 123);
        if (d.size() > 12) {
            MpReader r(d.data() + 12, d.size() - 12);
            MpPose p;
            mp_read_pose(r, &p);
            MpReader r2(d.data() + 12, d.size() - 12);
            MpSettings s;
            mp_read_settings(r2, &s);
            MpReader r3(d.data() + 12, d.size() - 12);
            MpObjKey k;
            mp_read_key(r3, &k);
            MpReader r4(d.data() + 12, d.size() - 12);   // (coop) greeting and pointer ping payloads
            MpEmoteMsg em;
            if (mp_read_emote(r4, &em)) CHECK(em.emote < MP_EMOTE_COUNT);
            MpReader r5(d.data() + 12, d.size() - 12);
            MpPointerMsg pm;
            if (mp_read_pointer(r5, &pm)) CHECK(pm.on <= 1 && mp_pose_float_ok(pm.pos[0]) && mp_pose_float_ok(pm.pos[1]) && mp_pose_float_ok(pm.pos[2]));
            MpReader r6(d.data() + 12, d.size() - 12);   // (bubble)
            MpBubbleMsg bm;
            if (mp_read_bubble(r6, &bm)) CHECK(bm.on <= 1 && (bm.flags == 0 || bm.flags == MP_BUBBLE_OUT_OF_LIVES || bm.flags == MP_BUBBLE_ARRIVAL) && bm.lives <= 99);   // (bubble2: arrival flag)
            MpReader r7(d.data() + 12, d.size() - 12);   // (chat) whatever it accepts is a clean phrase or text
            MpChatMsg cm;
            if (mp_read_chat(r7, &cm)) CHECK(cm.kind == MP_CHAT_PHRASE ? cm.phrase < MP_CHAT_PHRASES : (cm.len > 0 && cm.len <= MP_CHAT_TEXT_BYTES && mp_chat_text_ok(cm.text, cm.len)));
            bool complete = false;
            if (R() % 2) rx->begin(d.data() + 12, (uint16_t)(d.size() - 12));
            else rx->chunk(d.data() + 12, (uint16_t)(d.size() - 12), &complete);
        }
        if (it % 500 == 0) {
            Step(all, 5);
            MpMsg m;
            for (MpCore* c : all) while (c->poll(&m)) {}
            MpCoreEvent e;
            for (MpCore* c : all) while (c->pollEvent(&e)) {}
            scan.expire(g_now);
        }
        if (it % 2000 == 0) {
            // replace guests that are gone (kicked for bad messages, or ended by a fuzzed message)
            for (size_t i = 1; i < all.size();) {
                if (!all[i]->active()) {
                    delete all[i];
                    all.erase(all.begin() + (long)i);
                    replaced++;
                } else {
                    i++;
                }
            }
            if (!host->active()) {
                fprintf(stderr, "host ended at iteration %ld; restarting\n", it);
                if (getenv("FUZZ_LOG")) for (size_t i = g_log.size() > 40 ? g_log.size() - 40 : 0; i < g_log.size(); i++) fprintf(stderr, "  %s\n", g_log[i].c_str());
                host->uiLeave();
                CHECK(host->uiHost(&hp));
                WaitPhase(all, host, MP_UI_LOBBY, 2000);
            }
            while (all.size() < 3) addGuest();
        }
        if (it % 200000 == 0) printf("  %ld iterations, %d guests replaced\n", it, replaced);
    }
    // the session still works after all that (first let the kicks and goodbyes in flight settle)
    Step(all, 3000);
    for (size_t i = 1; i < all.size();) {
        if (!all[i]->active()) { delete all[i]; all.erase(all.begin() + (long)i); } else i++;
    }
    while (all.size() < 3) addGuest();
    Step(all, 1000);
    for (MpCore* c : all) {
        MpUiStatus st;
        c->uiStatus(&st);
        printf("  end: slot=%d phase=%d error=%d active=%d host=%d present=%d%d%d%d\n", c->localSlot(), st.phase, st.error, c->active(),
               c->hostSlot(), c->slotPresent(0), c->slotPresent(1), c->slotPresent(2), c->slotPresent(3));
    }
    if (g_fail || getenv("FUZZ_LOG")) {
        const size_t keep = getenv("FUZZ_LOG") && atoi(getenv("FUZZ_LOG")) > 1 ? g_log.size() : 400;
        for (size_t i = g_log.size() > keep ? g_log.size() - keep : 0; i < g_log.size(); i++) printf("  %s\n", g_log[i].c_str());
    }
    CHECK(host->active() && all.size() == 3 && all[1]->active() && all[2]->active());
    CHECK(host->slotPresent(all[1]->localSlot()) && host->slotPresent(all[2]->localSlot()));
    printf("fuzz: kicked for bad messages %d times\n", LogCount("reason=bad-messages"));
    printf("fuzz: %ld datagrams (to host from guests %ld, to guests %ld, strangers %ld), %d guests replaced, bad=%u\n", iters, toHost,
           toGuest, stranger, replaced, (unsigned)host->debugBadCount());
    for (MpCore* c : all) delete c;
    delete rx;
    delete hub;
    if (g_fail) {
        printf("FAILED: %d checks\n", g_fail);
        return 1;
    }
    printf("fuzz passed\n");
    return 0;
}
