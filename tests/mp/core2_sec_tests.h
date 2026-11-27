// core2 stage 3 (DESIGN.md section 10): the cookie join, the code secret, the failed-proof ceiling, ghost joiners that restart their
// HELLO (outside review), sealed rebinding, the departed notice, tampering. Included by core_tests.cpp; run with MP_ONLY=Core2Sec.
#pragma once

// (outside review, mp-int 9844032) a ghost that does the whole cookie join, never acknowledges, and sends a new HELLO every 4 s from the
// same address kept a 2-player room full for 60 s (the never-acknowledged clock restarted at each HELLO). Now the clock carries over,
// and a full room lets the oldest never-acknowledging joiner go when a real player asks.
static void TestCore2SecGhostRestart() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 2201);
    MpCore* real = MakeCore(hub, 3, "Real", 2202);
    std::vector<MpCore*> all = {h};
    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 2);
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    RawJoiner ghost(7, h->debugLocalAddr());
    ghost.fp = h->fingerprint();
    RawJoiner::Hello hl;
    hl.name = "Ghost";
    hl.consoleId = 0x77777777u;
    CHECK(ghost.join(hub, all, hl, 0x1000));
    ghost.done(hub);
    Step(all, 50);
    CHECK(h->slotPresent(1));
    // (1) the exact sequence: a new HELLO (new seq) every 4 s; the slot must be free within MP_ACK_STALL_MS + a margin
    uint64_t freedAt = 0;
    for (int t = 0, k = 1; t < 20000 && !freedAt; t += 50) {
        if (t % 4000 == 3950) {
            ghost.haveCookie = false;
            CHECK(ghost.join(hub, all, hl, (uint16_t)(0x1000 + k++)));
            ghost.done(hub);
        }
        Step(all, 50);
        if (!h->slotPresent(1)) freedAt = (uint64_t)t;
    }
    printf("core2: a ghost restarting its HELLO every 4 s holds the slot %llu ms (limit %d)\n", (unsigned long long)freedAt, MP_ACK_STALL_MS);
    CHECK(freedAt > 0 && freedAt <= (uint64_t)MP_ACK_STALL_MS + 1500);
    // (2) a full room: the ghost (in for a second, never acknowledging) gives way to a real player at once
    ghost.haveCookie = false;
    CHECK(ghost.join(hub, all, hl, 0x2000));
    ghost.done(hub);
    Step(all, 1000);
    CHECK(h->slotPresent(1));
    all.push_back(real);
    CHECK(ScanJoin(all, real));
    CHECK(WaitPhase(all, real, MP_UI_LOBBY, 3000));
    CHECK(real->localSlot() > 0 && LogCount("a joiner that never acknowledged gives way") >= 1);   // (slot 1 stays reserved for the ghost)
    Free(all);
}

// the code secret (Play by Address): a joiner with the wrong secret gets no slot and no answer; the right one joins. Proofs that fail
// are looked at at most MP_PROOF_FAILS_PER_SEC a second, whoever sends them.
static void TestCore2SecCodeSecret() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 2211);
    MpCore* bad = MakeCore(hub, 2, "Wrong", 2212);
    MpCore* good = MakeCore(hub, 3, "Right", 2213);
    const uint8_t secret[] = "SECRET-12345", wrong[] = "SECRET-12346";
    for (MpCore* c : {h, bad, good}) c->debugRoomSecretAnyConn(true);
    h->setRoomSecret(secret, sizeof secret);
    bad->setRoomSecret(wrong, sizeof wrong);
    good->setRoomSecret(secret, sizeof secret);
    std::vector<MpCore*> all = {h, bad};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    const int proofs = LogCount("hello rejected reason=proof");
    CHECK(ScanJoin(all, bad));
    Step(all, 1500);
    CHECK(Phase(bad) != MP_UI_LOBBY && !h->slotPresent(1) && LogCount("hello rejected reason=proof") > proofs);
    bad->uiLeave();
    all.push_back(good);
    CHECK(ScanJoin(all, good));
    CHECK(WaitPhase(all, good, MP_UI_LOBBY, 3000));
    CHECK(h->slotPresent(good->localSlot()));
    // the ceiling: 20 guessers in one second (each with its own address and a valid cookie): at most MP_PROOF_FAILS_ALL_PER_SEC proofs are
    // checked (corefix D3: each address has its own small allowance; the ceiling for all strangers together is the larger one)
    std::vector<RawJoiner*> js;
    for (int i = 0; i < 20; i++) {
        RawJoiner* j = new RawJoiner(100 + (uint32_t)i, h->debugLocalAddr());
        j->fp = h->fingerprint();
        js.push_back(j);
    }
    // (cookies: the tap catches them one joiner at a time)
    for (RawJoiner* j : js) {
        hub.setTap(RawJoiner::Tap, j);
        j->hello1(hub);
        Step(all, 5);
        hub.setTap(0, 0);
        CHECK(j->haveCookie);
    }
    const uint32_t stray0 = h->debugStray();
    const int p0 = LogCount("hello rejected reason=proof");
    for (RawJoiner* j : js) {
        RawJoiner::Hello hl;
        hl.badProof = true;
        std::vector<uint8_t> b = j->hello2(hl, 0x3000);
        hub.inject(j->me, j->host, b.data(), b.size());
    }
    Step(all, 1);
    printf("core2: 20 wrong proofs in one second: %d looked at, %u dropped unread\n", LogCount("hello rejected reason=proof") - p0,
           (unsigned)(h->debugStray() - stray0));
    // corefix2: the work stays bounded by the key-exchange budget (20 a second, MP_PAIRKEYS_PER_SEC: one X25519 per proof checked), but the
    // room-wide ceiling of 16 only holds suspects (sources that failed a proof lately): 20 first-time sources are each looked at once, so
    // that wrong proofs from a few addresses cannot keep a newcomer out (TestProofCeilingFair)
    CHECK(LogCount("hello rejected reason=proof") - p0 <= MP_PAIRKEYS_PER_SEC);
    for (RawJoiner* j : js) delete j;
    for (int s = 1; s < MP_MAX_PLAYERS; s++) CHECK(s == good->localSlot() || !h->slotPresent(s));
    Free(all);
}

// sealed traffic: a member's datagram from a new address (NAT rebinding) moves it; the same bytes again are a replay; any single
// flipped byte of a sealed datagram is dropped unread (stray), never counted against the member, and the session goes on.
static std::vector<std::vector<uint8_t>> g_sealedSeen;
static void SealedTap(void*, const MpAddr& from, const MpAddr&, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    if (from.host == 2 && len >= 2 && d[0] == 'G' && d[1] == 'S' && g_sealedSeen.size() < 40) g_sealedSeen.emplace_back(d, d + len);
}
static void TestCore2SecSealed() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    g_sealedSeen.clear();
    hub.setTap(SealedTap, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 2221);
    all[0]->uiStart();
    Step(all, 500);
    hub.setTap(0, 0);
    CHECK(g_sealedSeen.size() >= 10);
    const MpAddr haddr = all[0]->debugLocalAddr();
    // tamper: every byte of a few captured datagrams flipped in turn
    const uint32_t bad0 = all[0]->debugBadCount(), stray0 = all[0]->debugStray();
    int sent = 0;
    for (size_t k = 0; k < g_sealedSeen.size() && k < 4; k++)
        for (size_t i = 0; i < g_sealedSeen[k].size(); i++) {
            std::vector<uint8_t> d = g_sealedSeen[k];
            d[i] ^= (uint8_t)(1u << (i % 8));
            hub.inject(all[1]->debugLocalAddr(), haddr, d.data(), d.size());
            sent++;
        }
    Step(all, 200);
    printf("core2: %d tampered datagrams: stray +%u, bad +%u\n", sent, (unsigned)(all[0]->debugStray() - stray0),
           (unsigned)(all[0]->debugBadCount() - bad0));
    CHECK(all[0]->debugBadCount() == bad0);
    CHECK(all[0]->debugStray() - stray0 >= (uint32_t)sent);
    // replays of what the guest really sent: dropped (the window), never counted against it
    for (auto& d : g_sealedSeen) hub.inject(all[1]->debugLocalAddr(), haddr, d.data(), d.size());
    Step(all, 200);
    CHECK(all[0]->debugBadCount() == bad0);
    // a datagram sealed by guest 1 from another address: the host moves slot 1 there (the key proves it)
    std::vector<MpCore*> hostOnly = {all[0]};
    uint8_t hb[8] = {0};
    std::vector<uint8_t> pk;
    WritePacket(pk, MP_MSG_HEARTBEAT, 1, 0, 0, hb, 8);
    uint8_t out[256];
    const size_t n = all[1]->debugSealAs(0, pk.data(), pk.size(), out, sizeof out);
    CHECK(n > 0);
    hub.inject(MpAddr{55, MP_SESSION_PORT}, haddr, out, n);
    Step(hostOnly, 1);
    CHECK(all[0]->debugSlotOf(MpAddr{55, MP_SESSION_PORT}) == 1);
    // the same heartbeat in plain from another address (a stranger who saw the move): dropped, the slot does not move
    hub.inject(MpAddr{56, MP_SESSION_PORT}, haddr, pk.data(), pk.size());
    Step(hostOnly, 1);
    CHECK(all[0]->debugSlotOf(MpAddr{56, MP_SESSION_PORT}) == -1 && all[0]->debugSlotOf(MpAddr{55, MP_SESSION_PORT}) == 1);
    // (the real guest still sends from its own address with its key: the host follows it back)
    Step(all, 1000);
    CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2) && all[1]->active() && all[2]->active());
    Free(all);
}

// the host removes a guest it took for gone while the guest is in fact alive (a long freeze): the guest is told so (sealed, its old key)
// and rejoins at once (it used to learn it from 5 s of silence)
static void TestCore2SecDeparted() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 2231);
    all[0]->uiStart();
    Step(all, 500);
    std::vector<MpCore*> others = {all[0], all[2]};
    Step(others, MP_PEER_LOST_MS + 1000);   // guest 1 is frozen: the host removes it
    CHECK(!all[0]->slotPresent(1));
    const int told = LogCount("host says this console was removed");
    Step(all, 3000);
    CHECK(LogCount("host says this console was removed") == told + 1);
    CHECK(WaitPhase(all, all[1], MP_UI_PLAYING, 5000));
    CHECK(all[0]->slotPresent(all[1]->localSlot()));
    Free(all);
}

// owner's addition: in rooms without the service a console with an account announces its own code (sealed, after the keys); the
// others see it as not verified; a late joiner learns the room's codes; the service's own view wins (verified); hostile announcements
// (another player's slot, wrong length, other alphabets, 1000 a second) are refused or cost nothing; the code leaves with the player
// and never appears in the log.
static void TestCore2SecFriendCodes() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 2241);
    all[0]->setOwnFriendCode("HHHH-2222");
    all[1]->setOwnFriendCode("K7QM-4DXT");
    Step(all, 300);
    MpPlayerInfo pi;
    CHECK(all[2]->slotPlayer(1, &pi) && !strcmp(pi.code, "K7QM-4DXT") && !pi.codeVerified && pi.name[0]);
    CHECK(all[0]->slotPlayer(1, &pi) && !strcmp(pi.code, "K7QM-4DXT") && !pi.codeVerified);
    CHECK(all[1]->slotPlayer(0, &pi) && !strcmp(pi.code, "HHHH-2222") && !pi.codeVerified);
    CHECK(all[2]->slotPlayer(2 == all[2]->localSlot() ? 0 : 2, &pi));
    CHECK(!all[1]->slotPlayer(all[1]->localSlot(), &pi));   // (own slot)
    char c[16];
    CHECK(!all[2]->slotFriendCode(1, c, sizeof c));   // the verified-only accessor does not show announced codes
    // a late joiner learns the codes already announced
    MpCore* late = MakeCore(hub, 4, "Late", 2244);
    all.push_back(late);
    CHECK(ScanJoin(all, late));
    CHECK(WaitPhase(all, late, MP_UI_LOBBY, 3000) || WaitPhase(all, late, MP_UI_PLAYING, 10));
    Step(all, 300);
    CHECK(late->slotPlayer(1, &pi) && !strcmp(pi.code, "K7QM-4DXT"));
    CHECK(late->slotPlayer(0, &pi) && !strcmp(pi.code, "HHHH-2222"));
    // the account appears later on another console: announced then
    late->setOwnFriendCode("M4TT-ZE7B");
    Step(all, 300);
    CHECK(all[1]->slotPlayer(late->localSlot(), &pi) && !strcmp(pi.code, "M4TT-ZE7B"));
    // the service's view wins: verified
    hub.setMemberCode(1, 2, "PQ2R-9HXA");
    Step(all, 50);
    CHECK(all[2]->slotPlayer(1, &pi) && !strcmp(pi.code, "PQ2R-9HXA") && pi.codeVerified);
    hub.setMemberCode(1, 0, "");
    // hostile announcements at the host from slot 2
    const uint32_t bad0 = all[0]->debugBadCount();
    all[0]->debugDeliverCore(2, MP_MSG_FRIEND, 1, "ZZZZ-2222");   // about another player
    const char* wrong[] = {"ZZZZ-222", "ZZZZ-22222", "zzzz-2222", "ZZZZ-2221", "ZZZZ_2222", "\xc3\xa9ZZZ-222"};
    for (const char* w : wrong) all[0]->debugDeliverCore(2, MP_MSG_FRIEND, 2, w, strlen(w));
    printf("core2 friend codes: hostile announcements counted bad: %u of 7\n", (unsigned)(all[0]->debugBadCount() - bad0));
    CHECK(all[0]->debugBadCount() - bad0 == 7);
    CHECK(!all[1]->slotPlayer(2, &pi) || strcmp(pi.code, "ZZZZ-2222") != 0);
    // 1000 a second with good codes from slot 2: at most about one change a second sticks, the rest cost a stray count
    const uint32_t stray0 = all[0]->debugStray();
    for (int i = 0; i < 1000; i++) {
        char code[10];
        snprintf(code, sizeof code, "AB%c%c-2345", "ABCDEFGHJKMNPQRSTUVWXYZ23456789"[i % 31], "ABCDEFGHJKMNPQRSTUVWXYZ23456789"[(i / 31) % 31]);
        all[0]->debugDeliverCore(2, MP_MSG_FRIEND, 2, code);
        if (i % 10 == 0) Step(all, 1);
    }
    printf("core2 friend codes: 1000 announcements in ~100 ms: stray +%u\n", (unsigned)(all[0]->debugStray() - stray0));
    CHECK(all[0]->debugStray() - stray0 >= 990);
    CHECK(all[1]->active() && all[2]->active() && all[0]->slotPresent(2));
    // the code leaves with the player
    all[1]->uiLeave();
    Step(all, 1500);
    CHECK(!all[2]->slotPlayer(1, &pi) && !late->slotPlayer(1, &pi));
    CHECK(LogCount("K7QM-4DXT") == 0 && LogCount("HHHH-2222") == 0 && LogCount("M4TT-ZE7B") == 0);
    Free(all);
}

// the game side's verdict (mp_core_report_bad): counts like a malformed message on the host only; MP_BAD_KICK of them in the window
// remove the player; a guest's report, the host's own slot and an empty slot are ignored
static void TestCore2SecReportBad() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 2251);
    all[0]->uiStart();
    Step(all, 300);
    const uint32_t bad0 = all[0]->debugBadCount();
    for (int i = 0; i < 100; i++) all[1]->reportBad(2, "guest says");   // (a guest: nothing)
    all[0]->reportBad(0, "own");
    all[0]->reportBad(3, "empty");
    CHECK(all[0]->debugBadCount() == bad0 && all[0]->slotPresent(2));
    for (int i = 0; i < MP_BAD_KICK; i++) all[0]->reportBad(2, "world guard");
    Step(all, 50);
    CHECK(all[0]->slotPresent(2));   // (MP_BAD_KICK are tolerated)
    all[0]->reportBad(2, "world guard");
    Step(all, 2000);
    CHECK(!all[0]->slotPresent(2) && LogCount("kicked slot=2 reason=bad-messages") >= 1);
    CHECK(Phase(all[2]) == MP_UI_IDLE && Error(all[2]) == MP_ERR_KICKED);
    CHECK(all[1]->active() && all[1]->slotPresent(1));
    Free(all);
}

static void TestCore2Sec() {
    TestCore2SecGhostRestart();
    TestCore2SecCodeSecret();
    TestCore2SecSealed();
    TestCore2SecDeparted();
    TestCore2SecFriendCodes();
    TestCore2SecReportBad();
    printf("core2 stage 3: cookie join, code secret, proof ceiling, ghost restarts, sealed rebinding, replay, tamper, departed: ok\n");
}
