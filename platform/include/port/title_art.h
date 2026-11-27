#pragma once
// Pictures the game builds from the player's own running copy, so the public package carries no game artwork.
//
// The first time the real title screen has been fully shown, the Vita runtime (vita/vgx-game/title_art_runtime.cpp) copies the
// presented 960x544 frame to ux0:data/smg/art/title.rgba. From that capture it makes the loading-screen picture used from the next
// start on, the Settings backdrop, and the home-screen bubble and LiveArea pictures. This header is the part that has no console
// calls, so the host tests run it as it is: when to capture, what a title frame looks like, taking the two text lines out of it,
// finding the logo, cutting the four pictures, and where they go. tools/handheld/make_livearea_title.py is the host-side reference
// of the picture algorithm (it uses OpenCV's inpainting where this uses a plain neighbour fill).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "title_art_png.h"

namespace port::titleart {

constexpr int kW = 960, kH = 544;
constexpr size_t kRgbaBytes = size_t(kW) * kH * 4;
constexpr int kCaptureVersion = 1;  // raise when what is captured (not how it is turned into pictures) changes
constexpr int kBubbleVersion = 2;   // raise when the pictures made from the capture change (2: integer resampling, round-bubble framing, calm LiveArea zones)
constexpr const char* kArtDir = "ux0:data/smg/art";
constexpr const char* kTitlePath = "ux0:data/smg/art/title.rgba";
constexpr const char* kStampPath = "ux0:data/smg/art/title.ver";
constexpr const char* kDefaultTitleId = "SMGV00001";

// ------------------------------------------------------------------------------------------------ what to do
enum class Plan { kNone, kBuildOnly, kCapture };
struct Stamp {
    int capture = 0, bubble = 0;
    long icon = -1;      // size of the app folder's icon0.png right after the pictures were last made
    uint32_t sum = 0;    // BubbleSum of the bubble files right after the pictures were last made (since picture version 2)
    bool hasSum = false; // (a stamp written before version 2 has no sum)
};
inline bool ParseStamp(const char* text, Stamp& out) {
    out = Stamp{};
    if (!text) return false;
    int c = 0, b = 0;
    long i = 0;
    unsigned sum = 0;
    const int n = std::sscanf(text, "SMGART capture=%d bubble=%d icon=%ld sum=%x", &c, &b, &i, &sum);
    if (n < 3) return false;
    out.capture = c;
    out.bubble = b;
    out.icon = i;
    out.sum = sum;
    out.hasSum = n == 4;
    return true;
}
inline int FormatStamp(const Stamp& s, char* out, size_t size) {
    if (!s.hasSum) return std::snprintf(out, size, "SMGART capture=%d bubble=%d icon=%ld\n", s.capture, s.bubble, s.icon);
    return std::snprintf(out, size, "SMGART capture=%d bubble=%d icon=%ld sum=%08x\n", s.capture, s.bubble, s.icon, unsigned(s.sum));
}
// A checksum of what the home screen shows: the bytes of the app folder's icon0.png and of the shell's copy in ur0:appmeta, each
// preceded by its length (a file that cannot be read counts as length 0xFFFFFFFF and no bytes). Fed piece by piece.
struct BubbleSum {
    uint32_t crc = 0;
    void File(const uint8_t* data, long length) {
        const uint32_t n = length < 0 ? 0xFFFFFFFFu : uint32_t(length);
        const uint8_t head[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
        crc = detail::Crc32(head, 4, crc);
        if (length > 0 && data) crc = detail::Crc32(data, size_t(length), crc);
    }
};
// mode: GXV_TITLEART (0 off, 1 default, 2 force a new capture). `haveTitle`: the capture file exists with the exact size.
// `appIconBytes`: size of the app folder's icon0.png now (-1 when it cannot be read). `sum`: BubbleSum of the bubble files now.
// A package update (or the console's "Refresh LiveArea", or anything else) that puts other pictures in place shows as a different
// size or, since picture version 2, as a different checksum even when the size happens to be the same.
inline Plan Decide(int mode, bool haveTitle, const Stamp& stamp, long appIconBytes, uint32_t sum = 0) {
    if (mode <= 0) return Plan::kNone;
    if (mode >= 2) return Plan::kCapture;
    if (!haveTitle || stamp.capture != kCaptureVersion) return Plan::kCapture;
    if (stamp.bubble != kBubbleVersion || appIconBytes < 0 || appIconBytes != stamp.icon) return Plan::kBuildOnly;
    if (stamp.hasSum && stamp.sum != sum) return Plan::kBuildOnly;
    return Plan::kNone;
}

// ------------------------------------------------------------------------------------------------ when to capture
// A sparse grid over the logo's rows and columns, read from the display twice 0.5 s apart: nearly equal means the title has settled.
constexpr int kSampleRow0 = 48, kSampleRow1 = 352, kSampleCol0 = 216, kSampleCol1 = 744, kSampleStep = 6;
constexpr int kSampleRows = (kSampleRow1 - kSampleRow0 + kSampleStep - 1) / kSampleStep;
constexpr int kSampleCols = (kSampleCol1 - kSampleCol0 + kSampleStep - 1) / kSampleStep;
constexpr int kSampleBytes = kSampleRows * kSampleCols * 3;
// Reads the grid from an A8B8G8R8 frame (bytes R, G, B, A in memory) with `pitchBytes` between rows.
inline void SampleFrame(const uint8_t* base, size_t pitchBytes, uint8_t* out) {
    int o = 0;
    for (int r = 0; r < kSampleRows; ++r) {
        const uint8_t* row = base + size_t(kSampleRow0 + r * kSampleStep) * pitchBytes;
        for (int c = 0; c < kSampleCols; ++c) {
            const uint8_t* p = row + size_t(kSampleCol0 + c * kSampleStep) * 4;
            out[o++] = p[0]; out[o++] = p[1]; out[o++] = p[2];
        }
    }
}
inline double MeanAbsDiff(const uint8_t* a, const uint8_t* b, int n) {
    unsigned long long sum = 0;
    for (int i = 0; i < n; ++i) sum += unsigned(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
    return n ? double(sum) / n : 255.0;
}
struct TriggerParams {
    double minDwell = 2.0;     // seconds the title must have been up (and the loading screen gone) before it counts as fully shown
    double pairGap = 0.5;      // the two samples of the stability test
    double maxDiff = 2.5;      // mean absolute difference (0..255) of the two samples that still counts as the same picture
    double forceAfter = 10.0;  // a title that never settles (a moving background) is taken anyway after this long
    double giveUp = 120.0;     // no capture this session after this long
    int maxTries = 40;         // captures that failed their checks (the title camera moves: a comet or nebula can cross the frame for a while)
};
// The decision of the capture worker, fed with the clock and whether the title is up with the loading screen gone.
class CaptureTrigger {
public:
    enum Act { kWait, kSample, kCapture, kGiveUp };
    explicit CaptureTrigger(const TriggerParams& p = TriggerParams()) : mP(p) {}
    Act Poll(double now, bool ready) {
        if (mDone) return kGiveUp;
        if (!ready) { mReadySince = -1; mLastSample = -1; mDiff = -1; return kWait; }
        if (mReadySince < 0) mReadySince = now;
        const double up = now - mReadySince;
        if (up >= mP.giveUp) { mDone = true; return kGiveUp; }
        if (mDiff >= 0 && ((up >= mP.minDwell && mDiff <= mP.maxDiff) || up >= mP.forceAfter)) return kCapture;
        if (up >= mP.minDwell - mP.pairGap && (mLastSample < 0 || now - mLastSample >= mP.pairGap)) return kSample;
        return kWait;
    }
    // After a kSample: `diff` against the sample before it, or a negative value when there was none.
    void Sampled(double now, double diff) { mLastSample = now; mDiff = diff; }
    // After a kCapture: whether the frame passed its checks and was saved.
    void Captured(bool ok) {
        if (ok || ++mFailures >= mP.maxTries) mDone = true;
        mDiff = -1;
        mLastSample = -1;
    }
    bool Done() const { return mDone; }
private:
    TriggerParams mP;
    double mReadySince = -1, mLastSample = -1, mDiff = -1;
    int mFailures = 0;
    bool mDone = false;
};

// ------------------------------------------------------------------------------------------------ the picture
// Blue or cyan (the title camera also passes over the planet's glowing cyan horizon); not green, grey or white.
inline bool IsSky(const uint8_t* p) { return p[2] > p[0] + 35 && p[2] + 25 > p[1]; }

struct LogoBox {
    int left = 0, right = 0, top = 0, bottom = 0;  // inclusive
};
// The title logo: everything that is not blue sky, in the rows above the text lines. `bpp` is 3 (RGB) or 4 (RGBA).
inline bool FindLogoBox(const uint8_t* px, int bpp, LogoBox& box) {
    Mem<int> cols, rows;
    if (!cols.AllocZero(kW) || !rows.AllocZero(kH)) return false;
    for (int y = 20; y < 380; ++y)
        for (int x = 0; x < kW; ++x) cols[x] += !IsSky(px + (size_t(y) * kW + x) * bpp);
    int left = -1, right = -1;
    for (int x = 0; x < kW; ++x)
        if (cols[x] > 25) { if (left < 0) left = x; right = x; }
    if (left < 0) return false;
    for (int y = 0; y < kH; ++y)
        for (int x = left; x <= right; ++x) rows[y] += !IsSky(px + (size_t(y) * kW + x) * bpp);
    int top = -1, bottom = -1;
    for (int y = 0; y < 390; ++y)
        if (rows[y] > 25) { if (top < 0) top = y; bottom = y; }
    if (top < 0) return false;
    box = {left, right, top, bottom};
    return true;
}
// Is this frame the title screen: blue sky down the sides, along the top and along the bottom edge, and a logo of the right size in
// the middle. (A black frame, a half-faded one, the loading screen or the first stage's picture all fail.)
inline bool LooksLikeTitle(const uint8_t* px, int bpp) {
    auto fraction = [&](int x0, int x1, int y0, int y1) {
        long sky = 0, all = 0;
        for (int y = y0; y < y1; y += 2)
            for (int x = x0; x < x1; x += 2) { sky += IsSky(px + (size_t(y) * kW + x) * bpp); ++all; }
        return all ? double(sky) / double(all) : 0.0;
    };
    if (fraction(0, kW, 0, 16) < 0.9 || fraction(0, kW, 528, kH) < 0.9 || fraction(0, 60, 0, kH) < 0.9 || fraction(900, kW, 0, kH) < 0.9) return false;
    LogoBox b;
    if (!FindLogoBox(px, bpp, b)) return false;
    const int w = b.right - b.left + 1, h = b.bottom - b.top + 1, cx = (b.left + b.right) / 2;
    return w >= 380 && w <= 720 && h >= 200 && h <= 340 && cx >= 420 && cx <= 540 && b.top >= 20 && b.top <= 120;
}

// The two text lines ("Press START ..." and the copyright) lie in this rectangle of the capture.
constexpr int kTextY0 = 392, kTextY1 = 518, kTextX0 = 222, kTextX1 = 738;

// ------------------------------------------------------------------------------------------------ geometry of the outputs
// Every number the pictures depend on is here, each tunable from a real capture or a console screenshot (docs/ASSETS.md). The host
// reference tools/handheld/make_livearea_title.py has the same constants with the same names.
constexpr int kSkyY0 = 380;        // capture rows from here down (the text taken out) are the plain sky behind the cut-out logo
constexpr int kLogoPad = 14;       // capture pixels of the title's own sky kept around the logo box
constexpr int kLogoMaskRound = 12; // the logo's cut-out is a rounded rectangle with a corner radius of 1/12 of its width...
constexpr int kLogoMaskSoft = 40;  // ...softened over 1/40 of its width (three box passes), so it melts into the sky
// icon0.png 128x128: the home screen shows it inside a circle (the bubble) of the full width, whose rim covers about 3 px.
constexpr int kIconLogoRadius = 54;   // every logo pixel lies within this many pixels of the icon's centre (circle 64, margin 10)
constexpr int kIconFill = 96;         // ...and the logo box is at most this many pixels wide (a small logo is not blown up)
constexpr int kIconVignetteInner = 600, kIconVignette = 100;  // darker towards the rim: from 600/1024 of the radius out, by up to 100/256
// startup.png 280x158: the gate picture above the system's Start button.
constexpr int kGateMarginX = 16, kGateMarginY = 12;          // pixels of sky left and right / above and below the logo
constexpr int kGateVignetteInner = 560, kGateVignette = 70;
// bg.png 840x500: the system draws its own gate, frames and buttons over it (style a1, as the template asks). Assumed places, in
// background pixels (tune from a console screenshot): the gate picture and its Start button sit in the middle; the frames along the
// bottom. There the picture is softened and darkened so that what the system draws stays legible.
constexpr int kGateX0 = 262, kGateY0 = 96, kGateX1 = 578, kGateY1 = 392;
constexpr int kCalmFeather = 96;      // pixels over which the treatment fades out around the gate
constexpr int kCalmKeep = 200;        // brightness kept under the gate, of 256
constexpr int kCalmBlur = 3;          // box radius of the softening (three passes: about a Gaussian of 3 px)
constexpr int kFrameBandY = 400;      // from this row down, darker towards the bottom edge...
constexpr int kFrameBandKeep = 190;   // ...down to this brightness (of 256) at the edge

namespace detail {
inline int64_t FloorDiv(int64_t a, int64_t b) {  // rounds towards minus infinity, as Python's // does
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline uint64_t ISqrt(uint64_t v) {  // the exact integer square root (a first guess, then corrected)
    uint64_t r = uint64_t(std::sqrt(double(v)));
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}
inline uint8_t Clamp8(int v) { return v <= 0 ? 0 : (v >= 255 ? 255 : uint8_t(v)); }
}  // namespace detail

// The median brightness (sum of the three channels) of row y of `rgb` outside the text rectangle's columns.
inline int SideMedian(const uint8_t* rgb, int y) {
    int hist[766] = {0};
    int count = 0;
    for (int x = 0; x < kW; ++x) {
        if (x >= kTextX0 && x < kTextX1) { x = kTextX1 - 1; continue; }
        const uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
        ++hist[p[0] + p[1] + p[2]];
        ++count;
    }
    int cum = 0;
    for (int v = 0; v < 766; ++v) { cum += hist[v]; if (cum * 2 >= count) return v; }
    return 765;
}

// Removes the text lines from `rgb` (960x544x3, changed in place). The text pixels of the rectangle (grey, or much brighter than
// the row's sky; with their dark outlines), grown by 4 pixels, are filled from the sky around them: each such pixel is interpolated between the nearest sky
// pixels above and below it and between those left and right of it (each end the median of five sky pixels across, so that a star
// at the edge is not drawn out into a streak), the two weighted towards the shorter span: the title's sky is a smooth, mostly
// vertical gradient and comes back as one. Then a few passes of a 5x5 mean over the filled pixels, and the stars the surrounding
// sky has, in the same density and brightness. `mask` is 960*544 bytes of scratch. Deterministic, integers only.
inline void CleanTitle(uint8_t* rgb, uint8_t* mask) {
    constexpr int by0 = kTextY0 - 4, by1 = kTextY1 + 4, bx0 = kTextX0 - 4, bx1 = kTextX1 + 4;  // the masked pixels lie inside this band
    std::memset(mask, 0, size_t(kW) * kH);
    // Text: grey (the outlines, and white letters on any sky: blue not well above red), or much brighter than the sky beside the
    // rectangle in the same row (so a glowing horizon behind the text is sky, not text).
    constexpr int kTextLift = 150;  // brighter than the row's median sky (sum of the three channels) by more than this: text
    for (int y = kTextY0; y < kTextY1; ++y) {
        const int median = SideMedian(rgb, y);
        for (int x = kTextX0; x < kTextX1; ++x) {
            const uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            if (p[0] + p[1] + p[2] > median + kTextLift || p[2] < p[0] + 30) mask[y * kW + x] = 1;
        }
    }
    {  // grow by 4 pixels in each direction (two passes of a 5x5 box)
        uint8_t row[kW];
        for (int y = by0; y < by1; ++y) {
            uint8_t* m = mask + y * kW;
            std::memcpy(row, m, kW);
            for (int x = bx0; x < bx1; ++x) {
                uint8_t any = 0;
                for (int k = -4; k <= 4; ++k) any |= row[x + k];
                m[x] = any;
            }
        }
        uint8_t col[by1 - by0 + 8];
        for (int x = bx0; x < bx1; ++x) {
            for (int y = by0; y < by1; ++y) col[y - by0] = mask[y * kW + x];
            for (int y = by0; y < by1; ++y) {
                uint8_t any = 0;
                for (int k = -4; k <= 4; ++k) {
                    const int yy = y + k;
                    if (yy >= by0 && yy < by1) any |= col[yy - by0];
                }
                mask[y * kW + x] = any;
            }
        }
    }
    // The ends of a span: the median, per channel, of the (up to five) sky pixels across the span's direction at (ex, ey).
    auto end = [&](int ex, int ey, bool vertical, int out[3]) {
        uint8_t v[3][5];
        int n = 0;
        for (int k = -2; k <= 2; ++k) {
            const int xx = vertical ? ex + k : ex, yy = vertical ? ey : ey + k;
            if (xx < 0 || yy < 0 || xx >= kW || yy >= kH || (mask[yy * kW + xx] & 1)) continue;
            const uint8_t* q = rgb + (size_t(yy) * kW + xx) * 3;
            for (int c = 0; c < 3; ++c) v[c][n] = q[c];
            ++n;
        }
        for (int c = 0; c < 3; ++c) {
            std::sort(v[c], v[c] + n);
            out[c] = v[c][n / 2];
        }
    };
    for (int y = by0; y < by1; ++y)
        for (int x = bx0; x < bx1; ++x) {
            if (!(mask[y * kW + x] & 1)) continue;
            int yu = y, yd = y, xl = x, xr = x;
            while (mask[yu * kW + x] & 1) --yu;  // (the band is surrounded by sky, which is never masked)
            while (mask[yd * kW + x] & 1) ++yd;
            while (mask[y * kW + xl] & 1) --xl;
            while (mask[y * kW + xr] & 1) ++xr;
            int eu[3], ed[3], el[3], er[3];
            end(x, yu, true, eu);
            end(x, yd, true, ed);
            end(xl, y, false, el);
            end(xr, y, false, er);
            const int du = y - yu, dd = yd - y, dl = x - xl, dr = xr - x, vs = du + dd, hs = dl + dr;
            uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            for (int c = 0; c < 3; ++c) {
                const int v = (eu[c] * dd + ed[c] * du + vs / 2) / vs;
                const int h = (el[c] * dr + er[c] * dl + hs / 2) / hs;
                p[c] = uint8_t((v * hs + h * vs + (vs + hs) / 2) / (vs + hs));
            }
        }
    {  // a few passes of a 5x5 mean over the filled pixels only, to soften the joins of the two interpolations
        constexpr int bw = bx1 - bx0, bh = by1 - by0;
        Mem<uint8_t> copy;
        if (copy.Alloc(size_t(bw) * bh * 3)) {
            for (int pass = 0; pass < 3; ++pass) {
                for (int y = 0; y < bh; ++y) std::memcpy(copy.p + size_t(y) * bw * 3, rgb + (size_t(by0 + y) * kW + bx0) * 3, size_t(bw) * 3);
                for (int y = by0; y < by1; ++y)
                    for (int x = bx0; x < bx1; ++x) {
                        if (!(mask[y * kW + x] & 1)) continue;
                        int sum[3] = {0, 0, 0}, n = 0;
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx) {
                                const int yy = y + dy - by0, xx = x + dx - bx0;
                                if (yy < 0 || yy >= bh || xx < 0 || xx >= bw) continue;
                                const uint8_t* q = copy.p + (size_t(yy) * bw + xx) * 3;
                                sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2];
                                ++n;
                            }
                        uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
                        for (int c = 0; c < 3; ++c) p[c] = uint8_t((sum[c] + n / 2) / n);
                    }
            }
        }
    }
    // Stars: the sky beside the rectangle (same rows) gives their density and colours. A star is a pixel well above the median
    // brightness of its own row (the title's sky brightens towards a glowing horizon, so one median for all rows would count the
    // glow as stars).
    constexpr int kStarLift = 60;  // brighter than the row's median (sum of the three channels) by more than this: a star
    constexpr int kMaxStars = 4096;
    uint32_t stars[kMaxStars];
    long starCount = 0, starSeen = 0, side = 0;
    for (int y = kTextY0; y < kTextY1; ++y) {
        const int median = SideMedian(rgb, y), count = kW - (kTextX1 - kTextX0);
        side += count;
        for (int x = 0; x < kW; ++x) {
            if (x >= kTextX0 && x < kTextX1) { x = kTextX1 - 1; continue; }
            const uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            if (p[0] + p[1] + p[2] > median + kStarLift) {
                ++starSeen;
                if (starCount < kMaxStars) stars[starCount++] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
            }
        }
    }
    if (!starCount || !side) return;
    const uint64_t threshold = uint64_t(starSeen) * 4294967296ull / uint64_t(side);
    uint32_t s = 1985u;
    auto next = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    for (int y = by0; y < by1; ++y)
        for (int x = bx0; x < bx1; ++x) {
            if (!(mask[y * kW + x] & 1)) continue;
            if (uint64_t(next()) >= threshold) continue;
            const uint32_t star = stars[next() % uint32_t(starCount)];
            uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            for (int c = 0; c < 3; ++c) p[c] = std::max<uint8_t>(p[c], uint8_t(star >> (8 * c)));
        }
}

// ------------------------------------------------------------------------------------------------ resizing and compositing
// All in integers, so that the console, the host tests and the Python reference give the very same pixels.
namespace detail {
constexpr int kWeightBits = 14;
// Keys' cubic convolution (a = -1/2, the "bicubic" of most tools) at |t| = num / den, in 1/65536 units.
inline int64_t Cubic(int64_t num, int64_t den) {
    if (num < 0) num = -num;
    constexpr int64_t S = 65536;
    const int64_t t = num * S / den;
    if (t >= 2 * S) return 0;
    if (t < S) return FloorDiv(3 * t * t * t - 5 * t * t * S + 2 * S * S * S, 2 * S * S);
    return FloorDiv(-t * t * t + 5 * t * t * S - 8 * t * S * S + 4 * S * S * S, 2 * S * S);
}
struct Taps {
    Mem<int> first, count, weight;
    int stride = 0;
};
// The filter taps of outputs [firstOut, firstOut + countOut) of a resize from inSize to outSize samples. Downscaling stretches the
// kernel by the scale (an area-weighted, antialiased cubic); weights in 1/16384 that sum to exactly 16384.
inline bool MakeTaps(Taps& t, int inSize, int outSize, int firstOut, int countOut) {
    const int64_t big = std::max(inSize, outSize), q = 2 * big;
    const int support = int((2 * big + outSize - 1) / outSize) + 1;  // input samples on each side of the centre
    t.stride = 2 * support + 2;
    if (!t.first.Alloc(countOut) || !t.count.Alloc(countOut) || !t.weight.Alloc(size_t(countOut) * t.stride)) return false;
    int64_t raw[512];
    if (t.stride > 512) return false;
    for (int i = 0; i < countOut; ++i) {
        const int64_t c2 = (2 * int64_t(firstOut + i) + 1) * inSize;  // the output's centre, in 1/(2 outSize) input samples
        const int xc = int(c2 / (2 * int64_t(outSize)));
        const int xmin = std::max(0, xc - support), xmax = std::min(inSize - 1, xc + support);
        const int n = xmax - xmin + 1;
        int64_t sum = 0;
        int peak = 0;
        for (int k = 0; k < n; ++k) {
            raw[k] = Cubic((2 * int64_t(xmin + k) + 1) * outSize - c2, q);
            sum += raw[k];
            if (raw[k] > raw[peak]) peak = k;
        }
        int* w = t.weight.p + size_t(i) * t.stride;
        if (sum <= 0) {  // (cannot happen for these kernels; nearest sample as a safe fallback)
            for (int k = 0; k < n; ++k) w[k] = 0;
            w[std::min(std::max(xc - xmin, 0), n - 1)] = 1 << kWeightBits;
        } else {
            int total = 0;
            for (int k = 0; k < n; ++k) { w[k] = int(FloorDiv(raw[k] * (2 << kWeightBits) + sum, 2 * sum)); total += w[k]; }
            w[peak] += (1 << kWeightBits) - total;
        }
        t.first[i] = xmin;
        t.count[i] = n;
    }
    return true;
}
inline uint8_t Weighted(int64_t acc) { return Clamp8(int((acc + (1 << (kWeightBits - 1))) >> kWeightBits)); }
}  // namespace detail

// Resizes the sw x sh RGB image `src` (`stride` pixels per row) to fullW x fullH and writes only the window [ox, ox+ow) x [oy, oy+oh)
// of the result (tightly packed) to `dst`. Separable, horizontal pass first into an 8-bit intermediate.
inline bool Resample(const uint8_t* src, int sw, int sh, int stride, int fullW, int fullH, int ox, int oy, int ow, int oh, uint8_t* dst) {
    using namespace detail;
    Taps tx, ty;
    if (!MakeTaps(tx, sw, fullW, ox, ow) || !MakeTaps(ty, sh, fullH, oy, oh)) return false;
    int rowLo = sh, rowHi = 0;
    for (int i = 0; i < oh; ++i) { rowLo = std::min(rowLo, ty.first[i]); rowHi = std::max(rowHi, ty.first[i] + ty.count[i]); }
    if (rowHi <= rowLo) return false;
    Mem<uint8_t> mid;
    if (!mid.Alloc(size_t(rowHi - rowLo) * ow * 3)) return false;
    for (int y = rowLo; y < rowHi; ++y) {
        const uint8_t* s = src + size_t(y) * stride * 3;
        uint8_t* d = mid.p + size_t(y - rowLo) * ow * 3;
        for (int x = 0; x < ow; ++x) {
            const int* w = tx.weight.p + size_t(x) * tx.stride;
            const uint8_t* p = s + size_t(tx.first[x]) * 3;
            int64_t a = 0, b = 0, c = 0;
            for (int k = 0; k < tx.count[x]; ++k, p += 3) { a += int64_t(w[k]) * p[0]; b += int64_t(w[k]) * p[1]; c += int64_t(w[k]) * p[2]; }
            d[x * 3] = Weighted(a); d[x * 3 + 1] = Weighted(b); d[x * 3 + 2] = Weighted(c);
        }
    }
    for (int y = 0; y < oh; ++y) {
        const int* w = ty.weight.p + size_t(y) * ty.stride;
        for (int x = 0; x < ow * 3; ++x) {
            int64_t a = 0;
            for (int k = 0; k < ty.count[y]; ++k) a += int64_t(w[k]) * mid.p[size_t(ty.first[y] + k - rowLo) * ow * 3 + x];
            dst[size_t(y) * ow * 3 + x] = Weighted(a);
        }
    }
    return true;
}
// The sw x sh picture scaled up or down until it covers w x h, centred and cut to w x h.
inline bool Cover(const uint8_t* src, int sw, int sh, int stride, int w, int h, uint8_t* dst) {
    int rw, rh;
    if (int64_t(w) * sh >= int64_t(h) * sw) { rw = w; rh = int((2 * int64_t(sh) * w + sw) / (2 * int64_t(sw))); }
    else { rh = h; rw = int((2 * int64_t(sw) * h + sh) / (2 * int64_t(sh))); }
    return Resample(src, sw, sh, stride, rw, rh, (rw - w) / 2, (rh - h) / 2, w, h, dst);
}
// Three passes of a box blur of radius r over a w x h plane of ints (edges repeat), in place; `tmp` holds w*h ints.
inline void BoxBlur3(int* a, int* tmp, int w, int h, int r) {
    if (r <= 0) return;
    const int n = 2 * r + 1;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int64_t s = 0;
                for (int k = -r; k <= r; ++k) s += a[size_t(y) * w + std::min(std::max(x + k, 0), w - 1)];
                tmp[size_t(y) * w + x] = int(s / n);
            }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int64_t s = 0;
                for (int k = -r; k <= r; ++k) s += tmp[size_t(std::min(std::max(y + k, 0), h - 1)) * w + x];
                a[size_t(y) * w + x] = int(s / n);
            }
    }
}
// The square of the largest distance of a logo pixel (not sky) from the logo box's centre, in half pixels.
inline uint64_t LogoRadius2(const uint8_t* clean, const LogoBox& b) {
    uint64_t best = 0;
    for (int y = b.top; y <= b.bottom; ++y)
        for (int x = b.left; x <= b.right; ++x) {
            if (IsSky(clean + (size_t(y) * kW + x) * 3)) continue;
            const int64_t dx = 2 * x + 1 - (b.left + b.right + 1), dy = 2 * y + 1 - (b.top + b.bottom + 1);
            best = std::max<uint64_t>(best, uint64_t(dx * dx + dy * dy));
        }
    return best;
}
enum class Fit { kCircle, kBox };
// The logo (with kLogoPad of its own sky) scaled onto the sky of the bottom of the capture, centred in w x h. kCircle: every logo
// pixel within kIconLogoRadius of the centre (for the round bubble); kBox: the logo box within the margins (for the gate).
inline bool PlaceLogo(const uint8_t* clean, const LogoBox& box, int w, int h, Fit fit, uint8_t* dst) {
    if (!Cover(clean + size_t(kSkyY0) * kW * 3, kW, kH - kSkyY0, kW, w, h, dst)) return false;
    const int x0 = std::max(0, box.left - kLogoPad), x1 = std::min(kW, box.right + 1 + kLogoPad);
    const int y0 = std::max(0, box.top - kLogoPad), y1 = std::min(kH, box.bottom + 1 + kLogoPad);
    const int bw = x1 - x0, bh = y1 - y0, lw0 = box.right - box.left + 1, lh0 = box.bottom - box.top + 1;
    if (bw < 8 || bh < 8) return false;
    int64_t num, den;  // output pixels per capture pixel
    if (fit == Fit::kCircle) {
        num = 2 * kIconLogoRadius;
        den = int64_t(detail::ISqrt(LogoRadius2(clean, box))) + 1;  // (half pixels)
        if (num * lw0 > int64_t(kIconFill) * den) { num = kIconFill; den = lw0; }
    } else {
        const int64_t aw = w - 2 * kGateMarginX, ah = h - 2 * kGateMarginY;
        if (aw * lh0 <= ah * lw0) { num = aw; den = lw0; } else { num = ah; den = lh0; }
    }
    const int lw = std::max(1, int((2 * bw * num + den) / (2 * den))), lh = std::max(1, int((2 * bh * num + den) / (2 * den)));
    Mem<uint8_t> logo;
    Mem<int> mask, tmp;
    if (!logo.Alloc(size_t(lw) * lh * 3) || !mask.Alloc(size_t(lw) * lh) || !tmp.Alloc(size_t(lw) * lh)) return false;
    if (!Resample(clean + (size_t(y0) * kW + x0) * 3, bw, bh, kW, lw, lh, 0, 0, lw, lh, logo.p)) return false;
    // A rounded rectangle, softened, so that the box melts into the sky at its edges (0..256).
    const int radius = std::max(4, lw / kLogoMaskRound);
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            const int dx = x - std::min(std::max(x, radius), lw - 1 - radius), dy = y - std::min(std::max(y, radius), lh - 1 - radius);
            mask[size_t(y) * lw + x] = dx * dx + dy * dy <= radius * radius ? 256 : 0;
        }
    BoxBlur3(mask.p, tmp.p, lw, lh, std::max(1, lw / kLogoMaskSoft));
    const int px = (w - lw) / 2, py = (h - lh) / 2;
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            const int tx = px + x, ty = py + y;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            uint8_t* d = dst + (size_t(ty) * w + tx) * 3;
            const uint8_t* l = logo.p + (size_t(y) * lw + x) * 3;
            const int m = mask[size_t(y) * lw + x];
            for (int c = 0; c < 3; ++c) d[c] = detail::Clamp8(d[c] + int(detail::FloorDiv((int(l[c]) - int(d[c])) * m + 128, 256)));
        }
    return true;
}
// Darker towards the edge of the ellipse that fills w x h: nothing inside `inner` (of 1024 of the radius), then easing in to
// `strength` (of 256) at the edge and beyond.
inline void Vignette(uint8_t* img, int w, int h, int inner, int strength) {
    const int64_t in2 = int64_t(inner) * inner, full = 1024 * 1024;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int64_t dx = int64_t(2 * x + 1 - w) * 1024 / w, dy = int64_t(2 * y + 1 - h) * 1024 / h;
            const int64_t d2 = dx * dx + dy * dy;
            if (d2 <= in2) continue;
            int64_t f = std::min<int64_t>(256, (d2 - in2) * 256 / (full - in2));
            f = f * f / 256;
            const int k = 256 - int(f * strength / 256);
            uint8_t* p = img + (size_t(y) * w + x) * 3;
            for (int c = 0; c < 3; ++c) p[c] = uint8_t((p[c] * k + 128) >> 8);
        }
}
// The LiveArea background: softened and darkened where the system draws its gate, darker towards the bottom frames. The blur is
// only computed around the gate (the treatment's reach plus 3 blur radii, so that it equals a blur of the whole picture wherever
// it is used): about 3 MiB for the three planes instead of 8.
inline bool CalmZones(uint8_t* img, int w, int h) {
    const int rx0 = std::max(0, kGateX0 - kCalmFeather - 3 * kCalmBlur), rx1 = std::min(w, kGateX1 + 1 + kCalmFeather + 3 * kCalmBlur);
    const int ry0 = std::max(0, kGateY0 - kCalmFeather - 3 * kCalmBlur), ry1 = std::min(h, kGateY1 + 1 + kCalmFeather + 3 * kCalmBlur);
    const int rw = rx1 - rx0, rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) return false;
    Mem<int> plane, tmp;
    Mem<uint8_t> blurred;
    if (!plane.Alloc(size_t(rw) * rh) || !tmp.Alloc(size_t(rw) * rh) || !blurred.Alloc(size_t(rw) * rh * 3)) return false;
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x) plane[size_t(y) * rw + x] = img[(size_t(ry0 + y) * w + rx0 + x) * 3 + c];
        BoxBlur3(plane.p, tmp.p, rw, rh, kCalmBlur);
        for (size_t i = 0; i < size_t(rw) * rh; ++i) blurred[i * 3 + c] = uint8_t(plane[i]);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int64_t dx = std::max({kGateX0 - x, x - kGateX1, 0}), dy = std::max({kGateY0 - y, y - kGateY1, 0});
            const int d = int(detail::ISqrt(uint64_t(dx * dx + dy * dy)));
            int t = d >= kCalmFeather ? 0 : 256 - d * 256 / kCalmFeather;
            t = t * t * (768 - 2 * t) / 65536;  // smoothstep, 0..256
            int k = 256 - t * (256 - kCalmKeep) / 256;
            if (y >= kFrameBandY) k = k * (256 - (y - kFrameBandY) * (256 - kFrameBandKeep) / (h - kFrameBandY)) / 256;
            uint8_t* p = img + (size_t(y) * w + x) * 3;
            for (int c = 0; c < 3; ++c) {
                int v = p[c];
                if (t > 0) {  // (t > 0 only inside the blurred region)
                    const int b = blurred[(size_t(y - ry0) * rw + (x - rx0)) * 3 + c];
                    v += int(detail::FloorDiv(int64_t(b - v) * t + 128, 256));
                }
                p[c] = detail::Clamp8((v * k + 128) >> 8);
            }
        }
    return true;
}

// ------------------------------------------------------------------------------------------------ the four pictures
enum Picture { kPic0, kBackground, kStartup, kIcon, kPictureCount };
inline void PictureSize(int which, int& w, int& h) {
    static const int sizes[kPictureCount][2] = {{960, 544}, {840, 500}, {280, 158}, {128, 128}};
    w = sizes[which][0];
    h = sizes[which][1];
}
// Where a picture goes: the app folder and, for the pictures the shell shows from its own copy, ur0:appmeta. Returns the count (1 or 2).
// The shell shows icon0 / pic0 from ur0:appmeta/<ID>/ and the LiveArea from ur0:appmeta/<ID>/livearea/contents/ (copied there when the
// package is installed: 2.2 handoff, the LiveArea only changed after a fresh install). `liveAreaMeta` (GXV_TITLEART_LIVEAREA, and only
// when that folder exists: the worker checks) also sends bg.png and startup.png to the shell's LiveArea copy; without it they go to the
// app folder only, as before 2.3.
inline int Targets(int which, const char* titleId, char (&out)[2][96], bool liveAreaMeta = false) {
    static const char* const app[kPictureCount] = {"sce_sys/pic0.png", "sce_sys/livearea/contents/bg.png", "sce_sys/livearea/contents/startup.png", "sce_sys/icon0.png"};
    std::snprintf(out[0], sizeof out[0], "ux0:app/%s/%s", titleId, app[which]);
    if (which == kPic0 || which == kIcon) {
        std::snprintf(out[1], sizeof out[1], "ur0:appmeta/%s/%s", titleId, which == kPic0 ? "pic0.png" : "icon0.png");
        return 2;
    }
    if (liveAreaMeta) {
        std::snprintf(out[1], sizeof out[1], "ur0:appmeta/%s/livearea/contents/%s", titleId, which == kBackground ? "bg.png" : "startup.png");
        return 2;
    }
    return 1;
}
inline bool ValidTitleId(const char* id) {
    if (!id || std::strlen(id) != 9) return false;
    for (int i = 0; i < 9; ++i)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return false;
    return true;
}

using PictureSink = bool (*)(void* context, int which, const uint8_t* png, size_t length);
// Makes the four pictures from a 960x544 RGBA capture and hands each PNG to `sink`. `okCount` = how many it accepted.
//   pic0.png    the title without its text lines;
//   bg.png      the same, cut to 840x500, softened and darkened where the system draws its gate and frames (CalmZones);
//   startup.png the logo on the title's sky, filling the gate picture within its margins, with a light vignette;
//   icon0.png   the logo on the title's sky, every logo pixel inside the bubble's circle with a margin, darker towards the rim.
// Memory: about 7 MiB at the peak (while the LiveArea background is softened) on top of the 2 MiB capture itself, all from
// malloc, released before it returns (the worker does not start with less than 32 MiB of the C heap free).
inline bool BuildPictures(const uint8_t* rgba, PictureSink sink, void* context, int* okCount = nullptr) {
    if (okCount) *okCount = 0;
    Mem<uint8_t> rgb;
    if (!rgb.Alloc(size_t(kW) * kH * 3)) return false;
    for (size_t i = 0; i < size_t(kW) * kH; ++i) { rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2]; }
    {
        Mem<uint8_t> mask;
        if (!mask.Alloc(size_t(kW) * kH)) return false;
        CleanTitle(rgb.p, mask.p);
    }
    LogoBox box;
    if (!FindLogoBox(rgb.p, 3, box)) return false;
    Mem<uint8_t> pic, idx, png;
    if (!pic.Alloc(size_t(kW) * kH * 3) || !idx.Alloc(size_t(kW) * kH) || !png.Alloc(PngBound(kW, kH))) return false;
    bool all = true;
    for (int which = 0; which < kPictureCount; ++which) {
        int w, h;
        PictureSize(which, w, h);
        const uint8_t* source = rgb.p;
        bool ok = true;
        if (which == kBackground) ok = Cover(rgb.p, kW, kH, kW, w, h, pic.p) && CalmZones(pic.p, w, h), source = pic.p;
        else if (which == kStartup) ok = PlaceLogo(rgb.p, box, w, h, Fit::kBox, pic.p), Vignette(pic.p, w, h, kGateVignetteInner, kGateVignette), source = pic.p;
        else if (which == kIcon) ok = PlaceLogo(rgb.p, box, w, h, Fit::kCircle, pic.p), Vignette(pic.p, w, h, kIconVignetteInner, kIconVignette), source = pic.p;
        Palette pal;
        size_t length = 0;
        ok = ok && Quantize(source, w, h, idx.p, pal) && EncodePng8(idx.p, w, h, pal, png.p, png.n, length);
        if (ok && sink(context, which, png.p, length) && okCount) ++*okCount;
        all = all && ok;
    }
    return all;
}

}  // namespace port::titleart
