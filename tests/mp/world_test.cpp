// Galaxy 3.0 multiplayer: unit tests for the world module's pure logic (decomp/src/Game/MP/MpWorldLogic.cpp).
// Build (from the worktree root). The container's clang has no sanitizer runtimes, so ASan+UBSan run with g++; clang runs
// UBSan in trap mode (no runtime needed):
//   g++ -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra -Iplatform/include -Idecomp/include
//       tests/mp/world_test.cpp decomp/src/Game/MP/MpWorldLogic.cpp -o /tmp/world_test && /tmp/world_test
//   clang++ -std=c++20 -fsanitize=undefined -fsanitize-trap=all -g -Wall -Wextra -Iplatform/include -Idecomp/include
//       tests/mp/world_test.cpp decomp/src/Game/MP/MpWorldLogic.cpp -o /tmp/world_test_clang && /tmp/world_test_clang
#include "Game/MP/MpWorldLogic.hpp"
#include "Game/MP/MpHeldPool.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

using namespace MpWorldLogic;

static int g_fail, g_checks;
#define CHECK(c)                                                                 \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(c)) {                                                              \
            g_fail++;                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                        \
    } while (0)

static MpObjKey K(int zone, int hash, int lid, int sub) {
    MpObjKey k;
    k.zone = (uint8_t)zone; k.nameHash = (uint16_t)hash; k.lid = (uint16_t)lid; k.sub = (uint8_t)sub;
    return k;
}

static void testHashes() {
    CHECK(crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(crc32(crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);   // incremental
    CHECK(mp_name_hash15("Kuribo") < 0x8000);
    // Bytes as uint8_t: a Shift-JIS name hashes the same whatever the char signedness.
    const char sj[] = "\x83\x4e\x83\x8a\x83\x7b";
    uint32_t h = 2166136261u;
    for (int i = 0; sj[i]; i++) { h ^= (uint8_t)sj[i]; h *= 16777619u; }
    CHECK(nameHash32(sj) == h);
    CHECK(nameHash32("") == 2166136261u && nameHash32(nullptr) == 2166136261u);
}

static Registry g_reg, g_reg2;

static void testRegistry() {
    int actors[16];
    g_reg.clear();
    CHECK(!g_reg.ready() && g_reg.findKey(K(0, 1, 1, 0)) == -1);
    CHECK(g_reg.add(K(1, 0x123, 7, 0), 11, &actors[0], ENTRY_ENEMY));
    CHECK(g_reg.add(K(0, 0x123, 7, 0), 12, &actors[1], 0));
    CHECK(g_reg.add(K(0, 0x123, 7, 1), 13, &actors[2], 0));
    CHECK(g_reg.add(K(0, 0x7FFF, 3, 0), 14, &actors[3], 0));
    CHECK(g_reg.add(K(2, 0x10, 5, 0), 15, &actors[4], 0));   // duplicate pair
    CHECK(g_reg.add(K(2, 0x10, 5, 0), 15, &actors[5], 0));
    g_reg.finalize();
    CHECK(g_reg.ready() && g_reg.count() == 6 && g_reg.dupCount() == 2);
    CHECK(!g_reg.add(K(3, 1, 1, 0), 1, &actors[6], 0));      // closed after finalize
    int i = g_reg.findKey(K(1, 0x123, 7, 0));
    CHECK(i >= 0 && g_reg.at(i).actor == &actors[0] && (g_reg.at(i).flags & ENTRY_ENEMY));
    CHECK(g_reg.findKey(K(0, 0x123, 7, 1)) >= 0 && g_reg.at(g_reg.findKey(K(0, 0x123, 7, 1))).actor == &actors[2]);
    CHECK(g_reg.findKey(K(0, 0x123, 7, 2)) == -1);           // sub not registered
    CHECK(g_reg.findKey(K(2, 0x10, 5, 0)) == -1);            // duplicate keys are never synced
    CHECK(g_reg.findActor(&actors[5]) >= 0 && (g_reg.at(g_reg.findActor(&actors[5])).flags & ENTRY_DUP));
    CHECK(g_reg.findActor(&actors[3]) >= 0 && g_reg.at(g_reg.findActor(&actors[3])).key.nameHash == 0x7FFF);
    CHECK(g_reg.findActor(&actors[9]) == -1 && g_reg.findActor(nullptr) == -1);
    CHECK(g_reg.setFlagsForActor(&actors[1], ENTRY_PURPLE) && (g_reg.at(g_reg.findActor(&actors[1])).flags & ENTRY_PURPLE));
    CHECK(!g_reg.setFlagsForActor(&actors[10], ENTRY_PURPLE));

    // CRC: independent of insertion order and actor addresses, sensitive to keys and class names.
    g_reg2.clear();
    g_reg2.add(K(2, 0x10, 5, 0), 15, &actors[11], 0);
    g_reg2.add(K(0, 0x7FFF, 3, 0), 14, &actors[12], 0);
    g_reg2.add(K(2, 0x10, 5, 0), 15, &actors[13], 0);
    g_reg2.add(K(0, 0x123, 7, 1), 13, &actors[14], 0);
    g_reg2.add(K(0, 0x123, 7, 0), 12, &actors[15], 0);
    g_reg2.add(K(1, 0x123, 7, 0), 11, &actors[0], 0);
    g_reg2.finalize();
    CHECK(g_reg2.crc() == g_reg.crc());
    g_reg2.clear();
    g_reg2.add(K(1, 0x123, 7, 0), 99, &actors[0], 0);         // other class name
    g_reg2.finalize();
    g_reg.clear();
    g_reg.add(K(1, 0x123, 7, 0), 11, &actors[0], 0);
    g_reg.finalize();
    CHECK(g_reg2.crc() != g_reg.crc());

    // Capacity: 2560 entries fill it, the next one overflows (object sync is then off: CRC differs).
    g_reg.clear();
    static char many[Registry::kMax + 1];
    bool ok = true;
    for (int n = 0; n < Registry::kMax; n++) ok &= g_reg.add(K(n % 20, n / 20, n, 0), (uint32_t)n, &many[n], 0);
    CHECK(ok && !g_reg.overflowed());
    CHECK(!g_reg.add(K(31, 1, 1, 1), 1, &many[Registry::kMax], 0) && g_reg.overflowed());
    g_reg.finalize();
    CHECK(g_reg.count() == Registry::kMax && g_reg.dupCount() == 0);
    int found = 0;
    for (int n = 0; n < Registry::kMax; n++) found += g_reg.findActor(&many[n]) >= 0 && g_reg.findKey(K(n % 20, n / 20, n, 0)) >= 0;
    CHECK(found == Registry::kMax);
}

static void testPolicy() {
    CHECK(switchPolicy("SwitchCube", "SW_A", true, 0) == SW_LOCAL);       // level mode
    CHECK(switchPolicy("SwitchCube", "SW_A", true, -1) == SW_SYNC);       // one-shot area: synced
    CHECK(switchPolicy("SwitchCube", "SW_A", false, 0) == SW_SYNC);
    CHECK(switchPolicy("SwitchSphere", "SW_A", true, 3) == SW_LOCAL);
    CHECK(switchPolicy("SwitchCylinder", "SW_B", true, 3) == SW_SYNC);    // SW_B is only read
    CHECK(switchPolicy("Tamakoro", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("Tamakoro", "SW_DEAD", false, 0) == SW_SYNC);
    CHECK(switchPolicy("TamakoroWithTutorial", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("Fluff", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("MarioLauncher", "SW_B", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("MarioLauncher", "SW_A", false, 0) == SW_SYNC);
    CHECK(switchPolicy("SwitchSynchronizerReverse", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("SwitchSynchronizer", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("GroupSwitchWatcher", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("TimerSwitch", "SW_A", false, 0) == SW_LOCAL);
    CHECK(switchPolicy("HipDropTimerSwitch", "SW_A", false, 0) == SW_SYNC);   // exact names only
    CHECK(switchPolicy("Kuribo", "SW_DEAD", false, 0) == SW_SYNC);
    CHECK(switchPolicy(nullptr, "SW_A", false, 0) == SW_SYNC);
    CHECK(switchPolicy("Tamakoro", nullptr, false, 0) == SW_SYNC);

    CHECK(isBossName("BossBegoman") && isBossName("BossKameck") && isBossName("Koopa") && isBossName("KoopaVs1"));
    CHECK(isBossName("TripodBossCore") && isBossName("TriPodBossLeg1A") && isBossName("DinoPackunVs1"));
    CHECK(isBossName("SkeletalFishBoss") && isBossName("TombSpider") && isBossName("OtaKing") && isBossName("Polta"));
    CHECK(!isBossName("Kuribo") && !isBossName("Kameck") && !isBossName("Bos") && !isBossName(nullptr) && !isBossName(""));
}

static SwitchMirror g_m, g_m2;

static void testOscGuard() {
    g_m.reset(5);
    bool tripped = false;
    uint32_t t = 1000;
    for (int i = 0; i < SwitchMirror::kOscEdges; i++) CHECK(!g_m.oscEdge(3, 9, t + (uint32_t)i * 5, &tripped) && !tripped);
    CHECK(g_m.oscEdge(3, 9, t + 40, &tripped) && tripped);      // 7th edge inside 1 s
    CHECK(g_m.isGuarded(3, 9));
    CHECK(g_m.oscEdge(3, 9, t + 5000, &tripped) && !tripped);   // stays local for the rest of the scene
    // Slow edges never trip: 6 per window, windows apart.
    for (int w = 0; w < 5; w++)
        for (int i = 0; i < SwitchMirror::kOscEdges; i++) CHECK(!g_m.oscEdge(4, 1, 2000 + (uint32_t)w * 61 + (uint32_t)i, &tripped));
    CHECK(!g_m.isGuarded(4, 1) && !g_m.isGuarded(3, 8));
    // Turn counter wrap (16-bit window start).
    for (int i = 0; i < SwitchMirror::kOscEdges; i++) CHECK(!g_m.oscEdge(kGlobalSlot, 0, 0xFFFFFFF0u + (uint32_t)i * 2, &tripped));
    CHECK(g_m.oscEdge(kGlobalSlot, 0, 0x00000004u, &tripped) && tripped);
    g_m.resetScene();
    CHECK(!g_m.isGuarded(3, 9));
}

struct ApplyLog { int n; int slot[512]; int bit[512]; bool on[512]; };
static void logApply(void* ctx, int slot, int bit, bool on) {
    ApplyLog* l = (ApplyLog*)ctx;
    if (l->n < 512) { l->slot[l->n] = slot; l->bit[l->n] = bit; l->on[l->n] = on; l->n++; }
}

static void testMapCodec() {
    uint8_t buf[MP_MAX_PAYLOAD];
    static MapMsg mm;
    g_m.reset(7);
    CHECK(g_m.crc() == 0 && g_m.zonesWithBits() == 0);
    size_t n = writeMap(buf, sizeof buf, 7, g_m, true);
    CHECK(n == kMapHeader && readMap(buf, n, 4, &mm) && mm.count == 0 && mapIsFull(mm));   // empty full map
    g_m.setOn(0, 0, true);
    g_m.setOn(0, 127, true);
    g_m.setOn(3, 64, true);
    g_m.setOn(kGlobalSlot, 5, true);
    g_m.markLocal(1, 2);
    g_m.setOn(1, 2, true);                                   // local bits never enter the mirror
    CHECK(!g_m.getOn(1, 2) && g_m.zonesWithBits() == 3);
    n = writeMap(buf, sizeof buf, 7, g_m, true);
    CHECK(n == (size_t)kMapHeader + 3 * kMapEntry);
    CHECK(readMap(buf, n, 4, &mm) && mm.count == 3 && mm.serial == 7 && mm.crc == g_m.crc() && mapIsFull(mm));
    CHECK(mm.zone[0] == 0 && mm.zone[1] == 3 && mm.zone[2] == 0xFF && mm.bits[2].get(5));
    CHECK(!readMap(buf, n, 3, &mm));                         // zone 3 >= zone count 3
    // Worst case fits the payload limit: every zone and the global set.
    g_m2.reset(1);
    for (int s = 0; s < kZoneSlots; s++) g_m2.setOn(s, s % 128, true);
    n = writeMap(buf, sizeof buf, 1, g_m2, true);
    CHECK(n == (size_t)kMapHeader + (size_t)kZoneSlots * kMapEntry && n <= MP_MAX_PAYLOAD && readMap(buf, n, 32, &mm) && mm.count == kZoneSlots);
    // CRC only
    n = writeMap(buf, sizeof buf, 7, g_m, false);
    CHECK(n == kMapHeader && readMap(buf, n, 4, &mm) && mm.count == 0 && mm.crc == g_m.crc() && !mapIsFull(mm));

    // Malformed maps are dropped whole.
    n = writeMap(buf, sizeof buf, 7, g_m, true);
    CHECK(!readMap(buf, n - 1, 4, &mm));                     // truncated
    buf[n] = 0;
    CHECK(!readMap(buf, n + 1, 4, &mm));                     // trailing byte
    uint8_t bad[MP_MAX_PAYLOAD];
    memcpy(bad, buf, n);
    bad[6] = 4;                                              // count says 4, only 3 present
    CHECK(!readMap(bad, n, 4, &mm));
    memcpy(bad, buf, n);
    bad[6] = 200;                                            // count above the limit
    CHECK(!readMap(bad, n, 4, &mm));
    memcpy(bad, buf, n);
    bad[kMapHeader + kMapEntry] = 0;                         // second zone = 0 again: duplicate / not ascending
    CHECK(!readMap(bad, n, 4, &mm));
    memcpy(bad, buf, n);
    bad[kMapHeader + 1] ^= 0x01;                             // bit flip: crc no longer matches
    CHECK(!readMap(bad, n, 4, &mm));
    memcpy(bad, buf, n);
    memset(bad + kMapHeader + 1, 0, 16);                     // empty zone entry
    CHECK(!readMap(bad, n, 4, &mm));
    memcpy(bad, buf, n);
    bad[kMapHeader] = 40;                                    // zone id 40 not representable
    CHECK(!readMap(bad, n, 64, &mm));
}

static void testApplyMap() {
    uint8_t buf[MP_MAX_PAYLOAD];
    static MapMsg mm;
    static ApplyLog log;
    g_m.reset(9);        // the host
    g_m.setOn(0, 1, true);
    g_m.setOn(2, 100, true);
    size_t n = writeMap(buf, sizeof buf, 9, g_m, true);
    CHECK(readMap(buf, n, 4, &mm));

    g_m2.reset(9);       // a guest: has bit 0/7 on (missed the off), local bit 2/100, guarded 0/1
    g_m2.setOn(0, 7, true);
    g_m2.markLocal(2, 100);
    bool tr;
    for (int i = 0; i < 7; i++) g_m2.oscEdge(0, 1, 50, &tr);
    CHECK(g_m2.isGuarded(0, 1));
    log.n = 0;
    int changed = applyMap(g_m2, mm, false, logApply, &log);
    CHECK(changed == 1 && g_m2.getOn(0, 1) && log.n == 0);          // mirror follows; guarded bit not written
    CHECK(g_m2.getOn(0, 7) && !g_m2.getOn(2, 100));                // on-only: 0/7 kept; local bit never set
    changed = applyMap(g_m2, mm, true, logApply, &log);             // authoritative (repair answer)
    CHECK(changed == 1 && !g_m2.getOn(0, 7) && log.n == 1 && log.slot[0] == 0 && log.bit[0] == 7 && !log.on[0]);
    g_m.markLocal(2, 100);   // both sides agree on local bits -> converged
    g_m.onBitsMutable(2).set(100, false);
    CHECK(g_m2.crc() == g_m.crc());
    CHECK(applyMap(g_m2, mm, true, logApply, &log) == 0);           // idempotent

    g_m2.reset(9);
    log.n = 0;
    CHECK(applyMap(g_m2, mm, false, logApply, &log) == 2 && log.n == 2 && log.on[0] && log.on[1]);
}

static void testMessages() {
    uint8_t buf[64];
    SwitchMsg s = {0x1234, 0xFF, 127, 1}, so;
    size_t n = writeSwitch(buf, sizeof buf, s);
    CHECK(n == kSwitchLen && readSwitch(buf, n, 1, &so) && so.serial == 0x1234 && so.zone == 0xFF && so.bit == 127 && so.on == 1);
    s.zone = 2;
    n = writeSwitch(buf, sizeof buf, s);
    CHECK(readSwitch(buf, n, 3, &so) && !readSwitch(buf, n, 2, &so));
    s.bit = 128;
    n = writeSwitch(buf, sizeof buf, s);
    CHECK(!readSwitch(buf, n, 3, &so));
    s.bit = 1; s.on = 2;
    n = writeSwitch(buf, sizeof buf, s);
    CHECK(!readSwitch(buf, n, 3, &so));
    s.on = 0;
    n = writeSwitch(buf, sizeof buf, s);
    CHECK(!readSwitch(buf, n - 1, 3, &so) && !readSwitch(buf, n + 1, 3, &so));
    s.zone = 32;
    n = writeSwitch(buf, sizeof buf, s);
    CHECK(!readSwitch(buf, n, 255, &so));                          // only 0..31 and 0xFF

    ObjMsg o = {7, K(1, 0x7FFF, 65535, 255)}, oo;
    n = writeObj(buf, sizeof buf, o);
    CHECK(n == kObjLen && readObj(buf, n, 2, &oo) && oo.key.zone == 1 && oo.key.nameHash == 0x7FFF && oo.key.lid == 65535 &&
          oo.key.sub == 255 && oo.serial == 7);
    CHECK(!readObj(buf, n, 1, &oo));                               // zone >= zone count
    CHECK(!readObj(buf, n - 1, 2, &oo) && !readObj(buf, n + 1, 2, &oo));
    o.key.nameHash = 0x8000;                                       // not a 15-bit hash
    n = writeObj(buf, sizeof buf, o);
    CHECK(!readObj(buf, n, 2, &oo));

    RegCrcMsg r = {3, 0xDEADBEEF}, ro;
    n = writeRegCrc(buf, sizeof buf, r);
    CHECK(n == kRegCrcLen && readRegCrc(buf, n, &ro) && ro.crc == 0xDEADBEEF && ro.serial == 3);
    CHECK(!readRegCrc(buf, n - 1, &ro) && !readRegCrc(buf, n + 1, &ro));
    CHECK(writeSwitch(buf, 3, s) == 0 && writeObj(buf, 4, o) == 0);   // writers refuse short buffers
}

static void testFuzz() {
    static uint8_t buf[MP_MAX_PAYLOAD + 8];
    static MapMsg mm;
    SwitchMsg s; ObjMsg o; RegCrcMsg r;
    srand(12345);
    int accepted = 0;
    for (int it = 0; it < 200000; it++) {
        size_t len = (size_t)(rand() % 40);
        if (it % 50 == 0) len = (size_t)(rand() % (MP_MAX_PAYLOAD + 8));
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rand();
        if (it % 3 == 0 && len > 6) buf[6] = (uint8_t)(rand() % 4);   // plausible map counts
        int zc = rand() % 34;
        accepted += readSwitch(buf, len, zc, &s);
        accepted += readObj(buf, len, zc, &o);
        accepted += readRegCrc(buf, len, &r);
        if (readMap(buf, len, zc, &mm)) {
            accepted++;
            CHECK(mm.count <= kMapMaxZones);
            CHECK(mm.count == 0 || mapCrc(mm) == mm.crc);
            g_m.reset(1);
            applyMap(g_m, mm, (it & 1) != 0, nullptr, nullptr);
        }
    }
    CHECK(accepted > 0);
}

static void testPending() {
    PendingQueue q;
    q.clear();
    q.push(PEND_KILL, 5, 100);
    q.push(PEND_KILL, 5, 120);   // dedup: refreshes the deadline
    q.push(PEND_PURPLE, 5, 0);
    CHECK(q.count() == 2);
    int calls = 0;
    q.run(110, [&](const PendingQueue::Item&) { calls++; return false; });
    CHECK(calls == 2 && q.count() == 2);
    calls = 0;
    q.run(121, [&](const PendingQueue::Item& it) { calls++; CHECK(it.kind == PEND_PURPLE); return false; });   // kill expired
    CHECK(calls == 1 && q.count() == 1);
    q.run(5000, [&](const PendingQueue::Item&) { return true; });
    CHECK(q.count() == 0);
    for (int i = 0; i < PendingQueue::kMax + 10; i++) q.push(PEND_CHIP, (uint16_t)i, 0);
    CHECK(q.count() == PendingQueue::kMax);
    int first = -1;
    q.run(0, [&](const PendingQueue::Item& it) { if (first < 0) first = it.index; return false; });
    CHECK(first == 10);          // the oldest were dropped
}

// worldsync: drop counters, the rate-limited log decision, the eviction / expiry reports of the pending queue
static void testDropStats() {
    DropStats d;
    CHECK(d.total() == 0);
    char line[200];
    d.format(line, sizeof line, "EggStarGalaxy:1");
    CHECK(strcmp(line, "world drops scene=EggStarGalaxy:1 switch=0 kill=0 purple=0 chip=0 other=0 reason=none") == 0);
    CHECK(d.note(DT_KILL, DR_SERIAL, 0));                 // the first of a (type, reason) is always logged
    int logged = 0;
    for (uint32_t i = 0; i < 100; i++) if (d.note(DT_KILL, DR_SERIAL, 1 + i / 10)) logged++;   // 100 drops inside 10 turns
    CHECK(logged == DropStats::kLogBurst);                // then only the bucket (4)
    CHECK(d.count(DT_KILL, DR_SERIAL) == 101);
    CHECK(d.note(DT_KILL, DR_EXPIRED, 11));               // another reason: its own first line
    uint32_t t = 2000;                                    // long later: the bucket has refilled
    CHECK(d.note(DT_KILL, DR_SERIAL, t));
    d.note(DT_PURPLE, DR_NOT_PLACED, t);
    d.note(DT_SWITCH, DR_SERIAL, t);
    d.noteMany(DT_CHIP, DR_HELD_FULL, 3);
    CHECK(d.typeTotal(DT_CHIP) == 3 && d.reasonTotal(DR_HELD_FULL) == 3);
    CHECK(!d.note(DT_COUNT, DR_SERIAL, 0) && !d.note(DT_KILL, DR_COUNT, 0));   // out of range: ignored
    d.format(line, sizeof line, "S");
    CHECK(strstr(line, "switch=1 kill=") != nullptr && strstr(line, "purple=1 chip=3") != nullptr);
    CHECK(strstr(line, "serial:") != nullptr && strstr(line, "held-full:3") != nullptr && strstr(line, "not-placed:1") != nullptr);
    char tiny[16];
    const size_t n = d.format(tiny, sizeof tiny, "S");   // truncation is safe
    CHECK(n < sizeof tiny && strlen(tiny) == n);
    CHECK(d.format(nullptr, 0, "S") == 0);
    d.reset();
    CHECK(d.total() == 0);
    CHECK(dropTypeOfMsg(MP_MSG_KILL) == DT_KILL && dropTypeOfMsg(MP_MSG_SWITCHMAP) == DT_OTHER && dropTypeOfMsg(MP_MSG_SWITCH) == DT_SWITCH);
    for (int r = 0; r < DR_COUNT; r++) CHECK(strcmp(dropReasonName(r), "?") != 0);
}
static void testPendingReports() {
    PendingQueue q;
    q.clear();
    PendingQueue::Item ev = {0, 0, 0};
    for (int i = 0; i < PendingQueue::kMax; i++) CHECK(!q.push(PEND_KILL, (uint16_t)i, 0, &ev));
    CHECK(q.push(PEND_KILL, 500, 0, &ev) && ev.index == 0 && q.count() == PendingQueue::kMax);   // full: the oldest is reported
    CHECK(!q.push(PEND_KILL, 500, 0, &ev));                                                        // a dedup is not a drop
    q.clear();
    q.push(PEND_KILL, 1, 50);
    q.push(PEND_CHIP, 2, 0);
    int exp = 0, calls = 0;
    q.run(100, [&](const PendingQueue::Item&) { calls++; return false; }, [&](const PendingQueue::Item& it) { exp++; CHECK(it.kind == PEND_KILL && it.index == 1); });
    CHECK(exp == 1 && calls == 1 && q.count() == 1);
}

// worldsync: the early queue (events that cannot be judged yet wait; bounded; oldest dropped and reported; expiry reported)
static void testEarlyQueue() {
    EarlyQueue q;
    q.clear();
    EarlyQueue::Item ev;
    memset(&ev, 0, sizeof ev);
    CHECK(!q.push(MP_MSG_KILL, 1, 7, K(0, 0x123, 5, 0), 0, &ev));
    CHECK(!q.push(MP_MSG_KILL, 1, 7, K(0, 0x123, 5, 0), 100, &ev));   // same (type, serial, key): refreshed, not doubled
    CHECK(!q.push(MP_MSG_PURPLE, 1, 7, K(0, 0x123, 5, 0), 0, &ev));   // another type: its own entry
    CHECK(!q.push(MP_MSG_KILL, 1, 8, K(0, 0x123, 5, 0), 0, &ev));     // another serial: its own entry
    CHECK(q.count() == 3);
    int exp = 0, kept = 0;
    q.run(200, [&](const EarlyQueue::Item& it) -> int { kept++; return it.serial == 8 ? EarlyQueue::EQ_DONE : EarlyQueue::EQ_KEEP; },
          [&](const EarlyQueue::Item& it) { exp++; CHECK(it.type == MP_MSG_KILL && it.serial == 7); });
    CHECK(exp == 1 && kept == 2 && q.count() == 1);   // the refreshed kill expired (deadline 100), the serial-8 one was done, the purple stays
    q.clear();
    for (int i = 0; i < EarlyQueue::kMax; i++) CHECK(!q.push(MP_MSG_KILL, (uint8_t)(i / EarlyQueue::kPerSender), 1, K(0, 1, (uint16_t)i, 0), 0, &ev));   // 4 senders x 24
    CHECK(q.count() == EarlyQueue::kMax);
    CHECK(q.push(MP_MSG_KILL, 0, 1, K(0, 1, 5000, 0), 0, &ev));      // sender 0 is at its cap: ITS oldest (lid 0) is dropped and reported
    CHECK(ev.key.lid == 0 && ev.fromSlot == 0 && q.count() == EarlyQueue::kMax && q.countFrom(0) == EarlyQueue::kPerSender);
    CHECK(q.countFrom(1) == EarlyQueue::kPerSender && q.countFrom(3) == EarlyQueue::kPerSender);   // the others are untouched
    int first = -1;
    q.run(0, [&](const EarlyQueue::Item& it) -> int { if (first < 0) first = it.key.lid; return EarlyQueue::EQ_KEEP; }, [](const EarlyQueue::Item&) {});
    CHECK(first == 1);
    // one sender alone can never fill the queue (worldsync E)
    q.clear();
    int evicted = 0;
    for (int i = 0; i < 1000; i++) if (q.push(MP_MSG_KILL, 2, 5, K(1, 2, (uint16_t)i, 0), 0, &ev)) evicted++;
    CHECK(q.count() == EarlyQueue::kPerSender && evicted == 1000 - EarlyQueue::kPerSender);
    CHECK(!q.push(MP_MSG_KILL, 3, 5, K(1, 2, 0, 0), 0, &ev) && q.countFrom(3) == 1);   // another sender still has room
    // dedup is per sender: the same event from two senders is two entries, a repeat from one is not
    CHECK(!q.push(MP_MSG_KILL, 3, 5, K(1, 2, 0, 0), 9, &ev) && q.countFrom(3) == 1);
    CHECK(serialWaitable(7, 7) && serialWaitable(8, 7) && !serialWaitable(9, 7) && !serialWaitable(6, 7) && !serialWaitable(7 + 32767, 7) &&
          serialWaitable(0, 0xFFFF) && !serialWaitable(0xFFFF, 0));   // current or the next scene only, wrap included
    CHECK(serialAhead(2, 1) && !serialAhead(1, 2) && !serialAhead(5, 5) && serialAhead(0, 0xFFFF) && !serialAhead(0xFFFF, 0));   // wraps
    // wait-then-apply: a queue run that finds the scene not ready keeps everything, and applies all once ready
    q.clear();
    for (int i = 0; i < 10; i++) q.push(MP_MSG_CHIP, 0, 3, K(1, 2, (uint16_t)i, 0), 0);
    bool ready = false;
    int applied = 0;
    auto fn = [&](const EarlyQueue::Item&) -> int { if (!ready) return EarlyQueue::EQ_KEEP; applied++; return EarlyQueue::EQ_DONE; };
    q.run(1, fn, [](const EarlyQueue::Item&) {});
    CHECK(q.count() == 10 && applied == 0);
    ready = true;
    q.run(2, fn, [](const EarlyQueue::Item&) {});
    CHECK(q.count() == 0 && applied == 10);
}

// worldsync: the object state map (kill / purple coin / chip) for a late joiner or a repair
struct ObjApplyLog { int n; int kind[600]; int idx[600]; };
static void applyLogFn(void* c, int kind, int idx) { ObjApplyLog* l = (ObjApplyLog*)c; if (l->n < 600) { l->kind[l->n] = kind; l->idx[l->n] = idx; } l->n++; }
static void testObjMap() {
    static Registry reg;
    reg.clear();
    const int N = 1100;   // > 128 * 8 bits: two chunks of a kind
    for (int i = 0; i < N; i++) {
        uint8_t f = (i % 3 == 0) ? ENTRY_ENEMY : (i % 3 == 1 ? ENTRY_PURPLE : ENTRY_CHIP);
        reg.add(K(1, 100 + i % 50, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x1000 + i * 16), f);   // (the actor pointer is only an identity here)
    }
    reg.finalize();
    CHECK(reg.count() == N);
    // mark: every enemy with index % 6 == 0 killed, the purples at index % 7 == 1 got, one chip got
    int wantK = 0, wantP = 0, wantC = 0;
    for (int i = 0; i < reg.count(); i++) {
        Entry& e = reg.at(i);
        if ((e.flags & ENTRY_ENEMY) && i % 2 == 0) { e.flags |= ENTRY_KILLED; wantK++; }
        if ((e.flags & ENTRY_PURPLE) && i % 5 == 0) { e.flags |= ENTRY_GOT; wantP++; }
        if ((e.flags & ENTRY_CHIP) && wantC == 0) { e.flags |= ENTRY_GOT; wantC++; }
        if ((e.flags & ENTRY_DUP) && (e.flags & ENTRY_ENEMY)) { e.flags |= ENTRY_KILLED; }   // a duplicate never counts
    }
    static ObjState st;
    packObjState(reg, &st);
    CHECK(st.usedBytes == (reg.count() + 7) / 8);
    CHECK(st.count[0] == wantK && st.count[1] == wantP && st.count[2] == wantC);
    // pack -> chunks -> parse -> apply gives back exactly the same sets, every message under the 1444-byte datagram (and under 138)
    static bool seen[3][2560];
    memset(seen, 0, sizeof seen);
    int msgs = 0;
    size_t biggest = 0;
    uint8_t buf[kObjMapHeader + kObjMapChunkBytes];
    for (int kind = OM_KILL; kind <= OM_CHIP; kind++) {
        for (int start = 0; start < st.usedBytes; start += kObjMapChunkBytes) {
            const int n = st.usedBytes - start < kObjMapChunkBytes ? st.usedBytes - start : (int)kObjMapChunkBytes;
            if (!objChunkHasBits(st, kind, start, n)) continue;
            const size_t len = writeObjChunk(buf, sizeof buf, 9, reg.crc(), kind, st, start, n);
            CHECK(len == (size_t)kObjMapHeader + n);
            if (len > biggest) biggest = len;
            ObjMapMsg m;
            CHECK(readObjMap(buf, len, &m) && m.serial == 9 && m.regCrc == reg.crc() && m.kind == kind && m.start == start && m.n == n);
            ObjApplyLog lg; lg.n = 0;
            applyObjChunk(m, reg.count(), applyLogFn, &lg);
            for (int i = 0; i < lg.n && i < 600; i++) seen[lg.kind[i] - 1][lg.idx[i]] = true;
            msgs++;
        }
    }
    CHECK(msgs >= 4 && biggest == 138 && biggest < 1444);
    for (int k = 0; k < 3; k++) for (int i = 0; i < reg.count(); i++) {
        const uint8_t f = reg.at(i).flags;
        const bool want = (f & ENTRY_DUP) ? false : (k == 0 ? ((f & ENTRY_ENEMY) && (f & ENTRY_KILLED)) : (k == 1 ? ((f & ENTRY_PURPLE) && (f & ENTRY_GOT)) : ((f & ENTRY_CHIP) && (f & ENTRY_GOT))));
        CHECK(seen[k][i] == want);
    }
    // summary round trip
    const size_t sl = writeObjSummary(buf, sizeof buf, 9, reg.crc(), st);
    ObjMapMsg sm;
    CHECK(sl == (size_t)kObjSummaryLen && readObjMap(buf, sl, &sm) && sm.kind == OM_SUMMARY && sm.count[0] == st.count[0] && sm.crc == st.crc);
    CHECK(!objStateBehind(sm, st));                                    // equal: nothing to ask for
    static ObjState lacking;
    lacking = st;
    lacking.count[0]--; lacking.crc ^= 1;
    CHECK(objStateBehind(sm, lacking));                                // host has more kills
    static ObjState more;
    more = st; more.count[1]++;
    CHECK(!objStateBehind(sm, more));                                  // we have extras the host lacks: not behind
    static ObjState swapped;
    swapped = st; swapped.crc ^= 1;
    CHECK(objStateBehind(sm, swapped));                                // same counts, another set
    // idempotent: applying the same chunk twice calls apply twice for the same bits (the glue skips what is done); no growth of state here
    // truncated at every length, one extra byte, oversize
    const size_t full = writeObjChunk(buf, sizeof buf, 9, reg.crc(), OM_KILL, st, 0, 128);
    CHECK(full == 138);
    for (size_t cut = 0; cut < full; cut++) { ObjMapMsg t; CHECK(!readObjMap(buf, cut, &t)); }
    uint8_t big[200];
    memcpy(big, buf, full); big[full] = 0;
    { ObjMapMsg t; CHECK(!readObjMap(big, full + 1, &t)); }
    for (size_t cut = 0; cut < (size_t)kObjSummaryLen; cut++) { uint8_t b2[32]; writeObjSummary(b2, sizeof b2, 1, 2, st); ObjMapMsg t; CHECK(!readObjMap(b2, cut, &t)); }
    // bad fields: kind 4, n 0, n 129, start + n over the maximum, summary count over the registry size
    { MpWriter w(big, sizeof big); w.u16(1); w.u32(2); w.u8(4); w.u16(0); w.u8(1); w.u8(0); ObjMapMsg t; CHECK(!readObjMap(big, w.n, &t)); }
    { MpWriter w(big, sizeof big); w.u16(1); w.u32(2); w.u8(1); w.u16(0); w.u8(0); ObjMapMsg t; CHECK(!readObjMap(big, w.n, &t)); }
    { MpWriter w(big, sizeof big); w.u16(1); w.u32(2); w.u8(1); w.u16(0); w.u8(129); for (int i = 0; i < 129; i++) w.u8(1); ObjMapMsg t; CHECK(!readObjMap(big, w.n, &t)); }
    { MpWriter w(big, sizeof big); w.u16(1); w.u32(2); w.u8(1); w.u16(300); w.u8(30); for (int i = 0; i < 30; i++) w.u8(1); ObjMapMsg t; CHECK(!readObjMap(big, w.n, &t)); }   // 330 > 320
    { MpWriter w(big, sizeof big); w.u16(1); w.u32(2); w.u8(0); w.u16(2561); w.u16(0); w.u16(0); w.u32(0); ObjMapMsg t; CHECK(!readObjMap(big, w.n, &t)); }
    // writer refuses bad arguments
    CHECK(writeObjChunk(buf, sizeof buf, 1, 1, 0, st, 0, 8) == 0 && writeObjChunk(buf, sizeof buf, 1, 1, 1, st, 0, 129) == 0 &&
          writeObjChunk(buf, sizeof buf, 1, 1, 1, st, st.usedBytes, 1) == 0 && writeObjChunk(buf, 12, 1, 1, 1, st, 0, 8) == 0);
    // bits past the registry count are ignored by the applier
    { ObjMapMsg m; memset(&m, 0, sizeof m); m.kind = OM_KILL; m.start = 0; m.n = 128; memset(m.bytes, 0xFF, 128); ObjApplyLog lg; lg.n = 0; const int c = applyObjChunk(m, 10, applyLogFn, &lg); CHECK(c == 10 && lg.n == 10); }
    // fuzz: random bytes never crash the reader and an accepted message is canonical
    srand(7);
    for (int i = 0; i < 20000; i++) {
        uint8_t r[160]; const size_t n = (size_t)(rand() % 160);
        for (size_t j = 0; j < n; j++) r[j] = (uint8_t)rand();
        if (n > 7 && (rand() & 1)) r[6] = (uint8_t)(rand() % 5);
        ObjMapMsg m;
        if (readObjMap(r, n, &m)) CHECK(m.kind == 0 ? n == (size_t)kObjSummaryLen : n == (size_t)kObjMapHeader + m.n);
    }
}
static void testRegCrcFlags() {
    uint8_t b[16];
    RegCrcMsg m = {5, 0xdeadbeef};
    CHECK(writeRegCrc(b, sizeof b, m) == 6);                   // the placement announcement is unchanged (6 bytes)
    RegCrcMsg r;
    CHECK(readRegCrc(b, 6, &r) && r.serial == 5 && r.crc == 0xdeadbeef && r.flags == 0);
    m.flags = kRegCrcRepair;
    CHECK(writeRegCrc(b, sizeof b, m) == 7);
    CHECK(readRegCrc(b, 7, &r) && r.flags == kRegCrcRepair);
    b[6] = 0;  CHECK(!readRegCrc(b, 7, &r));                   // 7 bytes with no flag is not canonical
    b[6] = 2;  CHECK(!readRegCrc(b, 7, &r));                   // unknown flag
    CHECK(!readRegCrc(b, 5, &r) && !readRegCrc(b, 8, &r) && !readRegCrc(b, 0, &r));
}

// worldsync review fixes: A (shared kinds), B (dead now), F (periodic checks), H (summary every 30 s)
static bool g_deadSet[3000];
static bool deadFnTest(const void* a) { return g_deadSet[(uintptr_t)a - 0x1000]; }
static void testPackGating() {
    static Registry reg;
    reg.clear();
    for (int i = 0; i < 9; i++) {
        uint8_t f = i < 3 ? ENTRY_ENEMY : (i < 6 ? ENTRY_PURPLE : ENTRY_CHIP);
        reg.add(K(1, 10, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x1000 + i), f);
    }
    reg.finalize();
    memset(g_deadSet, 0, sizeof g_deadSet);
    for (int i = 0; i < reg.count(); i++) {
        Entry& e = reg.at(i);
        if (e.flags & ENTRY_ENEMY) e.flags |= ENTRY_KILLED;
        else e.flags |= ENTRY_GOT;
        g_deadSet[(uintptr_t)e.actor - 0x1000] = true;
    }
    static ObjState st;
    packObjState(reg, &st);   // defaults: everything shared, no liveness check
    CHECK(st.count[0] == 3 && st.count[1] == 3 && st.count[2] == 3);
    // A: a kind the session does not share is never packed
    packObjState(reg, &st, SHARE_KILL);
    CHECK(st.count[0] == 3 && st.count[1] == 0 && st.count[2] == 0);
    packObjState(reg, &st, SHARE_PURPLE | SHARE_CHIP);
    CHECK(st.count[0] == 0 && st.count[1] == 3 && st.count[2] == 3);
    packObjState(reg, &st, 0);
    CHECK(st.count[0] == 0 && st.count[1] == 0 && st.count[2] == 0 && st.crc == crc32(crc32(crc32(0, st.bits[0], (size_t)st.usedBytes), st.bits[1], (size_t)st.usedBytes), st.bits[2], (size_t)st.usedBytes));
    // B: an enemy that is alive now (it came back) is not a defeat
    int comeback = -1;
    for (int i = 0; i < reg.count(); i++) if (reg.at(i).flags & ENTRY_ENEMY) { comeback = i; break; }
    g_deadSet[(uintptr_t)reg.at(comeback).actor - 0x1000] = false;
    packObjState(reg, &st, SHARE_ALL, deadFnTest);
    CHECK(st.count[0] == 2 && !((st.bits[0][comeback >> 3] >> (comeback & 7)) & 1));
    CHECK(st.count[1] == 3 && st.count[2] == 3);   // coins and chips have no "come back"
    packObjState(reg, &st, SHARE_ALL, nullptr);
    CHECK(st.count[0] == 3);                        // (without the liveness function the flag alone counts: the glue always passes it)
}
static void testPeriodicAndSummary() {
    uint8_t b[32];
    MapMsg none; memset(&none, 0, sizeof none);
    SwitchMirror* m = new SwitchMirror();
    m->reset(3);
    size_t n = writeMap(b, sizeof b, 3, *m, false);
    const size_t nf = [&] { uint8_t b2[kMapHeader + kMapEntry * kMapMaxZones]; return writeMap(b2, sizeof b2, 3, *m, true); }();
    CHECK(n == (size_t)kMapHeader && nf == n);                                              // an EMPTY mirror: check and full map are the same bytes (crc 0, count 0)
    CHECK(!isPeriodicCheck(MP_MSG_SWITCHMAP, b, n));                                        // so it is an answer, never dropped as a check (worldsync item 2)
    m->setOn(0, 5, true);
    n = writeMap(b, sizeof b, 3, *m, false);
    CHECK(n == (size_t)kMapHeader && isPeriodicCheck(MP_MSG_SWITCHMAP, b, n));              // bits in the mirror: a CRC-only check has crc != 0
    uint8_t big[kMapHeader + kMapEntry * kMapMaxZones];
    n = writeMap(big, sizeof big, 3, *m, true);
    CHECK(n > (size_t)kMapHeader && !isPeriodicCheck(MP_MSG_SWITCHMAP, big, n));          // a full map answer is held
    delete m;
    static ObjState st; memset(&st, 0, sizeof st);
    n = writeObjSummary(b, sizeof b, 3, 4, st);
    CHECK(n == (size_t)kObjSummaryLen && isPeriodicCheck(MP_MSG_OBJMAP, b, n));
    uint8_t ch[kObjMapHeader + kObjMapChunkBytes];
    st.usedBytes = 8; st.bits[0][0] = 1;
    n = writeObjChunk(ch, sizeof ch, 3, 4, OM_KILL, st, 0, 8);
    CHECK(n > 0 && !isPeriodicCheck(MP_MSG_OBJMAP, ch, n));                                // a chunk is held
    CHECK(!isPeriodicCheck(MP_MSG_KILL, b, 8) && !isPeriodicCheck(MP_MSG_SWITCHMAP, b, 3) && !isPeriodicCheck(MP_MSG_OBJMAP, b, 3));
    // H: a stuck console: one summary per 30 s, nothing when nothing new happens
    DropStats d;
    CHECK(!d.summaryDue(0));
    uint32_t lines = 0, summaries = 0;
    for (uint32_t t = 1; t <= 60 * 120; t++) {          // 2 minutes of one drop per turn
        if (d.note(DT_KILL, DR_NOT_PLACED, t)) lines++;
        if (d.summaryDue(t)) summaries++;
    }
    CHECK(lines <= 1 + DropStats::kLogBurst + 60 * 120 / DropStats::kLogRefillTurns + 1);     // not a line a second (was ~120)
    CHECK(summaries >= 3 && summaries <= 5);
    DropStats q;
    for (uint32_t t = 0; t < 10000; t += 100) CHECK(!q.summaryDue(t));                         // no drops: no summary
}

// worldsync review 2: the share flags on live events, the repair budget
static void testKindSharedAndBudget() {
    CHECK(eventKindShared(MP_MSG_KILL, SHARE_KILL) && !eventKindShared(MP_MSG_KILL, SHARE_PURPLE | SHARE_CHIP));
    CHECK(eventKindShared(MP_MSG_PURPLE, SHARE_PURPLE) && !eventKindShared(MP_MSG_PURPLE, SHARE_KILL | SHARE_CHIP) && !eventKindShared(MP_MSG_PURPLE, 0));
    CHECK(eventKindShared(MP_MSG_CHIP, SHARE_CHIP) && !eventKindShared(MP_MSG_CHIP, SHARE_KILL | SHARE_PURPLE) && !eventKindShared(MP_MSG_CHIP, 0));
    CHECK(eventKindShared(MP_MSG_SWITCH, 0) && eventKindShared(MP_MSG_REGCRC, 0));         // not object kinds: never gated here
    RepairBudget b;
    int ok = 0;
    for (uint32_t t = 1; t <= 5; t++) ok += b.take(t * 120);                                // 5 requests 2 s apart: the whole allowance
    CHECK(ok == RepairBudget::kMax);
    CHECK(!b.take(5 * 120 + 60));                                                           // spent (540 turns after the clock started)
    CHECK(b.take(120 + RepairBudget::kRefillTurns) && !b.take(120 + RepairBudget::kRefillTurns + 1));   // one back 10 s after the clock started
    ok = 0;
    for (uint32_t t = 10000; t < 10000 + 6000; t += 120) ok += b.take(t);              // a request every 2 s for 100 s
    CHECK(ok >= 13 && ok <= 16);                                                            // the full 5 again, then ~1 per 10 s: not a hard stop, not a flood
    b.reset();
    ok = 0;
    for (int i = 0; i < 20; i++) ok += b.take(50000);
    CHECK(ok == RepairBudget::kMax);
    CHECK(b.take(50000 + 100000) && b.take(50000 + 100000));                                // long later: full again
    // a wrapped turn counter
    RepairBudget w;
    for (int i = 0; i < 5; i++) CHECK(w.take(0xFFFFFF00u + (uint32_t)i));
    CHECK(!w.take(0xFFFFFF10u) && w.take(0xFFFFFF00u + RepairBudget::kRefillTurns + 4));
}

// regcrc false alarm after a reload ("Everyone was caught": the same serial is placed twice, tour finding)
static void testPeerCrcReload() {
    using namespace MpWorldLogic;
    PeerCrc p;
    // first placement: the host's crc agrees
    p.beginPlacement();
    p.received(5, 0x9818ec2c);
    CHECK(p.agrees(5, 0x9818ec2c));
    for (int i = 0; i < 500; i++) CHECK(!p.tick(true, 5, 0x9818ec2c));
    // the reload places serial 5 again with another crc: the old knowledge must not be judged against it
    p.beginPlacement();
    CHECK(!p.agrees(5, 0x81883945) && !p.disagrees(5, 0x81883945));
    for (int i = 0; i < 500; i++) CHECK(!p.tick(true, 5, 0x81883945));   // nothing known: never a mismatch line
    // the host's fresh crc arrives late: sync is on at once, no line
    p.received(5, 0x81883945);
    CHECK(p.agrees(5, 0x81883945));
    // a stale crc arriving after our placement: a disagreement, but only reported when it lasts; the fresh one heals it
    p.beginPlacement();
    p.received(5, 0x9818ec2c);
    CHECK(p.disagrees(5, 0x81883945));
    int lines = 0;
    for (int i = 0; i < (int)kMismatchGrace - 2; i++) lines += p.tick(true, 5, 0x81883945);
    p.received(5, 0x81883945);
    for (int i = 0; i < 500; i++) lines += p.tick(true, 5, 0x81883945);
    CHECK(lines == 0 && p.agrees(5, 0x81883945));
    // a real mismatch is reported exactly once, after the grace
    PeerCrc q;
    q.received(7, 1);
    int n = 0;
    for (int i = 0; i < 1000; i++) n += q.tick(true, 7, 2);
    CHECK(n == 1 && !q.agrees(7, 2));
    // another serial's crc is never a verdict
    CHECK(!q.disagrees(8, 2) && !q.tick(true, 8, 2));
}

// optfix item 16: the session block is allocated once, a failed allocation is remembered (no retry per message) and every release resets it.
static int g_allocs, g_frees;
static bool g_allocFails;
static Registry* testAlloc() {
    if (g_allocFails) return nullptr;
    g_allocs++;
    return static_cast< Registry* >(calloc(1, sizeof(Registry)));
}
static void testFree(Registry* r) { g_frees++; free(r); }
static void testLazyMem() {
    LazyMem< Registry > m;
    CHECK(m.get() == nullptr && !m.failed());
    Registry* a = m.acquire(testAlloc);
    CHECK(a != nullptr && m.get() == a && g_allocs == 1);
    CHECK(m.acquire(testAlloc) == a && g_allocs == 1);   // once
    m.release(testFree);
    CHECK(m.get() == nullptr && g_frees == 1);
    m.release(testFree);                                  // (releasing nothing is harmless)
    CHECK(g_frees == 1);
    g_allocFails = true;
    CHECK(m.acquire(testAlloc) == nullptr && m.failed());
    g_allocFails = false;
    CHECK(m.acquire(testAlloc) == nullptr && g_allocs == 1);   // the failure is remembered: no retry until the session is over
    m.release(testFree);
    CHECK(!m.failed());
    CHECK(m.acquire(testAlloc) != nullptr && g_allocs == 2);   // a later session tries again
    m.release(testFree);
    // fix2: a failure inside a session is forgotten at the next scene start (retryNext), not kept for the whole session
    g_allocFails = true;
    CHECK(m.acquire(testAlloc) == nullptr && m.failed());
    g_allocFails = false;
    CHECK(m.acquire(testAlloc) == nullptr);                    // (same scene: still no retry)
    m.retryNext();                                             // next scene start
    CHECK(!m.failed() && m.acquire(testAlloc) != nullptr);
    m.release(testFree);
}

// hostreload: the host reloads its own scene (a miss: MOVE types 5 / 7, same serial). The registry is rebuilt, every flag is gone; the state
// the guests cleared must come back on the host (and never go out as "nothing" to a joiner).
static void testHostReloadCarry() {
    static Registry reg;
    const int N = 40;
    auto build = [&](Registry& r) {
        r.clear();
        for (int i = 0; i < N; i++) {
            uint8_t f = (i % 3 == 0) ? ENTRY_ENEMY : (i % 3 == 1 ? ENTRY_PURPLE : ENTRY_CHIP);
            r.add(K(1, 100 + i, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x1000 + i * 16), f);
        }
        r.finalize();
    };
    build(reg);
    const uint32_t crc = reg.crc();
    ObjCarry carry;
    carryClear(&carry);
    // before the reload: guests killed enemies 0, 3, 9; the host took purple 1 and chip 5 (a duplicate-free scene)
    const int kills[3] = {0, 3, 9};
    for (int k : kills) { reg.at(k).flags |= ENTRY_KILLED; carryNote(&carry, 5, crc, OM_KILL, k); }
    reg.at(1).flags |= ENTRY_GOT; carryNote(&carry, 5, crc, OM_PURPLE, 1);
    reg.at(5).flags |= ENTRY_GOT; carryNote(&carry, 5, crc, OM_CHIP, 5);
    static ObjState before;
    packObjState(reg, &before);
    CHECK(before.count[0] == 3 && before.count[1] == 1 && before.count[2] == 1);
    // guest side (it did not reload): its summary state is the same as the host's was
    ObjMapMsg guestSum;
    { uint8_t b[32]; const size_t l = writeObjSummary(b, sizeof b, 5, crc, before); CHECK(readObjMap(b, l, &guestSum)); }
    // the reload: same serial, registry rebuilt (same keys, fresh flags)
    build(reg);
    CHECK(reg.crc() == crc);
    static ObjState after;
    packObjState(reg, &after);
    CHECK(after.count[0] == 0 && after.count[1] == 0 && after.count[2] == 0);   // the lost state (the bug: this is what the host used to hand out)
    // what the host sends right after the reload (its kills wait for the live scene) already holds the carry
    static ObjState merged;
    merged = after;
    carryMerge(carry, 5, reg.crc(), SHARE_ALL, reg, &merged);
    CHECK(merged.crc == before.crc && memcmp(merged.bits, before.bits, sizeof before.bits) == 0 && merged.count[0] == 3 && merged.count[1] == 1 && merged.count[2] == 1);
    { static ObjState other; other = after; carryMerge(carry, 6, reg.crc(), SHARE_ALL, reg, &other); CHECK(other.crc == after.crc && other.count[0] == 0); }   // another serial: unchanged
    { static ObjState part; part = after; carryMerge(carry, 5, reg.crc(), SHARE_KILL, reg, &part); CHECK(part.count[0] == 3 && part.count[1] == 0 && part.count[2] == 0); }
    // the carry is re-applied to the new placement: the same bits come back, nothing else
    ObjApplyLog lg; lg.n = 0;
    CHECK(carryApply(&carry, 5, reg.crc(), reg.count(), SHARE_ALL, applyLogFn, &lg) == 5 && lg.n == 5);
    for (int i = 0; i < lg.n; i++) {
        Entry& e = reg.at(lg.idx[i]);
        if (lg.kind[i] == OM_KILL) e.flags |= ENTRY_KILLED; else e.flags |= ENTRY_GOT;
    }
    static ObjState healed;
    packObjState(reg, &healed);
    CHECK(healed.crc == before.crc && memcmp(healed.bits, before.bits, sizeof before.bits) == 0);
    CHECK(!objStateBehind(guestSum, healed));   // host and guest agree again
    // shares: a kind the session does not share is not carried back
    build(reg);
    lg.n = 0;
    CHECK(carryApply(&carry, 5, reg.crc(), reg.count(), SHARE_KILL, applyLogFn, &lg) == 3);
    for (int i = 0; i < lg.n; i++) CHECK(lg.kind[i] == OM_KILL);
    // a different scene serial (a real MOVE) or another registry (another scenario / placement) never carries state over
    CHECK(carryApply(&carry, 6, reg.crc(), reg.count(), SHARE_ALL, applyLogFn, &lg) == 0 && !carry.valid);
    carryNote(&carry, 5, crc, OM_KILL, 2);
    CHECK(carryApply(&carry, 5, crc ^ 1, reg.count(), SHARE_ALL, applyLogFn, &lg) == 0 && !carry.valid);
    // a note for another binding replaces the old one; out of range / bad kind notes are ignored
    carryNote(&carry, 7, 111, OM_KILL, 4);
    carryNote(&carry, 8, 222, OM_PURPLE, 6);
    CHECK(carry.valid && carry.serial == 8 && carry.regCrc == 222 && carry.st.count[0] == 0 && carry.st.count[1] == 1);
    carryNote(&carry, 8, 222, OM_PURPLE, Registry::kMax);
    carryNote(&carry, 8, 222, 0, 3);
    carryNote(&carry, 8, 222, 4, 3);
    carryNote(&carry, 8, 222, OM_PURPLE, -1);
    CHECK(carry.st.count[1] == 1 && carry.st.count[0] == 0 && carry.st.count[2] == 0);
    carryNote(&carry, 8, 222, OM_PURPLE, 6);   // twice: counted once
    CHECK(carry.st.count[1] == 1);
    // only the bits below the registry count are applied
    lg.n = 0;
    carryNote(&carry, 8, 222, OM_KILL, 100);
    CHECK(carryApply(&carry, 8, 222, 50, SHARE_ALL, applyLogFn, &lg) == 1);
}

// hostreload: Try Again (one console reloads, others stay) keeps the world on the host; a GROUP restart resets it on every console.
static void testCarryPolicy() {
    CHECK(carryMayApply(true, 1) && carryMayApply(true, 3));      // the host, another player still in the scene: Try Again
    CHECK(!carryMayApply(true, 0));                              // alone: a fresh scene
    CHECK(!carryMayApply(false, 2));                             // a guest never re-applies (the host's OBJMAP answers it)
    static Registry reg;
    reg.clear();
    for (int i = 0; i < 6; i++) reg.add(K(1, 10 + i, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x1000 + i * 16), ENTRY_ENEMY);
    reg.finalize();
    // three consoles of one session, each with a carry of the scene (serial 5): host, a guest in the scene, a guest still loading
    ObjCarry con[3];
    for (int k = 0; k < 3; k++) { carryClear(&con[k]); carryNote(&con[k], 5, reg.crc(), OM_KILL, 2); carryNote(&con[k], 5, reg.crc(), OM_KILL, 4); }
    ObjApplyLog lg; lg.n = 0;
    // (a) Try Again on the host: nothing dropped, the carry comes back
    CHECK(carryApply(&con[0], 5, reg.crc(), reg.count(), SHARE_ALL, applyLogFn, &lg) == 2);
    // (b) group restart: RESTART reaches the host (it sends it) and the guest in the scene; the loading console drops the message but still
    // clears (G8): afterwards none of the three has a carry, and the last events of the wipe (a late KILL applied) are not noted either
    for (int k = 0; k < 3; k++) carryDrop(&con[k]);
    for (int k = 0; k < 3; k++) {
        CHECK(!con[k].valid && con[k].blocked);
        carryNote(&con[k], 5, reg.crc(), OM_KILL, 1);
        CHECK(!con[k].valid && con[k].st.count[0] == 0);
        lg.n = 0;
        CHECK(carryApply(&con[k], 5, reg.crc(), reg.count(), SHARE_ALL, applyLogFn, &lg) == 0 && lg.n == 0);
    }
    // the next placement arms it again: notes count, and the new scene's carry starts empty
    for (int k = 0; k < 3; k++) { carryArm(&con[k]); carryNote(&con[k], 5, reg.crc(), OM_KILL, 1); CHECK(con[k].valid && con[k].st.count[0] == 1); }
    // dropping twice / dropping an empty carry is harmless
    carryDrop(&con[0]); carryDrop(&con[0]); carryArm(&con[0]);
    CHECK(!con[0].valid);
}

// hostreload (review 14 Nov): outstanding-only merge, a came-back enemy, passes over more than 64 bits, the group-restart window.
static bool g_alive[64];
static bool deadByTable(const void* a) { return !g_alive[((uintptr_t)a - 0x1000) >> 4]; }
struct PassCtx { Registry* reg; int calls; int failFrom; };
static bool passTry(void* c, int kind, int idx) {
    PassCtx* p = (PassCtx*)c;
    p->calls++;
    if (idx >= p->failFrom) return false;                  // (e.g. a coin that has not appeared yet)
    Entry& e = p->reg->at(idx);
    if (kind == OM_KILL) e.flags |= ENTRY_KILLED; else e.flags |= ENTRY_GOT;
    return true;
}
static void testCarryReview() {
    static Registry reg;
    reg.clear();
    for (int i = 0; i < 12; i++) reg.add(K(1, 10 + i, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x1000 + i * 16), ENTRY_ENEMY);
    reg.finalize();
    const uint32_t crc = reg.crc();
    ObjCarry c; carryClear(&c);
    carryNote(&c, 5, crc, OM_KILL, 1); carryNote(&c, 5, crc, OM_KILL, 2); carryNote(&c, 5, crc, OM_KILL, 3);
    // 1. merge only what is outstanding: entry 1 already put back (flagged and dead), entry 2 came back by design (flagged, alive)
    for (int i = 0; i < 12; i++) g_alive[i] = true;
    reg.at(1).flags |= ENTRY_KILLED; g_alive[1] = false;
    reg.at(2).flags |= ENTRY_KILLED;                       // alive: Meramera's recovery
    CHECK(carryOutstanding(c, reg, OM_KILL, 3) && !carryOutstanding(c, reg, OM_KILL, 1) && !carryOutstanding(c, reg, OM_KILL, 4));
    CHECK(carryPrune(&c, reg, deadByTable) == 1 && c.st.count[0] == 2);     // entry 2 leaves the carry, 1 (dead) stays
    static ObjState st;
    packObjState(reg, &st, SHARE_ALL, deadByTable);        // the host packs only what is dead now: entry 1
    CHECK(st.count[0] == 1);
    carryMerge(c, 5, crc, SHARE_ALL, reg, &st);
    CHECK(st.count[0] == 2);                               // + entry 3 (outstanding); the came-back 2 is NOT in the map (no re-kill on a guest)
    CHECK(!(st.bits[0][0] & (1u << 2)) && (st.bits[0][0] & (1u << 1)) && (st.bits[0][0] & (1u << 3)));
    // a later host reload: the registry is rebuilt, the carry has no bit for the came-back enemy
    reg.clear();
    for (int i = 0; i < 12; i++) reg.add(K(1, 10 + i, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x1000 + i * 16), ENTRY_ENEMY);
    reg.finalize();
    PassCtx pc = {&reg, 0, 1000};
    int left = -1;
    CHECK(carryPass(&c, reg, SHARE_ALL, 8, passTry, &pc, &left) == 2 && left == 0 && pc.calls == 2);
    CHECK(!(reg.at(2).flags & ENTRY_KILLED) && (reg.at(1).flags & ENTRY_KILLED) && (reg.at(3).flags & ENTRY_KILLED));
    CHECK(carryPass(&c, reg, SHARE_ALL, 8, passTry, &pc, &left) == 0 && left == 0);   // done: nothing outstanding, nothing called

    // 2. more than 64 carried bits (a purple coin comet: 100): passes of 8 per live turn, never the 64-entry queue; a failing try stays outstanding
    static Registry big;
    big.clear();
    for (int i = 0; i < 120; i++) big.add(K(2, 20 + i % 90, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x9000 + i * 16), ENTRY_PURPLE);
    big.finalize();
    ObjCarry bc; carryClear(&bc);
    for (int i = 0; i < 100; i++) carryNote(&bc, 9, big.crc(), OM_PURPLE, i);
    CHECK(bc.st.count[1] == 100);
    PassCtx bp = {&big, 0, 95};                            // coins 95..99 have not appeared
    int turns = 0, total = 0;
    left = -1;
    while (left != 5 && turns < 100) { total += carryPass(&bc, big, SHARE_ALL, 8, passTry, &bp, &left); turns++; }
    CHECK(total == 95 && left == 5 && turns == 12);   // 95 done at 8 tries a pass: 12 passes
    bp.failFrom = 1000;                                    // they appear
    total += carryPass(&bc, big, SHARE_ALL, 8, passTry, &bp, &left);
    CHECK(total == 100 && left == 0);
    for (int i = 0; i < 100; i++) CHECK(big.at(i).flags & ENTRY_GOT);
    for (int i = 100; i < 120; i++) CHECK(!(big.at(i).flags & ENTRY_GOT));
    // a pass call never exceeds its bound
    ObjCarry bc2; carryClear(&bc2);
    for (int i = 0; i < 100; i++) carryNote(&bc2, 9, big.crc(), OM_PURPLE, i);
    big.clear();
    for (int i = 0; i < 120; i++) big.add(K(2, 20 + i % 90, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x9000 + i * 16), ENTRY_PURPLE);
    big.finalize();
    PassCtx bp2 = {&big, 0, 1000};
    CHECK(carryPass(&bc2, big, SHARE_ALL, 8, passTry, &bp2, &left) == 8 && bp2.calls == 8 && left == 92);

    // 3a. host, group restart window: a map request while the carry is blocked waits for the host's own placementDone
    CHECK(replyDeferred(true, true) && replyDeferred(false, false) && replyDeferred(false, true) && !replyDeferred(true, false));
    ObjCarry hc; carryClear(&hc);
    carryNote(&hc, 5, 77, OM_KILL, 1);
    carryDrop(&hc);                                        // RESTART: blocked
    CHECK(hc.blocked && replyDeferred(true, hc.blocked));
    carryArm(&hc);                                         // the host's beginPlacement: answered at placementDone
    CHECK(!replyDeferred(true, hc.blocked));
    // 3b. a console that got RESTART after its own beginPlacement but before it is live: pending + early objects of this scene cleared, the carry
    // cleared WITHOUT blocking (so it takes notes in the new scene), the map asked again
    PendingQueue pq; pq.clear();
    pq.push(PEND_KILL, 3, 0); pq.push(PEND_PURPLE, 4, 0);
    EarlyQueue eq; eq.clear();
    MpObjKey k1 = K(1, 5, 5, 0);
    eq.push(MP_MSG_KILL, 1, 5, k1, 0); eq.push(MP_MSG_KILL, 1, 6, k1, 0);
    ObjCarry lc; carryClear(&lc);
    carryNote(&lc, 5, 77, OM_KILL, 3);
    pq.clear();
    CHECK(eq.dropSerial(5) == 1 && eq.count() == 1);       // the next scene's event stays
    carryClear(&lc);
    CHECK(pq.count() == 0 && !lc.valid && !lc.blocked);
    carryNote(&lc, 5, 77, OM_KILL, 4);
    CHECK(lc.valid && lc.st.count[0] == 1);                // notes of the new scene count
    // the held pool: an OBJMAP chunk held before the restart is not applied after it; others stay
    static MpHeld::Pool hp;
    hp.clear();
    MpMsg hm; memset(&hm, 0, sizeof hm);
    hm.type = MP_MSG_OBJMAP; hm.fromSlot = 0; hm.len = 5; memset(hm.data, 7, 5); CHECK(hp.push(hm));
    hm.type = 0x13; hm.len = 3; CHECK(hp.push(hm));
    hm.type = MP_MSG_SWITCH; hm.len = 4; CHECK(hp.push(hm));
    const uint32_t usedBefore = hp.usedBytes();
    CHECK(hp.dropRange(0x40, 0x4F) == 2 && hp.count() == 1 && hp.usedBytes() == (uint32_t)(MpHeld::HEADER + 3) && hp.usedBytes() < usedBefore);   // compacted: the slots and bytes are free
    hp.rewind();
    MpMsg o; int seenWorld = 0, seen = 0;
    while (hp.next(&o)) { seen++; if (o.type >= 0x40 && o.type <= 0x4F) seenWorld++; }
    CHECK(seen == 1 && seenWorld == 0 && hp.dropRange(0x40, 0x4F) == 0);
    hm.type = MP_MSG_OBJMAP; hm.len = 9; CHECK(hp.push(hm));   // the freed room is usable at once
    hp.rewind(); seen = 0; while (hp.next(&o)) seen++;
    CHECK(seen == 2 && hp.count() == 2);
    // the pass: a failing bit at the head does not starve the ones behind it
    { ObjCarry fc; carryClear(&fc);
      static Registry fr; fr.clear();
      for (int i = 0; i < 20; i++) fr.add(K(3, 30 + i, (uint16_t)i, 0), 7, (const void*)(uintptr_t)(0x5000 + i * 16), ENTRY_PURPLE);
      fr.finalize();
      for (int i = 0; i < 20; i++) carryNote(&fc, 4, fr.crc(), OM_PURPLE, i);
      struct Head { static bool t(void* c, int, int idx) { Registry* r = (Registry*)c; if (idx < 3) return false; r->at(idx).flags |= ENTRY_GOT; return true; } };
      int l = 0;
      CHECK(carryPass(&fc, fr, SHARE_ALL, 8, Head::t, &fr, &l) == 8 && l == 12);   // 0..2 fail, 3..10 done
    }
}

// A shared pool (SceneObjHolder: CoinHolder creates its 32 shared Coins) is created inside whichever placement row first asks for it. Which row that
// is changed across a same-process reload (final run 14 Nov: grass row first, then the crystal cage row), so the per-row keys and the registry CRC
// differed and the host's Try Again switched object sync off. The pool's actors are kept out of the keys; the row's own actors keep theirs.
struct SimRow { int zone, lid, own; bool asksPool; };
static uint32_t simPlacement(const SimRow* rows, int n, int firstAsker, int* syncedKeysOut) {
    // group layout: every row creates `own` actors; the first asking row (in creation order from firstAsker) also creates the 32-actor pool inside it
    static int actors[4096];
    static Registry reg;
    reg.clear();
    PoolRanges pool;
    pool.clear();
    int count = 0;
    bool poolMade = false;
    int synced = 0;
    for (int r0 = 0; r0 < n; r0++) {
        const SimRow& row = rows[(r0 + firstAsker) % n];
        int before = count;
        count += 1;                                        // the row's own actor (sub 0)
        if (row.asksPool && !poolMade) {                   // lazily created scene object, in the middle of the row's init
            int tok = pool.begin(count, true);
            count += 32;
            pool.end(tok, count);
            poolMade = true;
        }
        count += row.own;                                  // the row's own children
        for (int i = before; i < count; i++) {
            if (pool.contains(i)) continue;
            int sub = pool.subFor(before, i, 0);
            uint8_t fl = (sub == 0 && row.lid == 7) ? ENTRY_ENEMY : 0;
            if (fl) synced++;
            reg.add(K(row.zone, 0x55 + row.lid, row.lid, sub), 1000u + (uint32_t)sub, &actors[i], fl);
        }
    }
    reg.finalize();
    if (syncedKeysOut) *syncedKeysOut = synced;
    return reg.crc();
}
static void testPoolKeys() {
    const SimRow rows[3] = {{4, 5, 7, true}, {13, 24, 2, true}, {9, 7, 1, false}};
    int s1 = 0, s2 = 0;
    uint32_t a = simPlacement(rows, 3, 0, &s1);   // row 0 asks for the pool first
    uint32_t b = simPlacement(rows, 3, 1, &s2);   // after the reload row 1 does
    CHECK(a == b);
    CHECK(s1 == 1 && s2 == 1);                    // the killable enemy of row 3 is registered in both, with the same key
    // the sub numbering of the children after the pool is compact (no gap where the pool was)
    PoolRanges p;
    p.clear();
    int tok = p.begin(11, true);
    p.end(tok, 43);                               // pool = [11, 43)
    CHECK(p.contains(11) && p.contains(42) && !p.contains(10) && !p.contains(43));
    CHECK(p.subFor(10, 10, 0) == 0 && p.subFor(10, 44, 0) == 2 && p.subFor(10, 10, 1) == 1);
    // nested creation records the outermost range once; outside a placement nothing is recorded
    PoolRanges q;
    q.clear();
    int t1 = q.begin(5, true), t2 = q.begin(9, true);
    q.end(t2, 12);
    q.end(t1, 20);
    CHECK(q.contains(5) && q.contains(19) && !q.contains(20) && q.count() == 1);
    PoolRanges o;
    o.clear();
    int t3 = o.begin(3, false);
    o.end(t3, 40);
    CHECK(!o.contains(3) && o.count() == 0);
    // adjacent creations (a scene object that makes two pools back to back) merge into one range; a gap keeps two
    PoolRanges m;
    m.clear();
    int ta = m.begin(10, true); m.end(ta, 20);
    int tb = m.begin(20, true); m.end(tb, 25);
    CHECK(m.count() == 1 && m.contains(10) && m.contains(24) && !m.contains(25) && !m.overflowed());
    int tc = m.begin(30, true); m.end(tc, 31);
    CHECK(m.count() == 2);
    // overflow: more separate ranges than fit sets the flag (the glue logs it), clear() resets it, nothing crashes
    PoolRanges v;
    v.clear();
    for (int k = 0; k < PoolRanges::kMax; k++) { int t = v.begin(k * 10, true); v.end(t, k * 10 + 2); }
    CHECK(v.count() == PoolRanges::kMax && !v.overflowed());
    int tx = v.begin(1000, true); v.end(tx, 1004);
    CHECK(v.overflowed() && v.count() == PoolRanges::kMax && !v.contains(1001));
    v.clear();
    CHECK(!v.overflowed() && v.count() == 0);
    // own children BEFORE the pool: row [10, 20): own 10..12, pool 13..44, own 45..
    PoolRanges b4;
    b4.clear();
    int tp = b4.begin(13, true); b4.end(tp, 45);
    CHECK(b4.subFor(10, 12, 0) == 2 && b4.subFor(10, 45, 0) == 3 && b4.subFor(10, 46, 0) == 4);
    // a row whose own object is not a LiveActor (base 1) with a pool in it
    CHECK(b4.subFor(10, 12, 1) == 3 && b4.subFor(10, 45, 1) == 4);
    // a range that starts before the row (an earlier row's pool reaching into this window): only the overlap is taken off
    PoolRanges early;
    early.clear();
    int te = early.begin(5, true); early.end(te, 14);
    CHECK(early.subFor(10, 14, 0) == 0 && early.subFor(10, 16, 0) == 2 && early.contains(10) && !early.contains(14));
    // a row with no pool in its window numbers exactly as before the change
    PoolRanges none;
    none.clear();
    CHECK(none.subFor(7, 9, 1) == 3 && !none.contains(9));
}

int main() {
    testLazyMem();
    testPeerCrcReload();
    testHashes();
    testRegistry();
    testPolicy();
    testOscGuard();
    testMapCodec();
    testApplyMap();
    testMessages();
    testFuzz();
    testPending();
    testDropStats();
    testPendingReports();
    testEarlyQueue();
    testObjMap();
    testRegCrcFlags();
    testPackGating();
    testPeriodicAndSummary();
    testKindSharedAndBudget();
    testHostReloadCarry();
    testCarryPolicy();
    testCarryReview();
    testPoolKeys();
    printf("world_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
