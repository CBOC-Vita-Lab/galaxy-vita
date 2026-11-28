#pragma once
// Gravity-aware analog Free Camera for Super Mario Galaxy (Vita 3.0.1, freecam31).
//
// Pure, standalone C++20 math and state machine (no decomp or SDK headers) so host unit tests
// can verify spherical planets, upside-down gravity, parallel-transport world-anchored orbit,
// gimbal poles, pitch clamping without wind-up, yaw wrapping, sign conventions, Triangle recenter
// behind Mario, subjective-view freeze, auto-yield on 2D/fixed/cutscene transitions, and
// line-of-sight wall/floor collision pull-in under ASan+UBSan.
#include <cmath>
#include <cstdint>

namespace port::freecam {

enum RightStickMode : int {
    kModeFree = 0,
    kModeClassic = 1,
    kModePointer = 2,
};

// Bitmask preserved for 3.0 settings.ini compatibility:
//   invert_camera = 1 in 3.0 meant Left / Right (X) inversion -> bit 0 = X, bit 1 = Y.
enum InvertMask : int {
    kInvertOff = 0,
    kInvertX = 1,
    kInvertY = 2,
    kInvertBoth = 3,
};

constexpr int kSpeedLevels = 10;
constexpr int kDefaultSpeed = 5;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kDegToRad = kPi / 180.0f;

// Pitch limits relative to local planet horizon (camera eye elevation above watch point):
// -5 deg lets the player tilt upward cleanly without burying the near plane under flat floors;
// +72 deg gives a clear top-down platforming view well clear of the 90 deg pole.
constexpr float kMinPitchRad = -5.0f * kDegToRad;
constexpr float kMaxPitchRad = 72.0f * kDegToRad;
constexpr float kDefaultRecenterPitchRad = 22.0f * kDegToRad;

// Collision pull-in parameters: stay kWallMargin in front of blocking map geometry, keep
// kFloorClearance above floor polygons (matching CameraViewInterpolator::mBinder radius ~131 u),
// and stay >= kMinCamDist from Mario unless a wall is closer than kMinCamDist.
constexpr float kWallMargin = 85.0f;
constexpr float kFloorClearance = 135.0f;
constexpr float kMinCamDist = 220.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline bool IsFinite(float v) { return std::isfinite(v); }
inline bool IsFinite(const Vec3& v) { return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z); }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Scale(const Vec3& v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float LengthSq(const Vec3& v) { return Dot(v, v); }
inline float Length(const Vec3& v) { return std::sqrt(LengthSq(v)); }

inline bool Normalize(Vec3& v, float eps = 1e-5f) {
    if (!IsFinite(v)) {
        v = {0.0f, 0.0f, 0.0f};
        return false;
    }
    const float len = Length(v);
    if (!(len > eps)) {
        v = {0.0f, 0.0f, 0.0f};
        return false;
    }
    const float inv = 1.0f / len;
    v = {v.x * inv, v.y * inv, v.z * inv};
    return true;
}

inline float WrapAngle(float a) {
    if (!IsFinite(a)) return 0.0f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

// Rodrigues' rotation formula: rotate `v` around unit `axis` by `angle` radians.
inline Vec3 RotateAroundAxis(const Vec3& v, const Vec3& axis, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const Vec3 kCrossV = Cross(axis, v);
    const float kDotV = Dot(axis, v);
    return {
        v.x * c + kCrossV.x * s + axis.x * (kDotV * (1.0f - c)),
        v.y * c + kCrossV.y * s + axis.y * (kDotV * (1.0f - c)),
        v.z * c + kCrossV.z * s + axis.z * (kDotV * (1.0f - c)),
    };
}

// Any unit vector orthogonal to unit `axis` (never zero).
inline Vec3 AnyPerpendicular(const Vec3& axis) {
    const Vec3 ref = std::fabs(axis.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    Vec3 p = Cross(ref, axis);
    if (!Normalize(p)) return {1.0f, 0.0f, 0.0f};
    return p;
}

// Signed angle from unit `a` to unit `b` around unit `up` (in [-pi, pi]).
inline float SignedAngleAround(const Vec3& a, const Vec3& b, const Vec3& up) {
    const float c = Clamp(Dot(a, b), -1.0f, 1.0f);
    const float s = Dot(up, Cross(a, b));
    return std::atan2(s, c);
}

// Parallel-transport vector `v` by the minimal rotation taking unit `fromUp` to unit `toUp`.
inline Vec3 TransportVector(const Vec3& v, const Vec3& fromUp, const Vec3& toUp) {
    const float d = Clamp(Dot(fromUp, toUp), -1.0f, 1.0f);
    if (d > 0.99999f) return v;
    if (d < -0.9999f) {
        const Vec3 axis = AnyPerpendicular(fromUp);
        return RotateAroundAxis(v, axis, kPi);
    }
    Vec3 axis = Cross(fromUp, toUp);
    if (!Normalize(axis)) return v;
    return RotateAroundAxis(v, axis, std::acos(d));
}

// Clamp speed level to [1, 10].
inline int ClampSpeedLevel(int level) {
    return level < 1 ? 1 : (level > kSpeedLevels ? kSpeedLevels : level);
}

// Max angular speeds (rad/s) at full stick deflection for speed levels 1..10 (level 5 = Normal).
// Level 5 (145 deg/s = 2.42 deg/turn at 60Hz) stays comfortably inside GXV_HRW's 4 deg/turn window.
inline float YawMaxRadPerSec(int level) {
    static constexpr float kYawDeg[kSpeedLevels] = {60.0f, 80.0f, 100.0f, 122.0f, 145.0f, 168.0f, 192.0f, 215.0f, 238.0f, 260.0f};
    return kYawDeg[ClampSpeedLevel(level) - 1] * kDegToRad;
}

inline float PitchMaxRadPerSec(int level) {
    static constexpr float kPitchDeg[kSpeedLevels] = {42.0f, 54.0f, 68.0f, 82.0f, 96.0f, 112.0f, 128.0f, 144.0f, 160.0f, 176.0f};
    return kPitchDeg[ClampSpeedLevel(level) - 1] * kDegToRad;
}

// Smooth response curve: precise small adjustments + brisk full deflection.
inline float ShapeStick(float raw) {
    if (!IsFinite(raw)) return 0.0f;
    const float v = Clamp(raw, -1.0f, 1.0f);
    const float a = std::fabs(v);
    if (a < 0.02f) return 0.0f;
    const float shaped = 0.35f * a + 0.65f * a * a;
    return v < 0.0f ? -shaped : shaped;
}

struct StepInput {
    float dt = 1.0f / 60.0f;         // Fixed 1/60 s simulation tick (Galaxy always simulates at 60 Hz)
    float stickX = 0.0f;             // right positive (-1..1)
    float stickY = 0.0f;             // up positive (-1..1)
    bool enabled = false;            // Free Camera mode active
    bool eligible = false;           // current camera & stage allow Free Camera
    bool hold = false;               // freeze current orbit angle without decaying (e.g. first-person view)
    bool worldAnchored = false;      // true for free-tracking cameras (Follow, WonderPlanet, CubePlanet, Water, etc.)
    bool hardCut = false;            // scene/camera reset or instant cut
    bool resumePause = false;        // wall-clock pause/suspend gap detected -> zero velocity
    bool chunkChanged = false;       // CameraManGame switched chunks
    bool resetTrigger = false;       // Triangle / C pressed (Free Camera is sole owner when enabled+eligible)
    int invertMask = kInvertOff;     // bit 0 = X (Left/Right), bit 1 = Y (Up/Down)
    int speedLevel = kDefaultSpeed;
    Vec3 basePos{};
    Vec3 baseWatch{};
    Vec3 baseUp{0.0f, 1.0f, 0.0f};
    Vec3 watchUp{0.0f, 1.0f, 0.0f};
    Vec3 targetGravity{0.0f, -1.0f, 0.0f};
    Vec3 targetFront{0.0f, 0.0f, 1.0f};
};

struct StepOutput {
    bool modified = false;          // false: leave base camera pose bit-for-bit untouched
    bool handledReset = false;      // true: Free Camera handled Triangle recenter
    bool releasedNative = false;    // true: 2nd-tap Triangle returned to native view (clears CameraParallel mRoundAngle)
    bool needsLosCheck = false;     // true: caller should raycast watch->pos and call ApplyCollisionPull
    bool needsFloorCheck = false;   // true: caller should probe floor/ceiling at pulled pos and call ApplyFloorLift
    Vec3 pos{};
    Vec3 up{};
    float baseDist = 0.0f;
};

struct State {
    // Base-relative offsets (used on authored-heading cameras like CameraParallel / CameraTower).
    float yaw = 0.0f;
    float pitch = 0.0f;
    // World-anchored parallel-transport state (used on Follow / WonderPlanet / CubePlanet / Water so
    // walking across the invisible underlying camera never causes a 180-deg whip or camera wrestling).
    bool anchored = false;
    Vec3 anchorHoriz{0.0f, 0.0f, 1.0f};
    float anchorPitch = 0.0f;
    bool recenterRelease = false;  // true: release `anchored` back to neutral when recenter finishes

    float velYaw = 0.0f;
    float velPitch = 0.0f;
    bool recentering = false;
    float recenterTargetYaw = 0.0f;
    float recenterTargetPitch = 0.0f;
    Vec3 recenterTargetHoriz{0.0f, 0.0f, 1.0f};

    float pull = 1.0f;
    float floorLift = 0.0f;
    Vec3 smoothUp{0.0f, 1.0f, 0.0f};
    bool hasSmoothUp = false;
    Vec3 prevBaseHoriz{0.0f, 0.0f, 1.0f};
    bool hasPrevBaseHoriz = false;

    void ResetHard() {
        yaw = 0.0f;
        pitch = 0.0f;
        anchored = false;
        anchorHoriz = {0.0f, 0.0f, 1.0f};
        anchorPitch = 0.0f;
        recenterRelease = false;
        velYaw = 0.0f;
        velPitch = 0.0f;
        recentering = false;
        recenterTargetYaw = 0.0f;
        recenterTargetPitch = 0.0f;
        recenterTargetHoriz = {0.0f, 0.0f, 1.0f};
        pull = 1.0f;
        floorLift = 0.0f;
        hasSmoothUp = false;
        hasPrevBaseHoriz = false;
    }

    bool IsNeutral() const {
        return !anchored &&
               std::fabs(yaw) < 1e-4f && std::fabs(pitch) < 1e-4f &&
               std::fabs(velYaw) < 1e-4f && std::fabs(velPitch) < 1e-4f &&
               !recentering && pull >= 0.9995f && std::fabs(floorLift) < 0.5f;
    }

    // Current angular orbit speed (rad/s) for ViewInterpolator chord-lag reduction, HRW guard band,
    // GXV_LATE_TURN headroom, and GXV_CLIP_PAIR frustum lead padding.
    float TurnSpeedRadPerSec() const {
        const float v = std::hypot(velYaw, velPitch);
        const float rec = recentering ? (120.0f * kDegToRad) : 0.0f;
        const float speed = v > rec ? v : rec;
        return IsFinite(speed) ? speed : 0.0f;
    }

    // Smooth [0..1] turning activity blend (0 when still -> 1 at >= 90 deg/s orbit or recenter).
    float TurnBlend() const {
        return Clamp(TurnSpeedRadPerSec() / (90.0f * kDegToRad), 0.0f, 1.0f);
    }

    // Frustum side-plane padding factor for ClippingJudge when turning (pre-unclips leading edge
    // across GXV_CLIP_PAIR's undrawn/drawn pair without extra drawn-turn CPU cost).
    float ClipPadFactor(float fovyDeg) const {
        const float speed = TurnSpeedRadPerSec();
        if (speed < 0.08f || !IsFinite(fovyDeg)) return 1.0f;
        const float marginRad = speed * (1.5f / 60.0f);
        const float halfFov = Clamp(fovyDeg, 15.0f, 80.0f) * 0.5f * kDegToRad;
        const float t0 = std::tan(halfFov);
        if (!(t0 > 1e-4f)) return 1.0f;
        const float t1 = std::tan(Clamp(halfFov + marginRad, halfFov, 78.0f * kDegToRad));
        return Clamp(t1 / t0, 1.0f, 1.22f);
    }

    // Resolve true local vertical up: ALWAYS prefer -targetGravity (true planet radial up / stage up,
    // never a camera's pitched local up vector from WaterFollow/FooFighter/RailWatch), falling back
    // to watchUp / baseUp only in zero-G.
    Vec3 UpdateOrbitUp(const StepInput& in, float dt, Vec3& prevUpOut) {
        Vec3 targetUp = Scale(in.targetGravity, -1.0f);
        if (!Normalize(targetUp)) {
            targetUp = in.watchUp;
            if (!Normalize(targetUp)) {
                targetUp = in.baseUp;
                if (!Normalize(targetUp)) targetUp = {0.0f, 1.0f, 0.0f};
            }
        }
        if (!hasSmoothUp || in.hardCut) {
            smoothUp = targetUp;
            hasSmoothUp = true;
            prevUpOut = smoothUp;
            return smoothUp;
        }
        prevUpOut = smoothUp;
        const float d = Clamp(Dot(smoothUp, targetUp), -1.0f, 1.0f);
        if (d > 0.9999f) {
            smoothUp = targetUp;
            return smoothUp;
        }
        const float blend = 1.0f - std::exp(-dt / 0.08f);
        const float maxUpStep = Clamp(15.0f * (dt * 60.0f), 4.0f, 18.0f) * kDegToRad;
        const float stepAngle = std::fmin(std::acos(d) * blend, maxUpStep);
        if (d < -0.95f) {
            Vec3 axis = Cross(smoothUp, in.baseUp);
            if (!Normalize(axis)) axis = AnyPerpendicular(smoothUp);
            smoothUp = RotateAroundAxis(smoothUp, axis, stepAngle);
            if (!Normalize(smoothUp)) smoothUp = targetUp;
            return smoothUp;
        }
        Vec3 axis = Cross(smoothUp, targetUp);
        if (!Normalize(axis)) {
            smoothUp = targetUp;
            return smoothUp;
        }
        smoothUp = RotateAroundAxis(smoothUp, axis, stepAngle);
        if (!Normalize(smoothUp)) smoothUp = targetUp;
        return smoothUp;
    }

    // Convert world-anchored state cleanly back into base-relative (yaw, pitch) when leaving a
    // world-anchored camera zone or beginning a decay back to authored camera.
    void ConvertAnchorToRelative(const Vec3& baseHoriz, float basePitch, const Vec3& orbitUp) {
        if (!anchored) return;
        yaw = SignedAngleAround(baseHoriz, anchorHoriz, orbitUp);
        pitch = anchorPitch - basePitch;
        if (recentering) {
            if (recenterRelease) {
                recenterTargetYaw = 0.0f;
                recenterTargetPitch = 0.0f;
            } else {
                const float minDelta = std::fmin(0.0f, kMinPitchRad - basePitch);
                const float maxDelta = std::fmax(0.0f, kMaxPitchRad - basePitch);
                recenterTargetYaw = SignedAngleAround(baseHoriz, recenterTargetHoriz, orbitUp);
                recenterTargetPitch = Clamp(recenterTargetPitch - basePitch, minDelta, maxDelta);
            }
        }
        anchored = false;
        recenterRelease = false;
    }

    // Advance one simulation turn and return the orbited camera pose (or modified=false for zero-cost pass-through).
    StepOutput Step(const StepInput& in) {
        StepOutput out;
        out.pos = in.basePos;
        out.up = in.baseUp;

        if (!IsFinite(in.basePos) || !IsFinite(in.baseWatch)) {
            ResetHard();
            return out;
        }

        const float dt = IsFinite(in.dt) ? Clamp(in.dt, 0.004f, 0.05f) : (1.0f / 60.0f);
        // Cap automatic glides/decays well below MarioActor's 30 deg single-frame threshold.
        const float maxAutoStep = Clamp(11.0f * (dt * 60.0f), 3.0f, 16.0f) * kDegToRad;

        if (in.hardCut) {
            ResetHard();
        }
        if (in.resumePause) {
            velYaw = 0.0f;
            velPitch = 0.0f;
        }

        // Fast zero-cost pass-through when disabled and already neutral.
        if (!in.enabled && IsNeutral()) {
            return out;
        }

        Vec3 prevSmoothUp{0.0f, 1.0f, 0.0f};
        const Vec3 orbitUp = UpdateOrbitUp(in, dt, prevSmoothUp);

        // Parallel-transport stored world-anchored horizontal vectors across planet gravity curves.
        if (anchored) {
            anchorHoriz = TransportVector(anchorHoriz, prevSmoothUp, orbitUp);
            anchorHoriz = Sub(anchorHoriz, Scale(orbitUp, Dot(anchorHoriz, orbitUp)));
            if (!Normalize(anchorHoriz)) anchorHoriz = AnyPerpendicular(orbitUp);
            if (recentering) {
                recenterTargetHoriz = TransportVector(recenterTargetHoriz, prevSmoothUp, orbitUp);
                recenterTargetHoriz = Sub(recenterTargetHoriz, Scale(orbitUp, Dot(recenterTargetHoriz, orbitUp)));
                if (!Normalize(recenterTargetHoriz)) recenterTargetHoriz = anchorHoriz;
            }
        }

        const Vec3 baseOff = Sub(in.basePos, in.baseWatch);
        const float dist = Length(baseOff);
        if (!(dist > 1.0f)) {
            if (!in.eligible || !in.enabled) ResetHard();
            return out;
        }
        out.baseDist = dist;

        const Vec3 baseDir = Scale(baseOff, 1.0f / dist);
        const float baseSinPitch = Clamp(Dot(baseDir, orbitUp), -1.0f, 1.0f);
        const float basePitch = std::asin(baseSinPitch);

        // Horizontal projection of base camera offset (safe even for straight top-down cameras).
        Vec3 baseHoriz = Sub(baseOff, Scale(orbitUp, Dot(baseOff, orbitUp)));
        if (Length(baseHoriz) < dist * 1e-3f) {
            Vec3 fallback = Scale(in.baseUp, -1.0f);
            fallback = Sub(fallback, Scale(orbitUp, Dot(fallback, orbitUp)));
            if (Normalize(fallback)) {
                baseHoriz = fallback;
            } else if (hasPrevBaseHoriz) {
                Vec3 prev = Sub(prevBaseHoriz, Scale(orbitUp, Dot(prevBaseHoriz, orbitUp)));
                baseHoriz = Normalize(prev) ? prev : AnyPerpendicular(orbitUp);
            } else {
                baseHoriz = AnyPerpendicular(orbitUp);
            }
        } else {
            Normalize(baseHoriz);
        }

        const bool largeBaseShift = in.chunkChanged && hasPrevBaseHoriz && Dot(prevBaseHoriz, baseHoriz) < 0.70f;
        prevBaseHoriz = baseHoriz;
        hasPrevBaseHoriz = true;

        // First-person subjective view freeze: hold current orbit pose (and floorLift) without decaying
        // or reading stick so subjective zoom-in never corkscrews or drops, and exiting returns cleanly.
        if (in.hold) {
            velYaw = 0.0f;
            velPitch = 0.0f;
            if (IsNeutral()) return out;
            const Vec3 finalHoriz = anchored ? anchorHoriz : RotateAroundAxis(baseHoriz, orbitUp, yaw);
            const float finalPitch = Clamp(anchored ? anchorPitch : (basePitch + pitch), -85.0f * kDegToRad, 85.0f * kDegToRad);
            const Vec3 orbitedDir = Add(Scale(finalHoriz, std::cos(finalPitch)), Scale(orbitUp, std::sin(finalPitch)));
            const Vec3 orbitedPos = Add(Add(in.baseWatch, Scale(orbitedDir, dist * pull)), Scale(orbitUp, floorLift));
            Vec3 front = Sub(in.baseWatch, orbitedPos);
            if (!Normalize(front)) return out;
            Vec3 newUp = Sub(orbitUp, Scale(front, Dot(orbitUp, front)));
            if (!Normalize(newUp)) newUp = in.baseUp;
            out.modified = true;
            out.needsLosCheck = false;
            out.needsFloorCheck = false;
            out.pos = orbitedPos;
            out.up = newUp;
            return out;
        }

        // If we switched from a world-anchored camera to a base-relative authored camera (e.g. WonderPlanet -> Parallel),
        // convert seamlessly (including any in-flight recenter target) so there is no snap.
        if (anchored && !in.worldAnchored) {
            ConvertAnchorToRelative(baseHoriz, basePitch, orbitUp);
        }

        float sx = 0.0f;
        float sy = 0.0f;
        if (in.enabled && in.eligible) {
            sx = ShapeStick(in.stickX);
            sy = ShapeStick(in.stickY);
            if (in.invertMask & kInvertX) sx = -sx;
            if (in.invertMask & kInvertY) sy = -sy;
        }
        const bool stickActive = (sx != 0.0f || sy != 0.0f);

        // Disabled or ineligible (cutscene, 2D/2.5D, fixed shot, dead, etc.):
        // smoothly ease any active offset (and floorLift) back to zero with maxAutoStep cap and NO LOS/floor hijack.
        if (!in.enabled || !in.eligible) {
            if (anchored) ConvertAnchorToRelative(baseHoriz, basePitch, orbitUp);
            velYaw = 0.0f;
            velPitch = 0.0f;
            recentering = false;
            recenterRelease = false;
            const float glide = 1.0f - std::exp(-dt / 0.10f);
            const float stepY = Clamp(-yaw * glide, -maxAutoStep, maxAutoStep);
            const float stepP = Clamp(-pitch * glide, -maxAutoStep, maxAutoStep);
            yaw = WrapAngle(yaw + stepY);
            pitch += stepP;
            pull += (1.0f - pull) * (1.0f - std::exp(-dt / 0.08f));
            floorLift *= std::exp(-dt / 0.14f);
            if (std::fabs(yaw) < 2e-3f) yaw = 0.0f;
            if (std::fabs(pitch) < 2e-3f) pitch = 0.0f;
            if (pull > 0.9995f) pull = 1.0f;
            if (std::fabs(floorLift) < 0.5f) floorLift = 0.0f;
            if (IsNeutral()) return out;

            const Vec3 orbitedHoriz = RotateAroundAxis(baseHoriz, orbitUp, yaw);
            const float finalPitch = Clamp(basePitch + pitch, -85.0f * kDegToRad, 85.0f * kDegToRad);
            const Vec3 orbitedDir = Add(Scale(orbitedHoriz, std::cos(finalPitch)), Scale(orbitUp, std::sin(finalPitch)));
            // Apply already-decayed pull and floorLift directly; do NOT trigger LOS/floor probes on cutscenes/2D.
            const Vec3 orbitedPos = Add(Add(in.baseWatch, Scale(orbitedDir, dist * pull)), Scale(orbitUp, floorLift));
            Vec3 front = Sub(in.baseWatch, orbitedPos);
            if (!Normalize(front)) return out;
            Vec3 newUp = Sub(orbitUp, Scale(front, Dot(orbitUp, front)));
            if (!Normalize(newUp)) newUp = in.baseUp;
            out.modified = true;
            out.needsLosCheck = false;
            out.needsFloorCheck = false;
            out.pos = orbitedPos;
            out.up = newUp;
            return out;
        }

        // Eligible & enabled!
        // Compute Mario's behind-back horizontal direction (used for Triangle recenter).
        Vec3 wantBack = Scale(in.targetFront, -1.0f);
        wantBack = Sub(wantBack, Scale(orbitUp, Dot(wantBack, orbitUp)));
        const bool haveBehind = Normalize(wantBack);

        // Engage world-anchored orbit on free-tracking cameras whenever the player steers, recenters,
        // OR carries an active non-neutral offset/recenter across a zone boundary (e.g. Parallel -> Follow).
        if (in.worldAnchored && !anchored && (stickActive || in.resetTrigger || !IsNeutral())) {
            anchored = true;
            anchorHoriz = std::fabs(yaw) > 1e-4f ? RotateAroundAxis(baseHoriz, orbitUp, yaw) : baseHoriz;
            anchorPitch = Clamp(basePitch + pitch, -85.0f * kDegToRad, 85.0f * kDegToRad);
            if (recentering) {
                if (std::fabs(recenterTargetYaw) < 1e-4f && std::fabs(recenterTargetPitch) < 1e-4f) {
                    recenterRelease = true;
                    recenterTargetHoriz = baseHoriz;
                    recenterTargetPitch = Clamp(basePitch, -85.0f * kDegToRad, 85.0f * kDegToRad);
                } else {
                    recenterRelease = false;
                    recenterTargetHoriz = RotateAroundAxis(baseHoriz, orbitUp, recenterTargetYaw);
                    recenterTargetPitch = Clamp(basePitch + recenterTargetPitch, kMinPitchRad, kMaxPitchRad);
                }
            }
            yaw = 0.0f;
            pitch = 0.0f;
        }

        if (stickActive) {
            recentering = false;
            recenterRelease = false;
        } else if (!anchored && largeBaseShift && (std::fabs(yaw) > 0.02f || std::fabs(pitch) > 0.02f)) {
            recentering = true;
            recenterTargetYaw = 0.0f;
            recenterTargetPitch = 0.0f;
        }

        if (in.resetTrigger) {
            // Free Camera is sole owner of Triangle / C recenter when enabled & eligible:
            //   1st tap: smoothly swing directly behind Mario's facing vector.
            //   2nd tap (already behind Mario): glide back to the stage's native camera view.
            if (anchored) {
                const float errToBehind = haveBehind ? std::fabs(SignedAngleAround(anchorHoriz, wantBack, orbitUp)) : 0.0f;
                const float idealPitch = Clamp(basePitch, 14.0f * kDegToRad, 35.0f * kDegToRad);
                const bool alreadyAtBehind = haveBehind && errToBehind < 12.0f * kDegToRad && std::fabs(anchorPitch - idealPitch) < 6.0f * kDegToRad;
                if (haveBehind && !alreadyAtBehind) {
                    recentering = true;
                    recenterRelease = false;
                    recenterTargetHoriz = wantBack;
                    recenterTargetPitch = idealPitch;
                    velYaw = 0.0f;
                    velPitch = 0.0f;
                    out.handledReset = true;
                } else {
                    recentering = true;
                    recenterRelease = true;
                    recenterTargetHoriz = baseHoriz;
                    recenterTargetPitch = Clamp(basePitch, -85.0f * kDegToRad, 85.0f * kDegToRad);
                    velYaw = 0.0f;
                    velPitch = 0.0f;
                    out.handledReset = true;
                    out.releasedNative = true;
                }
            } else {
                const float behindYaw = haveBehind ? SignedAngleAround(baseHoriz, wantBack, orbitUp) : 0.0f;
                const float errToBehind = haveBehind ? std::fabs(WrapAngle(behindYaw - yaw)) : 0.0f;
                const bool alreadyAtBehind = haveBehind && errToBehind < 12.0f * kDegToRad && std::fabs(pitch) < 5.0f * kDegToRad;
                if (haveBehind && !alreadyAtBehind) {
                    recentering = true;
                    recenterTargetYaw = behindYaw;
                    recenterTargetPitch = 0.0f;
                    velYaw = 0.0f;
                    velPitch = 0.0f;
                    out.handledReset = true;
                } else {
                    if (std::fabs(yaw) > 2.0f * kDegToRad || std::fabs(pitch) > 2.0f * kDegToRad) {
                        recentering = true;
                        recenterTargetYaw = 0.0f;
                        recenterTargetPitch = 0.0f;
                        velYaw = 0.0f;
                        velPitch = 0.0f;
                    }
                    out.handledReset = true;
                    out.releasedNative = true;
                }
            }
        }

        if (anchored) {
            if (recentering) {
                const float minClamp = recenterRelease ? -85.0f * kDegToRad : kMinPitchRad;
                const float maxClamp = recenterRelease ? 85.0f * kDegToRad : kMaxPitchRad;
                const Vec3 targetH = recenterRelease ? baseHoriz : recenterTargetHoriz;
                const float targetP = recenterRelease ? Clamp(basePitch, minClamp, maxClamp) : recenterTargetPitch;
                const float glide = 1.0f - std::exp(-dt / 0.075f);
                const float dy = SignedAngleAround(anchorHoriz, targetH, orbitUp);
                const float dp = targetP - anchorPitch;
                const float stepY = Clamp(dy * glide, -maxAutoStep, maxAutoStep);
                const float stepP = Clamp(dp * glide, -maxAutoStep, maxAutoStep);
                anchorHoriz = RotateAroundAxis(anchorHoriz, orbitUp, stepY);
                anchorHoriz = Sub(anchorHoriz, Scale(orbitUp, Dot(anchorHoriz, orbitUp)));
                if (!Normalize(anchorHoriz)) anchorHoriz = targetH;
                anchorPitch = Clamp(anchorPitch + stepP, minClamp, maxClamp);
                if (std::fabs(SignedAngleAround(anchorHoriz, targetH, orbitUp)) < 0.004f &&
                    std::fabs(targetP - anchorPitch) < 0.004f) {
                    anchorHoriz = targetH;
                    anchorPitch = targetP;
                    recentering = false;
                    if (recenterRelease) {
                        anchored = false;
                        recenterRelease = false;
                        yaw = 0.0f;
                        pitch = 0.0f;
                    }
                }
            } else {
                const float targetVelYaw = -sx * YawMaxRadPerSec(in.speedLevel);
                const float targetVelPitch = -sy * PitchMaxRadPerSec(in.speedLevel);
                const float tau = stickActive ? 0.052f : 0.062f;
                const float alpha = 1.0f - std::exp(-dt / tau);
                velYaw += (targetVelYaw - velYaw) * alpha;
                velPitch += (targetVelPitch - velPitch) * alpha;
                if (!stickActive) {
                    if (std::fabs(velYaw) < 1e-3f) velYaw = 0.0f;
                    if (std::fabs(velPitch) < 1e-3f) velPitch = 0.0f;
                }
                if (velYaw != 0.0f) {
                    anchorHoriz = RotateAroundAxis(anchorHoriz, orbitUp, velYaw * dt);
                    anchorHoriz = Sub(anchorHoriz, Scale(orbitUp, Dot(anchorHoriz, orbitUp)));
                    if (!Normalize(anchorHoriz)) anchorHoriz = baseHoriz;
                }
                const float minP = std::fmin(kMinPitchRad, anchorPitch);
                const float maxP = std::fmax(kMaxPitchRad, anchorPitch);
                anchorPitch += velPitch * dt;
                if (anchorPitch < minP) {
                    anchorPitch = minP;
                    if (velPitch < 0.0f) velPitch = 0.0f;
                } else if (anchorPitch > maxP) {
                    anchorPitch = maxP;
                    if (velPitch > 0.0f) velPitch = 0.0f;
                }
            }
            if (anchored) {
                yaw = SignedAngleAround(baseHoriz, anchorHoriz, orbitUp);
                pitch = anchorPitch - basePitch;
            }
        } else {
            if (recentering) {
                const float glide = 1.0f - std::exp(-dt / 0.075f);
                const float dy = WrapAngle(recenterTargetYaw - yaw);
                const float dp = recenterTargetPitch - pitch;
                const float stepY = Clamp(dy * glide, -maxAutoStep, maxAutoStep);
                const float stepP = Clamp(dp * glide, -maxAutoStep, maxAutoStep);
                yaw = WrapAngle(yaw + stepY);
                pitch += stepP;
                if (std::fabs(WrapAngle(recenterTargetYaw - yaw)) < 0.003f &&
                    std::fabs(recenterTargetPitch - pitch) < 0.003f) {
                    yaw = WrapAngle(recenterTargetYaw);
                    pitch = recenterTargetPitch;
                    recentering = false;
                }
            } else {
                const float targetVelYaw = -sx * YawMaxRadPerSec(in.speedLevel);
                const float targetVelPitch = -sy * PitchMaxRadPerSec(in.speedLevel);
                const float tau = stickActive ? 0.052f : 0.062f;
                const float alpha = 1.0f - std::exp(-dt / tau);
                velYaw += (targetVelYaw - velYaw) * alpha;
                velPitch += (targetVelPitch - velPitch) * alpha;
                if (!stickActive) {
                    if (std::fabs(velYaw) < 1e-3f) velYaw = 0.0f;
                    if (std::fabs(velPitch) < 1e-3f) velPitch = 0.0f;
                }
                yaw = WrapAngle(yaw + velYaw * dt);
                pitch += velPitch * dt;
            }
            const float minDelta = std::fmin(0.0f, kMinPitchRad - basePitch);
            const float maxDelta = std::fmax(0.0f, kMaxPitchRad - basePitch);
            if (pitch < minDelta) {
                pitch = minDelta;
                if (velPitch < 0.0f) velPitch = 0.0f;
            } else if (pitch > maxDelta) {
                pitch = maxDelta;
                if (velPitch > 0.0f) velPitch = 0.0f;
            }
        }

        // Exact zero-cost pass-through if neutral and not world-anchored.
        if (!anchored && std::fabs(yaw) < 1e-4f && std::fabs(pitch) < 1e-4f &&
            pull >= 0.9995f && std::fabs(floorLift) < 0.5f) {
            yaw = 0.0f;
            pitch = 0.0f;
            pull = 1.0f;
            floorLift = 0.0f;
            return out;
        }

        const Vec3 orbitedHoriz = anchored ? anchorHoriz : RotateAroundAxis(baseHoriz, orbitUp, yaw);
        const float finalPitch = Clamp(anchored ? anchorPitch : (basePitch + pitch), -85.0f * kDegToRad, 85.0f * kDegToRad);
        const Vec3 orbitedDir = Add(Scale(orbitedHoriz, std::cos(finalPitch)), Scale(orbitUp, std::sin(finalPitch)));
        const Vec3 orbitedPos = Add(in.baseWatch, Scale(orbitedDir, dist));

        Vec3 front = Sub(in.baseWatch, orbitedPos);
        if (!Normalize(front)) return out;
        Vec3 newUp = Sub(orbitUp, Scale(front, Dot(orbitUp, front)));
        if (!Normalize(newUp)) {
            newUp = in.baseUp;
            if (!Normalize(newUp)) newUp = AnyPerpendicular(front);
        }

        out.modified = true;
        out.needsLosCheck = true;
        out.needsFloorCheck = true;
        out.pos = orbitedPos;
        out.up = newUp;
        return out;
    }

    // Apply smooth line-of-sight wall collision pull-in after raycasting watchPos -> orbitedPos.
    // `hitFraction` is in (0, 1) if geometry was hit, or >= 1.0f if clear.
    // Floor clearance is handled continuously by ApplyFloorLift() (zero binary 1-frame pops).
    Vec3 ApplyCollisionPull(const Vec3& watchPos, const Vec3& orbitedPos, float hitFraction,
                            const Vec3& /*hitNormal*/, const Vec3& /*orbitUp*/, float dt) {
        const Vec3 off = Sub(orbitedPos, watchPos);
        const float dist = Length(off);
        if (!(dist > 1.0f)) return orbitedPos;
        const float clampedDt = IsFinite(dt) ? Clamp(dt, 0.004f, 0.05f) : (1.0f / 60.0f);

        float targetPull = 1.0f;
        const bool hasHit = IsFinite(hitFraction) && hitFraction > 0.0f && hitFraction < 1.0f;
        if (hasHit) {
            // Never push camera behind the struck wall even when Mario stands closer than kMinCamDist.
            const float safeLimit = Clamp(hitFraction * 0.85f, 0.05f, 1.0f);
            const float minFrac = Clamp(kMinCamDist / dist, 0.05f, safeLimit);
            const float marginFrac = kWallMargin / dist;
            targetPull = Clamp(hitFraction - marginFrac, minFrac, safeLimit);
        }
        const float tau = targetPull < pull ? 0.025f : 0.14f;
        pull += (targetPull - pull) * (1.0f - std::exp(-clampedDt / tau));
        if (pull > 0.9995f && targetPull >= 1.0f) pull = 1.0f;
        pull = Clamp(pull, 0.05f, 1.0f);
        return Add(watchPos, Scale(off, pull));
    }

    // Smooth continuous floor-clearance lift (fed by downward floor probe + upward ceiling clamp).
    // Rises fast (tau = 0.035s) before reaching mBinder / near-plane, falls gently (tau = 0.14s)
    // with zero binary 1-frame pops on flat floors or gentle ramps.
    Vec3 ApplyFloorLift(const Vec3& watchPos, const Vec3& pos, const Vec3& orbitUp,
                        float desiredLift, float dt, Vec3* outUp = nullptr) {
        const float clampedDt = IsFinite(dt) ? Clamp(dt, 0.004f, 0.05f) : (1.0f / 60.0f);
        const float targetLift = IsFinite(desiredLift) ? Clamp(desiredLift, 0.0f, kFloorClearance + 40.0f) : 0.0f;
        const float tau = targetLift > floorLift ? 0.035f : 0.14f;
        floorLift += (targetLift - floorLift) * (1.0f - std::exp(-clampedDt / tau));
        if (floorLift < 0.5f && targetLift <= 0.0f) floorLift = 0.0f;
        floorLift = Clamp(floorLift, 0.0f, kFloorClearance + 40.0f);

        const Vec3 lifted = Add(pos, Scale(orbitUp, floorLift));
        if (outUp != nullptr && floorLift > 0.5f) {
            Vec3 front = Sub(watchPos, lifted);
            if (Normalize(front)) {
                Vec3 newUp = Sub(orbitUp, Scale(front, Dot(orbitUp, front)));
                if (Normalize(newUp)) *outUp = newUp;
            }
        }
        return lifted;
    }

    // Convenience overload without floor normal (for callers/tests checking pure wall pull-in).
    Vec3 ApplyCollisionPull(const Vec3& watchPos, const Vec3& orbitedPos, float hitFraction, float dt) {
        return ApplyCollisionPull(watchPos, orbitedPos, hitFraction, {0.0f, 0.0f, 0.0f}, smoothUp, dt);
    }
};

}  // namespace port::freecam
