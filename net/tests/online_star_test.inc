// Galaxy 3.0 multiplayer: tests of the star topology (rooms of up to 4, guests peer only with the host), per-member loss, host migration
// (DESIGN.md section 8) and the service's members[] room view. Included by online_test.cpp (it uses its world, consoles and stub server).
// Not in Strikers: see net/GALAXY-CHANGES.txt.

namespace {

struct Star {
    World w;
    std::unique_ptr<TurnRig> rig;
    Console* c[4];
    int hosts[4];
    Addr privs[4];
    NatType nats[4];
    bool down[4];
    std::string invite;
    explicit Star(uint32_t seed = 41) : w(seed) {
        rig.reset(new TurnRig(w));
        SignalMemoryHubSetTurn(w.hub, kTurnUrl, kTurnSecret, 3600, 3600, -1);
        // host: port-restricted cone; guest 1: full cone; guest 2: no NAT; guest 3: symmetric (against the host's port-restricted NAT only the relay works)
        nats[0] = NatType::PortRestricted();
        nats[1] = NatType::FullCone();
        nats[2] = NatType::Open();
        nats[3] = NatType::Symmetric();
        const char* names[4] = {"HOSTY", "GUEST1", "GUEST2", "GUEST3"};
        for (int i = 0; i < 4; ++i) {
            const int lan = w.sim.AddLan(PublicIp(i), nats[i]);
            privs[i] = nats[i].open ? MakeAddr(203, 0, 113, uint8_t(50 + i), 41100) : MakeAddr(192, 168, uint8_t(i + 1), 10, 41100);
            hosts[i] = w.sim.AddHost(lan, privs[i]);
            c[i] = new Console(&w, hosts[i], privs[i], 1000u + uint32_t(i) * 77u, names[i]);
            c[i]->relay = true;
            down[i] = false;
        }
    }
    ~Star() { for (Console* x : c) delete x; }
    // a console that is "down" (crashed, unplugged) is no longer ticked
    void Advance(uint32_t ms, uint32_t step = 10) {
        for (uint32_t t = 0; t < ms; t += step) {
            w.now += step;
            for (int i = 0; i < 4; ++i)
                if (!down[i]) c[i]->net.Tick();
        }
    }
    int Until(uint32_t maxMs, const std::function<bool()>& done) {
        const uint64_t t0 = w.now;
        for (uint32_t t = 0; t < maxMs; t += 10) {
            Advance(10);
            if (done()) return int(w.now - t0);
        }
        return -1;
    }
    bool StartAll() {
        const PunchConfig pc = ProdPunch();
        if (!c[0]->Start(true, 0, &pc)) return false;
        Advance(50);
        OnlineInfo info;
        c[0]->net.GetInfo(&info);
        invite = info.invite;
        for (int i = 1; i < 4; ++i) {
            if (!c[i]->Start(false, invite.c_str(), &pc)) return false;
            Advance(30);
        }
        return true;
    }
    // console i restarts (a new process) and joins the same room again
    bool Rejoin(int i, uint32_t seed) {
        const std::string name = c[i]->name;
        delete c[i];
        c[i] = new Console(&w, hosts[i], privs[i], seed, name.c_str());
        c[i]->relay = true;
        down[i] = false;
        const PunchConfig pc = ProdPunch();
        return c[i]->Start(false, invite.c_str(), &pc);
    }
    int Connected(int i) {
        OnlineInfo info;
        c[i]->net.GetInfo(&info);
        return info.connectedCount;
    }
    bool Up(int i, int peer) { return c[i]->net.PeerAt(peer).GetState() == Peer::CONNECTED && c[i]->net.PeerAt(peer).Established(); }
    // `from` sends one frame per target, each target must receive it from the right slot (ids carry the sender)
    bool Flow(int from, const std::vector<int>& to, uint32_t id) {
        const std::vector<uint8_t> f = Frame(id, 300);
        c[from]->net.SendFrame(-1, &f[0], f.size());
        Advance(300);
        bool ok = true;
        for (int t : to) {
            uint8_t buf[2048];
            int slot = -1, got, seen = 0;
            while ((got = c[t]->net.RecvFrame(&slot, buf, sizeof buf)) > 0) {
                std::vector<uint8_t> v(buf, buf + got);
                uint32_t rid;
                if (FrameOk(v, &rid) && rid == id && slot == from) ++seen;
            }
            if (seen != 1) {
                fprintf(stderr, "  frame %u from %d: console %d saw it %d times\n", id, from, t, seen);
                ok = false;
            }
        }
        return ok;
    }
    void Dump(const char* what) {
        fprintf(stderr, "  [%s]\n", what);
        for (int i = 0; i < 4; ++i) {
            OnlineInfo info;
            c[i]->net.GetInfo(&info);
            fprintf(stderr, "    %s%s: slot %d phase %d connected %d host %d epoch %u migrating %d relay %d error \"%s\"\n", c[i]->name.c_str(), down[i] ? " (down)" : "",
                    info.mySlot, info.phase, info.connectedCount, info.hostSlot, unsigned(info.hostEpoch), info.migrating, info.relayState, info.error);
        }
    }
};

}  // namespace

static void TestStarRig() {
    printf("star: host + 3 guests (port-restricted host; full cone, no NAT, symmetric guests), guest drop and rejoin, host drop and migration\n");
    Star s;
    CHECK(s.StartAll());
    // 1. everyone is connected to the host, and only to the host
    const int ms = s.Until(40000, [&] { return s.Up(0, 1) && s.Up(0, 2) && s.Up(0, 3) && s.Up(1, 0) && s.Up(2, 0) && s.Up(3, 0); });
    CHECK(ms >= 0);
    if (ms < 0) { s.Dump("did not connect"); return; }
    s.Advance(2000);
    CHECK_EQ(s.Connected(0), 3);
    for (int g = 1; g < 4; ++g) {
        CHECK_EQ(s.Connected(g), 1);
        for (int o = 1; o < 4; ++o) CHECK(o == g || s.c[g]->net.PeerAt(o).GetState() == Peer::IDLE);   // guests never peer with each other
        CHECK(s.c[g]->net.GetPhase() == OnlineNet::READY);
    }
    CHECK(s.c[0]->net.GetPhase() == OnlineNet::READY);
    CHECK(s.c[3]->net.PeerAt(0).Path().kind == CAND_RELAY);   // the symmetric guest is carried by the relay
    CHECK(s.c[0]->net.PeerAt(3).Path().kind == CAND_RELAY);
    CHECK(s.c[1]->net.PeerAt(0).Path().kind != CAND_RELAY);
    CHECK(s.c[2]->net.PeerAt(0).Path().kind != CAND_RELAY);
    printf("  connected in %d ms; guest 3 via the relay, guests 1 and 2 direct\n", ms);
    // 2. data: the host to everyone, every guest to the host
    CHECK(s.Flow(0, {1, 2, 3}, 9001));
    for (int g = 1; g < 4; ++g) CHECK(s.Flow(g, {0}, 9100u + uint32_t(g)));
    {   // every punch report names its pair: the host's name guests 1..3, the guests' name the host
        PunchReport reps[32];
        const int n = SignalMemoryHubReports(s.w.hub, reps, 32);
        int toHost = 0, fromHost[4] = {0, 0, 0, 0};
        for (int i = 0; i < n; ++i) {
            if (reps[i].peerSlot == 0) ++toHost;
            else if (reps[i].peerSlot < 4) ++fromHost[reps[i].peerSlot];
        }
        CHECK(toHost >= 3);
        CHECK(fromHost[1] >= 1 && fromHost[2] >= 1 && fromHost[3] >= 1);
    }
    // 3. guest 2 crashes: the host loses that member only (onPeerLost(2), no onLost), the other guests play on
    s.down[2] = true;
    const int lostMs = s.Until(40000, [&] { return !s.c[0]->peerLost.empty(); });
    CHECK(lostMs >= 0);
    CHECK(s.c[0]->peerLost.size() == 1u && s.c[0]->peerLost[0] == 2);
    CHECK_EQ(s.c[0]->lostCalls, 0);
    CHECK(s.c[0]->net.GetPhase() == OnlineNet::READY);
    CHECK(s.Flow(0, {1, 3}, 9200));
    CHECK(s.Flow(1, {0}, 9201));
    CHECK(s.Flow(3, {0}, 9203));
    printf("  guest 2 dropped: the host noticed after %d ms, guests 1 and 3 unaffected\n", lostMs);
    // the service drops the silent member; guest 2 starts again and rejoins the same room (it gets slot 2 back)
    SignalMemoryHubEvict(s.w.hub, s.invite.c_str(), 2);
    s.Advance(500);
    CHECK(s.Rejoin(2, 4242));
    const int rejoinMs = s.Until(20000, [&] { return s.Up(0, 2) && s.Up(2, 0); });
    CHECK(rejoinMs >= 0);
    {
        OnlineInfo info;
        s.c[2]->net.GetInfo(&info);
        CHECK_EQ(info.mySlot, 2);
    }
    CHECK(s.Flow(0, {1, 2, 3}, 9300));
    CHECK(s.Flow(2, {0}, 9302));
    CHECK_EQ(s.Connected(0), 3);
    printf("  guest 2 rejoined in %d ms\n", rejoinMs);
    // 4. the host crashes. Every guest loses the host (onPeerLost(0)) but not the session: they wait for a new host. The service makes the lowest
    // remaining slot (1) the host; guests 2 and 3 punch / relay to it, it keeps them both, DATA flows again.
    s.down[0] = true;
    for (int g = 1; g < 4; ++g) s.c[g]->peerLost.clear();
    const int hostLostMs = s.Until(40000, [&] { return !s.c[1]->peerLost.empty() && !s.c[2]->peerLost.empty() && !s.c[3]->peerLost.empty(); });
    CHECK(hostLostMs >= 0);
    for (int g = 1; g < 4; ++g) {
        CHECK(s.c[g]->peerLost.size() == 1u && s.c[g]->peerLost[0] == 0);
        CHECK_EQ(s.c[g]->lostCalls, 0);   // not the whole link: a new host is expected
        OnlineInfo info;
        s.c[g]->net.GetInfo(&info);
        CHECK_EQ(info.migrating, 1);
    }
    SignalMemoryHubEvict(s.w.hub, s.invite.c_str(), 0);   // the service: the host is gone, slot 1 is the host now (epoch 1)
    const int migMs = s.Until(30000, [&] { return s.Up(1, 2) && s.Up(1, 3) && s.Up(2, 1) && s.Up(3, 1); });
    CHECK(migMs >= 0);
    if (migMs < 0) { s.Dump("no migration"); return; }
    s.Advance(1000);
    for (int g = 1; g < 4; ++g) {
        OnlineInfo info;
        s.c[g]->net.GetInfo(&info);
        CHECK_EQ(info.hostSlot, 1);
        CHECK_EQ(info.hostEpoch, 1u);
        CHECK_EQ(info.migrating, 0);
        CHECK_EQ(s.c[g]->lostCalls, 0);
        CHECK(s.c[g]->net.GetPhase() == OnlineNet::READY);
    }
    CHECK_EQ(s.Connected(1), 2);
    CHECK_EQ(s.Connected(2), 1);
    CHECK_EQ(s.Connected(3), 1);
    CHECK(s.c[2]->net.PeerAt(3).GetState() == Peer::IDLE);   // still a star: the guests do not peer with each other
    CHECK(s.Flow(1, {2, 3}, 9400));
    CHECK(s.Flow(2, {1}, 9402));
    CHECK(s.Flow(3, {1}, 9403));
    printf("  host dropped: guests noticed after %d ms; slot 1 took over, guests 2 and 3 reconnected to it %d ms after the service moved the host\n",
           hostLostMs, migMs);
    // 5. the new host crashes too and the service never names another: the remaining guests give up after the wait (onLost once each)
    s.down[1] = true;
    for (int g = 2; g < 4; ++g) s.c[g]->peerLost.clear();
    const int endMs = s.Until(120000, [&] { return s.c[2]->lostCalls > 0 && s.c[3]->lostCalls > 0; });
    CHECK(endMs >= 0);
    CHECK_EQ(s.c[2]->lostCalls, 1);
    CHECK_EQ(s.c[3]->lostCalls, 1);
    CHECK(endMs >= 30000);   // lostMs (25 s) + the migration wait (30 s)
    printf("  the second host dropped with nobody named in its place: the guests ended after %d ms\n", endMs);
}

// The host leaves gracefully (Stop: close messages, room left): the guests re-centre on slot 1 without waiting for a silence timeout.
static void TestStarHostLeaves() {
    printf("star: the host leaves (close message + leave): slot 1 becomes host, slots 2 and 3 follow\n");
    Star s(43);
    CHECK(s.StartAll());
    CHECK(s.Until(40000, [&] { return s.Up(0, 1) && s.Up(0, 2) && s.Up(0, 3) && s.Up(1, 0) && s.Up(2, 0) && s.Up(3, 0); }) >= 0);
    s.Advance(1000);
    s.c[0]->net.Stop();
    s.down[0] = true;
    const int ms = s.Until(30000, [&] { return s.Up(1, 2) && s.Up(1, 3) && s.Up(2, 1) && s.Up(3, 1); });
    CHECK(ms >= 0);
    if (ms < 0) { s.Dump("no migration after a leave"); return; }
    for (int g = 1; g < 4; ++g) {
        CHECK_EQ(s.c[g]->lostCalls, 0);
        CHECK(!s.c[g]->peerLost.empty() && s.c[g]->peerLost[0] == 0);
    }
    CHECK(s.Flow(1, {2, 3}, 9500));
    CHECK(s.Flow(3, {1}, 9503));
    printf("  re-centred on slot 1 in %d ms\n", ms);
}

// Strikers' mesh is still there (SMG_NET_TOPOLOGY=mesh): three consoles, every pair connected.
static void TestMeshTopology() {
    printf("mesh topology (Strikers' behaviour): three consoles, every pair connected\n");
    Star s(47);
    for (int i = 0; i < 4; ++i) s.c[i]->topology = TOPOLOGY_MESH;
    s.down[3] = true;
    const PunchConfig pc = ProdPunch();
    CHECK(s.c[0]->Start(true, 0, &pc));
    s.Advance(50);
    OnlineInfo info;
    s.c[0]->net.GetInfo(&info);
    CHECK(s.c[1]->Start(false, info.invite, &pc));
    CHECK(s.c[2]->Start(false, info.invite, &pc));
    CHECK(s.Until(30000, [&] { return s.Up(0, 1) && s.Up(0, 2) && s.Up(1, 2) && s.Up(2, 1); }) >= 0);
    CHECK(s.Flow(1, {0, 2}, 9600));
}

// The service's room view for rooms of up to 4: members[] (bounded and checked entry by entry), per-pair punchAtMs and steer, host_slot /
// host_epoch, the "peer" of a punch report, X-Msc-Game on every request.
static void TestRoomMembersHttp() {
    printf("room view members[]: entries checked one by one, per-pair punch time and relay advice, host_slot, punch reports name the peer\n");
    StubServer srv;
    CHECK(srv.Start());
    static int hostSlot = 0;
    hostSlot = 0;
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        (void)m;
        if (path.find("/v1/info") == 0) { *body = "{\"serverMs\":1000000,\"stun\":[]}"; return; }
        const std::string ep = "\"endpoint\":{\"public\":\"203.0.113.9:41100\",\"local\":\"192.168.0.5:41100\",\"nonce\":\"AAECAwQFBgc=\",\"nat\":\"cone\"}";
        char hs[64];
        // (the first view in the coordinator's spelling, later ones in the service's: both must be read)
        if (hostSlot) snprintf(hs, sizeof hs, "\"hostSlot\":%d,\"hostEpoch\":%d,", hostSlot, 1);
        else snprintf(hs, sizeof hs, "\"host_slot\":%d,\"host_epoch\":%d,", hostSlot, 0);
        *body = std::string("{\"code\":\"ABC234\",\"state\":\"ready\",\"version\":4,\"changed\":true,\"slot\":0,") + hs +
                "\"you\":{\"name\":\"Hosty\",\"vip\":\"10.77.0.1\"},\"members\":["
                "{\"slot\":0,\"name\":\"Me again\",\"vip\":\"10.77.0.9\"},"                                               // our own slot: ignored
                "{\"slot\":1,\"pid\":11,\"name\":\"One\",\"vip\":\"10.77.0.2\",\"link\":\"ok\"," + ep + ",\"punchAtMs\":1002000,\"steer\":{\"relay\":false,\"reason\":null}},"
                "{\"slot\":2,\"pid\":12,\"name\":\"Two\",\"vip\":\"10.77.0.3\",\"link\":\"quiet\",\"endpoint\":null,\"steer\":{\"relay\":true,\"reason\":\"pair_failures\"}},"
                "{\"slot\":2,\"name\":\"Duplicate\",\"vip\":\"10.77.0.99\"},"                                            // a repeated slot: ignored
                "{\"slot\":3,\"pid\":13,\"name\":\"Three\",\"vip\":\"10.77.0.4\",\"link\":\"lost\"," + ep + "},"
                "{\"slot\":7,\"name\":\"Bad slot\"},{\"slot\":-1},\"not an object\",{\"slot\":1e300},"                     // out of range / malformed
                "{\"slot\":1,\"name\":\"Ninth entry\"}],"                                                              // beyond the 8 entries looked at
                "\"session\":{\"key\":\"AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA=\",\"cookie\":5,\"punchAtMs\":1001000,\"steer\":{\"relay\":false}}}";
    };
    HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
    Signal* sig = CreateHttpSignal(c);
    sig->Host("Hosty");
    CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM && sig->Room().members[3].present; }));
    const SigRoom& r = sig->Room();
    CHECK_EQ(r.mySlot, 0);
    CHECK(strcmp(r.members[0].name, "Hosty") == 0);
    CHECK(r.members[1].present && strcmp(r.members[1].name, "One") == 0 && strcmp(r.members[2].name, "Two") == 0 && r.members[1].pid == 11u && r.members[1].cands.valid);
    CHECK_EQ(r.members[1].vip, MakeAddr(10, 77, 0, 2, 0).ip);
    CHECK(r.members[1].punchAtMs != 0 && r.members[1].punchAtMs != r.punchAtMs);   // the pair's own start time
    CHECK(!r.members[1].steerRelay);
    CHECK(r.members[2].present && strcmp(r.members[2].name, "Two") == 0 && !r.members[2].cands.valid && r.members[2].link == 1);
    CHECK(r.members[2].steerRelay && strcmp(r.members[2].steerReason, "pair_failures") == 0);
    CHECK(r.members[3].present && r.members[3].link == 2 && r.members[3].cands.valid && r.members[3].punchAtMs == 0);
    CHECK_EQ(r.hostSlot, 0);
    // a punch report names the member
    PunchReport rep;
    rep.peerSlot = 2;
    rep.ok = true;
    rep.ms = 900;
    rep.pathKind = CAND_PUBLIC;
    sig->ReportPunch(rep);
    hostSlot = 2;   // and the host role moves (host migration)
    CHECK(SignalUntil(sig, 5000, [&] { return sig->Room().hostSlot == 2; }));
    CHECK_EQ(sig->Room().hostEpoch, 1u);
    bool peerField = false, game = !srv.heads.empty();
    for (size_t i = 0; i < srv.bodies.size(); ++i)
        if (srv.seen[i].find("/punch") != std::string::npos && srv.bodies[i].find("\"peer\":2") != std::string::npos) peerField = true;
    for (size_t i = 0; i < srv.heads.size(); ++i) game = game && srv.heads[i].find("X-Msc-Game: smg") != std::string::npos;
    CHECK(peerField);
    CHECK(game);
    sig->Leave();
    for (int i = 0; i < 20; ++i) { HttpPump(); usleep(10000); }
    delete sig;
    srv.Stop();
    {   // the Strikers service is never used, whatever the configuration says
        HttpSignalConfig bad = ServiceConfig("https://msc-online.cboc-vita.workers.dev", "0123456789012345678901234567890123456789012");
        Signal* s2 = CreateHttpSignal(bad);
        s2->Host("Hosty");
        CHECK(s2->GetStatus() == Signal::FAILED);
        delete s2;
    }
}

// The same star against the service's own code (the local fake, GAME=smg): four accounts, members[] views, per-pair punch times, then the host
// leaves and the service moves the host role to slot 1 (hostSlot / hostEpoch); guests 2 and 3 re-centre on it. Real time (HTTP).
static bool FlowReal(Star& s, int from, const std::vector<int>& to, uint32_t id) {
    const std::vector<uint8_t> f = Frame(id, 300);
    s.c[from]->net.SendFrame(-1, &f[0], f.size());
    Console* live[4];
    int n = 0;
    for (int i = 0; i < 4; ++i)
        if (!s.down[i]) live[n++] = s.c[i];
    RunReal(s.w, live, n, 700, [] { return false; });
    bool ok = true;
    for (int t : to) {
        uint8_t buf[2048];
        int slot = -1, got, seen = 0;
        while ((got = s.c[t]->net.RecvFrame(&slot, buf, sizeof buf)) > 0) {
            std::vector<uint8_t> v(buf, buf + got);
            uint32_t rid;
            if (FrameOk(v, &rid) && rid == id && slot == from) ++seen;
        }
        if (seen != 1) { fprintf(stderr, "  frame %u from %d: console %d saw it %d times\n", id, from, t, seen); ok = false; }
    }
    return ok;
}

static void TestStarHttp(const char* base, const char* const tok[4]) {
    printf("star over HTTP against the local service: 4 accounts, members[] view, then the host leaves and slot 1 takes over\n");
    Star s(53);
    s.nats[3] = NatType::AddressRestricted();   // (no relay credentials in the fake: every pair can punch)
    for (int i = 0; i < 4; ++i) {
        s.c[i]->relay = false;
        s.c[i]->sigOverride = CreateHttpSignal(ServiceConfig(base, tok[i]));
    }
    // guest 3 sits behind an address-restricted NAT instead of the symmetric one of the in-memory rig
    {
        const int lan = s.w.sim.AddLan(PublicIp(7), s.nats[3]);
        s.privs[3] = MakeAddr(192, 168, 8, 10, 41100);
        s.hosts[3] = s.w.sim.AddHost(lan, s.privs[3]);
        delete s.c[3];
        s.c[3] = new Console(&s.w, s.hosts[3], s.privs[3], 3333, "GUEST3");
        s.c[3]->sigOverride = CreateHttpSignal(ServiceConfig(base, tok[3]));
    }
    Console* all[4] = {s.c[0], s.c[1], s.c[2], s.c[3]};
    CHECK(s.c[0]->Start(true, 0));
    CHECK(RunReal(s.w, all, 1, 10000, [&] { OnlineInfo i; s.c[0]->net.GetInfo(&i); return i.invite[0] != 0; }));
    OnlineInfo hi;
    s.c[0]->net.GetInfo(&hi);
    s.invite = hi.invite;
    for (int g = 1; g < 4; ++g) {   // one after the other, so that console g gets slot g (the service gives slots in join order)
        CHECK(s.c[g]->Start(false, s.invite.c_str()));
        Console* one[1] = {s.c[g]};
        CHECK(RunReal(s.w, one, 1, 10000, [&] { OnlineInfo i; s.c[g]->net.GetInfo(&i); return i.mySlot == g; }));
    }
    const uint64_t t0 = RealMs();
    const bool up = RunReal(s.w, all, 4, 40000, [&] { return s.Up(0, 1) && s.Up(0, 2) && s.Up(0, 3) && s.Up(1, 0) && s.Up(2, 0) && s.Up(3, 0); });
    CHECK(up);
    if (!up) { s.Dump("star over HTTP did not connect"); return; }
    printf("  all three guests connected to the host %llu ms after joining\n", (unsigned long long)(RealMs() - t0));
    for (int g = 1; g < 4; ++g) {
        CHECK_EQ(s.Connected(g), 1);
        OnlineInfo gi;
        s.c[g]->net.GetInfo(&gi);
        CHECK_EQ(gi.mySlot, g);
        CHECK_EQ(gi.peerCount, 3);   // the view names every member
    }
    CHECK_EQ(s.Connected(0), 3);
    CHECK(FlowReal(s, 0, {1, 2, 3}, 9700));
    CHECK(FlowReal(s, 3, {0}, 9703));
    // the host leaves (close messages, then its leave call): the service moves the host role to slot 1
    s.c[0]->net.Stop();
    s.down[0] = true;
    Console* rest[3] = {s.c[1], s.c[2], s.c[3]};
    const uint64_t t1 = RealMs();
    const bool moved = RunReal(s.w, rest, 3, 45000, [&] { return s.Up(1, 2) && s.Up(1, 3) && s.Up(2, 1) && s.Up(3, 1); });
    CHECK(moved);
    if (!moved) { s.Dump("no migration over HTTP"); return; }
    printf("  the host left; slot 1 is the host and guests 2 and 3 reconnected to it in %llu ms\n", (unsigned long long)(RealMs() - t1));
    for (int g = 1; g < 4; ++g) {
        OnlineInfo gi;
        s.c[g]->net.GetInfo(&gi);
        CHECK_EQ(gi.hostSlot, 1);
        CHECK(gi.hostEpoch >= 1u);
        CHECK_EQ(s.c[g]->lostCalls, 0);
    }
    CHECK(FlowReal(s, 1, {2, 3}, 9800));
    CHECK(FlowReal(s, 2, {1}, 9802));
    for (int g = 1; g < 4; ++g) s.c[g]->net.Stop();
    for (int i = 0; i < 30; ++i) { HttpPump(); usleep(10000); }
}

// Galaxy (fix3): the service drops a guest that is still running (403 not_member after a minute of silence: a Vita asleep, a long
// stall). Its online link fails at once with the code (a session nobody else is in must never go on), the host lets that member go
// and keeps the others, and a fresh join of the same room takes the guest back.
static void TestStarDroppedMember() {
    printf("star: a guest the service dropped fails with not_member (never plays on alone); the host keeps the others; a fresh join takes it back\n");
    Star s(43);
    CHECK(s.StartAll());
    const int ms = s.Until(40000, [&] { return s.Up(0, 1) && s.Up(0, 2) && s.Up(0, 3) && s.Up(1, 0) && s.Up(2, 0) && s.Up(3, 0); });
    CHECK(ms >= 0);
    if (ms < 0) { s.Dump("did not connect"); return; }
    s.Advance(1000);
    s.c[0]->peerLost.clear();
    // the guest hears it from the service before the host tears its path down (the host is not ticked meanwhile): its link to
    // the host is still up, and it must not play on in a room it is no longer in
    s.down[0] = true;
    SignalMemoryHubEvict(s.w.hub, s.invite.c_str(), 2);
    const int failMs = s.Until(3000, [&] { return s.c[2]->net.GetPhase() == OnlineNet::FAILED; });
    s.down[0] = false;
    CHECK(failMs >= 0);
    OnlineInfo info;
    s.c[2]->net.GetInfo(&info);
    CHECK(strcmp(info.errorCode, "not_member") == 0);
    CHECK(strstr(info.error, "not in that room") != 0);
    const int hostMs = s.Until(40000, [&] { return !s.c[0]->peerLost.empty(); });
    CHECK(hostMs >= 0);
    CHECK(!s.c[0]->peerLost.empty() && s.c[0]->peerLost[0] == 2);
    CHECK_EQ(s.c[0]->lostCalls, 0);
    CHECK(s.c[0]->net.GetPhase() == OnlineNet::READY);
    CHECK(s.Flow(0, {1, 3}, 9500));
    CHECK(s.Rejoin(2, 4343));
    const int backMs = s.Until(20000, [&] { return s.Up(0, 2) && s.Up(2, 0); });
    CHECK(backMs >= 0);
    CHECK(s.Flow(2, {0}, 9502));
    printf("  dropped guest failed after %d ms (code not_member); the host let it go after %d ms; it joined again in %d ms\n", failMs, hostMs, backMs);
}

static void RunStarTests() {
    TestRoomMembersHttp();
    TestStarRig();
    TestStarDroppedMember();
    TestStarHostLeaves();
    TestMeshTopology();
}
