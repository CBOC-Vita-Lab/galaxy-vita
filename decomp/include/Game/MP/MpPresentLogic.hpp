// Galaxy 3.0 multiplayer: what the in-play presentation shows, pure logic (owner: ingame; no game headers, host tested in
// tests/mp/present_test.cpp). The game sides: MpToast.cpp (toasts), MpNameTag.cpp (tags and edge heads), MpChat.cpp (balloons at
// the edge), MpPanelPlan.cpp (the heads drawn), MpTeam.cpp (the strip's arrivals and departures).
//
// The rule (Nintendo's own drop-in multiplayer: almost no words): a toast is shown only for something the player cannot see.
//   - no toast for what the picture shows: a bubble, a pop, a fall, everyone caught (the miss wipe), a Power Star (its cutscene),
//     a quick message or greeting (the balloon over the character), a join or a leave (the head slides in or out of the party
//     strip with the game's own sound), a rule change (the rules card);
//   - toasts stay for: the connection, who chooses the galaxy now, out of lives (needs help), the cutscene-skip vote, and the rest
//     that nothing on screen says (warps, the race's finish times, the save data, the update notice).
// Name tags: a tag shows for ~2 s when a player joins, comes back on screen, or changes character, and while their quick-message
// balloon is up; then it fades. Off screen: no words, a small head in the player's colour at the screen's edge, pointing at them,
// kept clear of the HUD's four counters and the party strip. Nothing while the game hides its own HUD (cutscenes, star get, the
// galaxy's fly-in).
#pragma once

#include <cmath>
#include <stdint.h>

namespace MpPresent {

// ---- toasts ---------------------------------------------------------------------------------------------------------------------
// Toast text ids (MpText / flow's MP_TXT_* 0x0301..): may this one be shown? (ids not listed: yes)
inline bool toastShown(uint16_t id) {
    switch (id) {
    case 1:    // MP_T_JOINED: the head slides into the strip
    case 2:    // MP_T_LEFT: the head slides out
    case 3:    // MP_T_GOT_STAR: the star-get cutscene shows it
    case 7: case 8: case 9: case 10:   // MP_T_STAMP_0..3: the greeting's balloon over the character
    case 18:   // MP_T_PLAYER_DOWN: seen
    case 24:   // MP_T_BUBBLED: the bubble is on screen
    case 25:   // MP_T_POPPED: seen
    case 26:   // MP_T_ALL_CAUGHT: the miss wipe and the reload say it
    case 0x0302:   // MP_TXT_STAR_GOT
    case 0x0303:   // MP_TXT_PLAYER_DOWN
    case 0x0309:   // MP_TXT_STAMP
        return false;
    default:
        return true;
    }
}

// Does `text` read as `tmpl` with its arguments filled in? The markup '\x01' '\x02' '\x03' '\x04' in the template stands for one or
// more characters (a name, a number, a time). Used on toasts formatted elsewhere (the menu sequence's "<name> left." and "<name>
// changed the rules: ..."), so the rule above holds for them too without their callers.
inline bool matchesTemplate(const char16_t* text, const char16_t* tmpl) {
    if (!text || !tmpl || !tmpl[0]) return false;
    const char16_t* t = text;
    const char16_t* p = tmpl;
    const char16_t* starP = nullptr;
    const char16_t* starT = nullptr;
    int guard = 0;
    while (*t && ++guard < 4096) {
        if (*p >= 1 && *p <= 4) {   // a wildcard: at least one character
            starP = p++;
            starT = ++t;
            continue;
        }
        if (*p && *p == *t) { ++p; ++t; continue; }
        if (starP) { p = starP + 1; t = ++starT; continue; }
        return false;
    }
    return *p == 0 && *t == 0;
}

// ---- name tags ------------------------------------------------------------------------------------------------------------------
constexpr int kTagShowTurns = 120;   // ~2 s
constexpr int kTagOffMin = 45;       // off screen at least this long before coming back counts as "reappeared"
constexpr int kFadeTurns = 8;

struct TagIn {
    bool present;    // the slot is in the session
    bool drawable;   // its puppet exists and is drawn in this scene
    bool onScreen;   // its head projects inside the picture, in front of the camera, and the puppet is not culled
    bool hud;        // the game shows its own HUD (counters) now: false in cutscenes, star get, the fly-in, menus over the play
    bool balloon;    // that player's quick-message balloon is up
    uint8_t look;
};
struct TagOut {
    float tag;    // the name tag's alpha (0: not drawn)
    float edge;   // the edge head's alpha (0: not drawn)
};
struct TagState {
    bool seen = false;     // present and drawable last turn
    uint8_t look = 0xFF;
    int timer = 0;         // turns of "show the name" left (counted only while it can be seen)
    int offFor = 0;        // turns off screen
    float tagA = 0.0f, edgeA = 0.0f;
    void reset() { *this = TagState(); }
    TagOut step(const TagIn& in) {
        const bool live = in.present && in.drawable;
        if (!live) {   // gone (or no puppet in this scene): nothing, at once; coming back is a "join" again
            seen = false;
            timer = 0;
            offFor = 0;
            tagA = edgeA = 0.0f;
            return TagOut{0.0f, 0.0f};
        }
        if (!seen) timer = kTagShowTurns;                                 // joined (or the scene started with them)
        if (look != 0xFF && look != in.look) timer = kTagShowTurns;       // a character change
        if (in.onScreen && offFor >= kTagOffMin) timer = kTagShowTurns;   // back on screen after a while away
        seen = true;
        look = in.look;
        offFor = in.onScreen ? 0 : (offFor < 100000 ? offFor + 1 : offFor);
        const bool visible = in.hud && in.onScreen;
        if (visible && timer > 0) --timer;
        const bool wantTag = visible && (timer > 0 || in.balloon);
        const bool wantEdge = in.hud && !in.onScreen;
        const float d = 1.0f / (float)kFadeTurns;
        if (!in.hud) {   // the game hid its HUD: ours goes with it, at once (never a tag over a cutscene's first frames)
            tagA = edgeA = 0.0f;
        } else {
            tagA = wantTag ? (tagA + d > 1.0f ? 1.0f : tagA + d) : (tagA - d < 0.0f ? 0.0f : tagA - d);
            edgeA = wantEdge ? (edgeA + d > 1.0f ? 1.0f : edgeA + d) : 0.0f;   // (the edge head goes at once when the player is in view)
        }
        if (!in.onScreen) tagA = 0.0f;   // a tag never hangs where no character is
        if (tagA < 0.0001f) tagA = 0.0f;
        return TagOut{tagA, edgeA};
    }
};

// ---- the HUD's corners and the edge heads ----------------------------------------------------------------------------------------
// Square units of the picture (y down, origin top left; the height is the game's 456 and the width that times the aspect). The
// HUD layouts sit at fixed layout units of the 608 x 456 screen layout; at 16:9 the game spreads their places over the wider
// picture (LayoutManager::calcAnim: location adjust 0.75) while their sizes stay, so a place is the layout's x times width / 608
// and a size is in square units. Measured on the desktop build's pictures (4:3 and 16:9): the Star counter (top left), the life
// meter (top right), the life counter and the party strip (bottom left), the coin and Star Bit counters (bottom right).
struct Rect { float x0, y0, x1, y1; };
constexpr int kHudRects = 5;
struct HudBox { float lx, ly, hw, hh; };   // centre in layout units (y up), half size in square units
constexpr HudBox kHud[kHudRects] = {
    {-233.0f, 175.0f, 52.0f, 30.0f},    // Star counter
    {232.0f, 148.0f, 44.0f, 52.0f},     // life meter (the LIFE ring)
    {-245.0f, -175.0f, 48.0f, 30.0f},   // life counter (head x n)
    {242.0f, -147.0f, 42.0f, 52.0f},    // coins over Star Bits
    {-126.0f, -175.0f, 0.0f, 30.0f},    // the party strip: its x span is set from the strip's heads below
};
// The party strip's first head and spacing (MpPanelPlan.hpp kStripX / kStripStep, layout units), its head radius in square units.
constexpr float kStripFirstX = -132.0f, kStripStepX = 40.0f, kStripHeadR = 22.0f;
inline float layoutX(float w, float lx) { return w * 0.5f + lx * (w / 608.0f); }
inline float layoutY(float h, float ly) { return h * 0.5f - ly; }
// stripHeads: the heads the strip shows (0..3); 0 = no strip rectangle.
inline int hudRects(float w, float h, int stripHeads, Rect* out) {
    int n = 0;
    for (int i = 0; i < kHudRects; i++) {
        const HudBox& b = kHud[i];
        if (i == 4) {
            if (stripHeads <= 0) continue;
            const float x0 = layoutX(w, kStripFirstX) - kStripHeadR, x1 = layoutX(w, kStripFirstX + kStripStepX * (stripHeads - 1)) + kStripHeadR;
            out[n++] = Rect{x0, layoutY(h, b.ly) - b.hh, x1, layoutY(h, b.ly) + b.hh};
            continue;
        }
        const float cx = layoutX(w, b.lx), cy = layoutY(h, b.ly);
        out[n++] = Rect{cx - b.hw, cy - b.hh, cx + b.hw, cy + b.hh};
    }
    return n;
}

struct EdgePlace {
    float x, y;    // the head's centre (square units)
    float ax, ay;  // unit vector from the head toward the player (the little pointer's direction)
};
inline bool finitef(float v) { return v == v && v > -1.0e30f && v < 1.0e30f; }
inline bool overlaps(const Rect& r, float x, float y, float rad) { return x + rad > r.x0 && x - rad < r.x1 && y + rad > r.y0 && y - rad < r.y1; }

// Where the edge head of an off-screen player goes. (sx, sy): the head's projection (square units); behind: the point is behind the
// camera (its projection is mirrored). The head (radius rad) sits on the picture's edge inset by `margin`, on the line from the
// centre toward the player, then slides along that edge out of every HUD rectangle (the nearest free place; around a corner if it must).
inline EdgePlace edgePlace(float sx, float sy, bool behind, float w, float h, float margin, float rad, const Rect* hud, int hudCount) {
    const float cx = w * 0.5f, cy = h * 0.5f;
    float vx = sx - cx, vy = sy - cy;
    if (!finitef(vx) || !finitef(vy)) { vx = 0.0f; vy = 1.0f; behind = false; }
    if (behind) { vx = -vx; vy = -vy; }
    if (fabsf(vx) < 1.0e-4f && fabsf(vy) < 1.0e-4f) vy = 1.0f;   // straight behind: below
    const float hw = cx - margin > 1.0f ? cx - margin : 1.0f, hh = cy - margin > 1.0f ? cy - margin : 1.0f;
    float k = fabsf(vx) / hw;
    const float ky = fabsf(vy) / hh;
    if (ky > k) k = ky;
    float x = cx + vx / k, y = cy + vy / k;
    // the perimeter of the inset rectangle as one coordinate s (clockwise from the top left), so the head can slide along it
    const float x0 = cx - hw, x1 = cx + hw, y0 = cy - hh, y1 = cy + hh;
    const float W = x1 - x0, H = y1 - y0, P = 2.0f * (W + H);
    auto toS = [&](float px, float py) {
        const float dl = fabsf(px - x0), dr = fabsf(px - x1), dt = fabsf(py - y0), db = fabsf(py - y1);
        float m = dt; int side = 0;
        if (dr < m) { m = dr; side = 1; }
        if (db < m) { m = db; side = 2; }
        if (dl < m) { side = 3; }
        switch (side) {
        case 0: return px - x0;
        case 1: return W + (py - y0);
        case 2: return W + H + (x1 - px);
        default: return 2.0f * W + H + (y1 - py);
        }
    };
    auto fromS = [&](float s, float* px, float* py) {
        s = fmodf(s, P);
        if (s < 0.0f) s += P;
        if (s < W) { *px = x0 + s; *py = y0; }
        else if (s < W + H) { *px = x1; *py = y0 + (s - W); }
        else if (s < 2.0f * W + H) { *px = x1 - (s - W - H); *py = y1; }
        else { *px = x0; *py = y1 - (s - 2.0f * W - H); }
    };
    auto blocked = [&](float px, float py) {
        for (int i = 0; i < hudCount; i++)
            if (overlaps(hud[i], px, py, rad)) return true;
        return false;
    };
    if (blocked(x, y)) {
        const float s0 = toS(x, y);
        const float step = 2.0f;
        bool found = false;
        for (float d = step; d < P * 0.5f && !found; d += step) {
            for (int dir = -1; dir <= 1 && !found; dir += 2) {
                float px, py;
                fromS(s0 + dir * d, &px, &py);
                if (!blocked(px, py)) { x = px; y = py; found = true; }
            }
        }
    }
    float ax = behind ? vx : sx - x, ay = behind ? vy : sy - y;
    const float l = sqrtf(ax * ax + ay * ay);
    if (!(l > 1.0e-4f) || !finitef(l)) { ax = 0.0f; ay = 1.0f; }
    else { ax /= l; ay /= l; }
    return EdgePlace{x, y, ax, ay};
}

// ---- the star row (Play Together's Power Star: no card) -------------------------------------------------------------------------
// While this console plays the game's own star-get demo, the faces of everyone in the session stand in a small row under the game's
// "You got a Star!" words, a small Power Star over the one who touched it. No numbers, no names, no screen of its own, no time added:
// it fades in with the words and goes when the demo ends. `age`: turns of the star-get demo so far.
constexpr int kStarRowFrom = 150;   // the demo's words come up about here (measured on the desktop build's Good Egg star get)
constexpr int kStarRowFade = 15;
inline float starRowAlpha(int age, bool demoRunning) {
    if (!demoRunning || age < kStarRowFrom) return 0.0f;
    const int t = age - kStarRowFrom;
    return t >= kStarRowFade ? 1.0f : (float)(t + 1) / (float)kStarRowFade;
}

// ---- the watch camera's pull-in ------------------------------------------------------------------------------------------------
// The follow camera's place may be inside a wall (its ideal place is computed without the map). As the game's own follow cameras do,
// the line from the watched character's focus to the camera is tested against the map; a hit pulls the camera in front of it.
// `cur` is the fraction of the focus -> camera distance kept last turn (1 = no pull), `hit` the fraction where the line first meets
// the map this turn (>= 1: no hit). The pull is at once (never a frame inside a wall), the release eases out (no pop).
constexpr float kPullMargin = 40.0f;    // game units kept in front of the wall
constexpr float kPullRelease = 0.04f;   // fraction per turn going back out
constexpr float kPullMin = 0.15f;       // never closer than this fraction (the character's own body)
inline float pullStep(float cur, float hit, float length) {
    if (!(length > 1.0f) || !finitef(length)) return 1.0f;
    if (!finitef(cur) || cur > 1.0f) cur = 1.0f;
    float target = 1.0f;
    if (finitef(hit) && hit < 1.0f) {
        target = hit - kPullMargin / length;
        if (target < kPullMin) target = kPullMin;
    }
    if (target < cur) return target;
    const float next = cur + kPullRelease;
    return next > target ? target : next;
}

}  // namespace MpPresent
