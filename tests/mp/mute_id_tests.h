// smallfix2 A: MpPlayerInfo::sid, the stable per-session player id ("Hide messages" keys on it, not on the name).
// Included by core_tests.cpp after core2_sec_tests.h (uses its helpers: StartSession, SameConsole, ScanJoin, WaitPhase, Step, Free).
#include "../../platform/src/mp/mp_seal.h"

static void TestMuteIdStableAcrossRejoin() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 901);
    MpCore* a = MakeCore(hub, 2, "Player", 902);
    MpCore* b = MakeCore(hub, 3, "Player", 903);   // (the same name as a)
    std::vector<MpCore*> all = {h, a, b};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    CHECK(ScanJoin(all, a));
    CHECK(WaitPhase(all, a, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, b));
    CHECK(WaitPhase(all, b, MP_UI_LOBBY, 3000));
    Step(all, 300);
    const int sa = a->localSlot(), sb = b->localSlot();
    MpPlayerInfo ia, ib, ia_seen_by_b;
    CHECK(h->slotPlayer(sa, &ia) && h->slotPlayer(sb, &ib));
    CHECK(ia.sid != 0 && ib.sid != 0);
    CHECK(!strcmp(ia.name, ib.name) || !strcmp(ia.name, "Player"));
    CHECK(ia.sid != ib.sid);   // two players with one name
    CHECK(b->slotPlayer(sa, &ia_seen_by_b) && ia_seen_by_b.sid == ia.sid);   // every console computes the same id for a player

    // not derivable back to the token: neither the token, nor its commitment (which is public in the roster), nor a plain hash of either
    uint8_t key[16], token[16], commit[16], m[15];
    a->debugConsoleKey(key);
    const uint32_t sid = h->sessionIdNow();
    memcpy(m, "SMG3 rejoin", 11);
    memcpy(m + 11, &sid, 4);
    mpseal::Mac(token, 16, key, 16, m, sizeof m);
    mpseal::Mac(commit, 16, 0, 0, token, 16);
    uint32_t w[4];
    memcpy(w, token, 16);
    for (int i = 0; i < 4; i++) CHECK(ia.sid != w[i]);
    memcpy(w, commit, 16);
    for (int i = 0; i < 4; i++) CHECK(ia.sid != w[i]);
    uint8_t plain[4];
    mpseal::Mac(plain, 4, 0, 0, commit, 16);
    uint32_t pv;
    memcpy(&pv, plain, 4);
    CHECK(ia.sid != pv);   // (the id is keyed: an unkeyed truncated hash of the commitment does not match)
    CHECK(ia.sid != 0 && sizeof ia.sid == 4);   // 32 bits only

    // the console drops and comes back as the same console, under another name: same id, whatever the slot
    MpCore* a2 = MakeCore(hub, 4, "Renamed", 904);
    SameConsole(a2, a);
    all.push_back(a2);
    delete a;
    all[1] = 0;
    std::vector<MpCore*> live = {h, b, a2};
    CHECK(ScanJoin(live, a2));
    CHECK(WaitPhase(live, a2, MP_UI_LOBBY, 3000));
    Step(live, 500);
    MpPlayerInfo ia2, ib2;
    CHECK(h->slotPlayer(a2->localSlot(), &ia2));
    CHECK(ia2.sid == ia.sid);
    CHECK(h->slotPlayer(sb, &ib2) && ib2.sid == ib.sid);

    // a console that is not a member of this slot's player has another id (a different console key)
    MpPlayerInfo none;
    CHECK(!h->slotPlayer(h->localSlot(), &none) && none.sid == 0);   // (own slot: nothing)
    all[1] = a2;
    all.erase(all.begin() + 1);
    Free(all);
    printf("mute id: stable over rejoin and rename, distinct for equal names, not derivable from the token: ok\n");
}
