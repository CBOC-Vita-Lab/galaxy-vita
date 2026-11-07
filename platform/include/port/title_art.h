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
constexpr int kBubbleVersion = 1;   // raise when the pictures made from the capture change
constexpr const char* kArtDir = "ux0:data/smg/art";
constexpr const char* kTitlePath = "ux0:data/smg/art/title.rgba";
constexpr const char* kStampPath = "ux0:data/smg/art/title.ver";
constexpr const char* kDefaultTitleId = "SMGV00001";

// ------------------------------------------------------------------------------------------------ what to do
enum class Plan { kNone, kBuildOnly, kCapture };
struct Stamp {
    int capture = 0, bubble = 0;
    long icon = -1;  // size of the app folder's icon0.png right after the pictures were last made
};
inline bool ParseStamp(const char* text, Stamp& out) {
    out = Stamp{};
    if (!text) return false;
    int c = 0, b = 0;
    long i = 0;
    if (std::sscanf(text, "SMGART capture=%d bubble=%d icon=%ld", &c, &b, &i) != 3) return false;
    out.capture = c;
    out.bubble = b;
    out.icon = i;
    return true;
}
inline int FormatStamp(const Stamp& s, char* out, size_t size) {
    return std::snprintf(out, size, "SMGART capture=%d bubble=%d icon=%ld\n", s.capture, s.bubble, s.icon);
}
// mode: GXV_TITLEART (0 off, 1 default, 2 force a new capture). `haveTitle`: the capture file exists with the exact size.
// `appIconBytes`: size of the app folder's icon0.png now (-1 when it cannot be read): a package update puts its own picture back,
// which shows as a different size than the one recorded after the last build.
inline Plan Decide(int mode, bool haveTitle, const Stamp& stamp, long appIconBytes) {
    if (mode <= 0) return Plan::kNone;
    if (mode >= 2) return Plan::kCapture;
    if (!haveTitle || stamp.capture != kCaptureVersion) return Plan::kCapture;
    if (stamp.bubble != kBubbleVersion || appIconBytes < 0 || appIconBytes != stamp.icon) return Plan::kBuildOnly;
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

// Removes the text lines from `rgb` (960x544x3, changed in place): the bright or grey pixels of the rectangle (and their dark
// outlines), grown by 4 pixels, are filled from the sky around them (each pass gives a masked pixel the mean of the known pixels
// within two, working inwards from the edge), then given back the stars the surrounding sky has, in the same density and brightness.
// `mask` is 960*544 bytes of scratch. Deterministic.
inline void CleanTitle(uint8_t* rgb, uint8_t* mask) {
    constexpr int by0 = kTextY0 - 4, by1 = kTextY1 + 4, bx0 = kTextX0 - 4, bx1 = kTextX1 + 4;  // the masked pixels lie inside this band
    std::memset(mask, 0, size_t(kW) * kH);
    for (int y = kTextY0; y < kTextY1; ++y)
        for (int x = kTextX0; x < kTextX1; ++x) {
            const uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            if (p[0] + p[1] + p[2] > 330 || p[2] < p[0] + 30) mask[y * kW + x] = 1;
        }
    {  // grow by 4 pixels in each direction (two passes of a 5x5 box, as the reference does)
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
    // mask bits: 1 = was text, 2 = filled (counts as known), 4 = filled in the current pass
    for (int pass = 0; pass < 200; ++pass) {
        bool open = false;
        for (int y = by0; y < by1; ++y)
            for (int x = bx0; x < bx1; ++x) {
                uint8_t& m = mask[y * kW + x];
                if (!(m & 1) || (m & 6)) continue;
                int sum[3] = {0, 0, 0}, n = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= kH) continue;
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = x + dx;
                        if (xx < 0 || xx >= kW) continue;
                        const uint8_t k = mask[yy * kW + xx];
                        if ((k & 1) && !(k & 2)) continue;  // still text (or filled only in this pass)
                        const uint8_t* q = rgb + (size_t(yy) * kW + xx) * 3;
                        sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2];
                        ++n;
                    }
                }
                if (!n) { open = true; continue; }
                uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
                for (int c = 0; c < 3; ++c) p[c] = uint8_t((sum[c] + n / 2) / n);
                m |= 4;
            }
        for (int y = by0; y < by1; ++y)
            for (int x = bx0; x < bx1; ++x) {
                uint8_t& m = mask[y * kW + x];
                if (m & 4) m = uint8_t((m & ~4) | 2);
            }
        if (!open) break;
    }
    {  // a few passes of a 5x5 mean over the filled pixels only, to take the diamond shapes of the edge-in fill out
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
    // Stars: the sky beside the rectangle (same rows) gives their density and colours.
    int hist[766] = {0};
    long side = 0;
    for (int y = kTextY0; y < kTextY1; ++y)
        for (int x = 0; x < kW; ++x) {
            if (x >= kTextX0 && x < kTextX1) { x = kTextX1 - 1; continue; }
            const uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            ++hist[p[0] + p[1] + p[2]];
            ++side;
        }
    long cum = 0;
    int median = 0;
    for (int v = 0; v < 766; ++v) { cum += hist[v]; if (cum * 2 >= side) { median = v; break; } }
    constexpr int kMaxStars = 4096;
    uint32_t stars[kMaxStars];
    long starCount = 0, starSeen = 0;
    for (int y = kTextY0; y < kTextY1; ++y)
        for (int x = 0; x < kW; ++x) {
            if (x >= kTextX0 && x < kTextX1) { x = kTextX1 - 1; continue; }
            const uint8_t* p = rgb + (size_t(y) * kW + x) * 3;
            if (p[0] + p[1] + p[2] > median + 60) {
                ++starSeen;
                if (starCount < kMaxStars) stars[starCount++] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
            }
        }
    if (!starCount || !side) return;
    const uint64_t threshold = uint64_t(double(starSeen) / double(side) * 4294967296.0);
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
namespace detail {
inline double Lanczos3(double x) {
    if (x < 0) x = -x;
    if (x >= 3.0) return 0.0;
    if (x < 1e-9) return 1.0;
    const double pi = 3.14159265358979323846;
    return (std::sin(pi * x) / (pi * x)) * (std::sin(pi * x / 3.0) / (pi * x / 3.0));
}
struct Taps {
    Mem<int> first, count;
    Mem<float> weight;
    int stride = 0;
};
// Pillow's Lanczos weights: output coordinate `o` (of `outSize`) over `inSize` input samples.
inline bool MakeTaps(Taps& t, int inSize, int outSize, int firstOut, int countOut) {
    const double scale = double(inSize) / outSize, fs = scale > 1.0 ? scale : 1.0, support = 3.0 * fs;
    t.stride = int(2 * support) + 3;
    if (!t.first.Alloc(countOut) || !t.count.Alloc(countOut) || !t.weight.Alloc(size_t(countOut) * t.stride)) return false;
    for (int i = 0; i < countOut; ++i) {
        const double center = (firstOut + i + 0.5) * scale;
        int xmin = int(center - support + 0.5), xmax = int(center + support + 0.5);
        if (xmin < 0) xmin = 0;
        if (xmax > inSize) xmax = inSize;
        const int n = xmax - xmin < t.stride ? xmax - xmin : t.stride;
        double sum = 0;
        float* w = t.weight.p + size_t(i) * t.stride;
        for (int x = 0; x < n; ++x) { const double v = Lanczos3((x + xmin - center + 0.5) / fs); w[x] = float(v); sum += v; }
        for (int x = 0; x < n; ++x) w[x] = sum != 0 ? float(w[x] / sum) : 0.f;
        t.first[i] = xmin;
        t.count[i] = n;
    }
    return true;
}
inline uint8_t Clamp8(float v) { return v <= 0.f ? 0 : (v >= 255.f ? 255 : uint8_t(v + 0.5f)); }
}  // namespace detail

// Resizes the sw x sh RGB image `src` (`stride` pixels per row) to fullW x fullH and writes only the window
// [ox, ox+ow) x [oy, oy+oh) of the result (tightly packed) to `dst`: Lanczos, horizontal pass first with an 8-bit
// intermediate, as Pillow does.
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
            const float* w = tx.weight.p + size_t(x) * tx.stride;
            const uint8_t* p = s + size_t(tx.first[x]) * 3;
            float a = 0, b = 0, c = 0;
            for (int k = 0; k < tx.count[x]; ++k, p += 3) { a += w[k] * p[0]; b += w[k] * p[1]; c += w[k] * p[2]; }
            d[x * 3] = Clamp8(a); d[x * 3 + 1] = Clamp8(b); d[x * 3 + 2] = Clamp8(c);
        }
    }
    for (int y = 0; y < oh; ++y) {
        const float* w = ty.weight.p + size_t(y) * ty.stride;
        for (int x = 0; x < ow * 3; ++x) {
            float a = 0;
            for (int k = 0; k < ty.count[y]; ++k) a += w[k] * mid.p[size_t(ty.first[y] + k - rowLo) * ow * 3 + x];
            dst[size_t(y) * ow * 3 + x] = Clamp8(a);
        }
    }
    return true;
}
inline int RoundHalfEven(double v) {
    const double f = std::floor(v), d = v - f;
    if (d > 0.5) return int(f) + 1;
    if (d < 0.5) return int(f);
    return (int(f) & 1) ? int(f) + 1 : int(f);
}
// The sw x sh picture scaled up or down until it covers w x h, centred and cut to w x h.
inline bool Cover(const uint8_t* src, int sw, int sh, int stride, int w, int h, uint8_t* dst) {
    const double s = std::max(double(w) / sw, double(h) / sh);
    const int rw = RoundHalfEven(sw * s), rh = RoundHalfEven(sh * s);
    return Resample(src, sw, sh, stride, rw, rh, (rw - w) / 2, (rh - h) / 2, w, h, dst);
}
// The logo box (14 px of margin) on the sky of the bottom of the capture, as startup.png and icon0.png are made.
inline bool LogoOnSky(const uint8_t* clean, const LogoBox& box, int w, int h, double fill, uint8_t* dst) {
    constexpr int pad = 14;
    if (!Cover(clean + size_t(380) * kW * 3, kW, kH - 380, kW, w, h, dst)) return false;
    const int x0 = std::max(0, box.left - pad), x1 = std::min(kW, box.right + pad), y0 = std::max(0, box.top - pad), y1 = std::min(kH, box.bottom + pad);
    const int bw = x1 - x0, bh = y1 - y0;
    if (bw < 8 || bh < 8) return false;
    const double s = std::min(w * fill / bw, h * fill / bh);
    const int lw = std::max(1, RoundHalfEven(bw * s)), lh = std::max(1, RoundHalfEven(bh * s));
    Mem<uint8_t> logo;
    Mem<float> mask, tmp;
    if (!logo.Alloc(size_t(lw) * lh * 3) || !mask.Alloc(size_t(lw) * lh) || !tmp.Alloc(size_t(lw) * lh)) return false;
    if (!Resample(clean + (size_t(y0) * kW + x0) * 3, bw, bh, kW, lw, lh, 0, 0, lw, lh, logo.p)) return false;
    // A rounded rectangle, blurred, so that the logo's box melts into the sky at its corners.
    const int radius = std::max(4, lw / 12);
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            const int dx = x - std::min(std::max(x, radius), lw - 1 - radius), dy = y - std::min(std::max(y, radius), lh - 1 - radius);
            mask[size_t(y) * lw + x] = dx * dx + dy * dy <= radius * radius ? 1.f : 0.f;
        }
    const double sigma = std::max(2, lw / 40);
    const int half = int(std::ceil(3 * sigma));
    float kernel[64];
    double ksum = 0;
    for (int k = -half; k <= half; ++k) { kernel[k + half] = float(std::exp(-0.5 * k * k / (sigma * sigma))); ksum += kernel[k + half]; }
    for (int k = 0; k <= 2 * half; ++k) kernel[k] = float(kernel[k] / ksum);
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            float a = 0;
            for (int k = -half; k <= half; ++k) a += kernel[k + half] * mask[size_t(y) * lw + std::min(std::max(x + k, 0), lw - 1)];
            tmp[size_t(y) * lw + x] = a;
        }
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            float a = 0;
            for (int k = -half; k <= half; ++k) a += kernel[k + half] * tmp[size_t(std::min(std::max(y + k, 0), lh - 1)) * lw + x];
            mask[size_t(y) * lw + x] = a;
        }
    const int px = (w - lw) / 2, py = (h - lh) / 2;
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            const int tx = px + x, ty = py + y;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            uint8_t* d = dst + (size_t(ty) * w + tx) * 3;
            const uint8_t* l = logo.p + (size_t(y) * lw + x) * 3;
            const float m = mask[size_t(y) * lw + x];
            for (int c = 0; c < 3; ++c) d[c] = detail::Clamp8(d[c] + (l[c] - d[c]) * m);
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
// Where a picture goes: the app folder (read by the LiveArea) and, for the two the shell caches, ur0:appmeta. Returns the count (1 or 2).
inline int Targets(int which, const char* titleId, char (&out)[2][96]) {
    static const char* const app[kPictureCount] = {"sce_sys/pic0.png", "sce_sys/livearea/contents/bg.png", "sce_sys/livearea/contents/startup.png", "sce_sys/icon0.png"};
    std::snprintf(out[0], sizeof out[0], "ux0:app/%s/%s", titleId, app[which]);
    if (which == kPic0 || which == kIcon) {
        std::snprintf(out[1], sizeof out[1], "ur0:appmeta/%s/%s", titleId, which == kPic0 ? "pic0.png" : "icon0.png");
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
// Memory: 5.5 MiB at the peak (measured) on top of the 2 MiB capture itself, all from malloc, released before it returns.
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
        if (which == kBackground) ok = Cover(rgb.p, kW, kH, kW, w, h, pic.p), source = pic.p;
        else if (which == kStartup) ok = LogoOnSky(rgb.p, box, w, h, 0.92, pic.p), source = pic.p;
        else if (which == kIcon) ok = LogoOnSky(rgb.p, box, w, h, 0.98, pic.p), source = pic.p;
        Palette pal;
        size_t length = 0;
        ok = ok && Quantize(source, w, h, idx.p, pal) && EncodePng8(idx.p, w, h, pal, png.p, png.n, length);
        if (ok && sink(context, which, png.p, length) && okCount) ++*okCount;
        all = all && ok;
    }
    return all;
}

}  // namespace port::titleart
