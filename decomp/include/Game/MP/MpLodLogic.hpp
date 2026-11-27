// Galaxy 3.0 multiplayer: puppet level of detail by distance, pure logic (owner: lod2; no game headers, host tested in
// tests/mp/lod_test.cpp). The game side is MpLod.cpp (state, switches) and MpRemotePlayer.cpp (what the puppet does with it).
//
// The puppets already cull themselves off screen beyond 1500 units, drop their shadow beyond 5500 and compute joints on every other
// picture when far. This makes far and tiny players nearly free at the governor's level 0 (the governor still steps in at 1..4):
//   - JOINT RATE: the camera distance picks a band; the band's period is 1 (every picture), 2, 3 or 4. The bands have a hysteresis
//     (in beyond rateIn, out again inside rateOut). The puppets of one period are staggered (phase = their index among the puppets of
//     that period) so a picture computes about the same number of puppets every time.
//   - SHADOW: the engine's shadow VOLUME (extra geometry passes) only for the puppets that are close (volIn/volOut) and for the
//     "nearest" ones (the nearest remote puppet, with a hysteresis so two equally near puppets never trade the title every turn; the
//     puppet a player is watching and the puppets standing in a formation / team photo always count as nearest); everything else gets
//     the cheap round surface shadow (the circle many small enemies use); beyond the far distance none.
//   - TINY: a puppet whose body is a few pixels tall (from its distance, size and the camera's field of view) is skipped like an
//     off-screen one (no joints, no draw); with a hysteresis.
//   - ONE PASS: see onePassOk (the second model pass is only needed while a blend is running).
#pragma once

#include <cmath>
#include <cstdint>

namespace MpLod {

enum { SLOTS = 4 };
enum ShadowMode : uint8_t { SH_OFF = 0, SH_VOLUME = 1, SH_BLOB = 2 };

struct Config {
    // distance bands (camera -> body middle, units): band k -> k+1 beyond rateIn[k]; k+1 -> k inside rateOut[k]
    float rateIn[3] = {3500.0f, 6000.0f, 9000.0f};
    float rateOut[3] = {3000.0f, 5200.0f, 8000.0f};
    // shadow volume for ordinary puppets: kept up to volOut, taken back inside volIn; for the nearest / exempt: nearVolOut / nearVolIn
    float volIn = 1800.0f, volOut = 2200.0f;
    float nearVolIn = 4000.0f, nearVolOut = 4500.0f;
    // tiny: body height in pixels below tinyIn is skipped, back above tinyOut
    float tinyIn = 6.0f, tinyOut = 8.0f;
    // optfix item 1: the shadow volume chosen by the shadow's ON-SCREEN size (the body's pixel height), not by distance. The numbers are the
    // old distance tiers seen from the reference camera (45 degree field of view, 544 px, a body 190 units tall at scale 1): ordinary
    // puppets 1800 / 2200 units = 69 / 57 px, the nearest / exempt 4000 / 4500 units = 31 / 28 px. So at the reference camera nothing
    // changes; a zoomed-in (narrow) camera or a giant puppet keeps the real volume farther out, a wide camera or a small puppet drops it
    // sooner, which is when the volume is not worth its cost (the shadow is a few pixels). Enter volume at >= *Enter, keep it while >= *Stay.
    // fix2: these pixel numbers are in REFERENCE pixels (a 544 px screen). The game passes MR::getScreenHeight() = the EFB height (456 on the
    // Vita), so the body's pixels are first scaled to the reference screen (refPixels); without that the volume reached only 456/544 = 84% as
    // far as the old distance tiers (ordinary 1516 instead of 1808 units; the nearest 3374 instead of 4025).
    float refScreenH = 544.0f;
    float volPxEnter = 69.0f, volPxStay = 57.0f;
    float nearVolPxEnter = 31.0f, nearVolPxStay = 28.0f;
    // optfix item 2: FAR-PUPPET TRIM: the face and hand shapes (Face0, HandL0, HandR0: a few hundred vertices, three draw calls) are left out
    // while the body is under trimPxIn pixels, back at trimPxOut. NOT invisible at 40 px (the face is the most noticed part): off unless
    // SMG_MP_LOD2_TRIM=1 (the pixel limit can be tried with SMG_MP_LOD2_TRIMPX=<px>).
    float trimPxIn = 40.0f, trimPxOut = 46.0f;
    // the nearest holds the title until another is this much closer (a squared-distance factor)
    float nearestBias = 0.7f;
    // a skipped puppet is off screen when its sphere (radius * this) is outside the frustum
    float cullMargin = 1.25f;
    // joint passes: a blend in progress may take one pass beyond this distance (see onePassOk)
    float onePassFar = 2500.0f;
};

// A puppet that counts as the nearest whatever its distance: the nearest remote puppet, the one a player is watching (spectate),
// and the ones standing in a formation / team photo (formationMask: bit = slot). Such a puppet takes band 0 (every picture) and the
// larger shadow-volume distances.
static inline bool isSpecial(int slot, int nearest, int watched, uint32_t formationMask) {
    return slot >= 0 && slot < SLOTS && (slot == nearest || slot == watched || ((formationMask >> slot) & 1u) != 0);
}

// The radius of the sphere tested against the view frustum for the off-screen skip: the body's own radius with a margin (so a puppet
// whose arm or shell is still in view is not skipped), scaled with the puppet.
static inline float cullRadius(float baseRadius, float scale, const Config& c) { return baseRadius * c.cullMargin * scale; }

// Band 0..3 with hysteresis. d2: squared distance. The period is band + 1.
static inline int nextBand(int prev, float d2, const Config& c) {
    int b = prev < 0 ? 0 : (prev > 3 ? 3 : prev);
    while (b < 3 && d2 > c.rateIn[b] * c.rateIn[b]) b++;
    while (b > 0 && d2 < c.rateOut[b - 1] * c.rateOut[b - 1]) b--;
    return b;
}
static inline int bandFor(int prev, float d2, bool special, const Config& c) { return special ? 0 : nextBand(prev, d2, c); }
static inline int bandPeriod(int band) { return band < 0 ? 1 : (band > 3 ? 4 : band + 1); }

// True when this picture leaves the puppet's joints out. period <= 1: never.
static inline bool skipPicture(uint32_t pictures, int phase, int period) {
    if (period <= 1) return false;
    return (pictures + (uint32_t)phase) % (uint32_t)period != 0;
}

// The staggering: phase[s] = the index of slot s among the active slots of its period (so 3 puppets of period 4 take phases 0,1,2,
// and 2 puppets of period 2 take 0 and 1). period[s] <= 0: inactive.
static inline void assignPhases(const int* period, int slots, uint8_t* phase) {
    for (int s = 0; s < slots; s++) {
        phase[s] = 0;
        if (period[s] <= 0) continue;
        int idx = 0;
        for (int t = 0; t < s; t++) idx += period[t] == period[s] ? 1 : 0;
        phase[s] = (uint8_t)(idx % period[s]);
    }
}

// Shadow mode with hysteresis. level: the governor's level; far: beyond the far distance (no shadow at all); special: the nearest /
// watched / formation puppet. Level 0: volume near (volIn/volOut, or the larger nearVol* for a special one), else the surface
// circle; level 1..: the surface circle (the volume is what costs); far: none at any level.
static inline uint8_t nextShadow(uint8_t prev, float d2, int level, bool special, bool far, const Config& c) {
    if (far) return SH_OFF;
    if (level >= 1) return SH_BLOB;
    const float in = special ? c.nearVolIn : c.volIn, out = special ? c.nearVolOut : c.volOut;
    const bool vol = prev == SH_VOLUME ? d2 < out * out : d2 < in * in;
    return vol ? SH_VOLUME : SH_BLOB;
}

// fix2: the body's pixel height as it would be on the reference screen (screenH = the height actually used to measure it).
static inline float refPixels(float px, float screenH, const Config& c) { return screenH > 1.0f ? px * c.refScreenH / screenH : px; }

// Shadow mode by the body's pixel height (see Config::volPxEnter; px in REFERENCE pixels, see refPixels). Same contract as nextShadow
// otherwise: far: none; level >= 1: the circle. fix2: d2 (squared distance; default: unknown) keeps the OLD distance tiers as a floor: the
// volume is never dropped nearer than before (a wide field of view only used to lose it sooner), the pixel rule adds reach for a zoomed-in
// camera or a big puppet.
static inline uint8_t nextShadowPx(uint8_t prev, float px, int level, bool special, bool far, const Config& c, float d2 = 1.0e30f) {
    if (far) return SH_OFF;
    if (level >= 1) return SH_BLOB;
    const float enter = special ? c.nearVolPxEnter : c.volPxEnter, stay = special ? c.nearVolPxStay : c.volPxStay;
    const float in = special ? c.nearVolIn : c.volIn, out = special ? c.nearVolOut : c.volOut;
    const bool byPx = prev == SH_VOLUME ? px >= stay : px >= enter;
    const bool byDist = prev == SH_VOLUME ? d2 < out * out : d2 < in * in;
    return (byPx || byDist) ? SH_VOLUME : SH_BLOB;
}

// Far-puppet trim with hysteresis: true = leave the face and hand shapes out.
static inline bool nextTrim(bool prev, float px, const Config& c) { return prev ? px < c.trimPxOut : px < c.trimPxIn; }

// Height in pixels of a body `height` units tall at `dist` units, camera vertical field of view fovyDeg, screen height screenH px.
static inline float bodyPixels(float height, float dist, float fovyDeg, float screenH) {
    const float t = std::tan(fovyDeg * 0.5f * 3.14159265f / 180.0f);
    if (!(dist > 1.0f) || !(t > 1e-4f)) return 1.0e9f;   // (at the camera: not tiny)
    return height * 0.5f * screenH / (dist * t);
}
static inline bool nextTiny(bool prev, float px, const Config& c) { return prev ? px < c.tinyOut : px < c.tinyIn; }

// The nearest remote puppet, with hysteresis. d2[s] < 0: not a candidate. Returns the slot or -1. `holder` is last turn's answer:
// it keeps the title while it is a candidate and no other is closer than nearestBias * its squared distance.
static inline int pickNearest(int holder, const float* d2, int slots, const Config& c) {
    int best = -1;
    for (int s = 0; s < slots; s++) {
        if (d2[s] < 0.0f) continue;
        if (best < 0 || d2[s] < d2[best]) best = s;   // (ties: the lower slot)
    }
    if (best < 0) return -1;
    if (holder >= 0 && holder < slots && d2[holder] >= 0.0f && !(d2[best] < d2[holder] * c.nearestBias)) return holder;
    return best;
}

// ONE PASS. The two model passes of a puppet (MarioAnimator::calc's blend pass, _6 = 1, then the final one, _6 = 2) are needed to
// advance and apply the blend (XanimeCore::calcBlendSpecial keeps the freeze blend of every joint). With no blend running (the
// core's _1C == 1, _20 == 1, no freeze (_29 == 0) and every joint's _5C == 1) the blend pass computes exactly what the ordinary
// calc (_6 = 0: calcSingle / calcBlend + calcScaleBlendMaya) computes in one go, so one pass does the work of two (proved by the
// desktop self-check SMG_MP_LOD2_CHECK: both are run on a copy of the state and the matrices compared).
// blendRunning: any of the above is not at rest; d2: squared distance to the camera.
static inline bool onePassOk(bool blendRunning, float d2, const Config& c) {
    if (!blendRunning) return true;
    return d2 > c.onePassFar * c.onePassFar;   // (a blend in progress far away: the frozen-pose lerp is not worth a second pass)
}

}  // namespace MpLod
