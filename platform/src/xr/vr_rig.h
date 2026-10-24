// VR camera rig: places the game world in the player's space as a diorama.
//
// Mario is held near a fixed anchor in front of the player and the world is
// scaled down around him (without Mario, the point the game camera watches).
// The world's up follows his anti-gravity direction.  Its yaw stays where it
// is: the game camera swinging round Mario turned the whole world around the
// player, the strongest cause of nausea; the player turns it in steps (snap
// turns), and it takes the game camera's yaw again only on a cut (a new
// area).  The game camera's pitch and roll never move the player's head.
// Moving follows smoothly with a small dead zone, ordinary jumps do not move
// the world, and a sudden change of gravity (a wall that becomes the floor)
// happens at once behind a short blink instead of turning the world.
#pragma once

#include "../gx/gl_renderer.h"
#include "xmath.h"

namespace vr {

struct RigParams {
    float scale = 1.0f / 500.0f;             // metres per game unit
    xm::Vec3 anchor = {0.0f, -0.8f, -1.5f};  // stage position of the watched point
    float posHalfLife = 0.12f;               // seconds
    float rotHalfLife = 0.35f;               // seconds
    float snapDistance = 3000.0f;            // game units: jump instead of glide
    float followDeadZone = 100.0f;           // game units Mario moves along the ground before the world follows
    float gravitySnapAngle = 0.785f;         // radians (45 deg): gravity jumping this far at once snaps behind a blink
    float minEyeElevation = 0.17f;           // radians (10 deg): the eye above Mario's horizon; below, the up catches up
    float maxUpLag = 1.75f;                  // radians (100 deg): the up trailing further snaps behind a blink
    float maxCatchUpRate = 2.0f;             // radians per second the up turns at most while catching up
    bool turnWithCamera = false;             // the old way: the yaw follows the game camera, smoothed
    float jumpDeadZone = 350.0f;             // game units Mario rises off the ground before the world follows
    float landingHalfLife = 0.25f;           // seconds: back on Mario after landing
};

struct RigState {
    bool valid = false;
    bool snapped = false;  // the last update jumped instead of gliding
    float turnRate = 0.0f; // radians per second the world turned in the last update
    xm::Vec3 pivot{0, 0, 0};
    xm::Vec3 up{0, 1, 0};
    xm::Vec3 forward{0, 0, -1};
    // While Mario is off the ground the pivot stays this far below him
    // (0..jumpDeadZone), so the world holds still under a jump.
    float lift = 0.0f;
    bool centred = false;  // the last update centred Mario
    xm::Vec3 lastPlayer{0, 0, 0};
    // Snap turns asked for (radians about up; positive: the view goes round
    // Mario to the right), done at the next update.
    float pendingTurn = 0.0f;
    // Mario's up followed within about 0.1 s: gravity that jumps away from it
    // changed suddenly (a switch, a room's wall), not by Mario running round
    // a small planet (that it keeps up with).
    xm::Vec3 recentUp{0, 1, 0};
};

// Advances the rig towards the camera's framing by dt seconds and returns
// stage-from-view (the game's view space to the player's space, metres).
xm::Mat4 updateRig(RigState& rig, const RigParams& params, const gpu::CameraInfo& cam, float dt);

// Stage-space point / direction -> game world, for the rig's current state.
xm::Vec3 stageToWorld(const RigState& rig, const RigParams& params, xm::Vec3 p);
xm::Vec3 stageDirToWorld(const RigState& rig, xm::Vec3 d);

// Row-major copy for gpu::EyeView.
void toRowMajor(const xm::Mat4& m, float out[16]);

// Fills the eye's cutaway around the player (gpu::EyeView::focus / cut) for
// eye-from-game-view `eyeFromView`.  `amount` (0..1) grows it in and out;
// `widen` scales the cone (tests).
void setCutaway(gpu::EyeView& ev, const xm::Mat4& eyeFromView, const gpu::CameraInfo& cam, float amount, float widen = 1.0f);

}  // namespace vr
