// q30fin26 (12 Nov 2026): the transition rule of the Quality ladder's step 5 (large soft effect layers at reduced resolution), as pure logic. Host-tested by tests/q30lock_test.cpp.
//
// The compositor (GXV_FXLOWRES_AUTO, glow26 level 1; platform/vgx/src/vgx_backend_gl.cpp, namespace fxl) decides WHICH shader keys' blended draws may be drawn into a half-size target and laid over
// the picture (its own windows and confirmations). The owner's rule for a step is: no pop, one layer at a time, a weight that ramps over several pictures, nothing created at the switch.
//   - The layers the compositor routes are OFFERED here in the order it decides them. At most ONE layer is moving at any time; the next one starts `gapPictures` after the last one finished.
//   - A layer's weight w goes 0 -> 1 in `rampPictures` pictures (12 = 0.4 s at 30), and back the same way when the step is given back (the layer that came last goes first).
//   - The weight is a TIME blend: in a picture the layer is drawn at the reduced size with the frequency w (an error-diffusion counter, so the pictures are spread evenly: w = 1/4 is one
//     picture in four, never a burst). The reduced-size picture of a soft layer differs from the full-size one by a few levels at the layer's edges (host numbers, glow26 report: mean
//     0.004-0.04 levels, max 1-4 with the nearest-depth composite); a mix of the two in time is therefore a ramp from "the original" to "the cheaper look-alike" with no step in brightness.
//   - Nothing is created here: the target, the depth copy and the programs are made at the stage load (the back end's prebuild) before the step is offered to the ladder at all.
#pragma once

#include <cstdint>

namespace port {
namespace q30fx {

constexpr int kMaxKeys = 8;
struct Params {
    uint32_t rampPictures = 12;   // a layer's ramp
    uint32_t gapPictures = 30;    // between one layer finishing and the next starting (1 s at 30)
};
struct Census {
    uint32_t offered = 0, atFull = 0, moving = 0;      // gauges
    uint64_t picturesLow = 0, picturesFull = 0;        // key-pictures drawn at the reduced size / at full size, since the stage began
    uint32_t starts = 0, finishes = 0;
    uint32_t maxMovingAtOnce = 0;
};

class Rollout {
public:
    void Config(const Params& p) { p_ = p; }
    void Reset() { const Params p = p_; *this = Rollout(); p_ = p; }
    // The compositor routed this key: it joins the list at weight 0 (idempotent).
    void Offer(uint32_t key) {
        for (int i = 0; i < n_; ++i) if (k_[i].key == key) return;
        if (n_ >= kMaxKeys) return;
        k_[n_] = Slot();
        k_[n_].key = key;
        ++n_;
    }
    // The step is in force (or not): called every picture.
    void SetOn(bool on) { on_ = on; }
    bool On() const { return on_; }
    // Advance by one picture (call once per picture, before the Use calls of that picture).
    void Tick() {
        ++pic_;
        int moving = 0;
        if (cur_ >= 0 && !Moving(cur_)) {   // the layer that was moving has arrived
            cur_ = -1;
            idleSince_ = pic_;
            ++c_.finishes;
        }
        if (cur_ < 0 && pic_ - idleSince_ >= p_.gapPictures) cur_ = Pick();
        if (cur_ >= 0) {
            Slot& s = k_[cur_];
            const float step = 1.0f / static_cast<float>(p_.rampPictures < 1 ? 1 : p_.rampPictures);
            if (on_) { s.w += step; if (s.w > 1.0f) s.w = 1.0f; }
            else { s.w -= step; if (s.w < 0.0f) s.w = 0.0f; }
            if (s.started == 0) { s.started = 1; ++c_.starts; }
            moving = 1;
        }
        if (static_cast<uint32_t>(moving) > c_.maxMovingAtOnce) c_.maxMovingAtOnce = static_cast<uint32_t>(moving);
        // the error-diffusion decision of each layer for THIS picture, once
        for (int i = 0; i < n_; ++i) {
            Slot& s = k_[i];
            s.acc += s.w;
            if (s.w >= 1.0f) { s.acc = 0.0f; s.useNow = true; }
            else if (s.w <= 0.0f) { s.acc = 0.0f; s.useNow = false; }
            else if (s.acc >= 1.0f) { s.acc -= 1.0f; s.useNow = true; }
            else s.useNow = false;
            if (s.useNow) ++c_.picturesLow; else ++c_.picturesFull;
        }
    }
    // Is this key's layer drawn at the reduced size in this picture?
    bool Use(uint32_t key) const {
        for (int i = 0; i < n_; ++i) if (k_[i].key == key) return k_[i].useNow;
        return false;
    }
    float Weight(uint32_t key) const { for (int i = 0; i < n_; ++i) if (k_[i].key == key) return k_[i].w; return 0.0f; }
    // Everything is off and idle: the compositor may be switched off again.
    bool AllOff() const { for (int i = 0; i < n_; ++i) if (k_[i].w > 0.0f) return false; return true; }
    // Every offered layer is at full weight.
    bool AllOn() const { for (int i = 0; i < n_; ++i) if (k_[i].w < 1.0f) return false; return n_ > 0; }
    int Count() const { return n_; }
    bool AnyMoving() const { return cur_ >= 0; }
    Census Sample() const {
        Census c = c_;
        c.offered = static_cast<uint32_t>(n_);
        for (int i = 0; i < n_; ++i) { if (k_[i].w >= 1.0f) ++c.atFull; else if (k_[i].w > 0.0f) ++c.moving; }
        return c;
    }

private:
    struct Slot { uint32_t key = 0; float w = 0.0f, acc = 0.0f; bool useNow = false; uint8_t started = 0; };
    bool Moving(int i) const { return on_ ? k_[i].w < 1.0f : k_[i].w > 0.0f; }
    // the next layer to move: going on, the first not at full weight (in the order offered); going off, the last that is not at 0
    int Pick() const {
        if (on_) { for (int i = 0; i < n_; ++i) if (k_[i].w < 1.0f) return i; }
        else { for (int i = n_ - 1; i >= 0; --i) if (k_[i].w > 0.0f) return i; }
        return -1;
    }
    Params p_;
    Slot k_[kMaxKeys];
    int n_ = 0, cur_ = -1;
    bool on_ = false;
    uint64_t pic_ = 0, idleSince_ = 0;
    Census c_;
};

}  // namespace q30fx
}  // namespace port
