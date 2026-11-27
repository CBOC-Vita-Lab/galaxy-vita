// Galaxy 3.0 multiplayer (agent netx): host tests of internet play by address (platform/src/mp/mp_conn_direct.cpp) with two (three)
// real session cores in one process, real UDP sockets on 127.0.0.1, and the fake router of fake_igd.h for the host's UPnP mapping:
// join by address (HELLO / WELCOME / version fingerprint / roster / a game message both ways), a different version refused, a wrong
// address timing out, the host leaving (no migration by address), a silent host followed by the reconnect path, the mapping removed
// when the room closes, the address parser.
//   tests/mp/upnp_build.sh   (builds and runs this and upnp_test, ASan/UBSan)
#include "core_test_util.h"
#include "fake_igd.h"

#include "../../platform/src/mp/mp_portmap.h"

#include <sys/socket.h>

static MpCore* NewCore(const char* name, uint32_t seed, uint32_t dataHash) {
    MpCoreOptions o;
    memset(&o, 0, sizeof o);
    o.useThread = false;
    o.now = VNow;
    o.seed = seed;
    MpCore* c = new MpCore(o);
    c->setDataHash(dataHash);
    c->setProfile(name, MP_LOOK_MARIO);
    return c;
}

// virtual time for the cores, real time for the router (its answers come from a thread): every step also sleeps a little
static void StepReal(std::vector<MpCore*>& cs, int ms) {
    for (int t = 0; t < ms; t += 5) {
        g_now += 5;
        for (MpCore* c : cs)
            if (c) c->pump();
        mp_portmap_pump();
        usleep(500);
    }
}
static bool WaitFor(std::vector<MpCore*>& cs, MpCore* c, MpUiPhase p, int maxMs) {
    for (int t = 0; t < maxMs; t += 20) {
        if (Phase(c) == p) return true;
        StepReal(cs, 20);
    }
    return Phase(c) == p;
}
static int FreePort() {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr*)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(s, (sockaddr*)&a, &l);
    close(s);
    return ntohs(a.sin_port);
}

static void TestParse() {
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(mp_direct_parse_address("203.0.113.7", &ip, &port) && ip == 0xCB007107u && port == 41100);
    CHECK(mp_direct_parse_address(" 203.0.113.7:41101 ", &ip, &port) && port == 41101);
    CHECK(mp_direct_parse_address("203.0.113.7 41102", &ip, &port) && port == 41102);
    CHECK(mp_direct_parse_address("203.0.113.7, 41103", &ip, &port) && port == 41103);
    CHECK(mp_direct_parse_address("127.0.0.1:5000", &ip, &port) && ip == 0x7F000001);
    const char* bad[] = {"", "203.0.113", "203.0.113.7.1", "203.0.113.256", "203.0.113.7:0", "203.0.113.7:65536", "203.0.113.7:", "0.1.2.3",
                         "224.0.0.1", "255.255.255.255", "example.com", "203.0.113.7:41100x", "1.2.3.4:1:2", "１.2.3.4",
                         "203.0.113.7:123456", "1.2.3.4 5 6"};
    for (const char* b : bad) CHECK(!mp_direct_parse_address(b, &ip, &port));
    char huge[300];
    memset(huge, '1', sizeof huge - 1);
    huge[sizeof huge - 1] = 0;
    CHECK(!mp_direct_parse_address(huge, &ip, &port));
}

static MpHostParams DirectHost() {
    MpHostParams p = HostParams(MP_MODE_TOGETHER, 4);
    p.conn = MP_CONN_DIRECT;
    return p;
}

int main() {
    mp_log_set_sink(LogSink, !getenv("DIRECT_TEST_VERBOSE"));
    if (getenv("DIRECT_TEST_VERBOSE")) g_echo = true;
    TestParse();

    FakeIgd igd;
    igd.extIp = "203.0.113.7";
    CHECK(igd.Start());
    char ssdp[32];
    snprintf(ssdp, sizeof ssdp, "127.0.0.1:%d", igd.ssdpPort);
    setenv("SMG_MP_UPNP_SSDP", ssdp, 1);
    setenv("SMG_MP_NATPMP_GW", "127.0.0.1:9", 1);   // MP3 natpmp: NAT-PMP asks here (nobody answers: no traffic to the machine's real router)
    setenv("SMG_NET_LOCAL_IP", "127.0.0.1", 1);   // (the socket's own address: the loopback here)
    setenv("SMG_MP_STUN", "0", 1);                // (no STUN server in a host test)
    const int hostPort = FreePort();
    char portText[16];
    snprintf(portText, sizeof portText, "%d", hostPort);
    setenv("SMG_MP_DIRECT_PORT", portText, 1);

    // ---- host by address: the router maps the port, the lobby shows the public address -------------------------------------------
    MpCore* host = NewCore("Alex", 11, 0x1234);
    MpCore* guest = NewCore("Luigi64", 22, 0x1234);
    std::vector<MpCore*> all = {host, guest};
    MpHostParams hp = DirectHost();
    CHECK(host->uiHost(&hp));
    CHECK(WaitFor(all, host, MP_UI_LOBBY, 2000));
    MpAddressInfo ai;
    for (const uint64_t t0 = NowMs(); NowMs() - t0 < 4000;) {
        if (host->uiHostAddress(&ai) && ai.source != MP_ADDR_SRC_NONE) break;
        StepReal(all, 20);
    }
    char want[32];
    snprintf(want, sizeof want, "203.0.113.7:%d", hostPort);
    CHECK(host->uiHostAddress(&ai) && ai.source == MP_ADDR_SRC_ROUTER && ai.portmap == MP_PORTMAP_MAPPED && !ai.cgnat);
    CHECK(strcmp(ai.addr, want) == 0);
    CHECK(ai.port == hostPort);
    {
        char line[96];
        snprintf(line, sizeof line, "direct: address %s source=router", want);
        CHECK(LogCount(line) == 1);
        snprintf(line, sizeof line, "AddPortMapping %d lease=3600 127.0.0.1:%d", hostPort, hostPort);
        CHECK(igd.Count(line) == 1);
    }
    CHECK(!guest->uiHostAddress(&ai));

    // ---- a guest of another version: refused at its HELLO ------------------------------------------------------------------------
    MpCore* old = NewCore("Old", 33, 0x9999);
    all.push_back(old);
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", hostPort);
    CHECK(old->uiJoinAddress(addr));
    for (int t = 0; t < 3000 && Phase(old) != MP_UI_IDLE; t += 20) StepReal(all, 20);
    CHECK(Phase(old) == MP_UI_IDLE && Error(old) == MP_ERR_DIFFERENT_VERSION);
    CHECK(LogCount("hello rejected reason=version") >= 1);
    all.pop_back();
    delete old;

    // ---- join by address -----------------------------------------------------------------------------------------------------------
    CHECK(!guest->uiJoinAddress("not an address") && Error(guest) == MP_ERR_ROOM_NOT_FOUND);
    CHECK(guest->uiJoinAddress(addr));
    CHECK(WaitFor(all, guest, MP_UI_LOBBY, 3000));
    CHECK(LogCount("join start conn=direct") >= 1);
    CHECK(host->slotPresent(1) && guest->slotPresent(0) && guest->localSlot() == 1);
    char nm[MP_NAME_BYTES + 1];
    guest->slotName(0, nm);
    CHECK(strcmp(nm, "Alex") == 0);
    // a game message each way
    const uint8_t emote[2] = {2, 1};
    CHECK(guest->send(MP_MSG_EMOTE, emote, 2, true));
    CHECK(host->send(MP_MSG_EMOTE, emote, 2, true));
    StepReal(all, 200);
    MpMsg m;
    bool hostGot = false, guestGot = false;
    while (host->poll(&m)) if (m.type == MP_MSG_EMOTE && m.fromSlot == 1) hostGot = true;
    while (guest->poll(&m)) if (m.type == MP_MSG_EMOTE && m.fromSlot == 0) guestGot = true;
    CHECK(hostGot && guestGot);

    // ---- the host goes silent (a hiccup on the way): the guest does not migrate, it rejoins the same address ------------------------
    {
        std::vector<MpCore*> onlyGuest = {guest};
        for (int t = 0; t < MP_HOST_LOST_MS + 1500; t += 20) StepReal(onlyGuest, 20);
        CHECK(LogCount("reason=host-left (the host is silent)") == 1);
        CHECK(LogCount("becomes host") == 0 && LogCount("claim") == 0);
        CHECK(WaitFor(all, guest, MP_UI_LOBBY, 6000));
        CHECK(LogCount("rejoin start conn=direct") >= 1);
        CHECK(host->slotPresent(1) && guest->localSlot() == 1);
    }

    // ---- the host closes the room: the guest's session ends ("The host left"), no migration, the router mapping is removed ----------
    host->uiLeave();
    for (int t = 0; t < 3000 && (Phase(guest) != MP_UI_IDLE || Phase(host) != MP_UI_IDLE); t += 20) StepReal(all, 20);
    CHECK(Phase(host) == MP_UI_IDLE);
    CHECK(Phase(guest) == MP_UI_IDLE && Error(guest) == MP_ERR_HOST_LEFT);
    for (int t = 0; t < 3000 && mp_portmap_busy(); t += 20) StepReal(all, 20);
    {
        char line[64];
        snprintf(line, sizeof line, "DeletePortMapping %d", hostPort);
        CHECK(igd.Count(line) == 1);
        CHECK(LogCount("upnp: removed UDP") == 1);
    }

    // ---- nobody at the address: the attempt times out with an error, nothing hangs ------------------------------------------------
    char nobody[32];
    snprintf(nobody, sizeof nobody, "127.0.0.1:%d", FreePort());
    CHECK(guest->uiJoinAddress(nobody));
    for (int t = 0; t < MP_CONNECT_TIMEOUT_MS + 2000 && Phase(guest) != MP_UI_IDLE; t += 20) StepReal(all, 20);
    CHECK(Phase(guest) == MP_UI_IDLE && Error(guest) == MP_ERR_TIMEOUT);

    // ---- no UPnP router: the host still opens, the address falls back to the LAN one, the lobby says a forward is needed ----------
    setenv("SMG_MP_UPNP_SSDP", "127.0.0.1:9", 1);   // (nobody answers there)
    MpCore* host2 = NewCore("Ros", 44, 0x1234);
    std::vector<MpCore*> two = {host2};
    CHECK(host2->uiHost(&hp));
    CHECK(WaitFor(two, host2, MP_UI_LOBBY, 2000));
    for (const uint64_t t0 = NowMs(); NowMs() - t0 < 6000;) {   // (the router search runs on real time: 3 s)
        if (host2->uiHostAddress(&ai) && ai.source != MP_ADDR_SRC_NONE) break;
        StepReal(two, 20);
    }
    CHECK(host2->uiHostAddress(&ai) && ai.source == MP_ADDR_SRC_LAN && ai.portmap == MP_PORTMAP_FAILED);
    CHECK(LogCount("upnp: failed: no UPnP router answered") == 1);
    host2->uiLeave();
    StepReal(two, 500);

    // ---- mp74: a console that refuses every chosen port (the PS Vita, console log c48: 0x8041010D on all of them, port 0 fine): the host's
    //      room still opens on the port the system chose, the address carries that port, a guest joins it and the room does not end
    {
        setenv("SMG_NET_REFUSE_CHOSEN", "1", 1);
        setenv("SMG_MP_UPNP_SSDP", "127.0.0.1:9", 1);
        MpCore* h3 = NewCore("Ina", 55, 0x1234);
        MpCore* g3 = NewCore("Pip", 66, 0x1234);
        std::vector<MpCore*> three = {h3, g3};
        CHECK(h3->uiHost(&hp));
        CHECK(WaitFor(three, h3, MP_UI_LOBBY, 2000));
        CHECK(LogCount("could not be bound") >= 1 && LogCount("the system chooses one") >= 1);
        MpAddressInfo a3;
        CHECK(h3->uiHostAddress(&a3));
        CHECK(a3.port != 0 && a3.port != hostPort);   // (the port really bound, not the refused one)
        char to3[32];
        snprintf(to3, sizeof to3, "127.0.0.1:%d", (int)a3.port);
        CHECK(g3->uiJoinAddress(to3));
        CHECK(WaitFor(three, g3, MP_UI_LOBBY, 3000));
        CHECK(h3->slotPresent(1) && g3->localSlot() == 1);
        StepReal(three, 5000);   // the room lives on
        CHECK(Phase(h3) == MP_UI_LOBBY && Phase(g3) == MP_UI_LOBBY && h3->slotPresent(1));
        h3->shutdown();
        g3->shutdown();
        delete h3;
        delete g3;
        unsetenv("SMG_NET_REFUSE_CHOSEN");
    }

    host->shutdown();
    guest->shutdown();
    host2->shutdown();
    delete host;
    delete guest;
    delete host2;
    igd.Stop();
    if (g_fail) {
        fprintf(stderr, "---- log ----\n");
        for (auto& l : g_log) fprintf(stderr, "%s\n", l.c_str());
    }
    printf("direct_test: %s (%d failed checks)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
