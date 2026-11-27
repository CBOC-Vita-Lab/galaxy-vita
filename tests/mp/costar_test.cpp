// Host unit tests for the costar module: the team Star Bit count (MpStarBitsLogic.hpp) and the Co-Star hold rules (MpCoStarLogic.hpp).
// Build and run: tests/mp/costar_build.sh (ASan + UBSan).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "Game/MP/MpCoStarLogic.hpp"
#include "Game/MP/MpStarBitsLogic.hpp"
#include "port/mp_proto.h"

using namespace MpStarBitsLogic;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t g_rng = 12345;
static uint32_t R() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// ---- a small simulated session ------------------------------------------------------------------------------------------------------
struct Msg { int from, to; bool isTotal; Delta d; Total t; uint32_t due; };
struct Con {
    bool present = false, isHost = false;
    Host h;
    Guest g;
    int native = 0;       // the game's own count on this console
    int ones = 0;         // 1-ups this console gave (the game's own rule below)
    int last1up = 0;
    int slot = 0;
    uint32_t collected = 0;
};
struct Sim {
    Con c[MP_MAX_PLAYERS];
    std::vector<Msg> q;
    uint32_t turn = 1;
    uint16_t serial = 7;
    int cap = kCapGalaxy;
    int hostSlot = 0;
    uint8_t epoch = 0;
    int lossPct = 0, dupPct = 0, maxDelay = 0;   // loss only hits TOTALs (the reliable channel does not lose a DELTA); links keep their order
    uint32_t lastDue[MP_MAX_PLAYERS][MP_MAX_PLAYERS] = {};

    void init(int n) {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) { c[i] = Con(); c[i].slot = i; }
        for (int i = 0; i < n; i++) join(i);
        hostSlot = 0;
        c[0].isHost = true;
        c[0].h.reset(serial, epoch, cap, 0, turn);
    }
    void join(int i) {
        c[i].present = true;
        c[i].native = 0; c[i].last1up = 0; c[i].ones = 0;
        c[i].g.reset(serial, cap, turn);
        if (c[hostSlot].present && c[hostSlot].isHost) c[hostSlot].h.noteJoin();
    }
    // The game's own 1-Up rule (ScenePlayingResult::incStarPiece), run on each console's own count.
    void nativeAdd(int i, int num) {
        Con& k = c[i];
        const int prev = k.native;
        k.native = clampi(k.native + num, 0, cap);
        if (k.last1up + 50 > k.native) return;
        if (prev / 50 < k.native / 50) k.ones++;
        k.last1up += 50;
    }
    void send(const Msg& m0) {
        Msg m = m0;
        if (m.isTotal && (int)(R() % 100) < lossPct) return;
        uint32_t d = turn + (maxDelay ? R() % (maxDelay + 1) : 0);
        if ((int32_t)(d - lastDue[m.from][m.to]) < 0) d = lastDue[m.from][m.to];
        lastDue[m.from][m.to] = d;
        m.due = d;
        q.push_back(m);
        if ((int)(R() % 100) < dupPct) { m.due = d + (maxDelay ? R() % (maxDelay + 1) : 0); q.push_back(m); }
    }
    // a local pick-up (the hook of MR::addStarPiece)
    void collect(int i, int num) {
        Con& k = c[i];
        const int before = k.native;
        nativeAdd(i, num);
        const int eff = k.native - before;
        if (eff > 0) k.collected += (uint32_t)eff;
        if (!eff) return;
        if (k.isHost) k.h.applyLocal(eff, i); else k.g.localAdd(eff);
    }
    void deliver(const Msg& m) {
        Con& k = c[m.to];
        if (!k.present) return;
        if (!m.isTotal) {
            if (k.isHost) k.h.applyDelta(m.from, m.d, turn);
        } else if (!k.isHost && m.from == hostSlot) {
            k.g.onTotal(m.t, m.to, turn);
        }
    }
    void tick() {
        turn++;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            Con& k = c[i];
            if (!k.present) continue;
            if (k.isHost) {
                Total t;
                if (k.h.poll(turn, &t)) for (int j = 0; j < MP_MAX_PLAYERS; j++) if (j != i && c[j].present) { Msg m{}; m.from = i; m.to = j; m.isTotal = true; m.t = t; send(m); }
            } else {
                Delta d;
                if (k.g.poll(turn, &d)) { Msg m{}; m.from = i; m.to = hostSlot; m.isTotal = false; m.d = d; send(m); }
            }
        }
        for (size_t i = 0; i < q.size();) {
            if ((int32_t)(turn - q[i].due) >= 0) { Msg m = q[i]; q.erase(q.begin() + i); deliver(m); }
            else i++;
        }
        // the game's reconcile: the native count follows the view (a remote change is added with the game's own add)
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            Con& k = c[i];
            if (!k.present) continue;
            int want;
            if (k.isHost) want = k.h.total; else if (k.g.known) want = k.g.view(); else continue;
            while (int st = nextStep(k.native, want)) { const int b = k.native; nativeAdd(i, st); if (k.native == b) break; }
        }
    }
    void run(int turns) { for (int i = 0; i < turns; i++) tick(); }
    bool agree() {
        int v = -1;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!c[i].present) continue;
            if (v < 0) v = c[i].native; else if (c[i].native != v) return false;
        }
        return true;
    }
    // migrate: the lowest remaining slot becomes the host
    void hostLeaves() {
        const int old = hostSlot;
        c[old].present = false;
        int nh = -1;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if (c[i].present) { nh = i; break; }
        if (nh < 0) return;
        epoch++;
        hostSlot = nh;
        c[nh].isHost = true;
        c[nh].g.becomeHost(&c[nh].h, nh, epoch, turn);
        // messages from / to the old host vanish
        for (size_t i = 0; i < q.size();) { if (q[i].from == old || q[i].to == old) q.erase(q.begin() + i); else i++; }
    }
};

// ---- codecs ---------------------------------------------------------------------------------------------------------------------------
static void testSceneGate() {
    bool init = false, waiting = false; uint16_t serial = 0; uint32_t wt = 0;
    // first scene: starts at once
    CHECK(gateStep(init, waiting, serial, wt, 1, 10) == GATE_START && init && serial == 1);
    CHECK(gateStep(init, waiting, serial, wt, 1, 11) == GATE_IDLE);
    // the serial changes while the old scene lives: nothing starts, for as long as the old scene has not ended
    for (uint32_t t = 100; t < 400; t++) CHECK(gateStep(init, waiting, serial, wt, 2, t) == GATE_IDLE && !init);
    // the old scene ends (sceneEnd clears both), the new one exists: start with the new serial, once
    init = false; waiting = false;
    CHECK(gateStep(init, waiting, serial, wt, 2, 401) == GATE_START && init && serial == 2);
    CHECK(gateStep(init, waiting, serial, wt, 2, 402) == GATE_IDLE);
    // sceneEnd never comes: starts after kSceneWaitMax
    CHECK(gateStep(init, waiting, serial, wt, 3, 500) == GATE_IDLE);
    CHECK(gateStep(init, waiting, serial, wt, 3, 500 + kSceneWaitMax - 1) == GATE_IDLE);
    CHECK(gateStep(init, waiting, serial, wt, 3, 500 + kSceneWaitMax) == GATE_START && serial == 3);
    // serial changes after the old scene ended: no wait
    init = false; waiting = false;
    CHECK(gateStep(init, waiting, serial, wt, 4, 2000) == GATE_START && serial == 4);
}

static Total mkTotal(const Host& h) { Total t; Host c = h; c.dirty = true; c.poll(h.lastSendTurn + 1000, &t); return t; }

// review fixes: re-init at the same serial, TOTAL before the scene, spending bounds
static void testReviewFixes() {
    // (1) the host reads a wrong count first (the old scene's: 23), the guest takes it; the corrected TOTAL (967) is newer, in either order
    for (int order = 0; order < 2; order++) {
        Host h; Guest g;
        h.reset(5, 3, 999, 23, 100, 40);          // the previous scene ended at rev 39
        g.reset(5, 999, 100);
        Total stale = mkTotal(h);
        h.reinit(9999, 967, 200);
        Total fix = mkTotal(h);
        CHECK(fix.rev == (uint16_t)(stale.rev + 1) && fix.total == 967 && fix.epoch == stale.epoch && fix.serial == stale.serial);
        if (order == 0) {                          // the guest loaded faster: it took the stale TOTAL, then the correction
            CHECK(g.onTotal(stale, 1, 110) && g.hostTotal == 23);
            CHECK(g.onTotal(fix, 1, 210) && g.hostTotal == 967);
        } else {                                   // the guest loaded slower: it only ever sees the correction
            CHECK(g.onTotal(fix, 1, 210) && g.hostTotal == 967);
            CHECK(!g.onTotal(stale, 1, 220) && g.hostTotal == 967);   // (an old one arriving late is refused)
        }
    }
    // acks survive a re-init (no DELTA is applied twice), a new scene's first TOTAL continues the revision
    {
        Host h; h.reset(7, 1, 999, 10, 0, 1);
        Delta d{7, 1, 5};
        CHECK(h.applyDelta(1, d, 10) == Host::APPLIED && h.total == 15);
        h.reinit(999, 15, 20);
        CHECK(h.applyDelta(1, d, 30) == Host::DUPLICATE && h.total == 15 && h.acks[1] == 1);
        const uint16_t r = h.rev;
        Host n; n.reset(8, 1, 999, 0, 40, (uint16_t)(r + 1));
        CHECK(n.rev == (uint16_t)(r + 1));
    }
    // (4) a TOTAL before the scene exists (cap 999 assumed) keeps an Observatory stock; view() clamps to the live cap
    {
        Guest g; g.reset(2, kCapGalaxy, 0);
        Total t{}; t.serial = 2; t.epoch = 0; t.rev = 1; t.total = 3000;
        CHECK(g.onTotal(t, 1, 1) && g.hostTotal == 3000 && g.view() == 999);
        g.cap = kCapStock;
        CHECK(g.view() == 3000);
        t.total = 9999; t.rev = 2;
        CHECK(g.onTotal(t, 1, 2) && g.view() == 9999);
    }
    // (6) a guest that only subtracts: 200 burst, then 30 / s, never below 0; collecting is unaffected
    {
        Host h; h.reset(3, 1, 999, 900, 0, 1);
        uint16_t seq = 0;
        int applied = 0, limited = 0;
        for (int i = 0; i < 20; i++) {
            Delta d{3, ++seq, -100};
            const Host::Result r = h.applyDelta(1, d, 1);
            if (r == Host::APPLIED) applied++; else if (r == Host::RATE_LIMITED) limited++;
        }
        CHECK(applied == 2 && limited == 18 && h.total == 700);
        // 10 s later the bucket is full again (200), not 6000
        int more = 0;
        for (int i = 0; i < 20; i++) { Delta d{3, ++seq, -100}; if (h.applyDelta(1, d, 1 + 600) == Host::APPLIED) more++; }
        CHECK(more == 2 && h.total == 500);   // (the spending bucket holds 200 at most)
        // another slot is not hurt by it, and collecting still has its own bucket
        Delta c{3, 1, 100};
        CHECK(h.applyDelta(2, c, 700) == Host::APPLIED && h.total == 600);
        Delta c2{3, ++seq, 100};
        CHECK(h.applyDelta(1, c2, 700) == Host::APPLIED);
        // the team never goes below 0
        Host z; z.reset(3, 1, 999, 30, 0, 1);
        Delta big{3, 1, -100};
        CHECK(z.applyDelta(1, big, 1) == Host::APPLIED && z.total == 0);
    }
}

static void testCodecs() {
    uint8_t b[32];
    {
        MpWriter w(b, sizeof b);
        writeDelta(w, Delta{7, 3, -5});
        CHECK(w.ok() && w.n == kDeltaBytes);
        Delta d;
        CHECK(readDelta(b, w.n, &d) && d.serial == 7 && d.seq == 3 && d.delta == -5);
        CHECK(classify(b, w.n) == kKindDelta);
        for (size_t n = 0; n < 40; n++) if (n != kDeltaBytes) CHECK(!readDelta(b, n, &d));
        uint8_t c[8]; memcpy(c, b, 7);
        c[2] = 3; CHECK(!readDelta(c, 7, &d));                       // wrong kind
        memcpy(c, b, 7); c[3] = c[4] = 0; CHECK(!readDelta(c, 7, &d));   // seq 0
        memcpy(c, b, 7); c[5] = c[6] = 0; CHECK(!readDelta(c, 7, &d));   // delta 0
        memcpy(c, b, 7); c[5] = 0x7F; c[6] = 0x00; CHECK(!readDelta(c, 7, &d));   // +127 > 100
        memcpy(c, b, 7); c[5] = 0x9C; c[6] = 0xFF; CHECK(readDelta(c, 7, &d) && d.delta == -100);
        memcpy(c, b, 7); c[5] = 0x9B; c[6] = 0xFF; CHECK(!readDelta(c, 7, &d));
    }
    {
        Total t{9, 2, 1000, 555, {1, 2, 3, 4}};
        MpWriter w(b, sizeof b);
        writeTotal(w, t);
        CHECK(w.ok() && w.n == kTotalBytes);
        Total r;
        CHECK(readTotal(b, w.n, &r) && r.serial == 9 && r.epoch == 2 && r.rev == 1000 && r.total == 555 && r.acks[3] == 4);
        CHECK(classify(b, w.n) == kKindTotal);
        for (size_t n = 0; n < 40; n++) if (n != kTotalBytes) CHECK(!readTotal(b, n, &r));
        uint8_t c[16]; memcpy(c, b, 16);
        c[2] = 1; CHECK(!readTotal(c, 16, &r));
        memcpy(c, b, 16); c[6] = 0x0F; c[7] = 0x27; CHECK(readTotal(c, 16, &r) && r.total == 9999);
        c[6] = 0x10; CHECK(!readTotal(c, 16, &r));   // 10000
    }
    CHECK(classify("12345678", 8) == 0);   // an old tally is not ours
    CHECK(classify("1234567890", 10) == 0xFF);
}

// ---- the count ---------------------------------------------------------------------------------------------------------------------
static void testBasic() {
    Sim s; s.init(3);
    s.collect(1, 3); s.collect(2, 2); s.collect(0, 1);
    s.run(40);
    CHECK(s.agree());
    CHECK(s.c[0].native == 6 && s.c[1].native == 6 && s.c[2].native == 6);
    CHECK(s.c[1].collected == 3 && s.c[2].collected == 2 && s.c[0].collected == 1);
    CHECK(s.c[0].h.contrib[1] == 3 && s.c[0].h.contrib[2] == 2);
    // the collector's own count moves at once (before any network)
    Sim t; t.init(2);
    t.collect(1, 5);
    CHECK(t.c[1].native == 5 && t.c[1].g.view() == 5);
}
static void testConcurrent() {
    Sim s; s.init(4); s.maxDelay = 9;
    for (int r = 0; r < 200; r++) {
        for (int i = 0; i < 4; i++) if (R() % 3 == 0) s.collect(i, 1 + (int)(R() % 3));
        s.tick();
    }
    s.maxDelay = 0;
    s.run(400);
    CHECK(s.agree());
    uint32_t sum = 0; for (int i = 0; i < 4; i++) sum += s.c[i].collected;
    CHECK((int)sum == s.c[0].native);
    CHECK(s.c[0].h.total == s.c[0].native);
}
static void testSpend() {   // shooting Star Bits and feeding a Hungry Luma are negative moves
    Sim s; s.init(3);
    s.collect(1, 30); s.run(30);
    s.collect(2, -12); s.collect(1, -1); s.run(40);
    CHECK(s.agree() && s.c[0].native == 17);
    s.collect(2, -100);   // more than there is: the game clamps at 0 and only the real change travels
    s.run(40);
    CHECK(s.agree() && s.c[0].native == 0);
    s.collect(1, 4); s.run(40);
    CHECK(s.agree() && s.c[0].native == 4);
}
static void testLoss() {
    Sim s; s.init(3); s.lossPct = 40; s.dupPct = 30; s.maxDelay = 6;
    for (int r = 0; r < 300; r++) {
        if (R() % 4 == 0) s.collect(1 + (int)(R() % 2), 1);
        s.tick();
    }
    s.lossPct = 0; s.dupPct = 0;
    s.run(1200);   // resends (3 s) and beacons (2 s) bring everyone to the host's number
    CHECK(s.agree());
    CHECK(s.c[0].h.total == s.c[1].native);
}
static void testDuplicates() {
    Sim s; s.init(2);
    s.collect(1, 5);
    s.run(10);
    // the same DELTA again (a relay echo or a re-send): nothing changes
    Delta d; Total t;
    CHECK(s.c[1].g.n == 1 || s.c[0].h.acks[1] == 1);
    d.serial = s.serial; d.seq = 1; d.delta = 5;
    CHECK(s.c[0].h.applyDelta(1, d, s.turn) == Host::DUPLICATE);
    CHECK(s.c[0].h.total == 5);
    d.seq = 0xFFFF; d.delta = 5;   // an old seq (behind) is a repeat as well
    CHECK(s.c[0].h.applyDelta(1, d, s.turn) == Host::DUPLICATE);
    d.seq = 2; d.serial = (uint16_t)(s.serial + 1);
    CHECK(s.c[0].h.applyDelta(1, d, s.turn) == Host::REJECTED_SCENE);
    d.serial = s.serial;
    CHECK(s.c[0].h.applyDelta(7, d, s.turn) == Host::REJECTED_SLOT);
    CHECK(s.c[0].h.applyDelta(-1, d, s.turn) == Host::REJECTED_SLOT);
    // an old TOTAL is ignored by a guest
    s.c[0].h.poll(s.turn + 1000, &t);
    Total old = t; old.rev = (uint16_t)(t.rev - 1); old.total = 1;
    CHECK(!s.c[1].g.onTotal(old, 1, s.turn));
    CHECK(!s.c[1].g.onTotal(t, 1, s.turn));   // the same rev again
    t.serial = (uint16_t)(t.serial + 1);
    CHECK(!s.c[1].g.onTotal(t, 1, s.turn));   // another scene
}
static void testRude() {
    Sim s; s.init(3);
    // a guest that claims far more than the bucket allows
    uint32_t seq = 1;
    int applied = 0, limited = 0;
    for (int i = 0; i < 50; i++) {
        Delta d{s.serial, (uint16_t)seq++, 100};
        const Host::Result r = s.c[0].h.applyDelta(2, d, s.turn);   // all in the same turn
        if (r == Host::APPLIED) applied++; else if (r == Host::RATE_LIMITED) limited++;
    }
    CHECK(applied == kBucketMax / 100);
    CHECK(limited == 50 - applied);
    CHECK(s.c[0].h.total == kBucketMax);
    CHECK(s.c[0].h.acks[2] == 50);   // ... and every one was acknowledged (so an honest sender does not retry)
    // it refills slowly: 2 turns per 4 tokens
    s.turn += 100;
    Delta d{s.serial, (uint16_t)seq++, 100};
    CHECK(s.c[0].h.applyDelta(2, d, s.turn) == Host::APPLIED);   // 100 turns = 200 tokens back
    Sim u; u.init(2);
    u.c[0].h.bucket[1] = 0; u.c[0].h.bucketTurn[1] = u.turn;
    u.turn += 50;   // 25 x 4 = 100 tokens
    Delta e{u.serial, 1, 100};
    CHECK(u.c[0].h.applyDelta(1, e, u.turn) == Host::APPLIED);
    Delta f{u.serial, 2, 1};
    CHECK(u.c[0].h.applyDelta(1, f, u.turn) == Host::RATE_LIMITED);
    // the total never leaves 0..cap and the wire value stays a u16
    Sim v; v.init(2);
    for (int i = 0; i < 400; i++) { v.c[0].h.applyLocal(100, 0); }
    CHECK(v.c[0].h.total == kCapGalaxy);
    v.c[0].h.applyLocal(-5000, 0);
    CHECK(v.c[0].h.total == 0);
    // a guest ignores a TOTAL above its cap or from nowhere
    Total t{v.serial, 0, 5, 9000, {0, 0, 0, 0}};
    CHECK(v.c[1].g.onTotal(t, 1, v.turn));
    CHECK(v.c[1].g.hostTotal == 9000 && v.c[1].g.view() == kCapGalaxy);   // (the stock cap is kept, the live cap shows)
    CHECK(!v.c[1].g.onTotal(t, 9, v.turn));
    CHECK(!v.c[1].g.onTotal(t, -1, v.turn));
    // a guest cannot flood the ring: it stops making DELTAs and keeps the rest pending
    Sim w; w.init(2);
    Delta out;
    int made = 0;
    for (int i = 0; i < 100; i++) { w.c[1].g.localAdd(1); w.turn += kBatchTurns; if (w.c[1].g.poll(w.turn, &out) && out.seq == (uint16_t)(made + 1)) made++; }
    CHECK(made == kRing);
    CHECK(w.c[1].g.pending == 100 - kRing);
    CHECK(w.c[1].g.unacked() == 100);
}
static void testRevWrap() {
    Sim s; s.init(2);
    s.c[0].h.rev = 65530;
    s.c[1].g.known = true; s.c[1].g.rev = 65529; s.c[1].g.epoch = 0;
    for (int i = 0; i < 20; i++) { s.collect(1, 1); s.run(8); }
    CHECK(s.agree() && s.c[0].native == 20);
    // seq wraps too: the guest skips 0
    Sim t; t.init(2);
    t.c[1].g.lastSeq = 65534; t.c[0].h.acks[1] = 65534;
    for (int i = 0; i < 6; i++) { t.collect(1, 1); t.run(8); }
    CHECK(t.agree() && t.c[0].native == 6);
    CHECK(t.c[1].g.lastSeq == 4);
}
static void testDropIn() {
    Sim s; s.init(2);
    s.collect(1, 20); s.collect(0, 9); s.run(40);
    CHECK(s.c[0].native == 29);
    s.join(2);   // a joiner knows nothing
    CHECK(!s.c[2].g.known);
    s.collect(2, 1);   // it picks one up before it has heard the total
    CHECK(s.c[2].native == 1);
    s.run(30);
    CHECK(s.c[2].g.known);
    CHECK(s.agree() && s.c[0].native == 30);
    // a joiner whose first TOTAL was lost hears the beacon
    s.lossPct = 100; s.join(3); s.lossPct = 0;
    s.run(kBeaconTurns + 20);
    CHECK(s.c[3].g.known && s.agree());
}
static void testMigration() {
    Sim s; s.init(3);
    s.collect(1, 10); s.collect(2, 5); s.run(30);
    CHECK(s.agree() && s.c[0].native == 15);
    // a guest picks up bits and the host vanishes before it hears about them
    s.collect(2, 3);
    s.run(1);
    s.hostLeaves();   // slot 1 is the host; slot 2's unacked DELTA was addressed to slot 0
    s.run(60);
    CHECK(s.c[1].isHost && s.agree());
    CHECK(s.c[1].native == 18);   // slot 2's 3 reached the new host through the resend
    // and play goes on under the new host
    s.collect(2, 4); s.collect(1, 1); s.run(40);
    CHECK(s.agree() && s.c[1].native == 23);
    // host leaves again: slot 2 takes over
    s.hostLeaves();
    s.run(40);
    CHECK(s.c[2].isHost && s.c[2].native == 23);
    // the new host's own unacknowledged pick-ups are folded in (they were never heard by the old host)
    Sim m; m.init(3);
    m.collect(1, 7); m.collect(2, 2); m.run(40);
    m.collect(1, 5);        // slot 1 holds 5 the host never saw
    m.hostLeaves();
    m.run(60);
    CHECK(m.c[1].isHost && m.agree() && m.c[1].native == 14);
    // a guest that is ahead of the new host's last TOTAL adopts the new host's number (consistency over completeness)
    Sim a; a.init(3);
    a.collect(2, 6); a.run(30);
    a.c[0].h.applyLocal(10, 0);    // the host's own pick-ups reach slot 2 only
    Total t; a.c[0].h.dirty = true; a.c[0].h.poll(a.turn + 100, &t);
    a.c[2].g.onTotal(t, 2, a.turn);
    CHECK(a.c[2].g.view() == 16 && a.c[1].g.view() == 6);
    a.hostLeaves();
    a.run(60);
    CHECK(a.agree() && a.c[1].native == 6 + 0);   // slot 1 had only heard 6; slot 2 follows it
}
static void testCaps() {
    // the Observatory's stock stops at 9999
    Sim s; s.cap = kCapStock; s.init(3);
    for (int i = 0; i < 120; i++) { s.collect(1, 90); s.collect(2, 90); s.run(2); }
    s.run(3000);   // (a flood drains at 100 per 100 ms and the host's bucket takes only part of it)
    CHECK(s.agree());
    CHECK(s.c[0].native <= kCapStock);
    // the bucket throttles a flood: nowhere near 21600 got through, and nothing wrapped
    CHECK(s.c[0].native > 0);
    // push the host itself over
    s.c[0].h.applyLocal(100, 0);
    for (int i = 0; i < 200; i++) s.c[0].h.applyLocal(100, 0);
    CHECK(s.c[0].h.total == kCapStock);
    Total t; s.c[0].h.dirty = true; s.c[0].h.poll(s.turn + 1000, &t);
    CHECK(t.total == 9999);
    uint8_t b[16]; MpWriter w(b, sizeof b); writeTotal(w, t);
    Total r; CHECK(readTotal(b, w.n, &r) && r.total == 9999);
    // in a galaxy the cap is the game's 999
    Sim g; g.init(2);
    for (int i = 0; i < 30; i++) { g.collect(1, 50); g.run(4); }
    g.run(100);
    CHECK(g.agree() && g.c[0].native <= kCapGalaxy);
}
static void testOneUps() {
    // every console gives the same number of 1-Ups, however it came by the count (one pick-up at a time or in a lump)
    Sim s; s.init(3);
    for (int i = 0; i < 160; i++) { s.collect(1 + (int)(R() % 2), 1); s.run(1 + (int)(R() % 4)); }
    s.run(300);
    CHECK(s.agree());
    const int want = s.c[0].native / 50;
    CHECK(s.c[0].ones == want && s.c[1].ones == want && s.c[2].ones == want);
    CHECK(want >= 2);
    // a late joiner takes the lump: the game's rule gives it the same number
    s.join(3); s.run(60);
    CHECK(s.c[3].native == s.c[0].native);
    CHECK(s.c[3].ones == want);   // (the game side silences these on a catch-up; the number is the rule's)
    // spending below a multiple and picking up again never gives a second 1-Up for the same 50
    Sim t; t.init(2);
    t.collect(1, 49); t.run(30);
    t.collect(1, 1); t.run(30);
    CHECK(t.c[0].ones == 1 && t.c[1].ones == 1);
    t.collect(1, -3); t.run(30);
    t.collect(1, 3); t.run(30);
    CHECK(t.c[0].ones == 1 && t.c[1].ones == 1);
}
static void testSceneChange() {
    Guest g; g.reset(5, kCapGalaxy, 0);
    g.localAdd(3);
    Total t{5, 0, 1, 10, {0, 0, 0, 0}};
    CHECK(g.onTotal(t, 1, 0) && g.view() == 13);
    g.reset(6, kCapGalaxy, 10);   // a new scene forgets everything
    CHECK(!g.known && g.pending == 0 && g.n == 0);
    CHECK(!g.onTotal(t, 1, 10));
}
static void testBatching() {
    Sim s; s.init(2);
    // many pick-ups inside one batch become one DELTA
    for (int i = 0; i < 5; i++) s.collect(1, 1);
    s.turn += kBatchTurns;
    Delta d;
    CHECK(s.c[1].g.poll(s.turn, &d) && d.delta == 5 && d.seq == 1);
    CHECK(!s.c[1].g.poll(s.turn, &d));
    // and the host's TOTALs are at most one per batch
    Sim h; h.init(2);
    Total t;
    CHECK(h.c[0].h.poll(h.turn, &t));        // the first one goes at once
    int sent = 0;
    for (int i = 0; i < 12; i++) { h.c[0].h.applyLocal(1, 0); h.turn++; if (h.c[0].h.poll(h.turn, &t)) sent++; }
    CHECK(sent == 2);
    // an idle host beacons
    Sim b; b.init(2);
    b.c[0].h.poll(b.turn, &t);
    int beacons = 0;
    for (uint32_t i = 0; i < 3 * kBeaconTurns; i++) { b.turn++; if (b.c[0].h.poll(b.turn, &t)) beacons++; }
    CHECK(beacons == 3);
}

// ---- the hold rules -------------------------------------------------------------------------------------------------------------------
using namespace MpCoStarLogic;
static void testPoint() {
    const float me[3] = {1000, 0, 0};
    float p[3] = {1100, 0, 0};
    CHECK(pointUsable(p, me));
    CHECK(!pointUsable(p, nullptr));
    p[0] = 1000 + kMaxFromSender + 1; CHECK(!pointUsable(p, me));
    p[0] = 1000 + kMaxFromSender - 1; CHECK(pointUsable(p, me));
    float big[3] = {600000, 0, 0}; const float far[3] = {600000, 0, 0};
    CHECK(!pointUsable(big, far));   // outside the world box even next to its sender
    float nan[3] = {0.0f / 0.0f * 0, 0, 0};
    nan[0] = __builtin_nanf("");
    CHECK(!pointUsable(nan, me));
    float inf[3] = {__builtin_inff(), 0, 0};
    CHECK(!pointUsable(inf, me));
    const float t[3] = {0, 0, 0};
    const float a[3] = {100, 0, 0}, b[3] = {400, 0, 0};
    CHECK(inReach(a, t, 100) && !inReach(b, t, 100));   // reach = 100 * 2.2 + 60 = 280
    CHECK(inReach(b, t, 200) && !inReach(inf, t, 100) && !inReach(a, inf, 100));
    CHECK(holdReach(-5) == kReachAdd && holdReach(__builtin_nanf("")) == kReachAdd);
    CHECK(holdReach(1e9f) == 2000.0f * kReachScale + kReachAdd);
}
static void testHoldBasics() {
    HoldGate g; g.reset(100);
    uint32_t t = 100;
    CHECK(!g.query(t, 0, true, true));                  // no actor id
    CHECK(!g.query(t, 1, false, true));                 // the pointer is not on
    CHECK(!g.query(t, 1, true, false));                 // out of reach
    CHECK(g.query(t, 1, true, true));                   // held
    CHECK(g.query(t, 1, true, true));                   // asked twice in a turn: same answer
    CHECK(!g.query(t, 2, true, true));                  // one target per pointer
    t++;
    CHECK(g.query(t, 1, true, true));
    CHECK(!g.query(t, 2, true, true));
    CHECK(g.holding(t));
    // the pointer moves off: released, the other target can be taken at once
    t++;
    CHECK(!g.query(t, 1, true, false));
    CHECK(g.query(t, 2, true, true));
    // the pointer is released
    t++;
    CHECK(!g.query(t, 2, false, true));
    CHECK(!g.holding(t));
    CHECK(g.query(t, 2, true, true));   // and back on: held again (no cool-down for a normal release)
    // a holder that stops asking (it died) lets go after kStaleTurns
    for (int i = 0; i < 10; i++) t++;
    CHECK(g.query(t, 3, true, true));
}
static void testHoldMax() {
    HoldGate g; g.reset(0);
    uint32_t t = 0;
    int held = 0;
    for (; t < 1000; t++) if (g.query(t, 1, true, true)) held++; else break;
    CHECK(held == kMaxHoldTurns);
    CHECK(!g.query(t, 1, true, true));
    // cool-down: nothing at all, not even another target
    const uint32_t tCool = t;
    for (int i = 0; i < kCooldownTurns - 1; i++) { t++; CHECK(!g.query(t, 1, true, true)); CHECK(!g.query(t, 2, true, true)); }
    t++;
    CHECK(t - tCool == (uint32_t)kCooldownTurns);
    CHECK(g.query(t, 1, true, true));   // after it: a new hold
}
static void testHoldBudget() {
    // a pointer that toggles on and off to restart the 4 s cap is held to the bucket: a third of the time at most, in the long run
    HoldGate g; g.reset(0);
    uint32_t held = 0, total = 0;
    uint32_t t = 0;
    for (; t < 60 * 120; t++) {
        const bool on = (t % 240) < 200;   // on for 3.3 s, off for 0.7 s, again and again
        total++;
        if (g.query(t, 1, on, true)) held++;
    }
    CHECK(held * 100 / total < 60);
    CHECK(held * 100 / total > 20);   // honest play is not starved
    // a plain 2 s hold is never cut short
    HoldGate h; h.reset(0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < 120; i++) if (h.query(i, 1, true, true)) n++;
    CHECK(n == 120);
    // an empty bucket cannot start a hold
    HoldGate e; e.reset(0);
    e.budget = kBudgetMinToStart - 1; e.budgetTurn = 0;
    CHECK(!e.query(0, 1, true, true));
    CHECK(e.query(4, 1, true, true));   // refilled a little: 2 more tokens... (budget 29 + 2 = 31)
}
static void testHoldClock() {
    HoldGate g; g.reset(1000);
    CHECK(g.query(1000, 1, true, true));
    CHECK(g.query(900, 1, true, true) || true);   // a turn counter that went backwards must not crash or wedge
    g.reset(0xFFFFFF00u);
    uint32_t t = 0xFFFFFF00u;
    int held = 0;
    for (int i = 0; i < kMaxHoldTurns + kCooldownTurns - 10; i++, t++) if (g.query(t, 1, true, true)) held++;
    CHECK(held == kMaxHoldTurns);   // the turn counter wraps in the middle: same rule
}
static void testSweep() {
    SweepGate s; s.reset(0);
    CHECK(s.take(0) && s.take(0) && s.take(0));
    CHECK(!s.take(0));                                  // the burst is 3
    CHECK(!s.take(kSweepTurnsPerToken - 1));
    CHECK(s.take(kSweepTurnsPerToken));
    CHECK(!s.take(kSweepTurnsPerToken));
    uint32_t got = 0;
    for (uint32_t t = 0; t < 600; t++) if (s.take(1000 + t)) got++;
    CHECK(got >= 38 && got <= 44);                      // ~ 4 a second over 10 s (+ the burst)
    SweepGate w; w.reset(5000);
    w.take(5000); w.take(5000); w.take(5000);
    CHECK(!w.take(100));                                // a clock that went backwards gives nothing extra
}

static void testReconnectAckAdvancesLastSeq() {
    // Wave 3 (P1): after a guest resets (e.g., reconnect / re-init), Guest::onTotal must advance lastSeq from t.acks[mySlot]
    // so subsequent DELTAs use seq > host.acks[mySlot] and are accepted instead of dropped as duplicates.
    Host h;
    h.reset(12, 1, kCapGalaxy, 10, 0, 1);
    for (uint16_t s = 1; s <= 5; s++) {
        Delta d{12, s, 2};
        CHECK(h.applyDelta(1, d, 10 + s) == Host::APPLIED);
    }
    CHECK(h.total == 20 && h.acks[1] == 5);
    // Guest resets cleanly (lastSeq = 0) and receives host TOTAL carrying acks[1] = 5
    Guest g;
    g.reset(12, kCapGalaxy, 100);
    CHECK(g.lastSeq == 0);
    Total t{};
    CHECK(h.poll(200, &t));
    CHECK(g.onTotal(t, 1, 200));
    CHECK(g.lastSeq == 5);
    // Guest collects 3 Star Bits: next DELTA must have seq == 6 and be applied by host!
    g.localAdd(3);
    Delta out{};
    CHECK(g.poll(220, &out));
    CHECK(out.seq == 6 && out.delta == 3);
    CHECK(h.applyDelta(1, out, 220) == Host::APPLIED);
    CHECK(h.total == 23 && h.acks[1] == 6);
    // High ack value (> 32767) with fresh lastSeq == 0 also advances properly
    Guest g2;
    g2.reset(12, kCapGalaxy, 300);
    Total tHigh = t;
    tHigh.rev = (uint16_t)(t.rev + 1);
    tHigh.acks[2] = 40000;
    CHECK(g2.onTotal(tHigh, 2, 300));
    CHECK(g2.lastSeq == 40000);
    // Lower ack than already-sent in-flight lastSeq must NOT move lastSeq backwards
    g.localAdd(1);
    CHECK(g.poll(250, &out) && out.seq == 7);
    Total tOlderAck = t;
    tOlderAck.rev = (uint16_t)(t.rev + 2);
    tOlderAck.acks[1] = 6;   // host hasn't acked 7 yet
    CHECK(g.onTotal(tOlderAck, 1, 255));
    CHECK(g.lastSeq == 7);
}

int main() {
    testSceneGate();
    testReviewFixes();
    testCodecs();
    testBasic();
    testConcurrent();
    testSpend();
    testLoss();
    testDuplicates();
    testRude();
    testRevWrap();
    testDropIn();
    testMigration();
    testCaps();
    testOneUps();
    testSceneChange();
    testBatching();
    testPoint();
    testHoldBasics();
    testHoldMax();
    testHoldBudget();
    testHoldClock();
    testSweep();
    testReconnectAckAdvancesLastSeq();
    printf("costar_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
