// Pictures of the handheld for the game's three "how to hold the controller" animations
// (LayoutData/SpinGuidanceReplace, BallGuidanceReplace, TiltGuidanceReplace: 128x128 drawings of a
// hand with the remote). Drawn here, at texture upload, as a plain front view of a two-stick
// handheld (original drawing: rounded body, screen, D-pad, four face buttons, two sticks) that
// rolls and pitches, or has its Square button lit. No data file: a few signed-distance shapes
// per pixel, 3x3 samples.   Included by handheld_ui.cpp (and by tools/handheld/art_test.cpp).
#include <cmath>
#include <cstdint>

namespace handheld_art {

struct Pose {
    float roll;   // degrees, positive = right side down
    float pitch;  // -1..1, positive = top edge away from the player
    int square;   // 0: as the other buttons, 1: lit, 2: lit with a small ring, 3: lit with a large ring
};

namespace detail {
struct Rgb { float r, g, b; };
inline float RoundBox(float x, float y, float hx, float hy, float r) {
    const float qx = std::fabs(x) - (hx - r), qy = std::fabs(y) - (hy - r);
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float inside = qx > qy ? qx : qy;
    return std::sqrt(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}
inline float Circle(float x, float y, float cx, float cy, float r) { return std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r; }
// Colour of one sample; (x, y) in picture units of a 128-wide picture, origin at its middle.
inline bool Sample(float x, float y, const Pose& p, float c, float s, Rgb& out) {
    const float squash = 1.0f - 0.42f * std::fabs(p.pitch);
    // into the body's own frame
    float bx = x * c + y * s, by = (-x * s + y * c) / squash;
    const float kHx = 54, kHy = 24, kR = 24;
    // the side of the body that a pitched console shows (below when the top goes away, above otherwise)
    const float edge = p.pitch * 9.0f;
    const float dBody = RoundBox(bx, by, kHx, kHy, kR);
    if (dBody > 0) {
        if (std::fabs(edge) > 0.5f && RoundBox(bx, by - edge / squash, kHx, kHy, kR) <= 0) { out = {70, 74, 86}; return true; }
        // the ring round a lit Square button may reach past the body
        if (p.square >= 2) {
            const float ring = std::fabs(Circle(bx, by, 34, -3, p.square == 2 ? 9.0f : 14.0f));
            if (ring < 1.1f) { out = {255, 120, 190}; return true; }
        }
        return false;
    }
    if (dBody > -1.6f) { out = {120, 126, 140}; return true; } // rim
    out = {34, 36, 44};                                           // body
    if (RoundBox(bx, by, 26, 16, 2) <= 0) {                       // screen, with a lighter band across it
        const float band = bx * 0.5f - by;
        out = (band > 2 && band < 9) ? Rgb{96, 150, 226} : Rgb{58, 104, 190};
        return true;
    }
    // D-pad
    if (RoundBox(bx + 40, by + 3, 7, 2.4f, 0.8f) <= 0 || RoundBox(bx + 40, by + 3, 2.4f, 7, 0.8f) <= 0) { out = {150, 156, 170}; return true; }
    // face buttons: up, right, down, and Square on the left
    if (Circle(bx, by, 40, -9, 2.7f) <= 0 || Circle(bx, by, 46, -3, 2.7f) <= 0 || Circle(bx, by, 40, 3, 2.7f) <= 0) { out = {150, 156, 170}; return true; }
    if (p.square >= 2) {
        const float ring = std::fabs(Circle(bx, by, 34, -3, p.square == 2 ? 9.0f : 14.0f));
        if (ring < 1.1f) { out = {255, 120, 190}; return true; }
    }
    if (Circle(bx, by, 34, -3, p.square ? 4.2f : 2.7f) <= 0) {
        out = p.square ? Rgb{255, 96, 176} : Rgb{150, 156, 170};
        return true;
    }
    // sticks
    for (int side = -1; side <= 1; side += 2) {
        const float d = Circle(bx, by, side * 33.0f, 13, 4.6f);
        if (d <= 0) { out = d > -1.6f ? Rgb{120, 126, 140} : Rgb{60, 64, 76}; return true; }
    }
    return true;
}
} // namespace detail

// Fills a size x size RGBA8 picture (top row first). The ground is the colour the picture has
// in its first texel (the game's own picture has been decoded into it), opaque.
inline void Draw(uint8_t* rgba, int size, const Pose& p) {
    const float bg[3] = {static_cast<float>(rgba[0]), static_cast<float>(rgba[1]), static_cast<float>(rgba[2])};
    const float unit = 128.0f / static_cast<float>(size);
    const float angle = p.roll * 3.14159265f / 180.0f, cs = std::cos(angle), sn = std::sin(angle);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float acc[3] = {0, 0, 0};
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) {
                    detail::Rgb c;
                    const float px = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 3.0f) * unit - 64.0f;
                    const float py = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 3.0f) * unit - 64.0f;
                    if (detail::Sample(px, py, p, cs, sn, c)) { acc[0] += c.r; acc[1] += c.g; acc[2] += c.b; }
                    else { acc[0] += bg[0]; acc[1] += bg[1]; acc[2] += bg[2]; }
                }
            uint8_t* o = rgba + (static_cast<size_t>(y) * size + x) * 4;
            o[0] = static_cast<uint8_t>(acc[0] / 9.0f + 0.5f);
            o[1] = static_cast<uint8_t>(acc[1] / 9.0f + 0.5f);
            o[2] = static_cast<uint8_t>(acc[2] / 9.0f + 0.5f);
            o[3] = 255;
        }
}

// The game's pictures by the hash of their texels, and what replaces each.
struct Entry { uint64_t hash; Pose pose; };
inline const Entry* Find(uint64_t hash) {
    static const Entry kEntries[] = {
        // spin: "shake" becomes "press Square"
        {0x5c8ee03db0a44a08ull, {0, 0, 1}}, {0x8deeaa9e63cd26ffull, {0, 0, 2}}, {0x2e99253f110c8a84ull, {0, 0, 3}},
        // rolling ball: the remote held upright and leant; frames 4, 5 and 8 are one picture (upright)
        {0x86b80905932085d6ull, {-16, 0, 0}}, {0x55a7b9fe04cfb1e0ull, {16, 0, 0}}, {0x24e5ac61034c6daeull, {0, -0.8f, 0}},
        {0x7c891ac94b93a354ull, {0, 0, 0}}, {0x720a2c4a8d40f663ull, {0, 0.4f, 0}}, {0x11cb720a4576ec9bull, {0, 0.9f, 0}},
        // surfing: the remote held level and twisted
        {0x6f81bcf9cef94309ull, {0, 0, 0}}, {0x1f426fe7e5032b9dull, {0, 0.5f, 0}}, {0x93eb50681f48acccull, {-18, 0.5f, 0}},
        {0x4b5a247d08cc7680ull, {18, 0.5f, 0}},
    };
    for (const Entry& e : kEntries)
        if (e.hash == hash) return &e;
    return nullptr;
}

} // namespace handheld_art
