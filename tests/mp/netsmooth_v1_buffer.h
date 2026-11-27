// Galaxy 3.0 multiplayer: a frozen copy of the mp-int (789f89e) MpPoseBuffer, the "before" of tests/mp/netsmooth_sim.cpp.
// Test only: never built into the game.
#pragma once
#include "Game/MP/MpPoseMath.hpp"

struct MpPoseBufferV1 {
    // 8 samples: at 30 Hz they cover 233 ms, enough for the 100..200 ms render delay (3 samples would cover only 67 ms and the
    // render point would sit on the oldest sample).
    enum { N = 8 };
    static constexpr float kMinDelay = 6.0f;       // 100 ms
    static constexpr float kMaxDelay = 12.0f;      // 200 ms
    static constexpr float kMaxExtrap = 9.0f;      // 150 ms
    static constexpr float kFastSpeed = 60.0f;     // units/turn: launch stars, cannons, slings: extrapolate 2 turns at most
    // (perf) The render clock slews instead of jumping: the clock estimate (offset) and the delay move in steps (an early packet,
    // a late window, the jitter estimate), and a step of the render point is a visible skip or back-step of the puppet. The
    // render point follows at +5 % / -15 % speed. A correction larger than kSnap turns: forward (we are far behind: a stall, a
    // sender restart) is taken at once; backward (the path got slower and the render point ran past the samples) at half speed,
    // never as a jump back in time (the puppet would rewind by the size of the step).
    static constexpr float kSlewUp = 0.05f;
    static constexpr float kSlewDown = 0.15f;
    static constexpr float kSlewFast = 0.5f;
    static constexpr float kSnap = 8.0f;

    MpPoseF s[N];        // oldest first
    int count;
    bool haveClock;
    uint32_t offset;     // sender turn - local turn (best case, i.e. the earliest arrival seen)
    float jitter;        // EWMA of the lateness in turns
    int32_t winMin;      // smallest lateness in the current window
    int winCount;
    uint32_t lastArrival;
    uint32_t received;   // statistics
    uint32_t dropped;
    float shift;         // (perf) rendered sender turn = local turn + offset + shift; slews toward -delay()
    uint32_t lastAdvance;
    bool advanced;

    void reset() { memset(this, 0, sizeof *this); winMin = 0x7FFFFFFF; }

    float delay() const { return MpPoseMath::clampf(kMinDelay + 2.0f * jitter, kMinDelay, kMaxDelay); }

    const MpPoseF* newest() const { return count > 0 ? &s[count - 1] : nullptr; }

    // Adds a decoded sample that arrived at local turn `now`. Returns false when dropped (duplicate / too old).
    bool push(const MpPoseF& p, uint32_t now) {
        uint32_t est = p.turn - now;
        if (!haveClock) {
            haveClock = true; offset = est; jitter = 0.0f; winMin = 0x7FFFFFFF; winCount = 0;
            shift = -kMinDelay;
        } else {
            int32_t late = MpPoseMath::turnDiff(offset, est);
            if (late < -600 || late > 600) {          // the sender's clock jumped (new session, restart): start again
                offset = est; jitter = 0.0f; count = 0; late = 0; winMin = 0x7FFFFFFF; winCount = 0;
                shift = -kMinDelay;
            } else if (late < 0) {
                shift += (float)late;                 // (the render point stays where it is: it slews, see advance)
                offset = est; late = 0;               // earliest arrival so far
            }
            jitter = jitter * 0.9f + 0.1f * (float)late;
            if (late < winMin) winMin = late;
            if (++winCount >= 16) {                   // ~0.5 s of packets all late: the path got slower (or the sender's turns
                if (winMin > 2) {                     // slowed down): follow it
                    offset -= (uint32_t)(winMin - 1);
                    shift += (float)(winMin - 1);
                }
                winCount = 0; winMin = 0x7FFFFFFF;
            }
        }
        advance(now);
        if (count > 0) {
            int32_t d = MpPoseMath::turnDiff(p.turn, s[count - 1].turn);
            if (d < -600) count = 0;                  // sender restarted its counter
        }
        int pos = count;
        for (int i = 0; i < count; i++) {
            int32_t d = MpPoseMath::turnDiff(p.turn, s[i].turn);
            if (d == 0) { dropped++; return false; }
            if (d < 0) { pos = i; break; }
        }
        if (count == N && pos == 0) { dropped++; return false; }   // older than everything we keep
        if (count == N) {                                          // make room: drop the oldest
            for (int i = 0; i < N - 1; i++) s[i] = s[i + 1];
            count--; pos--;
        }
        for (int i = count; i > pos; i--) s[i] = s[i - 1];
        s[pos] = p;
        count++;
        lastArrival = now;
        received++;
        return true;
    }

    // (perf) Moves the render clock's shift toward -delay() by the turns elapsed since the last call (see kSlewUp). Called by
    // sample() and push(): the clock moves whether or not a puppet samples every turn.
    void advance(uint32_t now) {
        int32_t turns = advanced ? MpPoseMath::turnDiff(now, lastAdvance) : 1;
        if (turns <= 0) return;
        if (turns > 60) turns = 60;
        advanced = true;
        lastAdvance = now;
        const float diff = -delay() - shift;
        const float n = (float)turns;
        if (diff > kSnap) shift = -delay();
        else if (diff < -kSnap) shift += diff > -kSlewFast * n ? diff : -kSlewFast * n;
        else if (diff > 0.0f) shift += diff < kSlewUp * n ? diff : kSlewUp * n;
        else shift += diff > -kSlewDown * n ? diff : -kSlewDown * n;
    }

    // Where in sender time we render at local turn `now`, relative to the newest sample (<= 0: inside the buffer).
    float renderRel(uint32_t now) const {
        if (count == 0) return 0.0f;
        int32_t rel = MpPoseMath::turnDiff(now + offset, s[count - 1].turn);
        return (float)rel + shift;
    }

    MpSampleResult sample(uint32_t now, MpPoseF* out) {
        advance(now);
        return sampleAt(renderRel(now), out);
    }

    // t: render time relative to the newest sample, in turns.
    MpSampleResult sampleAt(float t, MpPoseF* out) const {
        using namespace MpPoseMath;
        if (count == 0) return MP_SAMPLE_NONE;
        const MpPoseF& nw = s[count - 1];
        if (t >= 0.0f || count == 1) {
            *out = nw;
            float e = t > 0.0f ? t : 0.0f;
            float v[3] = {nw.vel[0], nw.vel[1], nw.vel[2]};
            if (count >= 2) {
                const MpPoseF& pv = s[count - 2];
                int32_t dt = turnDiff(nw.turn, pv.turn);
                if (dt >= 1 && dt <= 30) {
                    for (int i = 0; i < 3; i++) v[i] = (nw.pos[i] - pv.pos[i]) / (float)dt;
                }
            }
            float speed = len3(v);
            float cap = speed > kFastSpeed ? 2.0f : kMaxExtrap;
            if (!(speed < 400.0f)) { v[0] = v[1] = v[2] = 0.0f; }   // a warp, not a motion
            float ee = e < cap ? e : cap;
            for (int i = 0; i < 3; i++) out->pos[i] = nw.pos[i] + v[i] * ee;
            out->frame = nw.frame + nw.rate * ee;
            if (out->frame < 0.0f) out->frame = 0.0f;
            if (t <= 0.0f) return MP_SAMPLE_INTERP;
            return e <= cap ? MP_SAMPLE_EXTRAP : MP_SAMPLE_HOLD;
        }
        float relOld = (float)turnDiff(s[0].turn, nw.turn);
        if (t <= relOld) { *out = s[0]; return MP_SAMPLE_INTERP; }
        int a = 0;
        for (int i = 0; i < count - 1; i++) {
            float rb = (float)turnDiff(s[i + 1].turn, nw.turn);
            if (t <= rb) { a = i; break; }
        }
        const MpPoseF& A = s[a];
        const MpPoseF& B = s[a + 1];
        float ra = (float)turnDiff(A.turn, nw.turn), rb = (float)turnDiff(B.turn, nw.turn);
        float span = rb - ra;
        float k = span > 0.0f ? clampf((t - ra) / span, 0.0f, 1.0f) : 1.0f;
        *out = A;   // discrete fields (anim, flags, form, tracks) follow the older sample until the newer one is reached
        float d[3] = {B.pos[0] - A.pos[0], B.pos[1] - A.pos[1], B.pos[2] - A.pos[2]};
        float dist = len3(d);
        if (dist > 2500.0f) {                       // a warp: no sliding across the level
            for (int i = 0; i < 3; i++) out->pos[i] = A.pos[i];
        } else {
            float ua[3], ub[3];
            quatUp(A.quat, ua);
            quatUp(B.quat, ub);
            arcLerp(A.pos, ua, B.pos, ub, k, out->pos);
        }
        quatSlerp(A.quat, B.quat, k, out->quat);
        out->scale = lerpf(A.scale, B.scale, k);
        for (int i = 0; i < 3; i++) out->vel[i] = lerpf(A.vel[i], B.vel[i], k);
        float dtA = t - ra;
        if (A.anim == B.anim && B.frame >= A.frame && (B.flags & MP_POSE_SIMPLE_BCK) == (A.flags & MP_POSE_SIMPLE_BCK)) {
            out->frame = lerpf(A.frame, B.frame, k);
            for (int i = 0; i < 4; i++) out->weights[i] = lerpf(A.weights[i], B.weights[i], k);
        } else {
            out->frame = A.frame + A.rate * dtA;    // looped or changed: advance A on its own rate
            if (out->frame < 0.0f) out->frame = 0.0f;
        }
        if (A.upperAnim != 0 && A.upperAnim == B.upperAnim && B.upperFrame >= A.upperFrame) {
            out->upperFrame = lerpf(A.upperFrame, B.upperFrame, k);
        }
        if (k >= 1.0f) {
            out->anim = B.anim; out->flags = B.flags; out->form = B.form; out->upperAnim = B.upperAnim;
            out->hasTracks = B.hasTracks;
            for (int i = 0; i < 4; i++) out->tracks[i] = B.tracks[i];
        }
        return MP_SAMPLE_INTERP;
    }
};
