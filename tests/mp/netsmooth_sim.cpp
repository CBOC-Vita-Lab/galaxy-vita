// Galaxy 3.0 multiplayer: host-side simulator of the remote-player smoothing (owner: netsmooth). Not built into the game.
//
// A sender plays a recorded (or synthetic) 60-turn/s player motion and sends the STATE pose every 2nd turn (as MpSendGate:
// 30 Hz, quantised through writeState/readState) over a network model; the receiver runs 60 turns/s, pushes what arrived and
// samples every turn, with the frozen mp-int buffer (v1, tests/mp/netsmooth_v1_buffer.h) and the current MpPoseBuffer (v2).
//
// Network model (one way, per STATE):
//   latency = base + jitter * z, z an AR(1) normal (rho 0.5, queueing is correlated), clamped to [base/3, base + 4 jitter];
//   mostly FIFO (a packet never overtakes the previous one, except with probability `reorder`); loss: Bernoulli `loss`, a
//   loss is followed by another with 30 % (short bursts); psave: the receiving Vita's Wi-Fi power save: frames are delivered
//   only when the radio wakes, every 102.4 ms (beacon), and a wake is skipped with probability 15 % per beacon (up to 3 in a
//   row: 80..400 ms pauses, then the backlog arrives at once).
// Metrics (after a 2 s warm-up), per turn the receiver shows pose P at rendered sender turn st:
//   err     |P - truth(st)|: the shape error (extrapolation guesses, glides) [units; Mario runs ~13 units/turn]
//   delay   sender's current turn - st, in ms (perceived lag)
//   jerk    |second difference of P - second difference of truth(st)| [units/turn^2]: kinks the network added
//   bumps   turns with jerk > 20 (a visible bump; the "ideal" case shows what 30 Hz interpolation itself gives);  snaps: turns whose step differs from the truth's by > 25 units
//   hold    % of turns frozen (HOLD); extrap: % extrapolated
//   anim    turns where the shown animation frame steps differently from the truth's by > 1.5 frames (hiccups)
//
// Build: g++ -std=c++20 -O2 -Iplatform/include -Idecomp/include -Itests/mp tests/mp/netsmooth_sim.cpp -o /tmp/netsmooth_sim
// Run:   /tmp/netsmooth_sim [--trace FILE] [--case NAME] [--seeds N] [--csv]   (cases: ideal lan psave psave-heavy internet bad hitches desk-inet desk-bad, or all)
//   trace FILE: "t x y z qx qy qz qw vx vy vz flags anim frame" per line, one per sender turn (tools/mp/netsmooth/
//   poselog_trace.py makes it from a SMG_MP_POSELOG=1 game log); without it a synthetic course is used.
#include "Game/MP/MpPoseMath.hpp"
#include "netsmooth_v1_buffer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t u32() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
    double uni() { return (u32() & 0xFFFFFF) / 16777216.0; }
    double normal() {
        double u1 = uni() + 1e-12, u2 = uni();
        return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
    }
};

struct NetCase {
    const char* name;
    double baseMs, jitterMs, loss, reorder;
    double psave;   // > 0: Wi-Fi power save on the receiver, the probability per beacon that the radio sleeps on (1..3 beacons)
    double stalls;  // > 0: game-thread hitches (no turns for 100..250 ms) per second, on the sender and on the receiver each
    bool uniform;   // latency = base + uniform(0, jitter), every datagram on its own (vlink_udp's SMG_NET_DELAY_MS / _JITTER_MS)
};

static const NetCase kCases[] = {
    {"ideal", 0, 0, 0.0, 0.0, 0, 0},             // (reference: what 30 Hz interpolation itself does to the motion)
    {"lan", 5, 3, 0.0, 0.01, 0, 0},              // Wi-Fi LAN: 5 +- 3 ms
    {"psave", 5, 3, 0.005, 0.0, 0.04, 0},        // the same LAN, the receiving Vita in Wi-Fi power save: 100 ms bunching, a
                                                 // 200..400 ms sleep about every 2.5 s
    {"psave-heavy", 5, 3, 0.005, 0.0, 0.15, 0},  // power save with a 200..400 ms sleep about every 0.7 s
    {"internet", 80, 30, 0.03, 0.02, 0, 0},      // 80 +- 30 ms, 3 % loss
    {"bad", 150, 80, 0.10, 0.03, 0, 0},          // 150 +- 80 ms, 10 % loss
    {"hitches", 5, 3, 0.0, 0.01, 0, 0.1},        // LAN, and a 100..250 ms game hitch every ~10 s on each console (the desktop
                                                 // pair runs on a 2-core box show them; a Vita streaming a level too)
    {"desk-inet", 50, 60, 0.03, 1.0, 0, 0, true},   // the desktop pair runs' fault injection: 50 + U(0..60) ms, 3 % loss
    {"desk-bad", 100, 150, 0.10, 1.0, 0, 0, true},  // 100 + U(0..150) ms, 10 % loss (heavy reordering)
};

static const double kTurnMs = 1000.0 / 60.0;

// ---- the motion -------------------------------------------------------------------------------------------------------------
static std::vector<MpPoseF> gTruth;   // index = sender turn - gTruth[0].turn

static void quatFromUpYaw(const float up[3], float yaw, float q[4]) {
    // a frame with Y = up and the forward direction rotated by yaw around up
    float ref[3] = {0, 0, 1};
    if (fabsf(up[2]) > 0.9f) { ref[0] = 1; ref[2] = 0; }
    float x[3] = {up[1] * ref[2] - up[2] * ref[1], up[2] * ref[0] - up[0] * ref[2], up[0] * ref[1] - up[1] * ref[0]};
    float n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= n;
    float z[3] = {x[1] * up[2] - x[2] * up[1], x[2] * up[0] - x[0] * up[2], x[0] * up[1] - x[1] * up[0]};
    float c = cosf(yaw), s = sinf(yaw);
    float X[3], Z[3];
    for (int i = 0; i < 3; i++) { X[i] = x[i] * c - z[i] * s; Z[i] = x[i] * s + z[i] * c; }
    float m[3][4] = {{X[0], up[0], Z[0], 0}, {X[1], up[1], Z[1], 0}, {X[2], up[2], Z[2], 0}};
    MpPoseMath::quatFromMtx(m, q);
}

// A synthetic course (used when no recorded trace is given): runs, stops, sharp turns, jumps on flat ground and laps around a
// small planet (radius 900), gravity 1.6 units/turn^2, run speed 13 units/turn, a run cycle of 36 frames.
static void synthTrace(int turns, uint32_t seed) {
    Rng r(seed);
    gTruth.clear();
    float pos[3] = {0, 0, 0}, yaw = 0, speed = 0, vu = 0, h = 0;
    bool planet = false;
    float ang = 0;   // planet: angle along a great circle
    const float R = 900.0f, C[3] = {20000, -900, 0};
    float targetSpeed = 13, yawRate = 0, frame = 0;
    uint32_t anim = 1;
    int seg = 0;
    for (int t = 0; t < turns; t++) {
        if (--seg <= 0) {   // a new behaviour every 0.5..2 s
            seg = 30 + (int)(r.uni() * 90);
            double u = r.uni();
            targetSpeed = u < 0.15 ? 0.0f : (u < 0.35 ? 6.0f : 13.0f);
            yawRate = r.uni() < 0.4 ? (float)((r.uni() - 0.5) * 0.12) : 0.0f;
            if (r.uni() < 0.35 && h <= 0.0f) vu = 22.0f + (float)r.uni() * 8.0f;   // jump
            if (t % 1800 == 900) planet = !planet;
        }
        if (r.uni() < 0.01) yaw += (float)((r.uni() - 0.5) * 3.0);   // a sudden turn
        speed += (targetSpeed - speed) * 0.15f;
        yaw += yawRate;
        bool ground = h <= 0.0f && vu <= 0.0f;
        if (!ground || vu > 0.0f) {
            h += vu;
            vu -= 1.6f;
            if (vu < -40.0f) vu = -40.0f;
            if (h <= 0.0f) { h = 0.0f; vu = 0.0f; }
        }
        ground = h <= 0.0f;
        float up[3];
        MpPoseF p;
        memset(&p, 0, sizeof p);
        if (!planet) {
            pos[0] += cosf(yaw) * speed;
            pos[2] += sinf(yaw) * speed;
            up[0] = 0; up[1] = 1; up[2] = 0;
            p.pos[0] = pos[0]; p.pos[1] = h; p.pos[2] = pos[2];
        } else {
            ang += speed / R;
            up[0] = sinf(ang); up[1] = cosf(ang); up[2] = 0;
            for (int i = 0; i < 3; i++) p.pos[i] = C[i] + up[i] * (R + h);
        }
        quatFromUpYaw(up, yaw, p.quat);
        p.turn = 1000000u + (uint32_t)t;
        p.scale = 1.0f;
        uint32_t want = !ground ? 3u : (speed > 1.0f ? 1u : 2u);
        if (want != anim) { anim = want; frame = 0; }
        float rate = anim == 1 ? speed / 13.0f : 1.0f;
        frame += rate;
        if (anim != 3 && frame >= 36.0f) frame -= 36.0f;
        if (anim == 3 && frame > 30.0f) frame = 30.0f;
        p.anim = anim; p.frame = frame; p.rate = rate;
        p.flags = MP_POSE_VISIBLE | (ground ? MP_POSE_ON_GROUND : 0);
        p.weights[0] = 1.0f;
        gTruth.push_back(p);
    }
    for (size_t i = 0; i + 1 < gTruth.size(); i++)
        for (int k = 0; k < 3; k++) gTruth[i].vel[k] = gTruth[i + 1].pos[k] - gTruth[i].pos[k];
}

static bool loadTrace(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    gTruth.clear();
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        MpPoseF p;
        memset(&p, 0, sizeof p);
        unsigned flags = 0, anim = 0;
        unsigned t = 0;
        if (sscanf(line, "%u %f %f %f %f %f %f %f %f %f %f %x %x %f", &t, &p.pos[0], &p.pos[1], &p.pos[2], &p.quat[0], &p.quat[1],
                   &p.quat[2], &p.quat[3], &p.vel[0], &p.vel[1], &p.vel[2], &flags, &anim, &p.frame) != 14)
            continue;
        p.turn = t;
        p.flags = (uint16_t)flags;
        p.anim = anim;
        p.scale = 1.0f;
        p.weights[0] = 1.0f;
        if (!gTruth.empty() && p.turn != gTruth.back().turn + 1) {   // the trace must be contiguous: fill a gap by holding
            while (gTruth.back().turn + 1 < p.turn) { MpPoseF h = gTruth.back(); h.turn++; gTruth.push_back(h); }
            if (gTruth.back().turn + 1 != p.turn) continue;
        }
        gTruth.push_back(p);
    }
    fclose(f);
    for (size_t i = 0; i + 1 < gTruth.size(); i++) {   // rate: the frame step to the next turn (same animation, forward)
        float d = gTruth[i + 1].frame - gTruth[i].frame;
        gTruth[i].rate = (gTruth[i + 1].anim == gTruth[i].anim && d >= 0.0f && d < 4.0f) ? d : (i > 0 ? gTruth[i - 1].rate : 1.0f);
    }
    return gTruth.size() > 600;
}

static void truthAt(double st, float out[3], float* frame, uint32_t* anim) {
    const double rel = st - (double)gTruth[0].turn;
    int i = (int)floor(rel);
    if (i < 0) i = 0;
    if (i >= (int)gTruth.size() - 1) i = (int)gTruth.size() - 2;
    float k = (float)(rel - i);
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    const MpPoseF& a = gTruth[i];
    const MpPoseF& b = gTruth[i + 1];
    for (int j = 0; j < 3; j++) out[j] = a.pos[j] + (b.pos[j] - a.pos[j]) * k;
    *anim = a.anim;
    *frame = a.frame + a.rate * k;
}

// ---- one run -------------------------------------------------------------------------------------------------------------
struct Stats {
    std::vector<float> err, delay, jerk;
    int turns = 0, kinks = 0, snaps = 0, holds = 0, extraps = 0, anim = 0, animTurns = 0, sent = 0, lost = 0;
    void add(const Stats& o) {
        err.insert(err.end(), o.err.begin(), o.err.end());
        delay.insert(delay.end(), o.delay.begin(), o.delay.end());
        jerk.insert(jerk.end(), o.jerk.begin(), o.jerk.end());
        turns += o.turns; kinks += o.kinks; snaps += o.snaps; holds += o.holds; extraps += o.extraps; anim += o.anim;
        animTurns += o.animTurns; sent += o.sent; lost += o.lost;
    }
};

static float pct(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    size_t k = (size_t)(q * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}
static float mean(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += x;
    return v.empty() ? 0 : (float)(s / v.size());
}

struct Packet {
    double at;
    MpPoseF p;
};

template < class Buf >
static Stats runOnce(const NetCase& nc, uint64_t seed, bool redundant, FILE* dump) {
    Rng r(seed);
    Stats st;
    Buf b;
    b.reset();
    // sender: turn j happens at j * kTurnMs (+ phase); sends at every 2nd turn
    const int turns = (int)gTruth.size();
    const double phase = r.uni() * kTurnMs;
    // hitches: the wall time of each sender turn and of each receiver turn (a stalled game runs no turns, then goes on)
    std::vector<double> sendT(turns), recvT(turns);
    for (int pass = 0; pass < 2; pass++) {
        std::vector<double>& T = pass == 0 ? sendT : recvT;
        double extra = 0;
        for (int j = 0; j < turns; j++) {
            if (nc.stalls > 0 && r.uni() < nc.stalls / 60.0) extra += 100.0 + r.uni() * 150.0;
            T[j] = j * kTurnMs + (pass == 0 ? phase : 0.0) + extra;
        }
    }
    std::vector<Packet> q;
    q.reserve(turns);
    double z = 0, lastAt = -1e9;
    bool lostPrev = false;
    for (int j = 0; j < turns; j += 2) {
        st.sent++;
        bool lose = r.uni() < (lostPrev ? 0.3 : nc.loss);
        lostPrev = lose;
        z = 0.5 * z + 0.8660254 * r.normal();
        double lat = nc.baseMs + nc.jitterMs * z;
        lat = std::max(nc.baseMs / 3.0, std::min(nc.baseMs + 4.0 * nc.jitterMs, lat));
        if (nc.uniform) lat = nc.baseMs + nc.jitterMs * r.uni();
        double at = sendT[j] + lat;
        if (r.uni() >= nc.reorder) at = std::max(at, lastAt);   // FIFO unless reordered
        if (lose) { st.lost++; continue; }
        lastAt = std::max(lastAt, at);
        uint8_t buf[MP_POSE_WIRE_BYTES + MP_POSE_TAIL_BYTES];
        MpPoseF src = gTruth[j];
        size_t n = MpPoseMath::writeState(buf, sizeof buf, src);
        MpPoseF dec;
        if (!MpPoseMath::readState(buf, n, &dec)) continue;
        q.push_back({at, dec});
        if (redundant && j >= 2) {   // (an option studied: each datagram also carries the previous STATE)
            MpPoseF prev = gTruth[j - 2];
            size_t n2 = MpPoseMath::writeState(buf, sizeof buf, prev);
            if (MpPoseMath::readState(buf, n2, &dec)) q.push_back({at, dec});
        }
    }
    if (nc.psave > 0) {   // the receiver's radio: frames wait for the next wake
        double wake = r.uni() * 102.4;
        std::vector<double> wakes;
        int skip = 0;
        while (wake < turns * kTurnMs + 2000) {
            if (skip == 0 && r.uni() < nc.psave) skip = 1 + (int)(r.uni() * 3);
            if (skip > 0) skip--; else wakes.push_back(wake);
            wake += 102.4;
        }
        for (Packet& p : q) {
            auto it = std::lower_bound(wakes.begin(), wakes.end(), p.at);
            if (it != wakes.end()) p.at = *it + 1.0;   // (+ the transfer)
        }
    }
    std::stable_sort(q.begin(), q.end(), [](const Packet& a, const Packet& c) { return a.at < c.at; });
    // receiver: turn k at k * kTurnMs (receiver turn counter starts at 50)
    size_t next = 0;
    float prev[2][3];
    float prevT[2][3];
    float prevFrame = 0, prevTF = 0;
    uint32_t prevAnim = 0, prevTA = 0;
    int have = 0;
    const int warm = 120;
    for (int k = 0; k < turns - 30; k++) {
        const double now = recvT[k];
        const uint32_t rt = 50u + (uint32_t)k;
        while (next < q.size() && q[next].at <= now) { b.push(q[next].p, rt); next++; }
        MpPoseF o;
        MpSampleResult res = b.sample(rt, &o);
        if (res == MP_SAMPLE_NONE) { have = 0; continue; }
        const double stTurn = (double)b.s[b.count - 1].turn + (double)b.renderRel(rt);
        float tp[3], tf;
        uint32_t ta;
        truthAt(stTurn, tp, &tf, &ta);
        // the sender's turn at this wall time (its last turn; inside a hitch it stays)
        const size_t js = (size_t)(std::upper_bound(sendT.begin(), sendT.end(), now) - sendT.begin());
        const double senderNow = (double)gTruth[0].turn + (js == 0 ? 0.0 : (double)(js - 1));
        if (dump) fprintf(dump, "%d %.3f %.2f %.2f %.2f %.2f %.2f %.2f %d\n", k, stTurn, o.pos[0], o.pos[1], o.pos[2], tp[0], tp[1], tp[2], (int)res);
        if (k >= warm) {
            st.turns++;
            float d[3] = {o.pos[0] - tp[0], o.pos[1] - tp[1], o.pos[2] - tp[2]};
            st.err.push_back(MpPoseMath::len3(d));
            st.delay.push_back((float)((senderNow - stTurn) * kTurnMs));
            if (res == MP_SAMPLE_HOLD) st.holds++;
            if (res == MP_SAMPLE_EXTRAP) st.extraps++;
            if (have >= 2) {
                float j2[3], s1[3];
                for (int i = 0; i < 3; i++) {
                    float a = o.pos[i] - 2 * prev[0][i] + prev[1][i];
                    float at = tp[i] - 2 * prevT[0][i] + prevT[1][i];
                    j2[i] = a - at;
                    s1[i] = (o.pos[i] - prev[0][i]) - (tp[i] - prevT[0][i]);
                }
                float jj = MpPoseMath::len3(j2);
                st.jerk.push_back(jj);
                if (jj > 20.0f) st.kinks++;
                if (MpPoseMath::len3(s1) > 25.0f) st.snaps++;
            }
            if (have >= 1 && o.anim == prevAnim && ta == prevTA && ta == o.anim) {
                st.animTurns++;
                float dd = o.frame - prevFrame, dt = tf - prevTF;
                if (fabsf(dt) < 5.0f && fabsf(dd) < 5.0f && fabsf(dd - dt) > 1.5f) st.anim++;   // (loop wraps: not compared)
            }
        }
        for (int i = 0; i < 3; i++) { prev[1][i] = prev[0][i]; prev[0][i] = o.pos[i]; prevT[1][i] = prevT[0][i]; prevT[0][i] = tp[i]; }
        prevFrame = o.frame; prevAnim = o.anim; prevTF = tf; prevTA = ta;
        if (have < 2) have++;
    }
    return st;
}

// --bench: host nanoseconds per receiver turn (a push every 2nd turn, a sample every turn), a jittery stream with corrections
template < class Buf >
static double benchNs() {
    static Buf b;
    b.reset();
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int turns = 400000;
    float sink = 0;
    for (int k = 0; k < turns; k++) {
        if ((k & 1) == 0) {
            const MpPoseF& src = gTruth[(size_t)k % (gTruth.size() - 1)];
            MpPoseF p = src;
            p.turn = 7000000u + (uint32_t)k;
            b.push(p, 100u + (uint32_t)k + (uint32_t)((k * 7919) % 5));
        }
        MpPoseF o;
        b.sample(100u + (uint32_t)k, &o);
        sink += o.pos[0];
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (sink == 12345.0f) printf(" ");
    return ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / turns;
}

static void report(const char* label, const Stats& s, bool csv) {
    const float n = (float)std::max(1, s.turns);
    const float perMin = 3600.0f / n;
    if (csv) {
        printf("%s,%.2f,%.2f,%.1f,%.0f,%.0f,%.2f,%.2f,%.1f,%.1f,%.2f,%.2f,%.1f\n", label, mean(s.err), pct(s.err, 0.95), pct(s.err, 0.999),
               mean(s.delay), pct(s.delay, 0.95), mean(s.jerk), pct(s.jerk, 0.999), s.kinks * perMin, s.snaps * perMin,
               100.0f * s.holds / n, 100.0f * s.extraps / n, s.anim * perMin);
        return;
    }
    printf("  %-10s err mean %6.2f p95 %6.2f p99.9 %6.1f | delay mean %4.0f p95 %4.0f ms | jerk mean %5.2f p99.9 %5.1f | "
           "bumps/min %5.1f snaps/min %5.1f | hold %5.2f%% extrap %5.2f%% | anim hiccups/min %5.1f\n",
           label, mean(s.err), pct(s.err, 0.95), pct(s.err, 0.999), mean(s.delay), pct(s.delay, 0.95), mean(s.jerk), pct(s.jerk, 0.999),
           s.kinks * perMin, s.snaps * perMin, 100.0f * s.holds / n, 100.0f * s.extraps / n, s.anim * perMin);
}

int main(int argc, char** argv) {
    const char* trace = nullptr;
    std::string only = "all";
    int seeds = 8;
    bool csv = false, redundant = false, bench = false;
    const char* dumpPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--case") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv")) csv = true;
        else if (!strcmp(argv[i], "--redundant")) redundant = true;
        else if (!strcmp(argv[i], "--bench")) bench = true;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpPath = argv[++i];
        else { fprintf(stderr, "usage: %s [--trace FILE] [--case ideal|lan|psave|psave-heavy|internet|bad|hitches|all] [--seeds N] [--csv] [--redundant] [--dump PREFIX] [--bench]\n", argv[0]); return 2; }
    }
    if (trace) {
        if (!loadTrace(trace)) { fprintf(stderr, "cannot read trace %s\n", trace); return 2; }
    } else {
        synthTrace(60 * 120, 7);
    }
    if (bench) {
        double a = benchNs< MpPoseBufferV1 >(), c = benchNs< MpPoseBuffer >();
        a = std::min(a, benchNs< MpPoseBufferV1 >());
        c = std::min(c, benchNs< MpPoseBuffer >());
        printf("bench: ns per receiver turn and puppet (host x86-64, -O2): v1 %.0f, v2 %.0f; sizeof v1 %zu, v2 %zu bytes\n", a, c,
               sizeof(MpPoseBufferV1), sizeof(MpPoseBuffer));
        return 0;
    }
    int ground = 0;
    for (const MpPoseF& p : gTruth) ground += (p.flags & MP_POSE_ON_GROUND) != 0;
    if (!csv)
        printf("motion: %s, %zu turns (%.0f s), on the ground %.0f%%; %d seeds per case%s\n", trace ? trace : "synthetic course", gTruth.size(),
               gTruth.size() / 60.0, 100.0 * ground / gTruth.size(), seeds, redundant ? "; redundant STATE (each carries the previous)" : "");
    else
        printf("case,err_mean,err_p95,err_p999,delay_mean,delay_p95,jerk_mean,jerk_p999,bumps_min,snaps_min,hold_pct,extrap_pct,anim_min\n");
    for (const NetCase& nc : kCases) {
        if (only != "all" && only != nc.name) continue;
        Stats a, c;
        for (int sd = 0; sd < seeds; sd++) {
            FILE* d1 = nullptr;
            FILE* d2 = nullptr;
            if (dumpPath && sd == 0) {
                std::string p1 = std::string(dumpPath) + "-" + nc.name + "-v1.txt", p2 = std::string(dumpPath) + "-" + nc.name + "-v2.txt";
                d1 = fopen(p1.c_str(), "w");
                d2 = fopen(p2.c_str(), "w");
            }
            a.add(runOnce< MpPoseBufferV1 >(nc, 1000 + sd, redundant, d1));
            c.add(runOnce< MpPoseBuffer >(nc, 1000 + sd, redundant, d2));
            if (d1) fclose(d1);
            if (d2) fclose(d2);
        }
        if (!csv) printf("%s (%.0f +- %.0f ms, loss %.1f%%%s): %d STATEs sent, %d lost\n", nc.name, nc.baseMs, nc.jitterMs, 100.0 * a.lost / std::max(1, a.sent),
                         nc.psave > 0 ? ", Wi-Fi power save on the receiver" : "", a.sent, a.lost);
        std::string l1 = std::string(nc.name) + (csv ? "-v1" : ""), l2 = std::string(nc.name) + (csv ? "-v2" : "");
        report(csv ? l1.c_str() : "before", a, csv);
        report(csv ? l2.c_str() : "after", c, csv);
    }
    return 0;
}
