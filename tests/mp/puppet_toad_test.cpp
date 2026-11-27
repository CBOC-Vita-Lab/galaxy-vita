// Host unit tests for the Toad retarget (mp_toad_map in decomp/include/Game/MP/MpPoseMath.hpp).
// Build: g++ -std=c++20 -fsanitize=address,undefined -g -Iplatform/include -Idecomp/include tests/mp/puppet_toad_test.cpp -o /tmp/puppet_toad_test
#include "Game/MP/MpPoseMath.hpp"

#include <math.h>
#include <stdio.h>

static int gFail = 0, gChecks = 0;
#define CHECK(c)                                                                                                                       \
    do {                                                                                                                               \
        gChecks++;                                                                                                                     \
        if (!(c)) {                                                                                                                    \
            gFail++;                                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                                        \
        }                                                                                                                              \
    } while (0)

// a pose standing on ground with "up" = +Y (identity rotation) unless rotated
static MpPoseF pose(float vx, float vy, float vz, uint16_t flags) {
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.quat[3] = 1.0f;
    p.vel[0] = vx; p.vel[1] = vy; p.vel[2] = vz;
    p.scale = 1.0f;
    p.flags = (uint16_t)(MP_POSE_VISIBLE | flags);
    return p;
}

static int anim(const MpPoseF& p, const char* bck, int prev = -1) { return mp_toad_map(p, bck, prev).anim; }

static void testGround() {
    const uint16_t G = MP_POSE_ON_GROUND;
    CHECK(anim(pose(0, 0, 0, G), "Wait") == MP_TOAD_WAIT);
    CHECK(anim(pose(0.5f, 0, 0, G), "WalkSoft") == MP_TOAD_WAIT);
    CHECK(anim(pose(4, 0, 0, G), "Walk") == MP_TOAD_WALK);
    CHECK(anim(pose(14, 0, 0, G), "Run") == MP_TOAD_RUN);
    // vertical velocity on the ground (slopes, gravity) does not count as walking
    CHECK(anim(pose(0, -20, 0, G), "Wait") == MP_TOAD_WAIT);
    // the rate follows the speed, clamped
    MpToadOut w = mp_toad_map(pose(3, 0, 0, G), "Walk", MP_TOAD_WALK);
    CHECK(w.anim == MP_TOAD_WALK && fabsf(w.rate - 1.0f) < 1e-5f && w.holdFrame < 0.0f);
    MpToadOut r = mp_toad_map(pose(60, 0, 0, G), "Run", MP_TOAD_RUN);
    CHECK(r.anim == MP_TOAD_RUN && r.rate == 2.2f);
    // hysteresis: 8 units/turn is walk when coming from walk, run when already running
    CHECK(anim(pose(8, 0, 0, G), "Run", MP_TOAD_WALK) == MP_TOAD_WALK);
    CHECK(anim(pose(8, 0, 0, G), "Run", MP_TOAD_RUN) == MP_TOAD_RUN);
    CHECK(anim(pose(1.0f, 0, 0, G), "Walk", MP_TOAD_WAIT) == MP_TOAD_WAIT);
    CHECK(anim(pose(1.0f, 0, 0, G), "Walk", MP_TOAD_WALK) == MP_TOAD_WALK);
    // on a planet's side: up = +X (rotation of 90 deg about Z), moving along up is not walking, across it is
    MpPoseF side = pose(10, 0, 0, G);
    side.quat[2] = -sinf(0.785398f); side.quat[3] = cosf(0.785398f);   // maps +Y to +X
    float up[3];
    MpPoseMath::quatUp(side.quat, up);
    CHECK(fabsf(up[0] - 1.0f) < 1e-4f);
    CHECK(anim(side, "Wait") == MP_TOAD_WAIT);
    side.vel[0] = 0; side.vel[2] = 10;
    CHECK(anim(side, "Run") == MP_TOAD_RUN);
}

static void testStates() {
    const uint16_t G = MP_POSE_ON_GROUND;
    CHECK(anim(pose(0, 0, 0, G), "Spin") == MP_TOAD_SPIN);
    CHECK(anim(pose(0, 5, 0, 0), "spinjump") == MP_TOAD_SPIN);                    // case-insensitive, even in the air
    CHECK(anim(pose(0, 0, 0, MP_POSE_IN_WATER), "SwimWait") == MP_TOAD_SWIM);
    CHECK(anim(pose(0, 0, 0, 0), "swimbreast") == MP_TOAD_SWIM);                   // swim clip without the flag
    MpToadOut s = mp_toad_map(pose(16, 0, 0, MP_POSE_IN_WATER), "SwimBreast", -1);
    CHECK(s.rate == 2.0f);
    CHECK(anim(pose(0, 0, 0, MP_POSE_IN_WATER), "SwimSpin") == MP_TOAD_SPIN);      // spin wins over swimming
    CHECK(anim(pose(0, 0, 0, G), "DamageSmallFront") == MP_TOAD_TRAMPLED);
    CHECK(anim(pose(0, 3, 0, 0), "damagemiddlebackair") == MP_TOAD_TUMBLE);
    CHECK(anim(pose(0, 0, 0, G), "HeavyDamegeFront") == MP_TOAD_TRAMPLED);         // (the game's own spelling)
    CHECK(anim(pose(0, 0, 0, G | MP_POSE_SQUASHED), "Wait") == MP_TOAD_TRAMPLED);
    CHECK(anim(pose(0, 0, 0, G), "PowerStarGet") == MP_TOAD_JOY);
    CHECK(anim(pose(0, 0, 0, G), "grandstarget") == MP_TOAD_JOY);
    CHECK(anim(pose(0, 0, 0, G), "DemoGetPower") == MP_TOAD_JOY);
    CHECK(anim(pose(0, 0, 0, G), "DieFall") == MP_TOAD_FREEZE);
    CHECK(anim(pose(0, 0, 0, G | MP_POSE_DEAD), "Wait") == MP_TOAD_FREEZE);
    CHECK(anim(pose(0, 0, 0, MP_POSE_IN_WATER), "SwimDie") == MP_TOAD_FREEZE);       // death wins over swimming
    CHECK(anim(pose(0, 0, 0, G | MP_POSE_DEAD), "PowerStarGet") == MP_TOAD_FREEZE);
}

static void testAir() {
    MpToadOut up = mp_toad_map(pose(5, 12, 0, 0), "Jump", MP_TOAD_RUN);
    CHECK(up.anim == MP_TOAD_AIR_UP && up.holdFrame >= 0.0f && up.hop > 1.0f);
    MpToadOut down = mp_toad_map(pose(5, -12, 0, 0), "Fall", MP_TOAD_AIR_UP);
    CHECK(down.anim == MP_TOAD_AIR_DOWN && down.holdFrame >= 0.0f && down.hop == 1.0f);
    CHECK(anim(pose(0, 12, 0, 0), "JumpBack") == MP_TOAD_AIR_UP);     // backflip
    CHECK(anim(pose(20, 3, 0, 0), "JumpBroad") == MP_TOAD_AIR_UP);    // long jump
    CHECK(anim(pose(0, 0, 0, 0), nullptr) == MP_TOAD_AIR_DOWN);       // unknown animation in the air
    CHECK(anim(pose(0, 0, 0, MP_POSE_ON_GROUND), nullptr) == MP_TOAD_WAIT);
}

static void testNamesAndRobustness() {
    for (int a = 0; a < MP_TOAD_ANIM_COUNT; a++) CHECK(mp_toad_anim_name(a) != nullptr && mp_toad_anim_name(a)[0] != '\0');
    CHECK(strcmp(mp_toad_anim_name(MP_TOAD_SWIM), "SwimWait") == 0);
    CHECK(strcmp(mp_toad_anim_name(MP_TOAD_AIR_DOWN), "Surprised") == 0);
    CHECK(strcmp(mp_toad_anim_name(99), "Wait") == 0);
    // non-finite velocity never yields garbage
    MpPoseF p = pose(NAN, INFINITY, 0, MP_POSE_ON_GROUND);
    MpToadOut o = mp_toad_map(p, "Walk", -1);
    CHECK(o.anim == MP_TOAD_WAIT && MpPoseMath::finitef(o.rate));
    CHECK(mp_has_ci("PowerStarGet", "starget") && !mp_has_ci("Star", "starget") && mp_starts_ci("DieFall", "die") && !mp_starts_ci("Di", "die"));
    // every name the archive carries for the clips we use exists (Kinopio.arc file list, lower case)
    static const char* const cArchive[] = {"wait", "walk", "run", "spin", "surprised", "swimwait", "tumble", "trampled", "joy", "freeze"};
    for (int a = 0; a < MP_TOAD_ANIM_COUNT; a++) {
        bool found = false;
        for (const char* n : cArchive) {
            const char* m = mp_toad_anim_name(a);
            found = found || (mp_starts_ci(m, n) && strlen(m) == strlen(n));
        }
        CHECK(found);
    }
}

static void testCharacters() {
    CHECK(MP_LOOK_COUNT == 6);
    for (int c = 0; c < MP_LOOK_COUNT; c++) CHECK(strcmp(mp_character_name(c), "?") != 0);
    CHECK(strcmp(mp_character_name(MP_LOOK_COUNT), "?") == 0);
    // models
    CHECK(strcmp(mp_character_model(MP_LOOK_MARIO), "Mario") == 0 && strcmp(mp_character_model(MP_LOOK_LUIGI), "Luigi") == 0);
    CHECK(strcmp(mp_character_model(MP_LOOK_COSMIC), "GhostMario") == 0 && strcmp(mp_character_model(MP_LOOK_COSMIC_LUIGI), "GhostLuigi") == 0);
    CHECK(strcmp(mp_character_model(MP_LOOK_TOAD), "Kinopio") == 0 && strcmp(mp_character_model(MP_LOOK_YELLOW_TOAD), "Kinopio") == 0);
    // kinds
    CHECK(mp_character_is_toad(MP_LOOK_TOAD) && mp_character_is_toad(MP_LOOK_YELLOW_TOAD) && !mp_character_is_toad(MP_LOOK_MARIO));
    CHECK(mp_character_is_cosmic(MP_LOOK_COSMIC) && mp_character_is_cosmic(MP_LOOK_COSMIC_LUIGI) && !mp_character_is_cosmic(MP_LOOK_LUIGI));
    CHECK(mp_character_luigi_swap(MP_LOOK_LUIGI) && mp_character_luigi_swap(MP_LOOK_COSMIC_LUIGI) && !mp_character_luigi_swap(MP_LOOK_COSMIC));
    // colours: red Toad 3, Yellow Toad 4
    CHECK(mp_toad_color_frame(MP_LOOK_TOAD) == 3.0f && mp_toad_color_frame(MP_LOOK_YELLOW_TOAD) == 4.0f);
    // archives beyond the stationed set (a Mario file stations Mario, a Luigi file Luigi)
    CHECK(mp_character_archive(MP_LOOK_MARIO, false) == nullptr && strcmp(mp_character_archive(MP_LOOK_MARIO, true), "Mario") == 0);
    CHECK(mp_character_archive(MP_LOOK_LUIGI, true) == nullptr && strcmp(mp_character_archive(MP_LOOK_LUIGI, false), "Luigi") == 0);
    for (int luigiFile = 0; luigiFile < 2; luigiFile++) {
        CHECK(strcmp(mp_character_archive(MP_LOOK_TOAD, luigiFile), "Kinopio") == 0);
        CHECK(strcmp(mp_character_archive(MP_LOOK_YELLOW_TOAD, luigiFile), "Kinopio") == 0);   // one archive for both Toads
        CHECK(strcmp(mp_character_archive(MP_LOOK_COSMIC, luigiFile), "GhostMario") == 0);
        CHECK(strcmp(mp_character_archive(MP_LOOK_COSMIC_LUIGI, luigiFile), "GhostLuigi") == 0);
    }
    CHECK(mp_character_archive(MP_LOOK_COUNT, false) == nullptr);
}

// MP3 profile: the name class (worked out once per bck name by the puppet) gives exactly what the per-turn scan gave. The reference is
// the mapping as it was before the split, copied here.
static int oldMap(const MpPoseF& p, const char* marioBck) {
    const char* n = marioBck != nullptr ? marioBck : "";
    if ((p.flags & MP_POSE_DEAD) || mp_starts_ci(n, "die") || mp_has_ci(n, "swimdie")) return MP_TOAD_FREEZE;
    if (mp_has_ci(n, "starget") || mp_has_ci(n, "getpower") || mp_has_ci(n, "resultwait") || mp_has_ci(n, "winghost") ||
        mp_has_ci(n, "elementget"))
        return MP_TOAD_JOY;
    if (p.flags & MP_POSE_SQUASHED) return MP_TOAD_TRAMPLED;
    if (mp_has_ci(n, "damage") || mp_has_ci(n, "dameg") || mp_has_ci(n, "flick"))
        return (p.flags & MP_POSE_ON_GROUND) ? MP_TOAD_TRAMPLED : MP_TOAD_TUMBLE;
    if (mp_has_ci(n, "spin")) return MP_TOAD_SPIN;
    if ((p.flags & MP_POSE_IN_WATER) || mp_starts_ci(n, "swim")) return MP_TOAD_SWIM;
    return -1;   // (movement decides: compared through the full mapping below)
}

static void testNameClass() {
    static const char* const names[] = {nullptr, "", "Wait", "die", "DieFall", "SwimDie", "swimdie2", "PowerStarGet", "starget",
                                        "GetPowerUp", "ResultWait", "WinGhost", "ElementGet", "Damage", "DamageFire", "Dameg", "Flick",
                                        "Spin", "SpinJump", "SwimWait", "swim", "Swimming", "Run", "Jump", "SpinDamage", "DieSpin",
                                        "StarGetSpin", "SwimSpin", "xdie", "Dashing"};
    const uint16_t flagSets[] = {0, MP_POSE_ON_GROUND, MP_POSE_DEAD, MP_POSE_SQUASHED, MP_POSE_IN_WATER,
                                 (uint16_t)(MP_POSE_SQUASHED | MP_POSE_ON_GROUND), (uint16_t)(MP_POSE_IN_WATER | MP_POSE_ON_GROUND)};
    const float vels[][3] = {{0, 0, 0}, {2, 0, 0}, {10, 0, 0}, {0, 3, 0}, {0, -3, 0}};
    for (const char* n : names) {
        const int cls = mp_toad_name_class(n);
        for (uint16_t f : flagSets) {
            for (const auto& v : vels) {
                for (int prev = -1; prev < MP_TOAD_ANIM_COUNT; prev++) {
                    const MpPoseF p = pose(v[0], v[1], v[2], f);
                    const MpToadOut a = mp_toad_map(p, n, prev), b = mp_toad_map_class(p, cls, prev);
                    CHECK(a.anim == b.anim && a.rate == b.rate && a.holdFrame == b.holdFrame && a.hop == b.hop);
                    const int o = oldMap(p, n);
                    if (o >= 0) CHECK(a.anim == o);
                }
            }
        }
    }
}

int main() {
    testCharacters();
    testGround();
    testStates();
    testAir();
    testNamesAndRobustness();
    testNameClass();
    printf("%s: %d checks, %d failed\n", gFail ? "FAILED" : "OK", gChecks, gFail);
    return gFail ? 1 : 0;
}
