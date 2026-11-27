// Galaxy 3.0 multiplayer: puppet level of detail by distance, game side (owner: lod2). The rules are in MpLodLogic.hpp; MpLod.cpp
// keeps the per-slot state (band, shadow mode, tiny, the nearest) once a logic turn and answers the puppets' questions. Game thread
// only. Every function is a no-op / neutral answer without a session or with SMG_MP_LOD2=0.
//
// Switches (env, read once; the desktop environment or ux0:data/smg/env.txt): SMG_MP_LOD2=0 turns everything off (the puppets
// behave exactly as before this module); with it on, each part has its own switch, 0 = that part off:
//   SMG_MP_LOD2_SHADOW  the round surface shadow for far / governed puppets (else the volume only, as before)
//   SMG_MP_LOD2_RATE    joints every 2nd / 3rd / 4th picture by distance, staggered; no light update while skipped
//   SMG_MP_LOD2_TINY    a puppet a few pixels tall is skipped like an off-screen one; the frustum margin
//   SMG_MP_LOD2_TAGS    steady name tags skip their layout animation; the party strip heads skip unchanged work
//   SMG_MP_LOD2_SHADOWPX the shadow volume by the body's pixel size, not by distance (default on; 0: the distance tiers as before)
//   SMG_MP_LOD2_TRIM    1: far puppets' face and hand shapes are left out under SMG_MP_LOD2_TRIMPX pixels (default OFF: visible)
//   SMG_MP_LOD2_ONEPASS one model pass instead of two when no blend is running
//   SMG_MP_LOD2_CHECK=1 (desktop self-check) run both pass kinds on a copy of the state and compare the matrices (logs the result)
#pragma once

#include "Game/MP/MpLodLogic.hpp"

namespace MpLod {
    struct Slot {
        uint8_t period;    // 1..4: joints every `period` pictures (the distance band; 1 for a special puppet)
        uint8_t phase;     // stagger
        uint8_t shadow;    // MpLod::ShadowMode the puppet wants (level 0: volume near / circle beyond; level 1+: circle)
        bool tiny;         // a few pixels tall: skipped like an off-screen puppet
        bool trim;         // optfix: small on screen: its face and hand shapes are left out of the draw (SMG_MP_LOD2_TRIM=1, off by default)
        bool special;      // the nearest / watched / in a formation
    };

    bool enabled();        // the master switch (and a session)
    bool shadowOn();
    bool rateOn();
    bool tinyOn();
    bool tagsOn();
    bool onePassOn();
    bool shadowPxOn();     // optfix: shadow volume by pixel size
    bool trimOn();         // optfix: face / hands left out of small puppets (off by default)
    int checkMode();       // 0 off, 1 compare the pass kinds

    void sceneInit();
    void endScene();
    void turn();           // MpPuppets::update, after the formation and before the governor: the per-slot answers for this turn
    const Slot& slot(int slot);   // neutral (period 1, volume, not tiny) for a slot outside 0..3
    bool onePassOk(bool blendRunning, float d2);   // MpLodLogic.hpp onePassOk with the configured distance
    float cullMargin();    // multiplies the culling sphere's radius (1.0 with the TINY part off)

    // Counters for the "[mp] lod2" line (SMG_MP_PERFLOG=1): what the puppets did this window.
    enum Count { C_CALC, C_SKIP_RATE, C_SKIP_CULL, C_SKIP_TINY, C_ONE_PASS, C_TWO_PASS, C_LIGHT_SKIP, C_TAG_STEADY, C_TAG_ACTIVE,
                 C_HEAD_STEADY, C_HEAD_ACTIVE, C_COUNTS };
    void count(Count c);
    // The self-check: compare one puppet's two pass kinds (called by MpRemotePlayer when the switch is on).
    void checkNote(bool equal, int bitDiffs, float maxAbs);
}  // namespace MpLod
