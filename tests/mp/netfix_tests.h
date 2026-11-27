// netfix: the outside review's open findings B1, B2, B4 (back-pressure removed healthy players). Included by core_tests.cpp; run with MP_ONLY=Netfix.
#pragma once

namespace netfix {

// Poses at ~60 Hz from the awake consoles, tracked events (KILL, Xo) from `evSenders` every `evEveryMs`; console `idle` is not polled
// (its game thread is busy: a scene load) for `idleMs`, then everyone polls.
static void Probe(const char* what, int players, int idle, int idleMs, std::vector<int> evSenders, int evEveryMs, int bigEveryMs, bool* okOut) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, players, 9100 + (uint32_t)idleMs / 100);
    all[0]->uiStart();
    Step(all, 500);
    Xo x(all);
    uint8_t pose[51];
    memset(pose, 0, sizeof pose);
    uint8_t smap[568];
    memset(smap, 0x5A, sizeof smap);
    int bigSent = 0;
    bool everGone = false;
    for (int t = 0; t < idleMs; t += 15) {   // (Step runs in 5 ms steps: 15 ms = a 66 Hz pose rate)
        for (int i = 0; i < players; i++) all[i]->send(MP_MSG_STATE, pose, 51, false);
        if (t % evEveryMs < 15) x.sendAll(evSenders);
        if (bigEveryMs && t % bigEveryMs < 15) { if (all[0]->send(MP_MSG_SWITCHMAP, smap, 568, true)) bigSent++; }
        Step(all, 15);
        for (int i = 0; i < players; i++) if (i != idle) x.drain(i);
        if (!all[0]->slotPresent(idle) || !all[idle]->active()) everGone = true;
    }
    for (int t = 0; t < 8000; t += 50) {
        x.sendAll(evSenders);
        Step(all, 50);
        x.drainAll();
        if (getenv("NETFIX_TRACE") && t % 500 == 0) printf("  trace +%d: host unacked to %d = %d\n", t, idle, all[0]->debugUnacked(idle));
    }
    XoStep(x, all, 3000, {}, 100);
    std::vector<int> everyone;
    for (int i = 0; i < players; i++) everyone.push_back(i);
    const bool ok = !everGone && all[0]->slotPresent(idle) && all[idle]->active() && all[0]->isHost() && x.dups == 0 && x.gaps == 0 &&
                    x.tailMissing(everyone) == 0 && x.memb[idle] == 1 && Phase(all[0]) == MP_UI_PLAYING;
    printf("netfix: %s: gone at some point=%d present=%d active=%d dups=%d gaps=%d memb=%u bad=%u phase=%d (big sent %d)\n", what, everGone ? 1 : 0,
           all[0]->slotPresent(idle) ? 1 : 0, all[idle]->active() ? 1 : 0, x.dups, x.gaps, x.memb[idle], (unsigned)all[0]->debugBadCount(), (int)Phase(all[0]), bigSent);
    CHECK(ok);
    if (okOut) *okOut = ok;
    Free(all);
}

// B1: guest 1's game does not poll for 20 s (a scene load) while the others pose at 60 Hz and the host sends 2 reliable events a second
static void TestNetfixPosesFillQueue() { Probe("guest 1 not polling 20 s, poses 60 Hz, host 2 events/s", 3, 1, 20000, {0}, 500, 0, 0); }

// B1 (big blocks): 20 s of 568-byte SWITCHMAPs at 2/s to a console that does not poll: 40 > MP_QBIG blocks; back-pressure, no removal
static void TestNetfixBigBlocks() { Probe("guest 1 not polling 20 s, host sends 2 SWITCHMAP/s", 3, 1, 20000, {0}, 500, 500, 0); }

// B2: the host's game does not poll for 12 s (a load) while a guest sends 10 and 40 reliable events a second (the other guest poses at 60 Hz)
static void TestNetfixHostLoad10() { Probe("host not polling 12 s, guest sends 10 events/s", 3, 0, 12000, {1}, 100, 0, 0); }
static void TestNetfixHostLoad40() { Probe("host not polling 12 s, guest sends 40 events/s", 3, 0, 12000, {1}, 25, 0, 0); }

// B4: a held (acknowledged) message the gate refuses must not stay in its slot once it is delivered in order
static bool g_refuse;
static uint16_t g_refuseSeq;
static int g_delivered;
static bool Gate(void*, const MpRelMsg& m) { return !(g_refuse && m.seq == g_refuseSeq); }
static void Deliv(void*, const MpRelMsg&) { g_delivered++; }
static void TestNetfixStaleHeldSlot() {
    MpReliable* r = new MpReliable();
    r->reset(1);
    r->setGate(Gate);
    uint8_t big[300];
    memset(big, 7, sizeof big);
    uint16_t base = 64;
    r->startRecv(base);
    int heldLater = 0;
    for (int round = 0; round < 20; round++) {
        MpRelMsg a = {(uint16_t)base, 0x42, 1, 9, big};
        MpRelMsg b = {(uint16_t)(base + 1), 0x41, 1, 300, big};
        g_refuse = true;
        g_refuseSeq = b.seq;
        CHECK(r->receive(b, Deliv, 0) == 1);   // held (out of order)
        CHECK(r->receive(a, Deliv, 0) == 1);   // in order: delivered; draining the held one is refused by the gate
        g_refuse = false;
        CHECK(r->receive(b, Deliv, 0) == 1);   // its resend arrives in order: delivered (once)
        base += 2;
    }
    // every big block must still be free: 16 held big messages fit
    for (int i = 0; i < 16; i++) {
        MpRelMsg h = {(uint16_t)(base + 1 + i), 0x41, 1, 300, big};
        if (r->receive(h, Deliv, 0) == 1) heldLater++;
    }
    printf("netfix: stale held slots: 20 refused-then-resent big messages, then %d of 16 big messages could be held out of order\n", heldLater);
    CHECK(heldLater == 16);
    // and the sequence that comes back around to the slot is not refused by a leftover
    g_delivered = 0;
    MpRelMsg first = {base, 0x42, 1, 9, big};
    CHECK(r->receive(first, Deliv, 0) == 1);
    CHECK(g_delivered == 17);   // the 16 held ones followed in order
    delete r;
}

// B5c: a stranger's incomplete fragments must not evict a member's partial frame (4 reassembly slots, no per-source cap)
static void Frag(std::vector<uint8_t>& out, uint16_t id, uint8_t index, uint8_t count, uint16_t offset, uint16_t total, const uint8_t* d, size_t n) {
    out.assign(12 + n, 0);
    revsec::P32(&out[0], 0x534D4746u);
    revsec::P16(&out[4], id);
    out[6] = index;
    out[7] = count;
    revsec::P16(&out[8], offset);
    revsec::P16(&out[10], total);
    memcpy(&out[12], d, n);
}
static void TestNetfixFragmentSlots() {
    revsec::FakeBackend hb;
    memset(&hb.b, 0, sizeof hb.b);
    hb.b.name = "fake"; hb.b.ctx = &hb; hb.b.local_vip = revsec::FbVip; hb.b.send = revsec::FbSend; hb.b.recv = revsec::FbRecv; hb.b.close = revsec::FbClose;
    hb.vip = 0x0A123457u;
    hb.directHost = false;
    VLink::Link host;
    host.SetBackend(&hb.b);
    host.SetFingerprint(MP_LINK_FINGERPRINT);
    host.SetClock(revsec::LinkNow, 0);
    CHECK(host.Start());
    const int sock = host.OpenSocket();
    CHECK(host.Bind(sock, MP_SESSION_PORT));
    const uint32_t memberVip = 0x0A0BCDEFu;
    const VLink::PeerAddr memberReal = revsec::PA(0xC6336401u, 50000), strangerReal = revsec::PA(0xCB007105u, 40404);
    uint8_t payload[40];
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i + 1);
    const std::vector<uint8_t> whole = revsec::Frame(memberVip, hb.vip, MP_SESSION_PORT, MP_SESSION_PORT, payload, sizeof payload);   // 64 bytes
    std::vector<uint8_t> f;
    Frag(f, 7, 0, 2, 0, (uint16_t)whole.size(), whole.data(), 32);
    hb.inbox.push_back({memberReal, f});
    host.Pump();
    uint8_t junk[32] = {0};
    for (uint16_t id = 100; id < 110; id++) {   // the stranger starts ten frames and never finishes one
        Frag(f, id, 0, 2, 0, 64, junk, 32);
        hb.inbox.push_back({strangerReal, f});
    }
    host.Pump();
    Frag(f, 7, 1, 2, 32, (uint16_t)whole.size(), whole.data() + 32, whole.size() - 32);
    hb.inbox.push_back({memberReal, f});
    uint8_t buf[256];
    uint32_t sv = 0;
    uint16_t sp = 0;
    const int n = host.RecvFrom(sock, buf, sizeof buf, &sv, &sp);
    printf("netfix vlink: member's partial frame after a stranger's ten unfinished frames: delivered=%d\n", n == (int)sizeof payload ? 1 : 0);
    CHECK(n == (int)sizeof payload && sv == memberVip && !memcmp(buf, payload, sizeof payload));
    host.Stop();
}

}  // namespace netfix

static void TestNetfixPosesFillQueue() { netfix::TestNetfixPosesFillQueue(); }
static void TestNetfixBigBlocks() { netfix::TestNetfixBigBlocks(); }
static void TestNetfixHostLoad10() { netfix::TestNetfixHostLoad10(); }
static void TestNetfixHostLoad40() { netfix::TestNetfixHostLoad40(); }
static void TestNetfixStaleHeldSlot() { netfix::TestNetfixStaleHeldSlot(); }
static void TestNetfixFragmentSlots() { netfix::TestNetfixFragmentSlots(); }
