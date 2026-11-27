// Outside security review (rev-sec): proofs of the findings. Included by core_tests.cpp; run with MP_ONLY=RevSec.
// Every test here asserts what SHOULD hold for a release to strangers; each one FAILS on the reviewed code (mp-perf 5f26822f).
#pragma once

#include <deque>

#include "vlink.h"
#include "../../platform/src/mp/mp_transport_vlink.h"

namespace revsec {

// ---- 1/2. VLink (the transport under Same Wi-Fi, Nearby and Play by Address): a frame's source vip is the peer's identity and
// re-binds that vip to whatever real address the frame came from; 16 fresh vips evict every real peer.
struct FakeBackend {
    VLink::Backend b;
    uint32_t vip;
    bool directHost;   // Play by Address host: a "broadcast" has no target and is dropped (mp_conn_direct.cpp BSend)
    struct In { VLink::PeerAddr from; std::vector<uint8_t> d; };
    std::deque<In> inbox;
    struct Out { bool bcast; VLink::PeerAddr to; std::vector<uint8_t> d; };
    std::vector<Out> sent;
};
static uint32_t FbVip(void* c) { return ((FakeBackend*)c)->vip; }
static int FbSend(void* c, const VLink::PeerAddr* to, bool bcast, const void* data, size_t len) {
    FakeBackend& f = *(FakeBackend*)c;
    FakeBackend::Out o;
    o.bcast = bcast || !to;
    memset(&o.to, 0, sizeof o.to);
    if (to) o.to = *to;
    o.d.assign((const uint8_t*)data, (const uint8_t*)data + len);
    f.sent.push_back(o);
    return (int)len;
}
static int FbRecv(void* c, VLink::PeerAddr* from, void* buf, size_t cap) {
    FakeBackend& f = *(FakeBackend*)c;
    if (f.inbox.empty()) return 0;
    FakeBackend::In i = f.inbox.front();
    f.inbox.pop_front();
    *from = i.from;
    const size_t n = i.d.size() < cap ? i.d.size() : cap;
    memcpy(buf, i.d.data(), n);
    return (int)n;
}
static void FbClose(void*) {}
static VLink::PeerAddr PA(uint32_t ip, uint16_t port) {
    VLink::PeerAddr a;
    memset(&a, 0, sizeof a);
    memcpy(a.bytes, &ip, 4);
    memcpy(a.bytes + 4, &port, 2);
    return a;
}
static bool SamePA(const VLink::PeerAddr& a, const VLink::PeerAddr& b) { return !memcmp(&a, &b, sizeof a); }
static void P32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void P16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
// A VLink frame exactly as any console builds it: everything in it is public (magic, version, the build's link fingerprint "SMGP").
static std::vector<uint8_t> Frame(uint32_t srcVip, uint32_t dstVip, uint16_t sport, uint16_t dport, const void* pl, size_t n) {
    std::vector<uint8_t> f(24 + n, 0);
    P32(&f[0], 0x534D474Bu);
    f[4] = 1;
    P16(&f[6], (uint16_t)n);
    P32(&f[8], MP_LINK_FINGERPRINT);
    P32(&f[12], srcVip);
    P32(&f[16], dstVip);
    P16(&f[20], sport);
    P16(&f[22], dport);
    if (n) memcpy(&f[24], pl, n);
    return f;
}
static uint64_t LinkNow(void*) { return g_now; }

static void TestRevSecVLinkIdentity() {
    FakeBackend hb;
    memset(&hb.b, 0, sizeof hb.b);
    hb.b.name = "fake-direct-host"; hb.b.ctx = &hb; hb.b.local_vip = FbVip; hb.b.send = FbSend; hb.b.recv = FbRecv; hb.b.close = FbClose;
    hb.vip = 0x0A123457u;
    hb.directHost = true;
    VLink::Link host;
    host.SetBackend(&hb.b);
    host.SetFingerprint(MP_LINK_FINGERPRINT);
    host.SetClock(LinkNow, 0);
    CHECK(host.Start());
    const int sock = host.OpenSocket();
    CHECK(host.Bind(sock, MP_SESSION_PORT));
    const uint32_t guestVip = 0x0A0BCDEFu;
    const VLink::PeerAddr guestReal = PA(0xC6336401u, 50000);      // the guest's NAT address (198.51.100.1)
    const VLink::PeerAddr strangerReal = PA(0xCB007105u, 40404);   // a stranger on the internet (203.0.113.5), no spoofing
    const uint8_t hello[4] = {'G', 'M', 3, 1};
    // the real guest talks to the host: the host learns guestVip -> guestReal
    hb.inbox.push_back({guestReal, Frame(guestVip, hb.vip, MP_SESSION_PORT, MP_SESSION_PORT, hello, 4)});
    uint8_t buf[2048];
    uint32_t sv = 0;
    uint16_t sp = 0;
    CHECK(host.RecvFrom(sock, buf, sizeof buf, &sv, &sp) == 4 && sv == guestVip);
    hb.sent.clear();
    host.SendTo(sock, guestVip, MP_SESSION_PORT, "ok", 2);
    CHECK(hb.sent.size() == 1 && !hb.sent[0].bcast && SamePA(hb.sent[0].to, guestReal));
    // (a) one frame from the stranger's own address, claiming the guest's vip: MpCore receives it as the guest ...
    const uint8_t forged[4] = {'G', 'M', 3, 0x42};
    hb.inbox.push_back({strangerReal, Frame(guestVip, hb.vip, MP_SESSION_PORT, MP_SESSION_PORT, forged, 4)});
    sv = 0;
    const int n = host.RecvFrom(sock, buf, sizeof buf, &sv, &sp);
    const bool asGuest = n == 4 && sv == guestVip && buf[3] == 0x42;
    // ... and everything the host sends the guest now goes to the stranger
    hb.sent.clear();
    host.SendTo(sock, guestVip, MP_SESSION_PORT, "secret", 6);
    const bool hijacked = hb.sent.size() == 1 && SamePA(hb.sent[0].to, strangerReal);
    printf("revsec vlink: a stranger's frame is taken as the guest's: %d; the host's next datagram to the guest goes to the stranger: %d\n",
           asGuest ? 1 : 0, hijacked ? 1 : 0);
    CHECK(!asGuest);    // FAILS: identity = the vip written in the frame
    CHECK(!hijacked);   // FAILS: Learn() moves the vip to the frame's source address
    // (a) without knowing any vip: 16 frames with made-up source vips, sent to the broadcast vip, evict the real guest's mapping
    hb.inbox.push_back({guestReal, Frame(guestVip, hb.vip, MP_SESSION_PORT, MP_SESSION_PORT, hello, 4)});   // (the guest takes it back)
    host.RecvFrom(sock, buf, sizeof buf, &sv, &sp);
    for (uint32_t i = 0; i < 16; i++)
        hb.inbox.push_back({strangerReal, Frame(0x0A700000u + i * 2 + 1, 0xFFFFFFFFu, 1, 1, "x", 1)});   // (port 1: no socket; learnt anyway)
    host.Pump();
    hb.sent.clear();
    host.SendTo(sock, guestVip, MP_SESSION_PORT, "ok", 2);
    const bool evicted = hb.sent.size() == 1 && hb.sent[0].bcast;   // a Play by Address host drops a "broadcast": the guest hears nothing
    printf("revsec vlink: 16 frames with invented vips from a stranger evict the guest (host's reply goes out as an undeliverable broadcast): %d\n",
           evicted ? 1 : 0);
    CHECK(!evicted);   // FAILS
    host.Stop();
}

// ---- 3. a stranger who can send one datagram with the host's source address ends a guest's session (no sequence, no state)
static void TestRevSecSpoofedRejectEndsGuest() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 9001);
    all[0]->uiStart();
    Step(all, 500);
    CHECK(Phase(all[1]) == MP_UI_PLAYING);
    const MpAddr haddr = all[0]->debugLocalAddr(), gaddr = all[1]->debugLocalAddr();
    std::vector<uint8_t> pkt;
    const uint8_t reason = MP_REJECT_CLOSED;
    WritePacket(pkt, MP_MSG_REJECT, 0 /* host slot */, 0, 0, &reason, 1);
    hub.inject(haddr, gaddr, pkt.data(), pkt.size());
    Step(all, 200);
    const bool ended = Phase(all[1]) != MP_UI_PLAYING;
    printf("revsec: one forged REJECT(closed) with the host's address: guest phase=%d error=%d (ended=%d)\n", (int)Phase(all[1]), (int)Error(all[1]),
           ended ? 1 : 0);
    CHECK(!ended);   // FAILS
    Free(all);
}

// ---- 4. ... and 51 junk datagrams with a guest's source address make the host kick that guest (the bad-message counter is the
// address's, not the guest's)
static void TestRevSecSpoofedJunkKicksGuest() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 9002);
    all[0]->uiStart();
    Step(all, 500);
    const MpAddr haddr = all[0]->debugLocalAddr(), gaddr = all[1]->debugLocalAddr();
    std::vector<uint8_t> pkt;
    for (int i = 0; i < 60; i++) {
        WritePacket(pkt, MP_MSG_HEARTBEAT, 1, 0, 0, 0, 0);
        pkt[2] = 0x77;   // wrong protocol byte -> bad(slot, "protocol")
        hub.inject(gaddr, haddr, pkt.data(), pkt.size());
    }
    Step(all, 2500);
    const bool kicked = !all[0]->slotPresent(1);
    printf("revsec: 60 junk datagrams with guest 1's address: host still has slot 1=%d, guest 1 phase=%d error=%d (kicked=%d)\n",
           all[0]->slotPresent(1) ? 1 : 0, (int)Phase(all[1]), (int)Error(all[1]), kicked ? 1 : 0);
    CHECK(!kicked);   // FAILS
    Free(all);
}

// ---- 5. unreliable delivery of event / host-only types: a forged datagram with the host's address moves a guest (MOVE), one with
// a guest's address gives a STAR to everyone (relayed) -- no reliable sequence number needed
static void TestRevSecUnreliableEvents() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 9003);
    all[0]->uiStart();
    Step(all, 500);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    const MpAddr haddr = all[0]->debugLocalAddr(), g1 = all[1]->debugLocalAddr();
    std::vector<uint8_t> pkt;
    uint8_t move[10] = {3, 0, 1, 2, 0, 0, 0x34, 0x12, 0, 0};   // serial 0x1234 at bytes 6..7 (the core's snoopWhere takes it too)
    WritePacket(pkt, MP_MSG_MOVE, 0, 0 /* unreliable */, 7, move, sizeof move);
    hub.inject(haddr, g1, pkt.data(), pkt.size());
    uint8_t star[8] = {0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0};
    WritePacket(pkt, MP_MSG_STAR, 1, 0 /* unreliable */, 7, star, sizeof star);
    hub.inject(g1, haddr, pkt.data(), pkt.size());
    Step(all, 200);
    bool moved = false, hostStar = false, g2Star = false;
    while (all[1]->poll(&m)) if (m.type == MP_MSG_MOVE && m.fromSlot == 0) moved = true;
    while (all[0]->poll(&m)) if (m.type == MP_MSG_STAR && m.fromSlot == 1) hostStar = true;
    while (all[2]->poll(&m)) if (m.type == MP_MSG_STAR && m.fromSlot == 1) g2Star = true;
    printf("revsec: forged unreliable MOVE as the host reaches guest 1's game: %d (core serial now %04x); forged unreliable STAR as guest 1 reaches "
           "the host's game: %d and is relayed to guest 2: %d\n", moved ? 1 : 0, (unsigned)all[1]->sceneSerial(), hostStar ? 1 : 0, g2Star ? 1 : 0);
    CHECK(!moved);      // FAILS: host-only types are accepted without the reliable channel
    CHECK(!hostStar);   // FAILS
    CHECK(!g2Star);     // FAILS
    Free(all);
}

// ---- 6. latest-wins with a forged sequence: one STATE with seq = real + 32000 freezes the victim's puppet on every console until the
// victim's own counter passes it (32,000 poses: ~9 minutes at 60 Hz). Sniffed seq (same Wi-Fi); blind needs 2-3 tries.
static uint16_t g_lastStateSeq = 0;
static void StateTap(void*, const MpAddr& from, const MpAddr&, const void* data, size_t len) {
    if (from.host != 2) return;   // guest 1 (endpoint 2)
    ForEachPacket((const uint8_t*)data, len, [](void*, const uint8_t* p, size_t) {
        if (p[3] == MP_MSG_STATE && !(p[5] & MP_HDR_RELIABLE)) g_lastStateSeq = (uint16_t)(p[6] | (p[7] << 8));
    }, 0);
}
static void TestRevSecPoseFreeze() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 9004);
    all[0]->uiStart();
    Step(all, 500);
    hub.setTap(StateTap, 0);
    MpPose pose;
    memset(&pose, 0, sizeof pose);
    uint8_t pb[64];
    auto sendPose = [&](float x) {
        pose.pos[0] = x;
        MpWriter w(pb, sizeof pb);
        mp_write_pose(w, pose);
        all[1]->send(MP_MSG_STATE, pb, (uint16_t)w.n, false);
    };
    for (int i = 0; i < 10; i++) { sendPose(1.0f); Step(all, 20); }
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    // the forged pose (the guest "far away"), with the guest's address and seq + 32000
    pose.pos[0] = 99999.0f;
    MpWriter w(pb, sizeof pb);
    mp_write_pose(w, pose);
    std::vector<uint8_t> pkt;
    WritePacket(pkt, MP_MSG_STATE, 1, 0, (uint16_t)(g_lastStateSeq + 32000), pb, (uint16_t)w.n);
    hub.inject(all[1]->debugLocalAddr(), all[0]->debugLocalAddr(), pkt.data(), pkt.size());
    Step(all, 50);
    int real = 0, forged = 0;
    for (int i = 0; i < 300; i++) {   // 5 s of the real guest's poses at 60 Hz
        sendPose(2.0f);
        Step(all, 15);
        for (int k : {0, 2})
            while (all[k]->poll(&m)) {
                if (m.type != MP_MSG_STATE || m.fromSlot != 1) continue;
                MpReader r(m.data, m.len);
                MpPose p;
                if (!mp_read_pose(r, &p)) continue;
                if (p.pos[0] == 2.0f) real++;
                if (p.pos[0] == 99999.0f) forged++;
            }
    }
    hub.setTap(0, 0);
    printf("revsec: after one forged STATE (seq+32000): the forged pose reached %d console(s); real poses of the next 5 s delivered: %d of 600\n",
           forged, real);
    CHECK(forged == 0);   // FAILS
    CHECK(real > 500);    // FAILS: 0
    Free(all);
}

// ---- 7. a forged acknowledgement (sniffed seq, same Wi-Fi) makes the host drop an in-flight reliable event for good: the victim
// never gets that STAR (exactly-once broken without any error or resync)
static uint16_t g_starSeq = 0;
static bool g_starSeen = false, g_cut = false;
static void AckTap(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    if (from.host != 1 || to.host != 2) return;
    ForEachPacket((const uint8_t*)data, len, [](void*, const uint8_t* p, size_t) {
        if ((p[5] & MP_HDR_RELIABLE) && p[3] == MP_MSG_STAR) { g_starSeq = (uint16_t)(p[6] | (p[7] << 8)); g_starSeen = true; }
    }, 0);
}
static bool AckFilter(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) { return !(g_cut && from.host == 1 && to.host == 2); }
static void TestRevSecForgedAck() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 9005);
    all[0]->uiStart();
    Step(all, 500);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    hub.setTap(AckTap, 0);
    hub.setFilter(AckFilter, 0);
    g_cut = true;   // (the host's datagrams to the guest are lost in the air for a moment, as on any Wi-Fi)
    uint8_t star[8] = {7, 0, 0, 0, 0xA5, 0x5A, 0, 0};
    CHECK(all[0]->send(MP_MSG_STAR, star, 8, true));
    g_now += 5;
    all[0]->pump();
    CHECK(g_starSeen);
    std::vector<uint8_t> pkt;
    WritePacket(pkt, 0, 1, MP_HDR_ACK | MP_HDR_ACK_ONLY, 0, 0, 0);
    pkt[8] = (uint8_t)g_starSeq;
    pkt[9] = (uint8_t)(g_starSeq >> 8);
    hub.inject(all[1]->debugLocalAddr(), all[0]->debugLocalAddr(), pkt.data(), pkt.size());
    g_now += 5;
    all[0]->pump();
    const int unacked = all[0]->debugUnacked(1);
    g_cut = false;
    Step(all, 5000);
    bool got = false;
    while (all[1]->poll(&m)) if (m.type == MP_MSG_STAR) got = true;
    hub.setTap(0, 0);
    hub.setFilter(0, 0);
    printf("revsec: forged ack for the in-flight STAR: host unacked after it=%d; the guest got the STAR within 5 s: %d; both still in the session: %d\n",
           unacked, got ? 1 : 0, all[0]->slotPresent(1) && all[1]->active() ? 1 : 0);
    CHECK(got);   // FAILS: lost for good, silently
    Free(all);
}


// ---- 8. a guest within the rate cap ends the whole session: 150 reliable EMOTEs a second while the host's game does not poll for 2 s
// (a level load) fill the host's 256-entry receive queue; a reliable message that does not fit ends the session for everyone
static void TestRevSecHostQueueFlood() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 9006);
    all[0]->uiStart();
    Step(all, 500);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    const uint8_t emote[2] = {0, 0};
    for (int t = 0; t < 2000; t += 20) {
        for (int k = 0; k < 3; k++) all[1]->send(MP_MSG_EMOTE, emote, 2, true);
        Step(all, 20);
        while (all[2]->poll(&m)) {}   // (guest 2's game keeps up; only the host is loading)
    }
    Step(all, 500);
    printf("revsec: guest sends 150 reliable EMOTEs/s for 2 s while the host loads: host phase=%d error=%d bad=%u; guest 2 phase=%d\n",
           (int)Phase(all[0]), (int)Error(all[0]), (unsigned)all[0]->debugBadCount(), (int)Phase(all[2]));
    CHECK(Phase(all[0]) == MP_UI_PLAYING);   // FAILS: "receive queue overflow" ends the host
    Free(all);
}

}  // namespace revsec

static void TestRevSecVLinkIdentity() { revsec::TestRevSecVLinkIdentity(); }
static void TestRevSecSpoofedRejectEndsGuest() { revsec::TestRevSecSpoofedRejectEndsGuest(); }
static void TestRevSecSpoofedJunkKicksGuest() { revsec::TestRevSecSpoofedJunkKicksGuest(); }
static void TestRevSecUnreliableEvents() { revsec::TestRevSecUnreliableEvents(); }
static void TestRevSecPoseFreeze() { revsec::TestRevSecPoseFreeze(); }
static void TestRevSecForgedAck() { revsec::TestRevSecForgedAck(); }
static void TestRevSecHostQueueFlood() { revsec::TestRevSecHostQueueFlood(); }
