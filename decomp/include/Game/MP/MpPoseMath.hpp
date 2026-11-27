// Galaxy 3.0 multiplayer: pure pose maths for the puppets (owner: puppet). No game headers: shared by the game side
// (MpPuppets.cpp, MpRemotePlayer.cpp, MpLocalCapture.cpp, MpNameTag.cpp) and the host tests (tests/mp/puppet_*).
//
//  - MpPoseF: a decoded pose in floats; encode/decode against the wire MpPose (mp_proto.h) with clamps and validation.
//  - quaternion <-> 3x4 base matrix (row-major, column vectors, the Wii Mtx layout), slerp, arc interpolation of positions
//    around the gravity centre (puppets on small planets must not cut through the ground between two samples).
//  - MpPoseBuffer: the 12 latest samples of one remote player, a clock-offset estimate (sender turns -> local turns), a jitter
//    buffer delay from the measured lateness (75..333 ms), interpolation, ground / gravity aware extrapolation <= 200 ms then
//    hold, and a glided (not snapped) correction when late data changes what is shown.
//  - MpAnimNameMap: resource-name hash -> name (simple-group bcks and per-track bcks are sent by hash).
//  - small helpers: UTF-8 -> UTF-16 for name tags, screen-edge clamp for off-screen name tags.
//
// The STATE payload is mp_write_pose() followed by an optional tail (MpPoseTail, 17 bytes) that carries the 4 Xanime track bck
// hashes (track-swapped bcks such as the slope idle on track 3). Receivers that do not find the tail keep the group's own bcks.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "port/mp_proto.h"

// An unknown form id (a newer build's, or garbage) is the normal form (puppetprep).
static inline uint8_t mp_form_clamp(uint32_t f) { return f < MP_FORM_COUNT ? (uint8_t)f : (uint8_t)MP_FORM_NORMAL; }

struct MpPoseF {
    uint32_t turn;
    uint16_t sceneSerial;
    uint8_t zone;
    uint8_t form;
    float pos[3];
    float quat[4];          // x, y, z, w (unit)
    float vel[3];           // units per turn
    float scale;
    uint32_t anim;
    float frame;
    float rate;
    float weights[4];
    uint32_t upperAnim;
    float upperFrame;
    uint16_t flags;
    bool hasTracks;         // the tail was present
    uint32_t tracks[4];     // per-track bck resource-name hash (0 = none)
};

// mp_write_pose writes 51 bytes (the comment in mp_proto.h says 52).
#define MP_POSE_WIRE_BYTES 51
#define MP_POSE_TAIL_TAG 0x54   // 'T'
#define MP_POSE_TAIL_BYTES 17   // tag + 4 x u32

namespace MpPoseMath {
    static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static inline bool finitef(float v) { return v == v && v > -3.0e38f && v < 3.0e38f; }
    static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
    static inline int32_t turnDiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

    // Same as MR::getHashCode (both builds use -fsigned-char: the bytes are signed there; we cast explicitly so a host test
    // built without the flag computes the same value).
    static inline uint32_t animHash(const char* s) {
        uint32_t h = 0;
        if (s == nullptr) return 0;
        for (; *s != '\0'; s++) h = (uint32_t)(int32_t)(int8_t)*s + h * 31u;
        return h;
    }

    static inline void quatNormalize(float q[4]) {
        float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(n > 1.0e-6f) || !finitef(n)) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
        float k = 1.0f / n;
        q[0] *= k; q[1] *= k; q[2] *= k; q[3] *= k;
    }

    // m: 3 rows x 4 (rotation 3x3 + translation column). Columns are normalised first (a base matrix may carry a little scale).
    static inline void quatFromMtx(const float m[3][4], float q[4]) {
        float c[3][3];
        for (int col = 0; col < 3; col++) {
            float x = m[0][col], y = m[1][col], z = m[2][col];
            float n = sqrtf(x * x + y * y + z * z);
            float k = (n > 1.0e-6f && finitef(n)) ? 1.0f / n : 0.0f;
            c[0][col] = x * k; c[1][col] = y * k; c[2][col] = z * k;
        }
        float tr = c[0][0] + c[1][1] + c[2][2];
        if (tr > 0.0f) {
            float s = sqrtf(tr + 1.0f) * 2.0f;
            q[3] = 0.25f * s; q[0] = (c[2][1] - c[1][2]) / s; q[1] = (c[0][2] - c[2][0]) / s; q[2] = (c[1][0] - c[0][1]) / s;
        } else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) {
            float s = sqrtf(1.0f + c[0][0] - c[1][1] - c[2][2]) * 2.0f;
            q[3] = (c[2][1] - c[1][2]) / s; q[0] = 0.25f * s; q[1] = (c[0][1] + c[1][0]) / s; q[2] = (c[0][2] + c[2][0]) / s;
        } else if (c[1][1] > c[2][2]) {
            float s = sqrtf(1.0f + c[1][1] - c[0][0] - c[2][2]) * 2.0f;
            q[3] = (c[0][2] - c[2][0]) / s; q[0] = (c[0][1] + c[1][0]) / s; q[1] = 0.25f * s; q[2] = (c[1][2] + c[2][1]) / s;
        } else {
            float s = sqrtf(1.0f + c[2][2] - c[0][0] - c[1][1]) * 2.0f;
            q[3] = (c[1][0] - c[0][1]) / s; q[0] = (c[0][2] + c[2][0]) / s; q[1] = (c[1][2] + c[2][1]) / s; q[2] = 0.25f * s;
        }
        quatNormalize(q);
        if (q[3] < 0.0f) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3]; }
    }

    // Writes the rotation part only (m[r][3] untouched).
    static inline void mtxFromQuat(const float q[4], float m[3][4]) {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        m[0][0] = 1.0f - 2.0f * (y * y + z * z); m[0][1] = 2.0f * (x * y - z * w);        m[0][2] = 2.0f * (x * z + y * w);
        m[1][0] = 2.0f * (x * y + z * w);        m[1][1] = 1.0f - 2.0f * (x * x + z * z); m[1][2] = 2.0f * (y * z - x * w);
        m[2][0] = 2.0f * (x * z - y * w);        m[2][1] = 2.0f * (y * z + x * w);        m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    }

    // Y axis of the rotation (the player's up vector).
    static inline void quatUp(const float q[4], float up[3]) {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        up[0] = 2.0f * (x * y - z * w); up[1] = 1.0f - 2.0f * (x * x + z * z); up[2] = 2.0f * (y * z + x * w);
    }

    static inline void quatSlerp(const float a[4], const float b0[4], float t, float out[4]) {
        float b[4] = {b0[0], b0[1], b0[2], b0[3]};
        float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (d < 0.0f) { d = -d; b[0] = -b[0]; b[1] = -b[1]; b[2] = -b[2]; b[3] = -b[3]; }
        float ka, kb;
        if (d > 0.9995f) { ka = 1.0f - t; kb = t; }
        else {
            float th = acosf(clampf(d, -1.0f, 1.0f));
            float s = sinf(th);
            ka = sinf((1.0f - t) * th) / s; kb = sinf(t * th) / s;
        }
        for (int i = 0; i < 4; i++) out[i] = a[i] * ka + b[i] * kb;
        quatNormalize(out);
    }

    // Angle between two unit quaternions (radians).
    static inline float quatAngle(const float a[4], const float b[4]) {
        float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return 2.0f * acosf(clampf(d, 0.0f, 1.0f));
    }

    static inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static inline float len3(const float a[3]) { return sqrtf(dot3(a, a)); }

    // Position interpolation that follows the planet: when the two up vectors differ, the centre of the gravity sphere is
    // estimated as the meeting point of the two lines p - s*up, and the position is interpolated in angle and radius around
    // it. Falls back to a straight lerp when the ups are almost parallel or the estimate is not plausible.
    static inline void arcLerp(const float pa[3], const float ua[3], const float pb[3], const float ub[3], float t, float out[3]) {
        for (int i = 0; i < 3; i++) out[i] = lerpf(pa[i], pb[i], t);
        float cosT = dot3(ua, ub);
        if (!(cosT < 0.9998f) || !(cosT > -0.5f)) return;   // nearly parallel (flat ground) or > 120 deg apart (a warp)
        float da[3] = {-ua[0], -ua[1], -ua[2]}, db[3] = {-ub[0], -ub[1], -ub[2]};
        float w0[3] = {pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]};
        float b = dot3(da, db), d = dot3(da, w0), e = dot3(db, w0);
        float den = 1.0f - b * b;
        if (!(den > 1.0e-6f)) return;
        float sa = (b * e - d) / den, sb = (e - b * d) / den;
        if (!(sa > 20.0f) || !(sb > 20.0f) || sa > 50000.0f || sb > 50000.0f) return;
        float ca[3], cb[3], c[3];
        for (int i = 0; i < 3; i++) { ca[i] = pa[i] + da[i] * sa; cb[i] = pb[i] + db[i] * sb; c[i] = 0.5f * (ca[i] + cb[i]); }
        float gap[3] = {ca[0] - cb[0], ca[1] - cb[1], ca[2] - cb[2]};
        float rmax = sa > sb ? sa : sb;
        if (len3(gap) > 0.25f * rmax) return;
        float va[3] = {pa[0] - c[0], pa[1] - c[1], pa[2] - c[2]}, vb[3] = {pb[0] - c[0], pb[1] - c[1], pb[2] - c[2]};
        float ra = len3(va), rb = len3(vb);
        if (!(ra > 1.0f) || !(rb > 1.0f)) return;
        for (int i = 0; i < 3; i++) { va[i] /= ra; vb[i] /= rb; }
        float cv = clampf(dot3(va, vb), -1.0f, 1.0f);
        float th = acosf(cv);
        float dir[3];
        if (th < 1.0e-4f) { for (int i = 0; i < 3; i++) dir[i] = lerpf(va[i], vb[i], t); }
        else {
            float s = sinf(th), ka = sinf((1.0f - t) * th) / s, kb = sinf(t * th) / s;
            for (int i = 0; i < 3; i++) dir[i] = va[i] * ka + vb[i] * kb;
        }
        float r = lerpf(ra, rb, t);
        for (int i = 0; i < 3; i++) out[i] = c[i] + dir[i] * r;
    }

    // ---- wire quantisation -------------------------------------------------------------------------------------------------
    static inline int16_t qs16(float v) { v = clampf(v, -1.0f, 1.0f) * 32767.0f; return (int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f); }
    static inline int roundi(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }

    static inline void encodePose(const MpPoseF& in, MpPose* out) {
        memset(out, 0, sizeof *out);
        out->turn = in.turn; out->sceneSerial = in.sceneSerial; out->zone = in.zone;
        out->form = in.form < MP_FORM_COUNT ? in.form : (uint8_t)MP_FORM_NORMAL;
        for (int i = 0; i < 3; i++) out->pos[i] = finitef(in.pos[i]) ? clampf(in.pos[i], -999999.0f, 999999.0f) : 0.0f;
        float q[4] = {in.quat[0], in.quat[1], in.quat[2], in.quat[3]};
        quatNormalize(q);
        if (q[3] < 0.0f) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3]; }
        for (int i = 0; i < 4; i++) out->quat[i] = qs16(q[i]);
        for (int i = 0; i < 3; i++) out->vel[i] = (int8_t)roundi(clampf(finitef(in.vel[i]) ? in.vel[i] * 0.5f : 0.0f, -127.0f, 127.0f));
        out->scale = (uint8_t)roundi(clampf(finitef(in.scale) ? in.scale * 64.0f : 64.0f, 1.0f, 255.0f));
        out->anim = in.anim;
        out->frame = (uint16_t)roundi(clampf(finitef(in.frame) ? in.frame * 16.0f : 0.0f, 0.0f, 65535.0f));
        out->rate = (int8_t)roundi(clampf(finitef(in.rate) ? in.rate * 32.0f : 0.0f, -127.0f, 127.0f));
        for (int i = 0; i < 4; i++)
            out->weights[i] = (uint8_t)roundi(clampf(finitef(in.weights[i]) ? in.weights[i] * 255.0f : 0.0f, 0.0f, 255.0f));
        out->upperAnim = in.upperAnim;
        out->upperFrame = (uint16_t)roundi(clampf(finitef(in.upperFrame) ? in.upperFrame * 16.0f : 0.0f, 0.0f, 65535.0f));
        out->flags = in.flags;
    }

    // Returns false for a pose that must be dropped (degenerate rotation; floats were checked by mp_read_pose). An unknown form id is
    // clamped to the normal form (a newer build's form, or garbage), the pose is kept.
    static inline bool decodePose(const MpPose& in, MpPoseF* out) {
        memset(out, 0, sizeof *out);
        for (int i = 0; i < 3; i++) { if (!mp_pose_float_ok(in.pos[i])) return false; }
        float q[4];
        float n2 = 0.0f;
        for (int i = 0; i < 4; i++) { q[i] = (float)in.quat[i] / 32767.0f; n2 += q[i] * q[i]; }
        if (n2 < 0.5f || n2 > 1.5f) return false;
        quatNormalize(q);
        out->turn = in.turn; out->sceneSerial = in.sceneSerial; out->zone = in.zone; out->form = mp_form_clamp(in.form);   // (puppetprep: unknown form = normal)
        for (int i = 0; i < 3; i++) out->pos[i] = in.pos[i];
        for (int i = 0; i < 4; i++) out->quat[i] = q[i];
        for (int i = 0; i < 3; i++) out->vel[i] = (float)in.vel[i] * 2.0f;
        out->scale = in.scale == 0 ? 1.0f : (float)in.scale / 64.0f;
        out->anim = in.anim;
        out->frame = (float)in.frame / 16.0f;
        out->rate = (float)in.rate / 32.0f;
        for (int i = 0; i < 4; i++) out->weights[i] = (float)in.weights[i] / 255.0f;
        out->upperAnim = (in.flags & MP_POSE_UPPER) ? in.upperAnim : 0;
        out->upperFrame = (float)in.upperFrame / 16.0f;
        out->flags = in.flags;
        return true;
    }

    // STATE payload: pose + tail. Returns bytes written (0 on failure).
    static inline size_t writeState(void* buf, size_t cap, const MpPoseF& p) {
        MpPose wire;
        encodePose(p, &wire);
        MpWriter w(buf, cap);
        mp_write_pose(w, wire);
        if (p.hasTracks) {
            w.u8(MP_POSE_TAIL_TAG);
            for (int i = 0; i < 4; i++) w.u32(p.tracks[i]);
        }
        return w.ok() ? w.n : 0;
    }

    // Parses a whole STATE payload. A tail with a wrong tag or length drops the message.
    static inline bool readState(const void* buf, size_t len, MpPoseF* out) {
        MpReader r(buf, len);
        MpPose wire;
        if (!mp_read_pose(r, &wire)) return false;
        if (!decodePose(wire, out)) return false;
        size_t rest = len - r.n;
        if (rest == 0) { out->hasTracks = false; return true; }
        if (rest != MP_POSE_TAIL_BYTES) return false;
        if (r.u8() != MP_POSE_TAIL_TAG) return false;
        for (int i = 0; i < 4; i++) out->tracks[i] = r.u32();
        out->hasTracks = true;
        return r.done();
    }
}  // namespace MpPoseMath

// ---- the latest samples of one remote player ---------------------------------------------------------------------------------
enum MpSampleResult { MP_SAMPLE_NONE = 0, MP_SAMPLE_INTERP = 1, MP_SAMPLE_EXTRAP = 2, MP_SAMPLE_HOLD = 3 };

// (netsmooth) What the puppet shows is built in three layers, all per remote player, no allocation, a few dozen flops a turn:
//  1. the jitter buffer: the render point sits delay() turns behind the earliest-arrival clock. delay() is chosen like a VoIP
//     jitter buffer: a histogram (forgetting factor ~3 s) of what each STATE needed (its lateness + the interval it closes:
//     the send interval, longer after a loss) gives the value only 5 % of the packets exceed; the delay is that plus 1.5 turns
//     (4.5..20 turns: 75..333 ms). Wi-Fi LAN: ~75 ms;
//     internet with jitter: ~100..200 ms; a Vita in Wi-Fi power save (packets bunched behind 100..400 ms radio sleeps): up to
//     333 ms, so the bursts are interpolated instead of extrapolated and held. The render clock slews toward it (+5 % / -15 %).
//  2. interpolation between the two samples around the render point (arc on planets, slerp, animation frames), or past the
//     newest sample an extrapolation that knows the ground: on the ground it never sinks or flies off (a landing's fall
//     speed is dropped, slopes are kept), in the air it follows the arc (gravity measured from the last three samples along the
//     player's up), at most kMaxExtrap turns (2 when fast), then held.
//  3. error correction: when a STATE changes what the current render point shows (a late packet after an extrapolation, an
//     older packet arriving out of order, a clock snap), the difference is not shown as a jump: it is taken into an offset that
//     a critically damped spring brings back to zero in ~250 ms (no velocity step). Position, animation frame (same animation)
//     and rotation. Warps (> kCorrMax units), visibility changes and a puppet that was not shown are snapped.
struct MpPoseBuffer {
    // 12 samples: at 30 Hz they cover 367 ms, enough for the longest render delay (333 ms) (netsmooth; was 8 for 200 ms).
    enum { N = 12 };
    enum { kLateBins = 32 };
    static constexpr float kMinDelay = 5.25f;      // 87.5 ms (+12.5 ms over 75 ms baseline to absorb Wi-Fi jitter without bumps)
    static constexpr float kMaxDelay = 20.0f;      // 333 ms
    static constexpr float kMaxExtrap = 12.0f;     // 200 ms (netsmooth: was 150 ms; with the glide a longer guess beats a freeze)
    static constexpr float kFastSpeed = 60.0f;     // units/turn: launch stars, cannons, slings: extrapolate 2 turns at most
    // (perf) The render clock slews instead of jumping: the clock estimate (offset) and the delay move in steps (an early packet,
    // a late window, the jitter estimate), and a step of the render point is a visible skip or back-step of the puppet. The
    // render point follows at +5 % / -15 % speed. A correction larger than kSnap turns: forward (we are far behind: a stall, a
    // sender restart) is taken at once (netsmooth: and glided by the error correction); backward (the path got slower and the
    // render point ran past the samples) at half speed once past kMicroExtrap turns, never as a jump back in time.
    static constexpr float kSlewUp = 0.05f;
    static constexpr float kSlewDown = 0.15f;
    static constexpr float kSlewFast = 0.5f;
    static constexpr float kSnap = 8.0f;
    static constexpr float kSnapBack = 30.0f;      // >0.5 s backward lag (outage / clock stall): snap back instead of slewing 16+ s in HOLD
    static constexpr float kMicroExtrap = 3.0f;    // first 3 turns of micro-extrapolation stay on kSlewDown (no instant 0.5x brake)
    static constexpr float kExtrapDecay = 0.18f;   // exponential velocity decay over turns 1..8 so brief hiccups coast smoothly
    // (netsmooth) jitter buffer
    static constexpr float kHistKeep = 0.996f;     // per STATE: ~250-packet (~8-15 s) memory so periodic 100 ms Wi-Fi hiccups stay absorbed
    static constexpr float kLateQuantile = 0.05f;  // the delay covers all but the latest 5 % of the packets
    static constexpr float kDelayExtra = 1.5f;     // + one turn of receive quantisation, + half a turn: the render point sits
                                                   // between the 30 Hz samples, so a sample's corner is spread over two
                                                   // pictures instead of one (the real motion's bumps halve, see the sim)
    static constexpr int kPriorBin = 5;            // start as if 3 packets had needed 5 turns (delay 6.5 = 108 ms until known)
    static constexpr float kHistPrior = 3.0f;
    // (netsmooth) error correction
    static constexpr float kCorrW = 0.16f;         // softer critically damped spring rate per turn (no 1-2 ft whip)
    static constexpr float kCorrMax = 400.0f;      // a larger correction is a snap (a warp, a respawn)
    static constexpr float kCorrCut = kCorrMax;    // smooth-correction cutoff; below this per-turn step is clamped to kCorrStepMax
    static constexpr float kCorrStepMax = 12.0f;   // max correction step per turn (units/turn) within kCorrCut
    static constexpr float kCorrFrames = 10.0f;    // animation frames: a larger step (a loop wrap) is not glided
    static constexpr float kCorrRotKeep = 0.8f;    // rotation correction left after each turn
    static constexpr float kMaxFallSpeed = 50.0f;  // extrapolated fall speed limit (units/turn)

    // Smoothly damped extrapolation time: full velocity for turn 0..1 (C1 at t=0), then exp(-kExtrapDecay*(s-1)) decay.
    static float extrapDistTime(float ee) {
        if (ee <= 1.0f) return ee;
        const float dt = ee - 1.0f;
        return 1.0f + (1.0f - expf(-kExtrapDecay * dt)) / kExtrapDecay;
    }

    // (netlog) the settings file's overrides (port/mp_nettune.h; MpPuppets copies them in when a session goes live). They start at the constants
    // above, so a key that is not set changes nothing, bit for bit.
    static inline float tMinDelay = kMinDelay;
    static inline float tMaxDelay = kMaxDelay;
    static inline float tMaxExtrap = kMaxExtrap;
    static inline float tDelayExtra = kDelayExtra;
    static inline float tLateQuantile = kLateQuantile;
    static void resetTune() {
        tMinDelay = kMinDelay; tMaxDelay = kMaxDelay; tMaxExtrap = kMaxExtrap; tDelayExtra = kDelayExtra; tLateQuantile = kLateQuantile;
    }

    MpPoseF s[N];        // oldest first
    int count;
    bool haveClock;
    uint32_t offset;     // sender turn - local turn (best case, i.e. the earliest arrival seen)
    float jitter;        // EWMA of the lateness in turns (statistics; the delay comes from the histogram)
    int32_t winMin;      // smallest lateness in the current window
    int winCount;
    uint32_t lastArrival;
    uint32_t received;   // statistics
    uint32_t dropped;
    float shift;         // (perf) rendered sender turn = local turn + offset + shift; slews toward -delay()
    uint32_t lastAdvance;
    bool advanced;
    // (netsmooth) jitter buffer
    float hist[kLateBins];
    float histSum;
    float target;        // delay() in turns
    int32_t lastLate;    // lateness of the sample being pushed
    // (netsmooth) error correction (displayed = raw + corr)
    bool shown;          // sample() ran recently (a correction only for a puppet that is on screen)
    uint32_t corrTurn;   // the turn the spring was last stepped
    float corr[3], corrV[3];
    float corrF, corrFV;
    uint32_t corrAnim;
    float corrQ[4];
    uint32_t snaps;      // statistics: corrections too large to glide

    void reset() {
        memset(this, 0, sizeof *this);
        winMin = 0x7FFFFFFF;
        corrQ[3] = 1.0f;
        hist[kPriorBin] = kHistPrior;
        histSum = kHistPrior;
        target = updateTarget();
    }

    float delay() const { return target; }

    const MpPoseF* newest() const { return count > 0 ? &s[count - 1] : nullptr; }

    // The lateness that only kLateQuantile of the weight exceeds, + kDelayExtra, clamped.
    float updateTarget() const {
        const float limit = histSum * tLateQuantile;
        float above = 0.0f;
        int q = kLateBins - 1;
        for (; q > 0; q--) {
            above += hist[q];
            if (above > limit) break;
        }
        return MpPoseMath::clampf((float)q + tDelayExtra, tMinDelay, tMaxDelay);
    }

    // The lateness reference moved by d turns (d > 0: every lateness seen so far is d turns smaller now).
    void shiftHist(int32_t d) {
        if (d == 0) return;
        float h[kLateBins];
        for (int i = 0; i < kLateBins; i++) h[i] = 0.0f;
        for (int i = 0; i < kLateBins; i++) {
            int j = i - d;
            j = j < 0 ? 0 : (j >= kLateBins ? kLateBins - 1 : j);
            h[j] += hist[i];
        }
        for (int i = 0; i < kLateBins; i++) hist[i] = h[i];
    }

    void noteLate(int32_t late) {
        int bin = late < 0 ? 0 : (late >= kLateBins ? kLateBins - 1 : (int)late);
        for (int i = 0; i < kLateBins; i++) hist[i] *= kHistKeep;
        hist[bin] += 1.0f;
        histSum = histSum * kHistKeep + 1.0f;
        target = updateTarget();
    }

    void dropCorr() {
        for (int i = 0; i < 3; i++) { corr[i] = 0.0f; corrV[i] = 0.0f; }
        corrF = corrFV = 0.0f;
        corrQ[0] = corrQ[1] = corrQ[2] = 0.0f; corrQ[3] = 1.0f;
    }

    // Adds a decoded sample that arrived at local turn `now`. Returns false when dropped (duplicate / too old).
    bool push(const MpPoseF& p, uint32_t now) {
        // (netsmooth) what the current render point shows before this sample: a change becomes a C1-glided correction
        const float jump = advance(now);
        const float relPre = renderRel(now) - jump;
        MpPoseF pre;
        float vPre[3] = {0.0f, 0.0f, 0.0f};
        bool havePre = jump >= 0.0f && shown && count > 0 && MpPoseMath::turnDiff(now, corrTurn) <= 2 &&
                       sampleAt(relPre, &pre) != MP_SAMPLE_NONE;
        bool haveVPre = havePre && sampleVel(relPre, vPre);
        uint32_t est = p.turn - now;
        if (!haveClock) {
            haveClock = true; offset = est; jitter = 0.0f; winMin = 0x7FFFFFFF; winCount = 0;
            shift = -target;
            havePre = false;
        } else {
            int32_t late = MpPoseMath::turnDiff(offset, est);
            const bool arrGap = count > 0 && MpPoseMath::turnDiff(now, lastArrival) > 90;
            if (late < -120 || late > 120 || arrGap) {   // clock jump or >1.5 s arrival outage: reset cleanly instead of holding for ~16 s
                offset = est; jitter = 0.0f; count = 0; late = 0; winMin = 0x7FFFFFFF; winCount = 0;
                for (int i = 0; i < kLateBins; i++) hist[i] = 0.0f;
                hist[kPriorBin] = kHistPrior; histSum = kHistPrior;
                target = updateTarget();
                shift = -target;
                dropCorr();
                havePre = false;
            } else if (late < 0) {
                shift += (float)late;                 // (the render point stays where it is: it slews, see advance)
                offset = est; late = 0;               // earliest arrival so far
                // (netsmooth) the histogram is not shifted up: an earlier arrival than ever is mostly a change of the clocks'
                // relation (our game hitched and lost turns, the sender's turns ran fast), not proof that every packet so far
                // was that much later; shifting would hold the delay up for ~10 s after each hitch
            }
            jitter = jitter * 0.9f + 0.1f * (float)late;
            lastLate = late;
            if (late < winMin) winMin = late;
            if (++winCount >= 16) {                   // ~0.5 s of packets all late: the path got slower (or the sender's turns
                if (winMin > 2) {                     // slowed down): follow it
                    offset -= (uint32_t)(winMin - 1);
                    shift += (float)(winMin - 1);
                    shiftHist(winMin - 1);
                    target = updateTarget();
                }
                winCount = 0; winMin = 0x7FFFFFFF;
            }
        }
        if (count > 0) {
            int32_t d = MpPoseMath::turnDiff(p.turn, s[count - 1].turn);
            if (d < -120) { count = 0; dropCorr(); havePre = false; }   // sender restarted its counter
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
        // (netsmooth) this sample closes the interval since the sample before it: render points in that interval needed it
        // `late + gap` turns behind the earliest-arrival clock (gap: the send interval, longer after a loss)
        if (count >= 2 && pos > 0) {
            int32_t gap = MpPoseMath::turnDiff(p.turn, s[pos - 1].turn);
            noteLate(lastLate + (gap < 1 ? 1 : (gap > 30 ? 30 : gap)));
        }
        if (havePre) {
            const float relPost = renderRel(now);
            MpPoseF post;
            if (sampleAt(relPost, &post) != MP_SAMPLE_NONE) {
                float vPost[3] = {0.0f, 0.0f, 0.0f};
                const bool haveVel = haveVPre && sampleVel(relPost, vPost);
                addCorr(now, pre, post, vPre, vPost, haveVel);
            }
        }
        return true;
    }

    // (perf) Moves the render clock's shift toward -delay() by the turns elapsed since the last call (see kSlewUp). Called by
    // sample() and push(): the clock moves whether or not a puppet samples every turn. Returns the size of a forward snap
    // (0: none) so the caller can glide over it.
    float advance(uint32_t now) {
        int32_t turns = advanced ? MpPoseMath::turnDiff(now, lastAdvance) : 1;
        if (turns <= 0) return 0.0f;
        if (turns > 60) turns = 60;
        advanced = true;
        lastAdvance = now;
        const float diff = -delay() - shift;
        const float n = (float)turns;
        if (diff > kSnap) { shift = -delay(); return diff; }
        if (diff < -kSnapBack) { shift = -delay(); dropCorr(); return diff; }
        // (netsmooth) back at half speed only past kMicroExtrap turns beyond the newest sample (first 3 turns stay on gentle
        // kSlewDown so brief Wi-Fi jitter hiccups coast instead of slamming half-speed)
        const float down = (diff < -kSnap || renderRel(now) > kMicroExtrap) ? kSlewFast : kSlewDown;
        if (diff > 0.0f) shift += diff < kSlewUp * n ? diff : kSlewUp * n;
        else shift += diff > -down * n ? diff : -down * n;
        return 0.0f;
    }

    // Where in sender time we render at local turn `now`, relative to the newest sample (<= 0: inside the buffer).
    float renderRel(uint32_t now) const {
        if (count == 0) return 0.0f;
        int32_t rel = MpPoseMath::turnDiff(now + offset, s[count - 1].turn);
        return (float)rel + shift;
    }

    // Forward per-turn raw velocity at render time `t` (same operator pre/post so multi-packet bursts telescope cleanly).
    bool sampleVel(float t, float v[3]) const {
        v[0] = v[1] = v[2] = 0.0f;
        MpPoseF a, b;
        if (sampleAt(t, &a) == MP_SAMPLE_NONE || sampleAt(t + 1.0f, &b) == MP_SAMPLE_NONE) return false;
        if (((a.flags ^ b.flags) & MP_POSE_VISIBLE) != 0) return false;
        for (int i = 0; i < 3; i++) v[i] = b.pos[i] - a.pos[i];
        if (!(MpPoseMath::len3(v) < kCorrCut)) {
            v[0] = v[1] = v[2] = 0.0f;
            return false;
        }
        return true;
    }

    // Steps the critically damped correction spring to `now` (exact analytical closed-form step; idempotent on the same turn).
    void stepCorr(uint32_t now) {
        int32_t turns = shown ? MpPoseMath::turnDiff(now, corrTurn) : 0;
        if (turns < 0 || turns > 30) { dropCorr(); turns = 0; }   // not shown for a while: nothing to glide from
        if (turns > 0) {
            const float w = kCorrW;
            const float e = expf(-w);
            for (int k = 0; k < turns; k++) {
                float nextC[3], nextV[3], delta[3];
                for (int i = 0; i < 3; i++) {
                    const float c0 = corr[i], v0 = corrV[i];
                    const float c1 = (c0 + (v0 + w * c0)) * e;
                    const float v1 = (v0 - w * (v0 + w * c0)) * e;
                    nextC[i] = c1;
                    nextV[i] = v1;
                    delta[i] = c1 - c0;
                }
                const float dl = MpPoseMath::len3(delta);
                if (dl > kCorrStepMax) {
                    const float sc = kCorrStepMax / dl;
                    for (int i = 0; i < 3; i++) {
                        corr[i] += delta[i] * sc;
                        corrV[i] = delta[i] * sc;
                    }
                } else {
                    for (int i = 0; i < 3; i++) {
                        corr[i] = nextC[i];
                        corrV[i] = nextV[i];
                    }
                }
                {
                    const float f0 = corrF, fv0 = corrFV;
                    corrF = (f0 + (fv0 + w * f0)) * e;
                    corrFV = (fv0 - w * (fv0 + w * f0)) * e;
                }
                if (corrQ[3] < 0.99999f) {
                    for (int i = 0; i < 3; i++) corrQ[i] *= kCorrRotKeep;
                    corrQ[3] = 1.0f - (1.0f - corrQ[3]) * kCorrRotKeep;
                    MpPoseMath::quatNormalize(corrQ);
                }
            }
        }
        corrTurn = now;
    }

    // (netsmooth) `pre` was shown, `post` is what the same render point shows now: step existing spring to `now`, then fold in
    // position error `d = pre - post` and bounded velocity continuity `vPre - vPost` so packet arrival is C1-smooth.
    void addCorr(uint32_t now, const MpPoseF& pre, const MpPoseF& post, const float vPre[3], const float vPost[3], bool haveVel) {
        using namespace MpPoseMath;
        stepCorr(now);
        float d[3] = {pre.pos[0] - post.pos[0], pre.pos[1] - post.pos[1], pre.pos[2] - post.pos[2]};
        float c[3] = {corr[0] + d[0], corr[1] + d[1], corr[2] + d[2]};
        if (((pre.flags ^ post.flags) & MP_POSE_VISIBLE) != 0 || !(len3(d) < kCorrCut) || !(len3(c) < kCorrCut)) {
            if (len3(d) > 1.0f) snaps++;
            dropCorr();
            return;
        }
        for (int i = 0; i < 3; i++) corr[i] = c[i];
        if (haveVel) {
            float dv[3] = {vPre[0] - vPost[0], vPre[1] - vPost[1], vPre[2] - vPost[2]};
            const float dvl = len3(dv);
            if (dvl > 0.0f && dvl < 120.0f) {
                const float maxDv = kCorrStepMax * 2.5f;   // allow 90-deg turn C1 velocity matching while bounding spikes
                const float sc = dvl > maxDv ? (maxDv / dvl) : 1.0f;
                for (int i = 0; i < 3; i++) corrV[i] += dv[i] * sc;
            }
        }
        const float df = pre.frame - post.frame;
        if (pre.anim == post.anim && ((pre.flags ^ post.flags) & MP_POSE_SIMPLE_BCK) == 0 && fabsf(df) < kCorrFrames) {
            if (corrAnim != post.anim) { corrF = 0.0f; corrFV = 0.0f; }
            corrF += df;
            corrAnim = post.anim;
        } else {
            corrF = 0.0f; corrFV = 0.0f;
        }
        // rotation: shown = corrQ * raw; keep corrQ_old * pre = corrQ_new * post
        if (quatAngle(pre.quat, post.quat) < 1.6f) {
            float inv[4] = {-post.quat[0], -post.quat[1], -post.quat[2], post.quat[3]};
            float dq[4], q[4];
            quatMul(pre.quat, inv, dq);
            quatMul(corrQ, dq, q);
            quatNormalize(q);
            for (int i = 0; i < 4; i++) corrQ[i] = q[i];
        } else {
            corrQ[0] = corrQ[1] = corrQ[2] = 0.0f; corrQ[3] = 1.0f;
        }
    }

    static void quatMul(const float a[4], const float b[4], float o[4]) {
        o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    }

    // (netsmooth) Steps the correction spring to `now` and applies it to `out`.
    void applyCorr(uint32_t now, MpPoseF* out) {
        stepCorr(now);
        shown = true;
        if (corr[0] != 0.0f || corr[1] != 0.0f || corr[2] != 0.0f) {
            for (int i = 0; i < 3; i++) out->pos[i] += corr[i];
        }
        if (corrF != 0.0f && out->anim == corrAnim) {
            out->frame += corrF;
            if (out->frame < 0.0f) out->frame = 0.0f;
        }
        if (corrQ[3] < 0.99999f) {
            float q[4];
            quatMul(corrQ, out->quat, q);
            MpPoseMath::quatNormalize(q);
            for (int i = 0; i < 4; i++) out->quat[i] = q[i];
        }
    }

    MpSampleResult sample(uint32_t now, MpPoseF* out) {
        const float jump = advance(now);
        const float rel = renderRel(now);
        MpSampleResult r = sampleAt(rel, out);
        if (r == MP_SAMPLE_NONE) return r;
        if (jump > 0.0f && shown && MpPoseMath::turnDiff(now, corrTurn) <= 2) {
            MpPoseF pre;
            const float relPre = rel - jump;
            if (sampleAt(relPre, &pre) != MP_SAMPLE_NONE) {
                float vPre[3] = {0.0f, 0.0f, 0.0f}, vPost[3] = {0.0f, 0.0f, 0.0f};
                const bool hv = sampleVel(relPre, vPre) && sampleVel(rel, vPost);
                addCorr(now, pre, *out, vPre, vPost, hv);   // a clock snap: glided
            }
        }
        applyCorr(now, out);
        return r;
    }

    // t: render time relative to the newest sample, in turns. The raw pose (no error correction).
    MpSampleResult sampleAt(float t, MpPoseF* out) const {
        using namespace MpPoseMath;
        if (count == 0) return MP_SAMPLE_NONE;
        const MpPoseF& nw = s[count - 1];
        if (t >= 0.0f || count == 1) {
            *out = nw;
            float e = t > 0.0f ? t : 0.0f;
            float v[3] = {nw.vel[0], nw.vel[1], nw.vel[2]};
            int32_t dt = 0;
            if (count >= 2) {
                const MpPoseF& pv = s[count - 2];
                dt = turnDiff(nw.turn, pv.turn);
                if (((nw.flags ^ pv.flags) & MP_POSE_VISIBLE) != 0) {
                    v[0] = v[1] = v[2] = 0.0f;           // (netsmooth) shown again: a teleport while hidden, not a motion
                    dt = 0;
                } else if (dt >= 1 && dt <= 30) {
                    for (int i = 0; i < 3; i++) v[i] = (nw.pos[i] - pv.pos[i]) / (float)dt;
                } else {
                    dt = 0;
                }
            }
            float speed = len3(v);
            float cap = speed > kFastSpeed ? 2.0f : tMaxExtrap;
            if (!(speed < 400.0f)) { v[0] = v[1] = v[2] = 0.0f; }   // a warp, not a motion
            float ee = e < cap ? e : cap;
            const float te = extrapDistTime(ee);
            // (netsmooth) the ground and gravity: split the velocity along the player's up (gravity up)
            float up[3];
            quatUp(nw.quat, up);
            float vu = dot3(v, up);
            float acc = 0.0f;   // along up, units/turn^2
            if (dt != 0 && (nw.flags & MP_POSE_IN_WATER) == 0) {
                const MpPoseF& pv = s[count - 2];
                if (nw.flags & MP_POSE_ON_GROUND) {
                    if ((pv.flags & MP_POSE_ON_GROUND) == 0) {
                        vu = 0.0f;                                  // just landed: the fall ends on the ground
                    } else {
                        float h2 = speed * speed - vu * vu;         // walking a slope: keep it, never steeper than 45 degrees
                        float h = h2 > 0.0f ? sqrtf(h2) : 0.0f;
                        vu = clampf(vu, -h, h);
                    }
                } else if (count >= 3 && (pv.flags & MP_POSE_ON_GROUND) == 0 && (s[count - 3].flags & MP_POSE_ON_GROUND) == 0) {
                    const MpPoseF& p0 = s[count - 3];
                    int32_t d0 = turnDiff(pv.turn, p0.turn);
                    if (d0 >= 1 && d0 <= 30) {                      // in the air: the arc (acceleration along up, measured)
                        float u0 = 0.0f, u1 = 0.0f;
                        for (int i = 0; i < 3; i++) {
                            u0 += (pv.pos[i] - p0.pos[i]) * up[i];
                            u1 += (nw.pos[i] - pv.pos[i]) * up[i];
                        }
                        acc = clampf(2.0f * (u1 / (float)dt - u0 / (float)d0) / (float)(dt + d0), -4.0f, 0.5f);
                        vu += acc * 0.5f * (float)dt;               // (the difference is the speed half an interval ago)
                    }
                }
            }
            float fall = vu + acc * te;                             // the extrapolated fall stays plausible
            if (fall < -kMaxFallSpeed && acc < 0.0f) {
                float tf = (-kMaxFallSpeed - vu) / acc;             // the time it reaches the limit
                tf = clampf(tf, 0.0f, te);
                float du = vu * tf + 0.5f * acc * tf * tf - kMaxFallSpeed * (te - tf);
                for (int i = 0; i < 3; i++) out->pos[i] = nw.pos[i] + (v[i] - up[i] * dot3(v, up)) * te + up[i] * du;
            } else {
                const float du = vu * te + 0.5f * acc * te * te;
                const float v0u = dot3(v, up);
                for (int i = 0; i < 3; i++) out->pos[i] = nw.pos[i] + (v[i] - up[i] * v0u) * te + up[i] * du;
            }
            out->frame = nw.frame + nw.rate * ee;
            if (out->frame < 0.0f) out->frame = 0.0f;
            if (nw.upperAnim != 0) {
                out->upperFrame = nw.upperFrame + nw.rate * ee;
                if (out->upperFrame < 0.0f) out->upperFrame = 0.0f;
            }
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
        if (k >= 1.0f) { *out = B; return MP_SAMPLE_INTERP; }
        *out = A;   // discrete fields (anim, flags, form, tracks) follow the older sample until the newer one is reached
        float d[3] = {B.pos[0] - A.pos[0], B.pos[1] - A.pos[1], B.pos[2] - A.pos[2]};
        float dist = len3(d);
        const bool visFlip = ((A.flags ^ B.flags) & MP_POSE_VISIBLE) != 0;
        if (visFlip) {                              // visibility toggle: hold A's full transform until B is reached (no sliding/spinning toward a hidden pose)
        } else {
            if (dist <= 2500.0f) {                  // a warp: no sliding across the level
                float ua[3], ub[3];
                quatUp(A.quat, ua);
                quatUp(B.quat, ub);
                const float cosT = dot3(ua, ub);
                if (cosT < 0.9998f) {
                    arcLerp(A.pos, ua, B.pos, ub, k, out->pos);
                } else {
                    // Monotone Fritsch-Carlson PCHIP cubic Hermite on flat/nearly-flat motion: C1-smooth curves, zero overshoot, exact on straight lines.
                    const uint16_t contMask = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
                    const bool sameGnd = ((A.flags ^ B.flags) & contMask) == 0;
                    bool havePrev = false, haveNext = false;
                    float dt0 = 0.0f, dt2 = 0.0f;
                    if (sameGnd && a > 0) {
                        const MpPoseF& P0 = s[a - 1];
                        const int32_t d0 = turnDiff(A.turn, P0.turn);
                        float dp0[3] = {A.pos[0] - P0.pos[0], A.pos[1] - P0.pos[1], A.pos[2] - P0.pos[2]};
                        if (d0 >= 1 && d0 <= 10 && ((P0.flags ^ A.flags) & contMask) == 0 && len3(dp0) <= 2500.0f) {
                            havePrev = true;
                            dt0 = (float)d0;
                        }
                    }
                    if (sameGnd && a + 2 < count) {
                        const MpPoseF& P2 = s[a + 2];
                        const int32_t d2 = turnDiff(P2.turn, B.turn);
                        float dp2[3] = {P2.pos[0] - B.pos[0], P2.pos[1] - B.pos[1], P2.pos[2] - B.pos[2]};
                        if (d2 >= 1 && d2 <= 10 && ((P2.flags ^ B.flags) & contMask) == 0 && len3(dp2) <= 2500.0f) {
                            haveNext = true;
                            dt2 = (float)d2;
                        }
                    }
                    if (!havePrev && !haveNext) {
                        for (int i = 0; i < 3; i++) out->pos[i] = lerpf(A.pos[i], B.pos[i], k);
                    } else {
                        const float k2 = k * k, k3 = k2 * k;
                        const float h00 = 2.0f * k3 - 3.0f * k2 + 1.0f;
                        const float h10 = k3 - 2.0f * k2 + k;
                        const float h01 = -2.0f * k3 + 3.0f * k2;
                        const float h11 = k3 - k2;
                        for (int i = 0; i < 3; i++) {
                            const float m1 = d[i] / span;
                            float ta = m1, tb = m1;
                            if (havePrev) {
                                const float m0 = (A.pos[i] - s[a - 1].pos[i]) / dt0;
                                ta = (m0 * m1 > 0.0f) ? (2.0f * m0 * m1 / (m0 + m1)) : 0.0f;
                            }
                            if (haveNext) {
                                const float m2 = (s[a + 2].pos[i] - B.pos[i]) / dt2;
                                tb = (m1 * m2 > 0.0f) ? (2.0f * m1 * m2 / (m1 + m2)) : 0.0f;
                            }
                            out->pos[i] = h00 * A.pos[i] + h10 * (ta * span) + h01 * B.pos[i] + h11 * (tb * span);
                        }
                    }
                }
            }
            quatSlerp(A.quat, B.quat, k, out->quat);
            out->scale = lerpf(A.scale, B.scale, k);
            for (int i = 0; i < 3; i++) out->vel[i] = lerpf(A.vel[i], B.vel[i], k);
        }
        float dtA = t - ra;
        const bool sameAnim = A.anim == B.anim && (B.flags & MP_POSE_SIMPLE_BCK) == (A.flags & MP_POSE_SIMPLE_BCK);
        if (sameAnim) {
            for (int i = 0; i < 4; i++) out->weights[i] = lerpf(A.weights[i], B.weights[i], k);
        }
        if (sameAnim && B.frame >= A.frame) {
            out->frame = lerpf(A.frame, B.frame, k);
        } else {
            out->frame = A.frame + A.rate * dtA;    // looped or changed: advance A on its own rate
            if (out->frame < 0.0f) out->frame = 0.0f;
        }
        if (A.upperAnim != 0) {
            if (A.upperAnim == B.upperAnim && B.upperFrame >= A.upperFrame) {
                out->upperFrame = lerpf(A.upperFrame, B.upperFrame, k);
            } else {
                out->upperFrame = A.upperFrame + A.rate * dtA;   // upper-body loop wrap or anim change: advance on A's rate
                if (out->upperFrame < 0.0f) out->upperFrame = 0.0f;
            }
        }
        return MP_SAMPLE_INTERP;
    }
};

// ---- (perf) STATE send pacing -------------------------------------------------------------------------------------------------
// At most one STATE per kMinNs of wall-clock time: every 2nd logic turn at 60 turns/s (33 ms apart, +-5 ms of jitter per turn
// still passes), never every turn (17 ms), and never faster than ~45/s when turns run faster than real time (a cutscene being
// skipped runs as many turns as fit in a frame). Independent of the picture rate (30 or 60 fps): the logic turn is 60 Hz in both.
struct MpSendGate {
    static constexpr int64_t kMinNs = 22000000;        // 22 ms: default wall-clock gate (used when SMG_MP_NET_SEND_MS overrides it)
    static constexpr int64_t kTurnMinNs = 18000000;    // 18 ms floor on 2-turn cadence (blocks fast-forward flood without dropping a jittery 2-turn beat to 3)
    static constexpr int64_t kSlowFallbackNs = 40000000;  // 40 ms fallback: allows 1-turn send under severe slowdown
    static inline int64_t tMinNs = kMinNs;             // (netlog) SMG_MP_NET_SEND_MS (port/mp_nettune.h); the constant unless the key is set
    int64_t last;
    uint32_t lastTurn;
    bool haveTurn;
    void reset() { last = 0; lastTurn = 0; haveTurn = false; }
    bool due(int64_t nowNs) {
        if (last != 0 && nowNs - last < tMinNs && nowNs >= last) return false;
        last = nowNs;
        return true;
    }
    // Steady logic-turn cadence (every 2nd turn = exact 30 Hz logic dt=2) when using the default send interval, with wall-clock
    // flood guard (kTurnMinNs) and slowdown fallback (kSlowFallbackNs). Explicit SMG_MP_NET_SEND_MS keeps pure wall-clock pacing.
    bool dueTurn(uint32_t turn, int64_t nowNs) {
        if (tMinNs != kMinNs) {
            if (!due(nowNs)) return false;
            lastTurn = turn;
            haveTurn = true;
            return true;
        }
        if (!haveTurn || last == 0 || nowNs < last) {
            last = nowNs;
            lastTurn = turn;
            haveTurn = true;
            return true;
        }
        const int32_t dt = MpPoseMath::turnDiff(turn, lastTurn);
        if (dt < 0) {
            last = nowNs;
            lastTurn = turn;
            return true;
        }
        if (dt == 0) return false;
        const int64_t dn = nowNs - last;
        if (dt >= 2) {
            if (dn < kTurnMinNs) return false;
        } else {
            if (dn < kSlowFallbackNs) return false;
        }
        last = nowNs;
        lastTurn = turn;
        return true;
    }
};

// ---- resource-name hash -> name ----------------------------------------------------------------------------------------------
struct MpAnimNameMap {
    enum { CAP = 1536 };
    struct Entry {
        uint32_t hash;
        const char* name;    // nullptr: two different names share this hash (ambiguous, never resolved)
    };
    Entry e[CAP];
    int n;
    int overflow;
    int collisions;
    bool sorted;

    void clear() { n = 0; overflow = 0; collisions = 0; sorted = false; }

    void add(uint32_t hash, const char* name) {
        if (name == nullptr) return;
        if (n >= CAP) { overflow++; return; }
        e[n].hash = hash; e[n].name = name; n++;
        sorted = false;
    }

    // Shell sort (no allocation, no <algorithm>), then mark collisions.
    void finish() {
        static const int gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
        for (int g : gaps) {
            for (int i = g; i < n; i++) {
                Entry t = e[i];
                int j = i;
                for (; j >= g && e[j - g].hash > t.hash; j -= g) e[j] = e[j - g];
                e[j] = t;
            }
        }
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (w > 0 && e[w - 1].hash == e[i].hash) {
                if (e[w - 1].name != nullptr && strcmp(e[w - 1].name, e[i].name) != 0) { e[w - 1].name = nullptr; collisions++; }
                continue;
            }
            e[w++] = e[i];
        }
        n = w;
        sorted = true;
    }

    const char* find(uint32_t hash) const {
        if (!sorted) return nullptr;
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (e[mid].hash == hash) return e[mid].name;
            if (e[mid].hash < hash) lo = mid + 1; else hi = mid - 1;
        }
        return nullptr;
    }
};

// ---- name tags ---------------------------------------------------------------------------------------------------------------
// UTF-8 -> UTF-16 (BMP; anything else, and malformed bytes, become '?'). Always NUL terminates when cap > 0. Returns length.
template < typename C >
static inline int mp_utf8_to_utf16(const char* s, C* out, int cap) {
    if (cap <= 0) return 0;
    int n = 0;
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    while (*p != 0 && n < cap - 1) {
        uint32_t c = *p;
        int extra = 0;
        if (c < 0x80) { p++; }
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; p++; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; p++; }
        else if ((c & 0xF8) == 0xF0) { c = 0x110000; extra = 3; p++; }
        else { out[n++] = (C)'?'; p++; continue; }
        bool bad = false;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) { bad = true; break; }
            if (c != 0x110000) c = (c << 6) | (*p & 0x3F);
            p++;
        }
        if (bad || c > 0xFFFF || (c >= 0xD800 && c <= 0xDFFF) || (extra == 1 && c < 0x80) || (extra == 2 && c < 0x800)) c = '?';
        out[n++] = (C)c;
    }
    out[n] = 0;
    return n;
}

// Clamps a projected point to a rectangle inset by `margin` on a w x h screen. Points behind the camera are mirrored and always
// pushed to the edge. Returns true when the point was moved (the tag is an off-screen marker).
static inline bool mp_edge_clamp(float sx, float sy, bool behind, float w, float h, float margin, float* ox, float* oy) {
    float cx = w * 0.5f, cy = h * 0.5f;
    float vx = sx - cx, vy = sy - cy;
    if (!MpPoseMath::finitef(vx) || !MpPoseMath::finitef(vy)) { vx = 0.0f; vy = 0.0f; behind = true; }
    if (behind) { vx = -vx; vy = -vy; }
    float hw = cx - margin, hh = cy - margin;
    if (hw < 1.0f) hw = 1.0f;
    if (hh < 1.0f) hh = 1.0f;
    float k = fabsf(vx) / hw;
    float ky = fabsf(vy) / hh;
    if (ky > k) k = ky;
    if (!behind && k <= 1.0f) { *ox = sx; *oy = sy; return false; }
    if (k < 1.0e-6f) { vx = 0.0f; vy = hh; k = 1.0f; }
    *ox = cx + vx / k;
    *oy = cy + vy / k;
    return true;
}

// ---- (perf) name tag visibility and fade ---------------------------------------------------------------------------------------
// Whether a puppet's name tag is wanted this turn, and its fade. Setting nameTags: 0 off, 1 on, 2 on + edge markers.
//   - mode 1: hidden when the puppet is off screen (its LOD cull), the head projects off screen / behind the camera, or the puppet
//     is far (beyond kFarIn units from the camera, back within kFarOut);
//   - mode 2: the edge marker for an off-screen or far puppet is what the player asked for: shown (clamped to the edge).
// The fade runs kFadeTurns turns each way; the plate is dead (no layout movement, calc or draw) only once it reached 0.
struct MpTagVis {
    static constexpr float kFarIn = 6000.0f;
    static constexpr float kFarOut = 5500.0f;
    static constexpr int kFadeTurns = 8;
    float alpha;   // 0..1
    bool far;
    void reset() { alpha = 0.0f; far = false; }
    // dist2: squared distance camera -> head; edge: the head projects off screen or behind the camera (mp_edge_clamp)
    bool wanted(int mode, bool drawable, bool culled, float dist2, bool edge) {
        far = far ? dist2 > kFarOut * kFarOut : dist2 > kFarIn * kFarIn;
        if (mode == 0 || !drawable) return false;
        if (mode == 2) return true;
        return !culled && !edge && !far;
    }
    // One turn of fade toward `want`. Returns the alpha; 0 = the plate can be dead.
    float step(bool want) {
        const float d = 1.0f / (float)kFadeTurns;
        alpha = want ? (alpha + d > 1.0f ? 1.0f : alpha + d) : (alpha - d < 0.0f ? 0.0f : alpha - d);
        if (alpha < 0.0001f) alpha = 0.0f;
        if (alpha > 0.9999f) alpha = 1.0f;
        return alpha;
    }
};

// ---- Toad retarget ---------------------------------------------------------------------------------------------------------
// Toad (Kinopio) has none of Mario's animations: Mario's state (the bck name of his current animation, velocity, flags) is mapped
// onto Toad's own clips. Pure logic, tested on the host. `prev` (the anim chosen last turn) gives hysteresis to walk/run.
enum MpToadAnim {
    MP_TOAD_WAIT = 0, MP_TOAD_WALK, MP_TOAD_RUN, MP_TOAD_SPIN, MP_TOAD_AIR_UP, MP_TOAD_AIR_DOWN, MP_TOAD_SWIM, MP_TOAD_TUMBLE,
    MP_TOAD_TRAMPLED, MP_TOAD_JOY, MP_TOAD_FREEZE, MP_TOAD_ANIM_COUNT
};

struct MpToadOut {
    int anim;            // MpToadAnim
    float rate;          // bck rate (when not held)
    float holdFrame;     // >= 0: stop on this frame
    float hop;           // extra y scale (a small stretch while rising)
};

static inline const char* mp_toad_anim_name(int a) {
    static const char* const cNames[MP_TOAD_ANIM_COUNT] = {"Wait",      "Walk",   "Run",    "Spin",     "Run",   "Surprised",
                                                           "SwimWait", "Tumble", "Trampled", "Joy", "Freeze"};
    return (a >= 0 && a < MP_TOAD_ANIM_COUNT) ? cNames[a] : cNames[0];
}

static inline char mp_lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static inline bool mp_has_ci(const char* s, const char* sub) {
    if (s == nullptr || sub == nullptr) return false;
    for (; *s != '\0'; s++) {
        const char* a = s;
        const char* b = sub;
        while (*a != '\0' && *b != '\0' && mp_lower_ascii(*a) == mp_lower_ascii(*b)) { a++; b++; }
        if (*b == '\0') return true;
    }
    return false;
}

static inline bool mp_starts_ci(const char* s, const char* pre) {
    if (s == nullptr || pre == nullptr) return false;
    for (; *pre != '\0'; s++, pre++) {
        if (*s == '\0' || mp_lower_ascii(*s) != mp_lower_ascii(*pre)) return false;
    }
    return true;
}

// What Mario's bck name says, in mp_toad_map's order of precedence (the first that matches). MP3 profile: a pure function of the
// name, so the Toad puppet keeps it while the bck stays the same (about 14 case-insensitive substring scans less a turn).
enum MpToadNameClass { MP_TOAD_NAME_NONE = 0, MP_TOAD_NAME_DIE, MP_TOAD_NAME_JOY, MP_TOAD_NAME_DAMAGE, MP_TOAD_NAME_SPIN, MP_TOAD_NAME_SWIM };

static inline int mp_toad_name_class(const char* marioBck) {
    const char* n = marioBck != nullptr ? marioBck : "";
    if (mp_starts_ci(n, "die") || mp_has_ci(n, "swimdie")) return MP_TOAD_NAME_DIE;
    if (mp_has_ci(n, "starget") || mp_has_ci(n, "getpower") || mp_has_ci(n, "resultwait") || mp_has_ci(n, "winghost") ||
        mp_has_ci(n, "elementget"))
        return MP_TOAD_NAME_JOY;
    if (mp_has_ci(n, "damage") || mp_has_ci(n, "dameg") || mp_has_ci(n, "flick")) return MP_TOAD_NAME_DAMAGE;
    if (mp_has_ci(n, "spin")) return MP_TOAD_NAME_SPIN;
    if (mp_starts_ci(n, "swim")) return MP_TOAD_NAME_SWIM;
    return MP_TOAD_NAME_NONE;
}

static inline MpToadOut mp_toad_map_class(const MpPoseF& p, int cls, int prev) {
    using namespace MpPoseMath;
    MpToadOut o = {MP_TOAD_WAIT, 1.0f, -1.0f, 1.0f};
    float up[3];
    quatUp(p.quat, up);
    float vu = dot3(p.vel, up);
    float hv[3] = {p.vel[0] - up[0] * vu, p.vel[1] - up[1] * vu, p.vel[2] - up[2] * vu};
    float h = len3(hv);
    if (!MpPoseMath::finitef(h)) h = 0.0f;
    if (!MpPoseMath::finitef(vu)) vu = 0.0f;

    if ((p.flags & MP_POSE_DEAD) || cls == MP_TOAD_NAME_DIE) {
        o.anim = MP_TOAD_FREEZE;
    } else if (cls == MP_TOAD_NAME_JOY) {
        o.anim = MP_TOAD_JOY;
    } else if (p.flags & MP_POSE_SQUASHED) {
        o.anim = MP_TOAD_TRAMPLED;
    } else if (cls == MP_TOAD_NAME_DAMAGE) {
        o.anim = (p.flags & MP_POSE_ON_GROUND) ? MP_TOAD_TRAMPLED : MP_TOAD_TUMBLE;
    } else if (cls == MP_TOAD_NAME_SPIN) {
        o.anim = MP_TOAD_SPIN;
    } else if ((p.flags & MP_POSE_IN_WATER) || cls == MP_TOAD_NAME_SWIM) {
        o.anim = MP_TOAD_SWIM;
        o.rate = clampf(0.8f + h / 8.0f, 0.8f, 2.0f);
    } else if (!(p.flags & MP_POSE_ON_GROUND)) {
        if (vu > 1.0f) {
            o.anim = MP_TOAD_AIR_UP;     // a held frame of the run, slightly stretched
            o.holdFrame = 6.0f;
            o.hop = 1.08f;
        } else {
            o.anim = MP_TOAD_AIR_DOWN;   // falling: "surprised", held
            o.holdFrame = 8.0f;
        }
    } else {
        const bool moving = prev == MP_TOAD_WALK || prev == MP_TOAD_RUN;
        const float walkOn = moving ? 0.6f : 1.2f;
        const float runOn = prev == MP_TOAD_RUN ? 7.0f : 9.0f;
        if (h < walkOn) {
            o.anim = MP_TOAD_WAIT;
        } else if (h < runOn) {
            o.anim = MP_TOAD_WALK;
            o.rate = clampf(h / 3.0f, 0.5f, 2.0f);
        } else {
            o.anim = MP_TOAD_RUN;
            o.rate = clampf(h / 9.0f, 0.8f, 2.2f);
        }
    }
    return o;
}

static inline MpToadOut mp_toad_map(const MpPoseF& p, const char* marioBck, int prev) {
    return mp_toad_map_class(p, mp_toad_name_class(marioBck), prev);
}

// ---- characters (MpLook) -----------------------------------------------------------------------------------------------------
static inline bool mp_character_is_toad(int c) { return c == MP_LOOK_TOAD || c == MP_LOOK_YELLOW_TOAD; }
static inline bool mp_character_is_cosmic(int c) { return c == MP_LOOK_COSMIC || c == MP_LOOK_COSMIC_LUIGI; }
static inline bool mp_character_luigi_swap(int c) { return c == MP_LOOK_LUIGI || c == MP_LOOK_COSMIC_LUIGI; }

static inline const char* mp_character_name(int c) {
    switch (c) {
    case MP_LOOK_MARIO: return "Mario";
    case MP_LOOK_LUIGI: return "Luigi";
    case MP_LOOK_TOAD: return "Toad";
    case MP_LOOK_COSMIC: return "Cosmic";
    case MP_LOOK_YELLOW_TOAD: return "YellowToad";
    case MP_LOOK_COSMIC_LUIGI: return "CosmicLuigi";
    default: return "?";
    }
}

// The model a character's puppet uses.
static inline const char* mp_character_model(int c) {
    switch (c) {
    case MP_LOOK_LUIGI: return "Luigi";
    case MP_LOOK_COSMIC: return "GhostMario";
    case MP_LOOK_COSMIC_LUIGI: return "GhostLuigi";
    case MP_LOOK_TOAD:
    case MP_LOOK_YELLOW_TOAD: return "Kinopio";
    default: return "Mario";
    }
}

// The archive a character needs beyond the stationed set (which holds only the MarioActor's own character); nullptr: none.
static inline const char* mp_character_archive(int c, bool localLuigi) {
    switch (c) {
    case MP_LOOK_MARIO: return localLuigi ? "Mario" : nullptr;
    case MP_LOOK_LUIGI: return localLuigi ? nullptr : "Luigi";
    case MP_LOOK_TOAD:
    case MP_LOOK_YELLOW_TOAD: return "Kinopio";
    case MP_LOOK_COSMIC: return "GhostMario";
    case MP_LOOK_COSMIC_LUIGI: return "GhostLuigi";
    default: return nullptr;
    }
}

// ---- player forms on a puppet (MP3 puppetprep) ------------------------------------------------------------------------------
// A puppet never loads a form's own model or archive (BeeMario, IceMario, HopperMario, BoneMario, ... are the local MarioActor's
// extras): every form is the character's plain model with the form's colour on its material (Mario / Luigi) and, for Mario /
// Luigi, the "ColorChange" btp frame (fire / flying). Nothing depends on a resource that this scene may not have loaded, so a
// form picked up in a stage the watcher never preloaded (or a Bee Mushroom taken by a drop-in's partner) cannot crash a puppet.
// Cosmic and Toad puppets keep their own model and materials (a shimmer / the NPC buffer): the form is ignored, plain character.
// An unknown form id (a newer build, or garbage) is the normal form.

struct MpFormLook {
    uint8_t form;       // the clamped form
    bool tinted;        // the material colour differs from white (Mario / Luigi only)
    uint8_t r, g, b;    // the material colour
    int btpFrame;       // "ColorChange" btp frame (0 normal, 1 flying, 2 fire); -1: the puppet has no such btp (Cosmic, Toad)
    bool ownModel;      // the form needs a model of its own on this puppet: never (always false)
    uint8_t alpha;      // 255 solid; less: drawn with the see-through list at this alpha (Boo: a ghost) (Mario / Luigi only)
};

static inline MpFormLook mp_form_look(uint32_t formId, int character, uint32_t rainbow) {
    MpFormLook o;
    o.form = mp_form_clamp(formId);
    o.tinted = false; o.r = o.g = o.b = 255; o.btpFrame = -1; o.ownModel = false; o.alpha = 255;
    if (mp_character_is_toad(character) || mp_character_is_cosmic(character)) return o;
    o.btpFrame = o.form == MP_FORM_FIRE ? 2 : (o.form == MP_FORM_FLYING ? 1 : 0);
    switch (o.form) {
    case MP_FORM_ICE: o.r = 170; o.g = 215; o.b = 255; break;
    case MP_FORM_BEE: o.r = 255; o.g = 225; o.b = 110; break;
    case MP_FORM_BOO: o.r = 235; o.g = 235; o.b = 255; o.alpha = 0xA0; break;   // (see-through: a Boo)
    case MP_FORM_SPRING: o.r = 200; o.g = 200; o.b = 210; break;
    case MP_FORM_BONE: o.r = 90; o.g = 90; o.b = 110; break;
    case MP_FORM_RAINBOW: {
        static const uint8_t cycle[6][3] = {{255, 90, 90}, {255, 220, 80}, {90, 255, 110}, {80, 220, 255}, {120, 120, 255}, {255, 100, 255}};
        const uint8_t* k = cycle[(rainbow / 3) % 6];
        o.r = k[0]; o.g = k[1]; o.b = k[2];
        break;
    }
    default: break;
    }
    o.tinted = o.r != 255 || o.g != 255 || o.b != 255;
    return o;
}

// Is the local player's body hidden from the others? The game hides Mario's own model for events (cannon, pipe, black hole, warp:
// h481 / h482 / h483 of MarioActor::isAllHidden, a hidden model flag). Boo Mario sets h483 and MR::isPlayerHidden() (= player mode
// Boo) because his own body is replaced by the Boo model: for the others he is still there, so the Boo form is never "hidden"
// by those two (he is drawn see-through, mp_form_look).
static inline bool mp_pose_hidden(bool h481, bool h482, bool h483, bool modeHidden, bool hiddenModelFlag, uint32_t formId) {
    const bool boo = mp_form_clamp(formId) == MP_FORM_BOO;
    return h481 || h482 || hiddenModelFlag || (!boo && (h483 || modeHidden));
}

// MpWarm (puppetprep): where a puppet's warm draw happens: cleanly INSIDE the frustum at cam + fwd * (nearZ + 120),
// identity rotation, tiny 0.001 scale (behind the closed opening wipe), so both solid (variant=0) and ghost (variant=1)
// pass StaticRunCulled and compile/warm their shaders on the back end. The base matrix is built from the DRAWN pose
// (formation), so this pose goes there and every mutated field is restored afterwards.
struct MpWarmPose {
    float pos[3];
    float rot[3][3];
    float scale;
};
static inline MpWarmPose mp_warm_pose(float camX, float camY, float camZ,
                                      float fwdX = 0.0f, float fwdY = 0.0f, float fwdZ = -1.0f,
                                      float nearZ = 100.0f) {
    MpWarmPose w;
    const float dist = (nearZ > 1.0f ? nearZ : 100.0f) + 120.0f;
    w.pos[0] = camX + fwdX * dist;
    w.pos[1] = camY + fwdY * dist;
    w.pos[2] = camZ + fwdZ * dist;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) w.rot[r][c] = r == c ? 1.0f : 0.0f;
    w.scale = 0.001f;
    return w;
}

// Kinopio colorchange.brk register colours read from the file: 0 blue, 1 green, 2 purple, 3 red, 4 yellow.
static inline float mp_toad_color_frame(int c) { return c == MP_LOOK_YELLOW_TOAD ? 4.0f : 3.0f; }
