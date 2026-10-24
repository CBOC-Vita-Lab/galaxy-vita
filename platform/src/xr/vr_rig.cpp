#include "vr_rig.h"

#include <math.h>

namespace vr {

namespace {

xm::Vec3 vec(const float* p) { return {p[0], p[1], p[2]}; }

// Removes the component of v along unit n.
xm::Vec3 flatten(xm::Vec3 v, xm::Vec3 n) { return v - n * xm::dot(v, n); }

float blend(float dt, float halfLife) { return halfLife > 0.0f ? 1.0f - exp2f(-dt / halfLife) : 1.0f; }

// Rotation taking unit vector a to unit vector b (the shortest way).
xm::Quat rotationBetween(xm::Vec3 a, xm::Vec3 b) {
    xm::Vec3 c = xm::cross(a, b);
    float d = xm::dot(a, b);
    if (d < -0.9999f) {
        // Opposite: half a turn about any axis at right angles to a.
        xm::Vec3 axis = xm::normalize(xm::cross(fabsf(a.x) < 0.9f ? xm::Vec3{1, 0, 0} : xm::Vec3{0, 1, 0}, a));
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    float w = 1.0f + d;
    float n = sqrtf(w * w + xm::dot(c, c));
    return {c.x / n, c.y / n, c.z / n, w / n};
}

// Turns `v` about unit `axis` by `angle` radians.
xm::Vec3 turn(xm::Vec3 v, xm::Vec3 axis, float angle) {
    float h = angle * 0.5f;
    xm::Quat q{axis.x * sinf(h), axis.y * sinf(h), axis.z * sinf(h), cosf(h)};
    return xm::rotate(q, v);
}

}  // namespace

xm::Mat4 updateRig(RigState& rig, const RigParams& params, const gpu::CameraInfo& cam, float dt) {
    // Camera basis in world space: the view matrix rows are the camera's
    // right / up / back axes.
    xm::Vec3 camUp{cam.view[4], cam.view[5], cam.view[6]};
    xm::Vec3 camBack{cam.view[8], cam.view[9], cam.view[10]};

    bool player = (cam.flags & PORT_GX_CAMERA_PLAYER) != 0;
    // Up is against Mario's gravity, so he always stands upright.  The
    // camera's watch-up vector is the fallback (while Mario has no gravity):
    // some camera modes report plain world up there, which turned the
    // diorama (and with it the controls) upside down on small planets.
    xm::Vec3 up = vec(cam.playerUp);
    if (!player || xm::length(up) < 0.5f) {
        up = vec(cam.watchUp);
    }
    up = xm::normalize(up);
    if (xm::length(up) < 0.5f) {
        up = xm::normalize(camUp);
    }
    // The game camera's facing along the ground: the yaw the diorama takes on
    // a cut.  The more the camera looks down, the more its up vector (the top
    // of the picture) says it: seen from straight above, as over the Dino
    // Piranha's planet, the view direction alone has no horizontal part.
    xm::Vec3 fwd = flatten(camBack * -1.0f + camUp * fmaxf(0.0f, xm::dot(camBack, up)), up);
    if (xm::length(fwd) < 1e-3f) {
        fwd = flatten(camUp, up);
    }
    fwd = xm::normalize(fwd);

    // The diorama holds Mario, not the point the game camera watches: that
    // point leads, trails or frames things for a camera 10-20 m away, while
    // the player's eye sits a fixed distance from the anchor.  On a small
    // planet it trailed Mario through the planet (up to 1000 units below
    // him around the Dino Piranha's), which put the eye inside it.  Jumps:
    // the pivot stays at the height Mario left the ground from until he is
    // jumpDeadZone above it (the game camera's own jump damping did that).
    // The watched point stays the pivot where the game shows something
    // else (PORT_GX_CAMERA_CENTRE_PLAYER off: event cameras aimed far away).
    xm::Vec3 target = vec(cam.watch);
    bool centre = player && (cam.flags & PORT_GX_CAMERA_CENTRE_PLAYER) != 0;
    if (centre) {
        xm::Vec3 p = vec(cam.player);
        if (rig.valid && rig.centred && !(cam.flags & PORT_GX_CAMERA_GROUNDED)) {
            rig.lift = fminf(params.jumpDeadZone, fmaxf(0.0f, rig.lift + xm::dot(p - rig.lastPlayer, up)));
        } else {
            rig.lift -= rig.lift * blend(dt, params.landingHalfLife);
        }
        rig.lastPlayer = p;
        target = p - up * rig.lift;
    }
    rig.centred = centre;

    bool cut = !rig.valid || xm::length(target - rig.pivot) > params.snapDistance;
    // A sudden change of gravity (a gravity switch, walking over the edge of
    // a box-shaped room) or a snap turn: done at once behind a blink (the
    // caller fades from black), not as a turn of the whole world.  Sudden:
    // away from where it was a moment ago; running round a small planet turns
    // it steadily, and the world turns with it smoothly.
    bool flip = rig.valid && acosf(fminf(1.0f, fmaxf(-1.0f, xm::dot(rig.recentUp, up)))) > params.gravitySnapAngle;
    // So does an up trailing Mario's very far (thrown round a small planet).
    flip = flip || (rig.valid && acosf(fminf(1.0f, fmaxf(-1.0f, xm::dot(rig.up, up)))) > params.maxUpLag);
    bool snapTurn = rig.valid && rig.pendingTurn != 0.0f;
    rig.snapped = cut || flip || snapTurn;
    rig.recentUp = rig.snapped ? up : xm::normalize(rig.recentUp + (up - rig.recentUp) * blend(dt, 0.07f));
    if (cut) {
        if (centre) {
            target = target + up * rig.lift;
            rig.lift = 0.0f;
        }
        rig.pivot = target;
        rig.up = up;
        rig.forward = fwd;  // a new area: the game camera's yaw
        rig.valid = true;
    }
    if (!cut && flip) {
        // The yaw carried over to the new up (the world keeps its facing).
        xm::Vec3 f = flatten(xm::rotate(rotationBetween(rig.up, up), rig.forward), up);
        rig.forward = xm::length(f) > 1e-3f ? xm::normalize(f) : fwd;
        rig.up = up;
    }
    if (snapTurn) {
        rig.forward = xm::normalize(flatten(turn(rig.forward, rig.up, rig.pendingTurn), rig.up));
    }
    rig.pendingTurn = 0.0f;
    if (!rig.snapped) {
        xm::Vec3 prevUp = rig.up, prevFwd = rig.forward;
        // Follow Mario once he is more than followDeadZone away along the
        // ground: small steps back and forth leave the world where it is.
        xm::Vec3 d = target - rig.pivot;
        xm::Vec3 along = flatten(d, rig.up);
        float n = xm::length(along);
        xm::Vec3 goal = rig.pivot + (d - along) + (n > params.followDeadZone ? along * (1.0f - params.followDeadZone / n) : xm::Vec3{0, 0, 0});
        rig.pivot = rig.pivot + (goal - rig.pivot) * blend(dt, params.posHalfLife);
        // The up follows Mario's gently.  While he runs round a small planet
        // away from the player it trails behind, and the eye (placed from the
        // trailing up) sank towards his horizon, then below it and into the
        // planet: while the eye is under minEyeElevation above his horizon
        // the up catches up faster, just as much as that needs.
        xm::Vec3 eye = rig.up * -params.anchor.y + rig.forward * params.anchor.z;  // from Mario, metres (anchor z < 0: behind)
        float height = xm::dot(eye, up);
        float elevation = atan2f(height, xm::length(eye - up * height));
        float halfLife = params.rotHalfLife;
        bool catchUp = elevation < params.minEyeElevation;
        if (catchUp) {
            halfLife /= fminf(4.0f, 1.0f + 4.0f * (params.minEyeElevation - elevation) / params.minEyeElevation);
        }
        float k = blend(dt, halfLife);
        xm::Vec3 nextUp = xm::normalize(rig.up + (up - rig.up) * k);
        // Catching up never turns the world faster than maxCatchUpRate.
        float step = acosf(fminf(1.0f, fmaxf(-1.0f, xm::dot(rig.up, nextUp))));
        float maxStep = fmaxf(params.maxCatchUpRate * dt, acosf(fminf(1.0f, fmaxf(-1.0f, xm::dot(rig.up, xm::normalize(rig.up + (up - rig.up) * blend(dt, params.rotHalfLife)))))));
        if (catchUp && step > maxStep && step > 1e-6f) {
            nextUp = turn(rig.up, xm::normalize(xm::cross(rig.up, nextUp)), maxStep);
        }
        rig.up = nextUp;
        // The yaw goes along with the up as it turns; it follows the game
        // camera only in the old way (turn_with_camera).
        xm::Vec3 f = xm::rotate(rotationBetween(prevUp, rig.up), rig.forward);
        if (params.turnWithCamera) {
            f = f + (fwd - f) * blend(dt, params.rotHalfLife);
        }
        f = flatten(f, rig.up);
        if (xm::length(f) < 1e-3f) {
            f = flatten(fwd, rig.up);
        }
        rig.forward = xm::normalize(f);
        float du = acosf(fminf(1.0f, fmaxf(-1.0f, xm::dot(prevUp, rig.up))));
        float df = acosf(fminf(1.0f, fmaxf(-1.0f, xm::dot(prevFwd, rig.forward))));
        rig.turnRate = dt > 0.0f ? fmaxf(du, df) / dt : 0.0f;
    } else {
        rig.turnRate = 0.0f;
    }

    // Stage-from-world: world axes (right, up, back) -> stage X, Y, Z, scaled,
    // with the pivot at the anchor.
    xm::Vec3 r = xm::normalize(xm::cross(rig.forward, rig.up));
    xm::Vec3 u = rig.up;
    xm::Vec3 b = rig.forward * -1.0f;
    xm::Mat4 rot = xm::Mat4::identity();
    rot.at(0, 0) = r.x, rot.at(0, 1) = r.y, rot.at(0, 2) = r.z;
    rot.at(1, 0) = u.x, rot.at(1, 1) = u.y, rot.at(1, 2) = u.z;
    rot.at(2, 0) = b.x, rot.at(2, 1) = b.y, rot.at(2, 2) = b.z;
    xm::Mat4 stageFromWorld = xm::translation(params.anchor) * xm::scale(params.scale) * rot * xm::translation(rig.pivot * -1.0f);

    // World-from-view: inverse of the (rigid) view matrix.
    xm::Mat4 worldFromView = xm::Mat4::identity();
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            worldFromView.at(i, j) = cam.view[j * 4 + i];  // transpose of the rotation
        }
    }
    xm::Vec3 t{cam.view[3], cam.view[7], cam.view[11]};
    for (int i = 0; i < 3; i++) {
        worldFromView.at(i, 3) = -(worldFromView.at(i, 0) * t.x + worldFromView.at(i, 1) * t.y + worldFromView.at(i, 2) * t.z);
    }
    return stageFromWorld * worldFromView;
}

// World axes of the rig: right, up, back map to stage X, Y, Z.
static void rigAxes(const RigState& rig, xm::Vec3* r, xm::Vec3* u, xm::Vec3* b) {
    *r = xm::normalize(xm::cross(rig.forward, rig.up));
    *u = rig.up;
    *b = rig.forward * -1.0f;
}

xm::Vec3 stageDirToWorld(const RigState& rig, xm::Vec3 d) {
    xm::Vec3 r, u, b;
    rigAxes(rig, &r, &u, &b);
    return r * d.x + u * d.y + b * d.z;
}

xm::Vec3 stageToWorld(const RigState& rig, const RigParams& params, xm::Vec3 p) {
    return rig.pivot + stageDirToWorld(rig, (p - params.anchor) * (1.0f / params.scale));
}

// A cone from the eye that is 3 cm wide at the eye and 20 cm at the player,
// ending 25 cm short of the player's centre, plus a 12 cm sphere around the
// eye.
void setCutaway(gpu::EyeView& ev, const xm::Mat4& eyeFromView, const gpu::CameraInfo& cam, float amount, float widen) {
    if (!(cam.flags & PORT_GX_CAMERA_PLAYER) || amount < 0.01f) {
        return;
    }
    const float* v = cam.view;
    const float* p = cam.player;
    xm::Vec3 inView = {v[0] * p[0] + v[1] * p[1] + v[2] * p[2] + v[3], v[4] * p[0] + v[5] * p[1] + v[6] * p[2] + v[7],
                       v[8] * p[0] + v[9] * p[1] + v[10] * p[2] + v[11]};
    xm::Vec3 f = xm::transformPoint(eyeFromView, inView);
    if (xm::length(f) < 0.3f) {
        return;  // the player is at the eye: nothing sensible to cut
    }
    ev.focus[0] = f.x;
    ev.focus[1] = f.y;
    ev.focus[2] = f.z;
    ev.focus[3] = 1.0f;
    ev.cut[0] = 0.03f * widen * amount;
    ev.cut[1] = 0.20f * widen * amount;
    ev.cut[2] = 0.25f;
    ev.cut[3] = 0.12f * amount;
}

void toRowMajor(const xm::Mat4& m, float out[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = m.at(r, c);
        }
    }
}

}  // namespace vr
