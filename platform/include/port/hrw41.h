#pragma once
// hrw41 (16 Nov 2026): the arithmetic of GXV_HRW41_DEPTH (the second picture of a pair re-projected with the first picture's depth mask) and GXV_HRW41_BOXKEY (the
// player's box keyed to his own depth). Pure logic, no GL: tests/hrw41_test.cpp runs this code against a small ray-cast scene. The shaders that do the same per
// pixel are in platform/vgx/src/vgx_backend_gl.cpp (kFsMask / MaskFragment, D41Fragment, kFsBoxKey); the reasoning is at Depth41Env / BoxKeyEnv there.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace port::hrw41 {

// The depth code of the snapshot and the mask (kFsSnap's): the log of the view distance over 32 .. 1M game units in 0..1.
inline double DepthCode(double dist) { return std::clamp((std::log2(std::max(dist, 1.0)) - 5.0) / 15.0, 0.0, 1.0); }
inline double CodeDist(double code) { return std::exp2(code * 15.0 + 5.0); }

inline bool DepthIndependent(float x, float y, float w) { return x == 0.f && y == 0.f && w == 0.f; }

enum class MaskStep { KeepLooking, BeforeCopy, Stop };
inline MaskStep MarkerLookahead(bool copy, bool boundary, bool leaves, bool writesNow, bool changesAfter) {
    if (boundary || writesNow) return MaskStep::Stop;
    if (copy && leaves) return MaskStep::BeforeCopy;  // strictly before the copy's own clear
    return leaves || changesAfter ? MaskStep::Stop : MaskStep::KeepLooking;
}

// finish44, GXV_HRW41_MASK_PASSFIX (HRW-DESIGN S2a): the first record after the game's "the scene's depth is complete" marker lies inside an offscreen pass (an
// image effect the front end isolated; Beach Bowl's bloom / indirect pictures). Such a picture got no depth mask and its pair the old warp. The mask may be made
// just before the pass begins instead: the pass's own draws go to the pass's target, so the EFB's depth before the pass's begin is the depth at the marked record
// as long as no position from the begin up to the marked record carries an event on the EFB's depth (a copy's clear, a write). ev: the plan's events, two positions
// a command (2 * i: the command, 2 * i + 1: its clear); depthBits: the bits that mean "the EFB's depth is written here". Returns the command before which the mask
// is made (passBegin), or SIZE_MAX: refused (no pass begin known, a depth event in between, the switch off): the picture has no mask, as before the switch.
inline size_t PassFixMark(bool on, const uint8_t* ev, size_t passBegin, size_t marked, uint8_t depthBits) {
    if (!on || !ev || passBegin == SIZE_MAX || passBegin >= marked) return SIZE_MAX;
    for (size_t p = 2 * passBegin; p < 2 * marked; ++p)
        if (ev[p] & depthBits) return SIZE_MAX;
    return passBegin;
}

// Between arming A and its display copy the render thread owns an immutable
// command buffer. Reset even on a recycled address and repeated pair number.
class ProjectionMemo {
public:
    void Reset() { source_ = nullptr; }
    template<class Lookup>
    bool Get(const void* source, uint32_t id, float* out, Lookup lookup) {
        if (!source || source != source_ || id != id_) {
            source_ = nullptr;
            if (!source || !lookup(matrix_)) return false;
            source_ = source;
            id_ = id;
        }
        std::copy_n(matrix_, 16, out);
        return true;
    }
private:
    const void* source_ = nullptr;
    uint32_t id_ = 0;
    float matrix_[16]{};
};

// Work only with completed presentation windows, not a possibly queued CPU render
// timer. Latch this decision on A; the matching B must use the same mask contract.
// (c183fix F2) The detail goes off at the first finished window under kCalm (90; it used to wait for one under 75 and accept 75-89 for
// ever), comes back after need_ calm windows in a row (3, then 6, 12, 24), and after kMaxFails switch-offs in one stage visit it stays
// off until Reset (the next stage load): a stage that cannot carry the mask stops paying a bad window for each retry. Fails() and
// GaveUp() are this visit's memory; LastEvent() is what the latest Admit changed, for the caller's log.
class DetailBudget {
public:
    static constexpr int kCalm = 90, kMaxFails = 3;
    enum class Event { None, Off, On, GaveUp };
    // skipWindows (hrwfeel36, GXV_HRW41_BUDGET_SKIP): finished windows after the stage load that are not judged (1 = as before)
    void Reset(uint32_t window, int skipWindows = 1) { last_ = window >> 8; skip_ = std::max(skipWindows, 1); on_ = true; good_ = 0; need_ = 3; fails_ = 0; gaveUp_ = false; event_ = Event::None; }
    bool Admit(uint32_t window) {
        event_ = Event::None;
        const uint32_t seq = window >> 8;
        if (!seq || seq == last_) return on_;
        if (seq != (last_ == 0xffffffu ? 1u : last_ + 1u)) good_ = 0;
        last_ = seq;
        if (skip_ > 0) { --skip_; return on_; }
        if (gaveUp_) return on_;
        const int score = static_cast<int>(window & 255u);
        if (on_ && score < kCalm) {
            on_ = false; good_ = 0;
            gaveUp_ = ++fails_ >= kMaxFails;
            event_ = gaveUp_ ? Event::GaveUp : Event::Off;
        } else if (!on_) {
            good_ = score >= kCalm ? good_ + 1 : 0;
            if (good_ >= need_) { on_ = true; need_ = std::min(need_ * 2, 24); good_ = 0; event_ = Event::On; }
        }
        return on_;
    }
    Event LastEvent() const { return event_; }
    int Fails() const { return fails_; }
    int Need() const { return need_; }   // (calm windows in a row the next switch-on asks for)
    bool GaveUp() const { return gaveUp_; }
private:
    uint32_t last_ = 0;
    int good_ = 0, need_ = 3, fails_ = 0;
    int skip_ = 1;
    bool on_ = true, gaveUp_ = false;
    Event event_ = Event::None;
};

// finish44 (HRW-DESIGN S3c, log only): one line for every switch of the detail budget inside a stage visit. A change between the depth warp and the old warp
// is itself something the player can see, so each is said with the stage and the window's score. Returns the length, 0 for an event that has no line here
// (None; GaveUp has its own line).
inline int BudgetEventLine(char* out, size_t cap, DetailBudget::Event e, const char* stage, uint32_t window, int fails, int need) {
    if (!out || !cap || (e != DetailBudget::Event::Off && e != DetailBudget::Event::On)) return 0;
    const unsigned score = window & 255u;
    const int n = e == DetailBudget::Event::Off
        ? std::snprintf(out, cap, "hrw: c182 | detail budget: OFF in stage %s: window score %u under %d (switch-off %d of %d this visit): pairs on the c181 warp until %d calm windows in a row",
                        stage ? stage : "?", score, DetailBudget::kCalm, fails, DetailBudget::kMaxFails, need)
        : std::snprintf(out, cap, "hrw: c182 | detail budget: ON again in stage %s: window score %u (%d switch-offs this visit; the next return needs %d calm windows)",
                        stage ? stage : "?", score, fails, need);
    return n < 0 ? 0 : std::min(n, static_cast<int>(cap) - 1);
}

// The mask stores A-space depths. Both poses must be behind the recovered
// surface by more than Mario's body/pose allowance, not just his centre in B.
inline double ForegroundLimit(double oldDepthA, double newDepthInA, double allowance) {
    if (!std::isfinite(oldDepthA) || !std::isfinite(newDepthInA)) return -1.0;
    const double near = std::min(oldDepthA, newDepthInA) - allowance;
    return near > 32.0 ? DepthCode(near) : -1.0;
}
// The code in two 8-bit channels (MaskFragment): hi = the whole 255ths, lo = the rest; Unpack16 is linear in both, so a linearly filtered read gives the filtered code.
inline void Pack16(double code, uint8_t& hi, uint8_t& lo) {
    const double s = std::clamp(code, 0.0, 1.0) * 255.0, h = std::floor(s);
    hi = static_cast<uint8_t>(h);
    lo = static_cast<uint8_t>(std::clamp(std::floor((s - h) * 255.0 + 0.5), 0.0, 255.0));
}
inline double Unpack16(double hi01, double lo01) { return hi01 + lo01 * (1.0 / 255.0); }
// zn (the clip-space depth over w) of a point `dist` in front of a camera whose projection block has e = P[10], f = P[14]: zn = f / dist - e (WarpFragment's).
inline double ZnOfCode(double code, double e, double f) { return f / CodeDist(code) - e; }

// GXV_HRW41_BOXKEY: his depth code, the tolerance in codes for `tolUnits` game units at his distance, and the slope that takes the key from 1 at the tolerance
// to 0 at one and a half times it. (The shader takes this weight at four taps round each place and averages: KeyWeight is one tap's.)
struct Key { float code = 0.0f, tol = 0.0f, slope = 0.0f; };
inline Key MakeKey(double hisDist, double tolUnits) {
    Key k;
    const double d = std::max(hisDist, 1.0), t = std::log2(1.0 + std::max(tolUnits, 1.0) / d) / 15.0;
    k.code = static_cast<float>(DepthCode(d));
    k.tol = static_cast<float>(t);
    k.slope = static_cast<float>(2.0 / t);
    return k;
}
// The key's weight (kFsBoxKey, line for line): codeBox = the mask where the box takes its source, codeScene = the mask where the scene's warp takes its source
// for a point at his distance. 1: the box's pixel is drawn; 0: the scene's own warp stays.
inline double KeyWeight(const Key& k, double codeBox, double codeScene) {
    const double d = std::min(std::fabs(codeBox - k.code), std::fabs(codeScene - k.code));
    return std::clamp((k.tol - d) * k.slope + 1.0, 0.0, 1.0);
}

// hrwfeel36, GXV_HRW_WARP_SHARP: the depth warp's read position along one axis, in texels. t = the texture coordinate times the size, minus a half (the bilinear
// footprint's origin); i = floor(t); f = the bilinear weight toward texel i + 1. A blend of width 1 / k round each texel edge instead of 1: the weight goes
// clamp((f - 0.5) * k + 0.5, 0, 1). k = 1 is plain bilinear (the identity), larger k is crisper; the read is at i + 0.5 + weight (texel centres), so the result for
// each pixel is a mix of at most two neighbouring texels, whole texels away from an edge. Returns the new t (the same origin).
inline double SharpWeight(double f, double k) { return std::clamp((f - 0.5) * k + 0.5, 0.0, 1.0); }
inline double SharpCoord(double t, double k) {
    const double i = std::floor(t);
    return i + SharpWeight(t - i, k);
}
// hrwfeel36, GXV_HRW_WARP_SNAP: a pair whose largest source move is under limitPx (> 0) is read with point sampling.
inline bool SnapPair(double maxMovePx, double limitPx) { return limitPx > 0.0 && std::isfinite(maxMovePx) && maxMovePx < limitPx; }

}  // namespace port::hrw41
