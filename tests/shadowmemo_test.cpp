// shadowmemo (6 Nov): host proof of port/shadow_memo.h, the rule behind GXV_SHADOWMEMO (decomp/src/Game/LiveActor/ShadowController.cpp).
//  1. Classify against the float tests it stands for (CollisionCategorizedKeeper::checkStrikeLine's zone and part culling:
//     isSphereOverlappingWithBox, then MR::checkHitSegmentSphere, written out below as the game has them): kOut must mean the
//     tests fail and kIn that they pass, on random, near-boundary, far, tiny and degenerate cases.
//  2. The whole rule on random worlds that change every turn (parts moved, turned in place, switched on and off, rescaled; zone
//     spheres moved; parts added, removed and reordered): whenever a memo is kept, what the cast depends on (the ordered list of
//     parts that pass the culling, with their full state) must be what it was when the memo was made.
// Run by tools/test_shadowmemo.sh. Prints PASS or FAIL.
#include "port/shadow_memo.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace port::shadowmemo;

namespace {
struct V {
    float x, y, z;
};
V Sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Sq(V a) { return Dot(a, a); }

// CollisionCategorizedKeeper::isSphereOverlappingWithBox (float, as in the game).
bool BoxTest(V mn, V mx, V c, float r) {
    if (c.x < mn.x - r || mx.x + r < c.x) return false;
    if (c.y < mn.y - r || mx.y + r < c.y) return false;
    if (c.z < mn.z - r || mx.z + r < c.z) return false;
    return true;
}
// MR::checkHitSegmentSphere's yes/no (float, as in the game: MathUtil.cpp).
bool SegSphere(V c, V a, V b, float r) {
    V pos = Sub(c, a);
    V seg = Sub(b, a);
    float dot = Dot(pos, seg);
    float r2 = r * r;
    if (dot < 0.0f) return Sq(Sub(a, c)) < r2;
    if (Sq(seg) < dot) return Sq(Sub(b, c)) < r2;
    float k = dot / Sq(seg);
    V off = {seg.x * k - pos.x, seg.y * k - pos.y, seg.z * k - pos.z};
    return Sq(off) <= r2;
}
// checkStrikeLine's culling of one sphere for a cast (start, dir).
bool Culled(V start, V dir, V c, float r) {
    V mn = start, mx = start;
    if (dir.x < 0) mn.x += dir.x; else mx.x += dir.x;
    if (dir.y < 0) mn.y += dir.y; else mx.y += dir.y;
    if (dir.z < 0) mn.z += dir.z; else mx.z += dir.z;
    V end = {start.x + dir.x, start.y + dir.y, start.z + dir.z};
    return !(BoxTest(mn, mx, c, r) && SegSphere(c, start, end, r));
}

int gFail = 0;
bool gQuiet = false;  // (mutant runs: failures expected, not printed)
void Check(bool ok, const char* what) {
    if (!ok && gFail++ < 10 && !gQuiet) std::printf("FAIL: %s\n", what);
}

// ---- 1. Classify ----
void TestClassify() {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    long outs = 0, ins = 0, edges = 0;
    for (int it = 0; it < 4000000; ++it) {
        const int kind = it % 6;
        const float scale = kind == 0 ? 100.0f : kind == 1 ? 5000.0f : kind == 2 ? 60000.0f : kind == 3 ? 3.0f : 1000.0f;
        V start = {u(rng) * scale, u(rng) * scale, u(rng) * scale};
        V dir = {u(rng) * scale * 0.3f, u(rng) * scale * 0.3f, u(rng) * scale * 0.3f};
        if (kind == 4) dir = {0, -u(rng) * 2000.0f - 1.0f, 0};       // a shadow drop
        if (kind == 5 && (it & 64)) dir = {0, 0, 0};                    // degenerate
        if (kind == 5 && (it & 128)) dir = {1e-20f, 0, 0};              // underflowing
        float r = std::fabs(u(rng)) * scale * 0.5f;
        if (it & 1) r = std::fabs(u(rng)) * 3.0f;
        V c;
        if (it & 2) {
            // near the boundary: a point on the segment pushed out by about r
            const float t = (u(rng) + 1.0f) * 0.6f - 0.1f;
            V p = {start.x + dir.x * t, start.y + dir.y * t, start.z + dir.z * t};
            V n = {u(rng), u(rng), u(rng)};
            const float nl = std::sqrt(Sq(n)) + 1e-6f;
            const float k = r * (1.0f + u(rng) * 0.01f) / nl;
            c = {p.x + n.x * k, p.y + n.y * k, p.z + n.z * k};
        } else {
            c = {u(rng) * scale, u(rng) * scale, u(rng) * scale};
        }
        Seg s;
        MakeSeg(&s, &start.x, &dir.x);
        Sphere sp;
        MakeSphere(&sp, c.x, c.y, c.z, r);
        const int cls = Classify(s, sp);
        const bool hit = !Culled(start, dir, c, r);
        if (cls == kOut) {
            ++outs;
            Check(!hit, "Classify kOut but the float tests pass");
        } else if (cls == kIn) {
            ++ins;
            Check(hit, "Classify kIn but the float tests fail");
        } else {
            ++edges;
        }
    }
    std::printf("classify: out %ld, in %ld, edge %ld\n", outs, ins, edges);
    Check(outs > 100000 && ins > 100000, "classify: too few sure answers");
    // non-finite and negative radius are never sure
    Seg s;
    float a[3] = {0, 0, 0}, d[3] = {0, -100, 0};
    MakeSeg(&s, a, d);
    Sphere sp;
    MakeSphere(&sp, NAN, 0, 0, 1);
    Check(Classify(s, sp) == kEdge, "NaN centre");
    MakeSphere(&sp, 0, -50, 0, -1);
    Check(Classify(s, sp) == kEdge, "negative radius");
}

// ---- 2. The rule on changing worlds ----
struct Part {
    PartState st;
    int id;
};
struct Zone {
    std::vector<int> parts;  // indices into gParts
    float c[3], r;
};
std::vector<Part> gParts;
std::vector<Zone> gZones;

V Trans(const PartState& p) { return {p.base[3], p.base[7], p.base[11]}; }

// What the cast depends on: the parts it would test, in order, with their state.
std::vector<PartState> Contributing(V start, V dir) {
    std::vector<PartState> out;
    V end = {start.x + dir.x, start.y + dir.y, start.z + dir.z};
    V mn = start, mx = start;
    if (dir.x < 0) mn.x += dir.x; else mx.x += dir.x;
    if (dir.y < 0) mn.y += dir.y; else mx.y += dir.y;
    if (dir.z < 0) mn.z += dir.z; else mx.z += dir.z;
    for (size_t zi = 0; zi < gZones.size(); ++zi) {
        const Zone& z = gZones[zi];
        if (zi != 0) {
            V c = {z.c[0], z.c[1], z.c[2]};
            if (!BoxTest(mn, mx, c, z.r) || !SegSphere(c, start, end, z.r)) continue;
        }
        for (int pi : z.parts) {
            const PartState& p = gParts[pi].st;
            if (!p.on) continue;
            if (!BoxTest(mn, mx, Trans(p), p.radius) || !SegSphere(Trans(p), start, end, p.radius)) continue;
            out.push_back(p);
        }
    }
    return out;
}
bool SameList(const std::vector<PartState>& a, const std::vector<PartState>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(PartState)) != 0) return false;
    return true;
}

// Mutants (must be caught): 1 = the snapshot misses _CC and the radius; 2 = part entries judged by the new state only.
int gMutant = 0;
bool MutantValid(const Log<1024>& log, const Seg& s, uint32_t memoSeq, uint32_t memoEpoch) {
    if (log.epoch[0] != memoEpoch || log.seq - memoSeq > 1024) return false;
    for (uint32_t i = memoSeq; i != log.seq; ++i) {
        const Change& c = log.ring[i % 1024];
        if (c.zone) {
            const int a = Classify(s, c.was), b = Classify(s, c.now);
            if (!((a == kOut && b == kOut) || (a == kIn && b == kIn))) return false;
        } else if (c.newOn && Classify(s, c.now) != kOut) {
            return false;
        }
    }
    return true;
}

void TestRule(unsigned seed, int nParts, int nZones, int moversPerTurn, float spread) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    gParts.clear();
    gZones.clear();
    static char ids[100000];
    auto newPart = [&](int id) {
        Part p;
        std::memset(&p, 0, sizeof p);
        p.id = id;
        p.st.parts = &ids[id];
        for (int i = 0; i < 12; ++i) p.st.base[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        p.st.base[3] = u(rng) * spread;
        p.st.base[7] = u(rng) * spread * 0.3f;
        p.st.base[11] = u(rng) * spread;
        for (int i = 0; i < 12; ++i) p.st.inv[i] = p.st.base[i];
        p.st.radius = 50.0f + std::fabs(u(rng)) * 800.0f;
        p.st.on = (rng() % 8) != 0;
        p.st.server = &ids[id];
        p.st.sensor = &ids[id];
        return p;
    };
    int nextId = 0;
    for (int i = 0; i < nParts; ++i) gParts.push_back(newPart(nextId++));
    gZones.resize(nZones);
    for (int i = 0; i < nParts; ++i) gZones[rng() % nZones].parts.push_back(i);
    auto zoneSphere = [&](Zone& z) {
        float c[3] = {0, 0, 0};
        float r = 0;
        for (int pi : z.parts) {
            V t = Trans(gParts[pi].st);
            c[0] += t.x / z.parts.size();
            c[1] += t.y / z.parts.size();
            c[2] += t.z / z.parts.size();
        }
        for (int pi : z.parts) {
            V t = Trans(gParts[pi].st);
            V d = {t.x - c[0], t.y - c[1], t.z - c[2]};
            float o = std::sqrt(Sq(d)) + gParts[pi].st.radius;
            if (o > r) r = o;
        }
        z.c[0] = c[0];
        z.c[1] = c[1];
        z.c[2] = c[2];
        z.r = r;
    };
    for (auto& z : gZones) zoneSphere(z);

    // shadows: start points and drops (some fixed, some moving)
    const int nShadows = 300;
    struct Shadow {
        V start, dir;
        bool moves;
        bool valid;
        uint32_t seq, epoch;
        std::vector<PartState> made;
    };
    std::vector<Shadow> sh(nShadows);
    for (auto& s : sh) {
        s.start = {u(rng) * spread, u(rng) * spread * 0.3f + 500.0f, u(rng) * spread};
        s.dir = {u(rng) * 0.1f, -(300.0f + std::fabs(u(rng)) * 3000.0f), u(rng) * 0.1f};
        s.moves = (rng() % 5) == 0;
        s.valid = false;
    }

    static Log<1024> log;
    log = Log<1024>();
    Snapshot<std::vector<SnapEntry>> snap;
    long kept = 0, cast = 0;
    for (int turn = 0; turn < 400; ++turn) {
        // change the world
        for (int m = 0; m < moversPerTurn; ++m) {
            Part& p = gParts[rng() % gParts.size()];
            switch (rng() % 6) {
            case 0: p.st.base[3] += u(rng) * 30.0f; p.st.base[11] += u(rng) * 30.0f; break;  // moved
            case 1: p.st.base[0] = std::cos(u(rng)); p.st.inv[0] = p.st.base[0]; break;      // turned in place
            case 2: p.st.on = !p.st.on; break;                                                // switched
            case 3: p.st.radius *= 1.0f + u(rng) * 0.05f; break;                              // rescaled
            case 4: p.st.base[7] += u(rng) * 400.0f; break;                                   // big jump
            case 5: p.st.inv[5] += 0.001f; break;                                             // inverse only
            }
        }
        if (turn % 7 == 0) zoneSphere(gZones[1 + rng() % (gZones.size() - 1)]);
        if (turn % 9 == 0) {
            Zone& z = gZones[1 + rng() % (gZones.size() - 1)];
            z.c[0] += u(rng) * 300.0f;
            z.r *= 1.0f + u(rng) * 0.3f;
        }
        if (turn % 53 == 0) {  // a part removed (swap with last, as eraseParts) and one added
            Zone& z = gZones[rng() % gZones.size()];
            if (!z.parts.empty()) {
                size_t k = rng() % z.parts.size();
                z.parts[k] = z.parts.back();
                z.parts.pop_back();
            }
            gParts.push_back(newPart(nextId++));
            gZones[rng() % gZones.size()].parts.push_back(static_cast<int>(gParts.size() - 1));
        }
        // the snapshot (as portShadowMemoSnap)
        snap.Begin(0);
        for (size_t zi = 0; zi < gZones.size(); ++zi) {
            ZoneState zs;
            std::memset(&zs, 0, sizeof zs);
            zs.zone = &gZones[zi];
            zs.count = static_cast<uint32_t>(gZones[zi].parts.size());
            if (zi != 0) {
                std::memcpy(zs.c, gZones[zi].c, sizeof zs.c);
                zs.r = gZones[zi].r;
            }
            snap.Zone(log, zs);
            for (int pi : gZones[zi].parts) {
                PartState st = gParts[pi].st;
                if (gMutant == 1) {
                    st.on = 1;
                    st.radius = 1.0f;
                }
                snap.Part(log, st);
            }
        }
        snap.End(log);
        // the shadows
        for (auto& s : sh) {
            if (s.moves && (rng() % 2)) s.start.x += u(rng) * 10.0f;
            bool reuse = false;
            if (s.valid) {
                Seg seg;
                MakeSeg(&seg, &s.start.x, &s.dir.x);
                uint32_t scanned = 0;
                reuse = gMutant == 2 ? MutantValid(log, seg, s.seq, s.epoch) : log.StillValid(0, seg, s.seq, s.epoch, &scanned);
            }
            std::vector<PartState> now = Contributing(s.start, s.dir);
            if (reuse) {
                ++kept;
                Check(SameList(now, s.made), "a kept memo whose cast would differ");
            } else {
                ++cast;
                s.made = now;
            }
            s.seq = log.seq;
            s.epoch = log.epoch[0];
            s.valid = true;
            // (a moving shadow's key changes: the game drops its memo on the key compare)
            if (s.moves) s.valid = false;
        }
    }
    std::printf("rule seed %u (parts %d zones %d movers %d): kept %ld, cast %ld, log %u\n", seed, nParts, nZones, moversPerTurn, kept, cast,
                log.written);
    Check(kept > 0, "rule: nothing kept (test is vacuous)");
}
// ---- 3. shadowmemo2: changes written by hooks, a reused memo moves to now, box prefilter, scan cap ----
// A world like Bowser's Star Reactor: many parts moving every turn (platforms sliding, turning in place), parts switched on and
// off, zone spheres refitted, now and then a part added or removed. Every write goes through Write*() below, which reports it as
// the game's hooks do (old and new sphere, old and new _CC). Mutant 3: one kind of write not reported (must be caught).
Log<1024> gHookLog;
bool gHookOn = true;
void HookPart(const Part& before, const Part& after) {
    if (!gHookOn) return;
    if (std::memcmp(&before.st, &after.st, sizeof(PartState)) == 0) return;
    Change c;
    std::memset(&c, 0, sizeof c);
    c.slot = kAllSlots;
    c.oldOn = before.st.on != 0;
    c.newOn = after.st.on != 0;
    c.who = after.st.parts;
    MakeSphere(&c.was, before.st.base[3], before.st.base[7], before.st.base[11], before.st.radius);
    MakeSphere(&c.now, after.st.base[3], after.st.base[7], after.st.base[11], after.st.radius);
    gHookLog.Push(c);
}

void TestHooks(unsigned seed, int nParts, int nMoving, int nShadows, int turns, bool print) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    gParts.clear();
    gZones.clear();
    static char ids[200000];
    int nextId = 0;
    const float spread = 15000.0f;
    auto newPart = [&]() {
        Part p;
        std::memset(&p, 0, sizeof p);
        p.id = nextId++;
        p.st.parts = &ids[p.id];
        for (int i = 0; i < 12; ++i) p.st.base[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        p.st.base[3] = u(rng) * spread;
        p.st.base[7] = u(rng) * 2000.0f;
        p.st.base[11] = u(rng) * spread;
        std::memcpy(p.st.inv, p.st.base, sizeof p.st.inv);
        p.st.radius = 80.0f + std::fabs(u(rng)) * 600.0f;
        p.st.on = 1;
        p.st.server = &ids[p.id];
        p.st.sensor = &ids[p.id];
        return p;
    };
    for (int i = 0; i < nParts; ++i) gParts.push_back(newPart());
    gZones.resize(3);
    for (int i = 0; i < nParts; ++i) gZones[i % 3].parts.push_back(i);
    for (size_t z = 1; z < gZones.size(); ++z) {
        gZones[z].c[0] = gZones[z].c[1] = gZones[z].c[2] = 0;
        gZones[z].r = spread * 2.0f;
    }
    gHookLog = Log<1024>();
    struct Shadow {
        V start, dir;
        bool moves, valid;
        uint32_t seq, epoch;
        std::vector<PartState> made;
    };
    std::vector<Shadow> sh(nShadows);
    for (auto& s : sh) {
        // a shadow over a part (often a moving one) or over open ground
        const Part& host = gParts[rng() % (gParts.size() / 2)];
        s.start = (rng() % 3) ? V{host.st.base[3] + u(rng) * 200.0f, host.st.base[7] + 600.0f, host.st.base[11] + u(rng) * 200.0f}
                              : V{u(rng) * spread, 2500.0f, u(rng) * spread};
        s.dir = {0.0f, -(1500.0f + std::fabs(u(rng)) * 2000.0f), 0.0f};
        s.moves = (rng() % 6) == 0;
        s.valid = false;
    }
    Snapshot<std::vector<SnapEntry>> walk;  // (only to time what the first version cost)
    Log<1024> walkLog;
    long kept = 0, cast = 0, capMiss = 0;
    double lookNs = 0, walkNs = 0, castNs = 0;
    static BatchIndex<1024> index;
    uint32_t runSeq = gHookLog.seq;
    uint64_t scanned = 0, checks = 0;
    for (int turn = 0; turn < turns; ++turn) {
        // writes, each reported (except the mutant's kind)
        for (int m = 0; m < nMoving; ++m) {
            Part& p = gParts[m % gParts.size()];  // the first nMoving parts move every turn
            const Part before = p;
            const float ph = 0.05f * turn + m;
            p.st.base[3] += std::sin(ph) * 25.0f;
            p.st.base[11] += std::cos(ph) * 25.0f;
            if (m % 4 == 0) {  // turning in place too
                p.st.base[0] = std::cos(ph);
                p.st.inv[0] = p.st.base[0];
            }
            HookPart(before, p);
        }
        if (turn % 5 == 0) {  // a part switched off or on (validate/invalidate also reshape; this is the direct write)
            Part& p = gParts[rng() % gParts.size()];
            const Part before = p;
            p.st.on = !p.st.on;
            if (!(gMutant == 3)) HookPart(before, p);
        }
        if (turn % 11 == 0) {  // a part rescaled
            Part& p = gParts[rng() % gParts.size()];
            const Part before = p;
            p.st.radius *= 1.0f + u(rng) * 0.2f;
            HookPart(before, p);
        }
        if (turn % 13 == 0) {  // a zone sphere refitted
            Zone& z = gZones[1 + rng() % 2];
            Change c;
            std::memset(&c, 0, sizeof c);
            c.slot = kAllSlots;
            c.zone = 1;
            MakeSphere(&c.was, z.c[0], z.c[1], z.c[2], z.r);
            z.c[0] += u(rng) * 2000.0f;
            z.r *= 1.0f + u(rng) * 0.4f;
            MakeSphere(&c.now, z.c[0], z.c[1], z.c[2], z.r);
            gHookLog.Push(c);
        }
        if (turn % 97 == 0) {  // a part added (a list change: the epoch)
            gParts.push_back(newPart());
            gZones[rng() % 3].parts.push_back(static_cast<int>(gParts.size() - 1));
            ++gHookLog.epoch[0];
        }
        // what the first version did every turn: walk every part
        {
            const auto t0 = std::chrono::steady_clock::now();
            walk.Begin(0);
            for (size_t zi = 0; zi < gZones.size(); ++zi) {
                ZoneState zs;
                std::memset(&zs, 0, sizeof zs);
                zs.zone = &gZones[zi];
                zs.count = static_cast<uint32_t>(gZones[zi].parts.size());
                walk.Zone(walkLog, zs);
                for (int pi : gZones[zi].parts) walk.Part(walkLog, gParts[pi].st);
            }
            walk.End(walkLog);
            walkNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        }
        // the shadows, as ShadowController::updateProjection does (the index first, as the holder's run does)
        {
            const auto t0 = std::chrono::steady_clock::now();
            index.Build(gHookLog, runSeq);
            runSeq = gHookLog.seq;
            lookNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        }
        for (auto& s : sh) {
            if (s.moves) {
                s.start.x += u(rng) * 5.0f;
                s.valid = false;  // (its key changed)
            }
            auto t0 = std::chrono::steady_clock::now();
            bool reuse = false, asked = false;
            if (s.valid && s.epoch == gHookLog.epoch[0]) {
                if (s.seq != index.from && gHookLog.seq - s.seq > 256) {
                    ++capMiss;
                } else {
                    Seg seg;
                    MakeSeg(&seg, &s.start.x, &s.dir.x);
                    uint32_t n = 0;
                    reuse = gHookLog.StillValid(0, seg, s.seq, s.epoch, &n, 256, &index);
                    asked = true;
                    scanned += n;
                    ++checks;
                }
            }
            lookNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            if (asked) {  // (the same question one by one, no cap: the grid must give the same answer)
                Seg seg;
                MakeSeg(&seg, &s.start.x, &s.dir.x);
                uint32_t n2 = 0;
                Check(reuse == gHookLog.StillValid(0, seg, s.seq, s.epoch, &n2, 1024), "hooks: the grid's answer differs from the one-by-one one");
            }
            if (reuse) s.seq = gHookLog.seq;
            const auto t1 = std::chrono::steady_clock::now();
            std::vector<PartState> now = Contributing(s.start, s.dir);  // (stands for the cast)
            castNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t1).count();
            if (reuse) {
                ++kept;
                Check(SameList(now, s.made), "hooks: a kept memo whose cast would differ");
            } else {
                ++cast;
                s.made = now;
                s.seq = gHookLog.seq;
                s.epoch = gHookLog.epoch[0];
                s.valid = true;
            }
        }
    }
    if (print) {
        const double t = turns;
        std::printf("hooks seed %u (parts %d, moving %d a turn, shadows %d): kept %.1f %%, cap misses %ld; a turn: changes %.1f, "
                    "lookups %.1f us (scanned %.1f a check), the old walk %.1f us, the reference culling of all shadows %.1f us\n",
                    seed, nParts, nMoving, nShadows, 100.0 * kept / (kept + cast), capMiss, gHookLog.written / t, lookNs / t / 1000.0,
                    checks ? double(scanned) / checks : 0.0, walkNs / t / 1000.0, castNs / t / 1000.0);
    }
    Check(kept > 0, "hooks: nothing kept (test is vacuous)");
}
}  // namespace

int main() {
    TestClassify();
    TestRule(1, 200, 4, 5, 20000.0f);
    TestRule(2, 400, 8, 40, 20000.0f);
    TestRule(3, 60, 3, 2, 3000.0f);
    TestRule(4, 800, 2, 200, 50000.0f);
    TestHooks(11, 133, 40, 120, 1500, true);   // (Bowser's Star Reactor's size: 133-217 parts)
    TestHooks(12, 220, 120, 200, 1500, true);  // (moving platforms everywhere)
    TestHooks(13, 800, 400, 300, 600, true);   // (far beyond any stage)
    gQuiet = true;
    for (gMutant = 1; gMutant <= 3; ++gMutant) {
        const int before = gFail;
        if (gMutant == 3) {
            TestHooks(14, 133, 40, 120, 1500, false);
        } else {
            TestRule(5, 400, 4, 40, 20000.0f);
        }
        const bool caught = gFail > before;
        std::printf("mutant %d: %s\n", gMutant, caught ? "caught" : "NOT caught");
        gFail = before + (caught ? 0 : 1);
    }
    std::printf(gFail ? "FAIL (%d)\n" : "PASS\n", gFail);
    return gFail ? 1 : 0;
}
