// Comprehensive ASan+UBSan host test for gravity-aware analog Free Camera (platform/include/port/free_camera.h),
// including regression tests for every Carbon adversarial audit finding.
#include "port/free_camera.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace port::freecam;

static float AngleBetween(const Vec3& a, const Vec3& b) {
    Vec3 ua = a, ub = b;
    assert(Normalize(ua) && Normalize(ub));
    return std::acos(Clamp(Dot(ua, ub), -1.0f, 1.0f));
}

static StepInput MakeFlatInput() {
    StepInput in;
    in.dt = 1.0f / 60.0f;
    in.enabled = true;
    in.eligible = true;
    in.speedLevel = kDefaultSpeed;
    in.baseWatch = {0.0f, 100.0f, 0.0f};
    // Camera at +Z, 1000 units back, 500 units up (elevation ~26.57 deg), looking toward -Z.
    in.basePos = {0.0f, 600.0f, 1000.0f};
    in.baseUp = {0.0f, 1.0f, 0.0f};
    in.watchUp = {0.0f, 1.0f, 0.0f};
    in.targetGravity = {0.0f, -1.0f, 0.0f};
    in.targetFront = {0.0f, 0.0f, -1.0f};  // Mario facing away from camera (-Z)
    return in;
}

int main() {
    // 1. Disabled / zero-input pass-through is exact bit-for-bit no-op.
    {
        State st;
        StepInput in = MakeFlatInput();
        in.enabled = false;
        in.stickX = 1.0f;
        in.stickY = 1.0f;
        StepOutput out = st.Step(in);
        assert(!out.modified && !out.needsLosCheck && !out.handledReset);
        assert(out.pos.x == in.basePos.x && out.pos.y == in.basePos.y && out.pos.z == in.basePos.z);

        in.enabled = true;
        in.stickX = 0.0f;
        in.stickY = 0.0f;
        out = st.Step(in);
        assert(!out.modified && st.IsNeutral());
    }

    // 2. Sign conventions & 3.0-compatible bitmask (kInvertX = 1 [bit 0], kInvertY = 2 [bit 1], kInvertBoth = 3).
    {
        State st;
        StepInput in = MakeFlatInput();
        Vec3 initFront = Sub(in.baseWatch, in.basePos);
        assert(Normalize(initFront));
        Vec3 initRight = Cross(initFront, {0.0f, 1.0f, 0.0f});
        assert(Normalize(initRight));  // +X

        // Non-inverted: stick Right (+X) turns view RIGHT; stick Up (+Y) tilts view UP (camera lowers).
        in.stickX = 1.0f;
        StepOutput out;
        for (int i = 0; i < 15; ++i) out = st.Step(in);
        assert(out.modified && out.needsLosCheck);
        Vec3 newFront = Sub(in.baseWatch, out.pos);
        assert(Normalize(newFront));
        assert(Dot(newFront, initRight) > 0.25f);
        assert(out.pos.x < -100.0f);

        st.ResetHard();
        in = MakeFlatInput();
        in.stickY = 1.0f;
        for (int i = 0; i < 15; ++i) out = st.Step(in);
        assert(out.modified);
        newFront = Sub(in.baseWatch, out.pos);
        assert(Normalize(newFront));
        assert(Dot(newFront, {0.0f, 1.0f, 0.0f}) > Dot(initFront, {0.0f, 1.0f, 0.0f}) + 0.1f);
        assert(out.pos.y < in.basePos.y - 50.0f);

        // kInvertX == 1: flips Left/Right only (matches 3.0 invert_camera = 1 upgrade semantics!).
        assert(kInvertX == 1 && kInvertY == 2 && kInvertBoth == 3);
        st.ResetHard();
        in = MakeFlatInput();
        in.invertMask = kInvertX;
        in.stickX = 1.0f;
        in.stickY = 1.0f;
        for (int i = 0; i < 15; ++i) out = st.Step(in);
        newFront = Sub(in.baseWatch, out.pos);
        assert(Normalize(newFront));
        assert(Dot(newFront, initRight) < -0.25f);  // X flipped!
        assert(out.pos.y < in.basePos.y - 50.0f);   // Y stays normal!

        // kInvertY == 2: flips Up/Down only.
        st.ResetHard();
        in = MakeFlatInput();
        in.invertMask = kInvertY;
        in.stickX = 1.0f;
        in.stickY = 1.0f;
        for (int i = 0; i < 15; ++i) out = st.Step(in);
        newFront = Sub(in.baseWatch, out.pos);
        assert(Normalize(newFront));
        assert(Dot(newFront, initRight) > 0.25f);   // X stays normal!
        assert(out.pos.y > in.basePos.y + 50.0f);   // Y flipped!

        // kInvertBoth == 3: flips both X and Y.
        st.ResetHard();
        in = MakeFlatInput();
        in.invertMask = kInvertBoth;
        in.stickX = 1.0f;
        in.stickY = 1.0f;
        for (int i = 0; i < 15; ++i) out = st.Step(in);
        newFront = Sub(in.baseWatch, out.pos);
        assert(Normalize(newFront));
        assert(Dot(newFront, initRight) < -0.25f && out.pos.y > in.basePos.y + 50.0f);
    }

    // 3. Pitch clamp without wind-up (-5 deg to +72 deg), immediate reversal on very next frame.
    {
        State st;
        StepInput in = MakeFlatInput();
        in.stickY = -1.0f;
        StepOutput out;
        for (int i = 0; i < 300; ++i) out = st.Step(in);
        Vec3 off = Sub(out.pos, in.baseWatch);
        const float elev = std::asin(Clamp(Dot(Scale(off, 1.0f / Length(off)), {0.0f, 1.0f, 0.0f}), -1.0f, 1.0f));
        assert(std::fabs(elev - kMaxPitchRad) < 0.02f && st.velPitch == 0.0f);

        const float clampedPitch = st.pitch;
        in.stickY = 1.0f;
        out = st.Step(in);
        assert(st.pitch < clampedPitch - 1e-4f);

        for (int i = 0; i < 300; ++i) out = st.Step(in);
        off = Sub(out.pos, in.baseWatch);
        const float elevLow = std::asin(Clamp(Dot(Scale(off, 1.0f / Length(off)), {0.0f, 1.0f, 0.0f}), -1.0f, 1.0f));
        assert(std::fabs(elevLow - kMinPitchRad) < 0.02f && st.velPitch == 0.0f);
    }

    // 4. HRW 60 Assist safety: at default Speed 5 (145 deg/s) and fixed dt=1/60s, single-turn
    //    angular rotation stays well below GXV_HRW_ROT_DEG (4.0 deg) even at diagonal full stick!
    {
        State st;
        StepInput in = MakeFlatInput();
        in.stickX = 0.707f;
        in.stickY = 0.707f;
        Vec3 prevFront = Sub(in.baseWatch, in.basePos);
        for (int i = 0; i < 60; ++i) {
            StepOutput out = st.Step(in);
            Vec3 curFront = Sub(in.baseWatch, out.pos);
            assert(AngleBetween(prevFront, curFront) < 3.5f * kDegToRad);
            prevFront = curFront;
        }
    }

    // 5. Speed levels 1..10 strictly monotonic.
    {
        float prevYaw = 0.0f;
        for (int lvl = 1; lvl <= kSpeedLevels; ++lvl) {
            State st;
            StepInput in = MakeFlatInput();
            in.speedLevel = lvl;
            in.stickX = 1.0f;
            for (int i = 0; i < 20; ++i) st.Step(in);
            const float mag = std::fabs(st.yaw);
            assert(mag > prevYaw);
            prevYaw = mag;
        }
    }

    // 6. CARBON #3 REGRESSION: World-anchored parallel transport on CameraFollow / CameraWonderPlanet.
    //    Walking Mario directly across the invisible underlying basePos (flipping baseHoriz by 180 deg)
    //    must cause ZERO visible camera whip!
    {
        State st;
        StepInput in = MakeFlatInput();
        in.worldAnchored = true;
        // Player orbits right for 15 frames -> engages world-anchored orbit.
        in.stickX = 1.0f;
        StepOutput out;
        for (int i = 0; i < 15; ++i) out = st.Step(in);
        assert(st.anchored && out.modified);
        in.stickX = 0.0f;
        for (int i = 0; i < 30; ++i) out = st.Step(in);  // let velocity settle
        Vec3 settledFront = Sub(in.baseWatch, out.pos);
        assert(Normalize(settledFront));

        // Now simulate Mario crossing directly past the invisible CameraWonderPlanet basePos
        // (basePos flips 180 deg from +Z to -Z and passes through zenith)!
        in.basePos = {0.0f, 1500.0f, 0.0f};  // zenith
        StepOutput outZenith = st.Step(in);
        in.basePos = {0.0f, 600.0f, -1000.0f};  // flipped 180 deg!
        StepOutput outFlipped = st.Step(in);
        Vec3 frontZ = Sub(in.baseWatch, outZenith.pos);
        Vec3 frontF = Sub(in.baseWatch, outFlipped.pos);
        assert(AngleBetween(settledFront, frontZ) < 1.0f * kDegToRad);
        assert(AngleBetween(settledFront, frontF) < 1.0f * kDegToRad);

        // Parallel transport across spherical planet curvature (gravity rotates 90 deg from -Y to -X):
        // world-anchored horizon rotates smoothly with the planet without flipping!
        Vec3 prevUp = st.smoothUp;
        for (int step = 1; step <= 45; ++step) {
            const float a = (step / 45.0f) * (kPi * 0.5f);
            in.targetGravity = {-std::sin(a), -std::cos(a), 0.0f};
            out = st.Step(in);
            assert(IsFinite(out.pos) && IsFinite(out.up));
            assert(std::fabs(Length(out.up) - 1.0f) < 1e-3f);
            assert(Dot(out.up, prevUp) > 0.5f);
            prevUp = st.smoothUp;
        }
    }

    // 7. CARBON #5 REGRESSION: CameraWaterFollow / CameraFooFighter pass pitched camera up as watchUp.
    //    Orbit axis must follow -targetGravity (true vertical), NOT pitched watchUp!
    {
        State st;
        StepInput in = MakeFlatInput();
        // Steeply pitched camera looking 60 deg down: watchUp is tilted 60 deg forward!
        in.basePos = {0.0f, 966.0f, 500.0f};
        in.watchUp = {0.0f, 0.5f, -0.866f};  // pitched camera local up (NOT gravity up!)
        in.targetGravity = {0.0f, -1.0f, 0.0f};  // true vertical!
        in.stickX = 1.0f;
        StepOutput out;
        for (int i = 0; i < 30; ++i) out = st.Step(in);
        // Camera height Y must stay constant when purely yawing around true vertical gravity!
        assert(std::fabs(out.pos.y - in.basePos.y) < 1.0f);
    }

    // 8. CARBON #4 REGRESSION: Triangle recenter sole ownership (both authored-heading & world-anchored).
    {
        // (a) Authored-heading camera (CameraParallel): 1st tap behind Mario, 2nd tap stage native (0, 0).
        State st;
        StepInput in = MakeFlatInput();
        in.targetFront = {1.0f, 0.0f, 0.0f};
        in.resetTrigger = true;
        StepOutput out = st.Step(in);
        assert(out.handledReset && st.recentering);
        in.resetTrigger = false;
        Vec3 prevFront = Sub(in.baseWatch, out.pos);
        assert(AngleBetween(Sub(in.baseWatch, in.basePos), prevFront) < 15.0f * kDegToRad);
        for (int i = 0; i < 60; ++i) {
            out = st.Step(in);
            Vec3 curFront = Sub(in.baseWatch, out.pos);
            assert(AngleBetween(prevFront, curFront) < 15.0f * kDegToRad);
            prevFront = curFront;
        }
        assert(!st.recentering);
        Vec3 finalFront = Sub(in.baseWatch, out.pos);
        finalFront.y = 0.0f;
        assert(Normalize(finalFront) && Dot(finalFront, in.targetFront) > 0.99f);

        // 2nd Triangle press returns smoothly to native stage view (0, 0).
        in.resetTrigger = true;
        out = st.Step(in);
        assert(out.handledReset && st.recentering);
        in.resetTrigger = false;
        for (int i = 0; i < 60; ++i) out = st.Step(in);
        assert(st.IsNeutral());

        // (b) World-anchored camera (CameraWonderPlanet / CameraFollow): 1st tap behind Mario, 2nd tap releases anchor.
        st.ResetHard();
        in = MakeFlatInput();
        in.worldAnchored = true;
        in.targetFront = {1.0f, 0.0f, 0.0f};
        in.resetTrigger = true;
        out = st.Step(in);
        assert(out.handledReset && st.anchored && st.recentering);
        in.resetTrigger = false;
        for (int i = 0; i < 60; ++i) out = st.Step(in);
        finalFront = Sub(in.baseWatch, out.pos);
        finalFront.y = 0.0f;
        assert(Normalize(finalFront) && Dot(finalFront, in.targetFront) > 0.99f);
        // 2nd tap releases world anchor back to neutral!
        in.resetTrigger = true;
        st.Step(in);
        in.resetTrigger = false;
        for (int i = 0; i < 60; ++i) st.Step(in);
        assert(!st.anchored && st.IsNeutral());
    }

    // 9. CARBON #2 & #6 REGRESSION: Auto-yield decay has maxAutoStep cap (NO 71 deg snap!)
    //    AND sets needsLosCheck = false so cutscenes/2D cameras can NEVER be raycast-hijacked!
    {
        State st;
        StepInput in = MakeFlatInput();
        in.stickX = 1.0f;
        for (int i = 0; i < 80; ++i) st.Step(in);  // large yaw (~180 deg)
        assert(std::fabs(st.yaw) > 2.0f);

        in.stickX = 0.0f;
        in.eligible = false;
        in.dt = 0.05f;  // worst-case clamped hitch
        const float prevYaw = st.yaw;
        StepOutput out = st.Step(in);
        assert(out.modified && !out.needsLosCheck);  // NEVER raycast on ineligible camera!
        assert(std::fabs(WrapAngle(st.yaw - prevYaw)) <= 17.0f * kDegToRad);  // well below 30 deg cut!

        in.dt = 1.0f / 60.0f;
        for (int i = 0; i < 120; ++i) out = st.Step(in);
        assert(st.IsNeutral() && !out.modified);
    }

    // 10. CARBON #14 REGRESSION: Subjective first-person view (`hold = true`) freezes orbit without decaying.
    {
        State st;
        StepInput in = MakeFlatInput();
        in.stickX = 1.0f;
        for (int i = 0; i < 30; ++i) st.Step(in);
        const float savedYaw = st.yaw;
        assert(std::fabs(savedYaw) > 0.5f);

        in.stickX = 0.0f;
        in.eligible = false;
        in.hold = true;
        for (int i = 0; i < 40; ++i) {
            StepOutput out = st.Step(in);
            assert(out.modified && !out.needsLosCheck);
            assert(std::fabs(st.yaw - savedYaw) < 1e-5f);  // frozen, zero corkscrew!
        }
    }

    // 11. CARBON #2 REGRESSION: Pause resume zeroes stale velocity (no unpause jerk).
    {
        State st;
        StepInput in = MakeFlatInput();
        in.stickX = 1.0f;
        for (int i = 0; i < 20; ++i) st.Step(in);
        assert(std::fabs(st.velYaw) > 1.0f);
        const float yawAtPause = st.yaw;

        in.stickX = 0.0f;
        in.resumePause = true;
        st.Step(in);
        assert(st.velYaw == 0.0f && std::fabs(st.yaw - yawAtPause) < 1e-5f);
    }

    // 12. CARBON #6 + PASS 2 #1 REGRESSION: Close-wall pull-in NEVER pushes behind a wall (`dist=1200, hit=100u`),
    //     and continuous ApplyFloorLift smoothly maintains >= 135u floor clearance with ZERO 1-frame 130u pop!
    {
        State st;
        const Vec3 watch{0.0f, 100.0f, 0.0f};
        const Vec3 cam{0.0f, 100.0f, 1200.0f};
        // Wall only 100u from Mario (hitFrac = 100/1200 = 0.0833 < kMinCamDist/1200 = 0.1833).
        Vec3 pulled = cam;
        for (int i = 0; i < 20; ++i) {
            pulled = st.ApplyCollisionPull(watch, cam, 100.0f / 1200.0f, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, 1.0f / 60.0f);
        }
        const float dClose = Length(Sub(pulled, watch));
        assert(dClose < 98.0f && dClose >= 60.0f);  // strictly IN FRONT of the 100u wall!

        // Continuous floor lift: camera hovering at y=20u above y=0 floor (desiredLift = 115u).
        // Must rise smoothly (no single-frame 115u teleport!), reach >= 134u clearance, and ease down smoothly when clear.
        st.ResetHard();
        const Vec3 watchFloor{0.0f, 170.0f, 0.0f};
        const Vec3 camLow{0.0f, 20.0f, 1200.0f};
        Vec3 prevLifted = camLow;
        Vec3 outUp{0.0f, 1.0f, 0.0f};
        for (int i = 0; i < 30; ++i) {
            const float desiredLift = kFloorClearance - camLow.y;
            Vec3 lifted = st.ApplyFloorLift(watchFloor, camLow, {0.0f, 1.0f, 0.0f}, desiredLift, 1.0f / 60.0f, &outUp);
            const float stepDelta = std::fabs(lifted.y - prevLifted.y);
            assert(stepDelta < 50.0f);  // smooth exponential rise, NEVER a 132u 1-frame pop!
            prevLifted = lifted;
        }
        assert(prevLifted.y >= kFloorClearance - 1.0f);

        // Floor lift stays preserved across subjective FP view (`hold = true`) and decays cleanly in cutscenes.
        StepInput in = MakeFlatInput();
        in.stickX = 1.0f;
        for (int i = 0; i < 15; ++i) st.Step(in);
        const float savedLift = st.floorLift;
        assert(savedLift > 100.0f);
        in.stickX = 0.0f;
        in.eligible = false;
        in.hold = true;
        StepOutput holdOut = st.Step(in);
        assert(holdOut.modified && !holdOut.needsFloorCheck && std::fabs(st.floorLift - savedLift) < 1e-4f);

        in.hold = false;  // cutscene decay
        for (int i = 0; i < 120; ++i) st.Step(in);
        assert(st.floorLift < 1.0f && st.IsNeutral());

        // Clear LOS eases back smoothly to full distance.
        st.pull = 0.3f;
        for (int i = 0; i < 90; ++i) {
            pulled = st.ApplyCollisionPull(watch, cam, 2.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.0f / 60.0f);
        }
        assert(std::fabs(Length(Sub(pulled, watch)) - 1200.0f) < 2.0f && st.pull >= 0.999f);
    }

    // 14. CARBON PASS 2 #2 REGRESSION: ConvertAnchorToRelative mid-Triangle-recenter must NOT chase
    //     stale recenterTargetYaw or treat absolute recenterTargetPitch as a base-relative delta!
    {
        State st;
        StepInput in = MakeFlatInput();
        in.worldAnchored = true;
        in.stickX = 1.0f;
        for (int i = 0; i < 40; ++i) st.Step(in);
        assert(st.anchored);

        // Trigger Triangle recenter behind Mario facing -X (wantBack = +X, ~180deg away from anchorHoriz ~ -X).
        in.stickX = 0.0f;
        in.targetFront = {-1.0f, 0.0f, 0.0f};
        in.resetTrigger = true;
        StepOutput out = st.Step(in);
        assert(out.handledReset && st.recentering && !st.recenterRelease);
        in.resetTrigger = false;

        // Cross into a base-relative CameraParallel mid-recenter!
        in.worldAnchored = false;
        out = st.Step(in);
        assert(!st.anchored && st.recentering);
        // Run to completion: must finish pointing along wantBack (+X), NOT off-axis or tilted into the sky!
        for (int i = 0; i < 90; ++i) out = st.Step(in);
        assert(!st.recentering);
        Vec3 finalH = Sub(out.pos, in.baseWatch);
        finalH.y = 0.0f;
        assert(Normalize(finalH));
        assert(Dot(finalH, {1.0f, 0.0f, 0.0f}) > 0.98f);
        // Pitch must land on basePitch (~26.57 deg), NEVER basePitch + 26.57 deg (~53 deg)!
        const float finalPitchDeg = std::asin(Clamp(Dot(Scale(Sub(out.pos, in.baseWatch), 1.0f / Length(Sub(out.pos, in.baseWatch))), {0.0f, 1.0f, 0.0f}), -1.0f, 1.0f)) * 180.0f / kPi;
        assert(std::fabs(finalPitchDeg - 26.57f) < 1.0f);
    }

    // 15. CARBON PASS 2 #3 REGRESSION: Walking from base-relative CameraParallel (with non-zero offset,
    //     stick released!) into worldAnchored CameraFollow engages anchored immediately (no camera wrestling).
    {
        State st;
        StepInput in = MakeFlatInput();
        in.worldAnchored = false;
        in.stickX = 1.0f;
        for (int i = 0; i < 40; ++i) st.Step(in);
        assert(!st.anchored && std::fabs(st.yaw) > 0.5f);

        // Release stick and enter CameraFollow (worldAnchored = true): must engage anchored immediately!
        in.stickX = 0.0f;
        in.worldAnchored = true;
        StepOutput out = st.Step(in);
        assert(st.anchored && out.modified);
        Vec3 hBefore = Sub(out.pos, in.baseWatch);
        hBefore.y = 0.0f;
        assert(Normalize(hBefore));

        // Now swing underlying CameraFollow basePos 180 deg behind Mario: world-anchored view must stay rock-solid!
        in.basePos = {0.0f, 500.0f, -1200.0f};
        out = st.Step(in);
        Vec3 hAfter = Sub(out.pos, in.baseWatch);
        hAfter.y = 0.0f;
        assert(Normalize(hAfter));
        assert(Dot(hBefore, hAfter) > 0.99f);
    }

    // 16. CARBON PASS 2 #5 & #8 REGRESSION: 2nd-tap Triangle release on steep basePitch=80deg glides
    //     all the way to 80deg without a 1-frame snap AND fires out.releasedNative (to clear CameraParallel::mRoundAngle).
    {
        State st;
        StepInput in = MakeFlatInput();
        in.worldAnchored = true;
        // Steep overhead native base camera (pitch = 80 deg).
        const float p80 = 80.0f * kDegToRad;
        in.basePos = {0.0f, 100.0f + 1200.0f * std::sin(p80), 1200.0f * std::cos(p80)};
        in.stickX = 1.0f;
        for (int i = 0; i < 20; ++i) st.Step(in);
        in.stickX = 0.0f;
        in.targetFront = {0.0f, 0.0f, -1.0f};  // Mario faces -Z -> wantBack = +Z
        // 1st tap: behind Mario
        in.resetTrigger = true;
        StepOutput out = st.Step(in);
        assert(out.handledReset && !out.releasedNative);
        in.resetTrigger = false;
        for (int i = 0; i < 60; ++i) out = st.Step(in);

        // 2nd tap: release to native (80 deg)! Must glide smoothly to 80 deg with no >12deg snap on release!
        in.resetTrigger = true;
        out = st.Step(in);
        assert(out.handledReset && out.releasedNative && st.recenterRelease);
        in.resetTrigger = false;
        Vec3 prevDir = Sub(out.pos, in.baseWatch);
        assert(Normalize(prevDir));
        for (int i = 0; i < 100; ++i) {
            out = st.Step(in);
            Vec3 curDir = Sub(out.pos, in.baseWatch);
            assert(Normalize(curDir));
            const float stepDeg = std::acos(Clamp(Dot(prevDir, curDir), -1.0f, 1.0f)) * 180.0f / kPi;
            assert(stepDeg < 13.0f);  // zero snap when anchored releases at 80deg!
            prevDir = curDir;
        }
        assert(!st.anchored && st.IsNeutral());
    }

    // 17. CARBON PASS 2 MINOR: Instant 180-deg gravity plate flip steps smoothUp by <= 15 deg/turn.
    {
        State st;
        StepInput in = MakeFlatInput();
        st.Step(in);
        assert(st.smoothUp.y > 0.99f);
        // Flip gravity 180 deg instantly!
        in.targetGravity = {0.0f, 1.0f, 0.0f};
        Vec3 prevUp = st.smoothUp;
        st.Step(in);
        const float flipDeg = std::acos(Clamp(Dot(prevUp, st.smoothUp), -1.0f, 1.0f)) * 180.0f / kPi;
        assert(flipDeg <= 15.5f);  // well below Mario's 30 deg movement-cut threshold!
    }

    // 18. NaN / degenerate inputs & hard cut.
    {
        State st;
        StepInput in = MakeFlatInput();
        in.stickX = std::numeric_limits<float>::quiet_NaN();
        in.stickY = std::numeric_limits<float>::infinity();
        in.dt = std::numeric_limits<float>::quiet_NaN();
        StepOutput out = st.Step(in);
        assert(IsFinite(out.pos) && IsFinite(out.up) && IsFinite(st.yaw) && IsFinite(st.pitch));

        in = MakeFlatInput();
        in.basePos = in.baseWatch;
        out = st.Step(in);
        assert(!out.modified && IsFinite(out.pos));

        in = MakeFlatInput();
        in.stickX = 1.0f;
        for (int i = 0; i < 20; ++i) st.Step(in);
        in.stickX = 0.0f;
        in.hardCut = true;
        out = st.Step(in);
        assert(!out.modified && st.IsNeutral());
    }

    // 19. Smoothness helpers: TurnSpeedRadPerSec(), TurnBlend(), and ClipPadFactor() for ViewInterpolator / ClipPair / HRW / LateTurn.
    {
        State st;
        StepInput in = MakeFlatInput();
        st.Step(in);
        assert(st.TurnSpeedRadPerSec() == 0.0f && st.TurnBlend() == 0.0f && st.ClipPadFactor(30.8f) == 1.0f);
        // Full right-stick orbit at default Speed 5 (145 deg/s) ramps TurnBlend smoothly to 1.0 and pads ClipPadFactor in (1.05, 1.22].
        in.stickX = 1.0f;
        float prevBlend = 0.0f;
        for (int i = 0; i < 20; ++i) {
            st.Step(in);
            const float b = st.TurnBlend();
            assert(b >= prevBlend - 1e-5f && b <= 1.0f);
            prevBlend = b;
        }
        assert(st.TurnBlend() > 0.99f);
        const float pad30 = st.ClipPadFactor(30.8f);
        assert(pad30 > 1.10f && pad30 <= 1.22f);
        // NaN / extreme FOV stays safe and finite.
        assert(st.ClipPadFactor(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
        assert(st.ClipPadFactor(1.0f) >= 1.0f && st.ClipPadFactor(1.0f) <= 1.22f);
        // Release stick: TurnBlend decays smoothly back to 0.0 and ClipPadFactor returns to exact 1.0.
        in.stickX = 0.0f;
        for (int i = 0; i < 45; ++i) st.Step(in);
        assert(st.TurnBlend() < 0.02f && st.ClipPadFactor(30.8f) == 1.0f);
        // Triangle recenter reports non-zero TurnBlend and ClipPadFactor > 1.0 throughout glide.
        in.resetTrigger = true;
        st.Step(in);
        assert(st.recentering && st.TurnBlend() > 0.99f && st.ClipPadFactor(30.8f) > 1.05f);
    }

    std::puts("PASS: free_camera (all Carbon Pass 1, Pass 2 & camera smoothness regressions verified)");
    return 0;
}
