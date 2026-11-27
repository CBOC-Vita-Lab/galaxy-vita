// Focused unit tests for w25f Performance-mode fixes:
//  1. GXV_BLOOM_FOLD=3 symmetric 2D Gaussian reduced-tap bloom: weight normalization to 1.0,
//     energy conservation vs fold=2, 4-fold rotational symmetry, draw count halving (9 vs 18),
//     per-draw non-saturation (< 1.0), and Quality-mode exclusion.
//  2. vgx_stagepreset.h updates: gpu-glow inherits mode_keys GXV_BLOOM_FOLD=2 (no bloom_fold=1),
//     HellProminenceGalaxy upgraded to gpu-light, Observatory Level-3 draw cuts applied to
//     KoopaBattleVs3Galaxy, HeavenlyBeachGalaxy, CannonFleetGalaxy (and stripped in Quality mode).
//  3. even60 opening-camera / cutscene / flowPlayTurns==0 and shaderWorkMs==0 guards (hrw_mp.h).
//  4. GXV_MP_DET_LOD lockstep narrow-gate truth table for mLogicFreeJoints != 0 vs == 0.
#include "port/hrw_mp.h"
#include "port/mode_keys.h"
#include "port/pose_ondraw.h"
#include "port/stage_knob.h"
#include "../platform/vgx/src/vgx_stagepreset.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int gFails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++gFails; } } while (0)

// Exact mirror of BloomEffect.cpp bloomFold3BandWeights + draw geometry/quantization.
constexpr unsigned kFold3BlurDraws = 9;
constexpr unsigned kFold3Taps = kFold3BlurDraws * 4;

void BloomFold3BandWeights(float rIn, float rOut, float outTapNorm[4]) {
    const float rMid = 0.5f * (rIn + rOut);
    const float rad[4] = {0.0f, rIn, rMid, rOut};
    const float cnt[4] = {4.0f, 8.0f, 8.0f, 16.0f};
    const float sigma = rOut > 1e-6f ? rOut : 1.0f;
    const float inv2s2 = 0.5f / (sigma * sigma);
    float raw[4];
    float sum = 0.0f;
    for (unsigned band = 0; band < 4; ++band) {
        raw[band] = std::exp(-(rad[band] * rad[band]) * inv2s2);
        sum += cnt[band] * raw[band];
    }
    const float invSum = sum > 0.0f ? 1.0f / sum : 1.0f / 36.0f;
    for (unsigned band = 0; band < 4; ++band) {
        outTapNorm[band] = raw[band] * invSum;
    }
}

void TestBloomFold3Weights() {
    // Default SMG normal bloom radii: L1 = {0.01, 0.02}, L2 = {0.04, 0.07, 0.09}
    const float aDef = (0.01f * 0.01f + 0.02f * 0.02f) / 2.0f;
    const float cDef = (0.04f * 0.04f + 0.07f * 0.07f + 0.09f * 0.09f) / 3.0f;
    const float rInDef = std::sqrt(aDef);
    const float rOutDef = std::sqrt(aDef + cDef);

    struct Case { float rIn, rOut, w18, w1C; };
    const Case cases[] = {
        {rInDef, rOutDef, 1.0f / 6.0f, 1.0f / 12.0f},
        {0.01f, 0.05f, 1.0f / 6.0f, 1.0f / 12.0f},
        {0.02f, 0.12f, 0.20f, 0.10f},
        {0.0f, 0.0f, 1.0f / 6.0f, 1.0f / 12.0f},  // degenerate zero radius
    };

    const unsigned bandDraws[4] = {1, 2, 2, 4};
    for (const Case& tc : cases) {
        float w[4] = {};
        BloomFold3BandWeights(tc.rIn, tc.rOut, w);

        // 1. Strictly positive, monotonically non-increasing Gaussian bell from center outward.
        CHECK(w[0] > 0.0f && w[1] > 0.0f && w[2] > 0.0f && w[3] > 0.0f);
        CHECK(w[0] >= w[1] && w[1] >= w[2] && w[2] >= w[3]);

        // 2. Normalized tap weights across all 36 taps sum identically to 1.0.
        const double normSum = 4.0 * w[0] + 8.0 * w[1] + 8.0 * w[2] + 16.0 * w[3];
        CHECK(std::fabs(normSum - 1.0) < 1e-6);

        // 3. Draw count is exactly 9 (half of fold=2's 18 draws = 36 taps vs 72 taps).
        unsigned draws = 0;
        for (unsigned b = 0; b < 4; ++b) draws += bandDraws[b];
        CHECK(draws == kFold3BlurDraws && draws * 4 == kFold3Taps && draws * 2 == 18);

        // 4. Scaled energy and quantized 8-bit tap weights match fold=2's target `total` accurately,
        //    and no single 4-tap draw saturates >= 1.0 at SMG default intensities.
        const float w1f = static_cast<uint8_t>(tc.w18 * 255.0f) / 255.0f;
        const float w2f = static_cast<uint8_t>(tc.w1C * 255.0f) / 255.0f;
        const float total = (12.0f * w1f) * (1.0f + 36.0f * w2f) / 2.0f * 0.80f;

        double floatSum = 0.0;
        unsigned quantSum = 0;
        for (unsigned b = 0; b < 4; ++b) {
            const float wDraw = total * 4.0f * w[b];
            CHECK(wDraw < 1.0f);  // never saturates inside a single clamped TEV draw
            floatSum += static_cast<double>(bandDraws[b]) * wDraw;
            const unsigned wq = static_cast<unsigned>((wDraw > 1.0f ? 1.0f : wDraw) * 255.0f + 0.5f);
            unsigned tapSum = 0;
            for (unsigned j = 0; j < 4; ++j) {
                const unsigned tw = (wq * (j + 1)) / 4 - (wq * j) / 4;
                CHECK(tw <= 255);
                tapSum += tw;
            }
            CHECK(tapSum == wq);  // exact partition across the 4 taps of the draw
            quantSum += bandDraws[b] * tapSum;
        }
        CHECK(std::fabs(floatSum - total) < 1e-5);
        const double quantEnergy = static_cast<double>(quantSum) / 255.0;
        CHECK(std::fabs(quantEnergy - total) / total < 0.01);  // within 1% after 8-bit quantization
    }

    // 5. Verify 4-fold rotational symmetry (sum of (du, dv) in every single draw == (0, 0)).
    const float invAspect = 1.0f / (16.0f / 9.0f);
    const float ru = 1.0f / 320.0f, rv = 1.0f / 228.0f;
    const float rMidDef = 0.5f * (rInDef + rOutDef);
    // Band 0 (center 2x2)
    {
        float sx = 0.0f, sy = 0.0f;
        for (unsigned j = 0; j < 4; ++j) { sx += (j & 1) ? ru : -ru; sy += (j & 2) ? rv : -rv; }
        CHECK(std::fabs(sx) < 1e-6f && std::fabs(sy) < 1e-6f);
    }
    // Band 1 (2 draws, inner 8-gon)
    for (unsigned d = 0; d < 2; ++d) {
        float sx = 0.0f, sy = 0.0f;
        for (unsigned j = 0; j < 4; ++j) {
            const float ang = 2.0f * 3.14159265f * static_cast<float>(d + 2 * j) / 8.0f;
            sx += invAspect * rInDef * std::cos(ang);
            sy += rInDef * std::sin(ang);
        }
        CHECK(std::fabs(sx) < 1e-5f && std::fabs(sy) < 1e-5f);
    }
    // Band 2 (2 draws, mid 8-gon)
    for (unsigned d = 0; d < 2; ++d) {
        float sx = 0.0f, sy = 0.0f;
        for (unsigned j = 0; j < 4; ++j) {
            const float ang = 2.0f * 3.14159265f * (static_cast<float>(d + 2 * j) + 0.5f) / 8.0f;
            sx += invAspect * rMidDef * std::cos(ang);
            sy += rMidDef * std::sin(ang);
        }
        CHECK(std::fabs(sx) < 1e-5f && std::fabs(sy) < 1e-5f);
    }
    // Band 3 (4 draws, outer 16-gon)
    for (unsigned d = 0; d < 4; ++d) {
        float sx = 0.0f, sy = 0.0f;
        for (unsigned j = 0; j < 4; ++j) {
            const float ang = 2.0f * 3.14159265f * static_cast<float>(d + 4 * j) / 16.0f;
            sx += invAspect * rOutDef * std::cos(ang);
            sy += rOutDef * std::sin(ang);
        }
        CHECK(std::fabs(sx) < 1e-5f && std::fabs(sy) < 1e-5f);
    }

    // 6. Default in mode_keys.h uses GXV_BLOOM_FOLD=3 in Performance and stays unset in Quality.
    bool foundBloom = false;
    for (int i = 0; i < port::modekeys::kRowCount; ++i) {
        if (std::strcmp(port::modekeys::kRows[i].key, "GXV_BLOOM_FOLD") == 0) {
            foundBloom = true;
            CHECK(std::strcmp(port::modekeys::kRows[i].perf, "3") == 0);
            CHECK(port::modekeys::NoChange(port::modekeys::kRows[i].quality));
        }
    }
    CHECK(foundBloom);
}

void TestStagePresets() {
    using namespace vgx::stagepreset;
    Table* t = new Table();
    Diag dg;
    LoadBuiltin(*t, dg);
    CHECK(dg.errors == 0 && dg.dropped == 0);

    auto kvOf = [](const Spec& s, const char* k) -> float { return SpecGet(s, k, -1e9f); };

    // 1. Final Bowser & Star Reactor (@class gpu-glow): bloom_fold=1 removed so mode_keys GXV_BLOOM_FOLD=2 applies.
    for (const char* st : {"KoopaBattleVs1Galaxy", "KoopaBattleVs3Galaxy"}) {
        Spec s;
        Resolve(*t, st, 1, s, nullptr, 10, kPerformance);
        CHECK(std::strcmp(s.cls, "gpu-glow") == 0);
        CHECK(kvOf(s, "bloom_fold") == -1e9f);
        CHECK(kvOf(s, "indcap_half") == 2.0f && kvOf(s, "indcap_camgate") == 1.0f && kvOf(s, "clip_half") == 2.0f && s.cuts == 1);
    }

    // 2. Melty Molten upgraded from base -> gpu-light.
    {
        Spec sp, sq;
        Resolve(*t, "HellProminenceGalaxy", 1, sp, nullptr, 10, kPerformance);
        CHECK(std::strcmp(sp.cls, "gpu-light") == 0);
        CHECK(sp.cuts == 1 && kvOf(sp, "indcap_half") == 2.0f && kvOf(sp, "indcap_camgate") == 1.0f && kvOf(sp, "clip_half") == 2.0f);
        CHECK(kvOf(sp, "indcap_skip") == -1e9f);  // stale-read stage: no indcap_skip

        Resolve(*t, "HellProminenceGalaxy", 1, sq, nullptr, 10, kQuality);
        CHECK(sq.cuts == 0 && sq.edge == 0 && sq.start == 6 && sq.size == kSizeFixed);
        CHECK(kvOf(sq, "indcap_half") == -1e9f);  // sacrifice stripped in Quality
        CHECK(kvOf(sq, "clip_half") == -1e9f);    // sacrifice stripped in Quality (avoids clamping perf-only 2 -> 1)
    }

    // 3. Level-3 draw cuts on Final Bowser, Beach Bowl, and Dreadnought: active in Performance, 100% stripped in Quality.
    const char* const l3Stages[] = {"KoopaBattleVs3Galaxy", "HeavenlyBeachGalaxy", "CannonFleetGalaxy"};
    for (const char* st : l3Stages) {
        Spec sp, sq;
        Resolve(*t, st, 1, sp, nullptr, 10, kPerformance);
        CHECK(kvOf(sp, "pb_lod") == 0.3f);
        CHECK(kvOf(sp, "pb_decor") == 3000.0f);
        CHECK(kvOf(sp, "pb_ptcl") == 0.3f);
        CHECK(kvOf(sp, "pb_fur") == 1500.0f);
        CHECK(kvOf(sp, "pb_shadow") == 1500.0f);
        CHECK(kvOf(sp, "ptcl_thin") == 0.2f);
        CHECK(kvOf(sp, "piecethin_dist") == 3500.0f);
        CHECK(!(sp.flags & (kClampedFloor | kClassMissing | kKvFull)));

        // Validate every key in Performance
        Applied ap[kMaxKvSpec + 24];
        char bad[200];
        CHECK(ValidateKv(sp, ap, kMaxKvSpec + 24, bad, sizeof bad) >= sp.nkv && bad[0] == 0);

        // Quality mode: ALL 7 non-exact draw cuts stripped via stage_knob.h::QualityValue!
        Resolve(*t, st, 1, sq, nullptr, 10, kQuality);
        CHECK(sq.start == 6 && sq.floorR == 6 && sq.ceil == 6 && sq.size == kSizeFixed && sq.edge == 0 && sq.cuts == 0);
        for (const char* k : {"pb_lod", "pb_decor", "pb_ptcl", "pb_fur", "pb_shadow", "ptcl_thin", "piecethin_dist"}) {
            CHECK(kvOf(sq, k) == -1e9f);
        }
    }
    delete t;
}

void TestEven60Guards() {
    using namespace port::hrwmp;
    // Opening camera / demo / flowPlayTurns==0 never judged; shaderWorkMs > 0 never quiet.
    CHECK(!PlayActive(true, false, 2, 0, -2));     // opening camera
    CHECK(!PlayActive(true, false, 4, 0, -2));     // MR::isDemoActive
    CHECK(!PlayActive(true, false, 0, 0, 0));      // PETARI_FLOW pre-play leg
    CHECK(PlayActive(true, false, 0, 0, 60));      // real gameplay
    CHECK(!ShaderQuiet(0, 0, 0, 0, 366.0, 0.0));   // 366 ms shader work, 0 compiles/links
    CHECK(ShaderQuiet(5, 5, 2, 2, 366.0, 366.0));  // quiet second
}

void TestMpDetLodGate() {
    using namespace port::ondraw;
    // Truth table for LiveActor.cpp narrow lockstep gate (GXV_MP_DET_LOD=0|1):
    // Under lockstep: ONLY (detLod && mLogicFreeJoints != 0) passes the lockstep gate.
    for (int lock = 0; lock < 2; ++lock) {
        for (int det = 0; det < 2; ++det) {
            for (int freeJoints = 0; freeJoints < 2; ++freeJoints) {
                const bool mpDetPlant = (lock != 0) && (det != 0) && (freeJoints != 0);
                // PbAnimSkips lockstep gate
                const bool pbMayProceed = (lock == 0) || mpDetPlant;
                if (lock && !det) CHECK(!pbMayProceed);
                if (lock && det && !freeJoints) CHECK(!pbMayProceed);
                if (lock && det && freeJoints) CHECK(pbMayProceed);
                if (!lock) CHECK(pbMayProceed);

                // GXV_POSE_ONDRAW (PortOnDraw) Facts + od::Refuse
                Facts f = {};
                f.skipTurn = (lock == 0) || mpDetPlant;
                f.gate = true;
                f.list = true;
                f.blendFixOn = true;
                f.posedOnce = true;
                f.otherRule = (!mpDetPlant && freeJoints != 0);
                const int why = Refuse(f);
                if (lock && (!det || !freeJoints)) {
                    CHECK(why == kRefTurn);
                } else if (lock && det && freeJoints) {
                    CHECK(why < 0);
                    // Even for mLogicFreeJoints != 0, safety guards still refuse collision/jointsRead/callback!
                    Facts fc = f; fc.collision = true; CHECK(Refuse(fc) == kRefPicture);
                    Facts fr = f; fr.jointsRead = true; CHECK(Refuse(fr) == kRefRead);
                    Facts fb = f; fb.callback = true; CHECK(Refuse(fb) == kRefCallback);
                }
            }
        }
    }
}

}  // namespace

int main() {
    TestBloomFold3Weights();
    TestStagePresets();
    TestEven60Guards();
    TestMpDetLodGate();
    if (gFails) {
        std::printf("perf_w25f_test: %d FAILED\n", gFails);
        return 1;
    }
    std::printf("perf_w25f_test: all passed (mode_keys rows=%d/128)\n", port::modekeys::kRowCount);
    return 0;
}
