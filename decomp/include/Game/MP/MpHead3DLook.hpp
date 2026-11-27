// Galaxy 3.0 multiplayer: what a 3D head shows for a state (owner: heads). Pure code (no game, no SDK): host tested in
// tests/mp/head3d_look_test.cpp. MpHead3D.cpp draws what this says.
//
// A head is FREE (nobody has it), TAKEN (another player has it) or MINE (the local player has it). Every state keeps the head's own
// colours and its spin: nothing is washed, faded or ghosted (a washed head looked the same as a Cosmic one, and nobody could tell WHO
// had it). A TAKEN or MINE head wears a small badge with its owner's player number ("2P") in that character's colour
// (MpCharColor: the same colour as the name tags and the pointers), on the head's upper right; a free head has none. Choosing a
// TAKEN head makes the head shake its head "no" (a short damped turn left and right).
#pragma once

#include <math.h>
#include <stdint.h>

#include "Game/MP/MpCharColor.hpp"

namespace MpHead3DLook {

constexpr int kMaxOwner = 4;   // MP_MAX_PLAYERS: the badge says 1P .. 4P

// ---- the state of one head, as the screens give it
struct State {
    int look = 0;           // MpLook: Mario, Luigi, Toad, Cosmic Mario, Yellow Toad, Cosmic Luigi
    int owner = -1;         // -1: free; 0..3: the slot that has this character
    bool mine = false;      // that slot is the local player
    bool dimmed = false;    // away / quiet: drawn at kDimAlpha
};

enum Tag : uint8_t { TAG_FREE = 0, TAG_TAKEN, TAG_MINE };
inline Tag tag(const State& s) {
    if (s.owner < 0 || s.owner >= kMaxOwner) return TAG_FREE;
    return s.mine ? TAG_MINE : TAG_TAKEN;
}

constexpr float kDimAlpha = 0.45f;
// The head's own alpha for a state: never faded for being taken (only a dimmed head, or the screen's own fade, lowers it)
inline float alpha(const State& s, float screenAlpha) {
    float a = screenAlpha < 0.0f ? 0.0f : (screenAlpha > 1.0f ? 1.0f : screenAlpha);
    return s.dimmed ? a * kDimAlpha : a;
}

// ---- the badge
struct Badge {
    bool shown = false;
    char text[3] = {0, 0, 0};        // "2P"
    MpCharColor::C fill = {0, 0, 0}; // the character's colour (plate), edge: its dark edge
    MpCharColor::C edge = {0, 0, 0};
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the pill, in square units (y down)
    float radius = 0;                       // its corner radius (half its height: a pill)
    float textSize = 0;                     // the words' line height
    float ring = 0;                         // the white rim's thickness (the local player's is thicker)
    bool mine = false;
    float alpha = 1.0f;
};

// The pill sits on the head's upper right corner. size: the head's box height (square units); scale: its pop and growth (1 = at rest)
constexpr float kBadgeH = 0.27f, kBadgeW = 1.75f, kBadgeDx = 0.36f, kBadgeDy = 0.34f;
inline Badge badge(const State& s, float x, float y, float size, float scale, float screenAlpha) {
    Badge b;
    if (tag(s) == TAG_FREE || !(size > 0.0f) || !(scale > 0.0f)) return b;
    b.shown = true;
    b.text[0] = static_cast<char>('1' + s.owner);
    b.text[1] = 'P';
    b.mine = s.mine;
    b.fill = MpCharColor::fill(s.look);
    b.edge = MpCharColor::edge(s.look);
    const float h = kBadgeH * size * scale, w = h * kBadgeW;
    const float cx = x + kBadgeDx * size * scale, cy = y - kBadgeDy * size * scale;
    b.x0 = cx - w * 0.5f, b.x1 = cx + w * 0.5f, b.y0 = cy - h * 0.5f, b.y1 = cy + h * 0.5f;
    b.radius = h * 0.5f;
    b.textSize = h * 0.74f;
    b.ring = h * (s.mine ? 0.13f : 0.08f);
    b.alpha = alpha(s, screenAlpha);
    return b;
}

// ---- the "no" shake of a taken head that was chosen: the yaw (degrees) `step` frames after, 0 once it is over
constexpr int kShakeFrames = 22;
constexpr float kShakeDeg = 15.0f;
inline bool shaking(int step) { return step >= 0 && step < kShakeFrames; }
inline float shakeYaw(int step) {
    if (!shaking(step)) return 0.0f;
    const float t = static_cast<float>(step) / static_cast<float>(kShakeFrames);
    const float fade = (1.0f - t) * (1.0f - t);   // (damped)
    return kShakeDeg * fade * sinf(t * 3.0f * 2.0f * 3.14159265f);   // three swings
}

}  // namespace MpHead3DLook
