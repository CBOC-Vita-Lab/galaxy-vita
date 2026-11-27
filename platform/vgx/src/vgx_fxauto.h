// compositor25: the automatic choice of draws for the reduced-size effects compositor (GXV_FXLOWRES_AUTO), as a pure model with no GL and
// no game state, so that the host tests can drive it with synthetic pictures (tests/vgx_fxauto_test.cpp) and the back end only feeds it.
//
// What the model knows. The back end hands it, for every picture it observes, the picture's draws in order as Items:
//   IK_ROUTABLE  a blended effect draw whose render state could be drawn into the reduced target (additive or src-alpha, depth tested, no
//                depth write, colour written; alpha rules and hud / offscreen pass already applied by the back end), with its shader's
//                stable key (fh, low 24 bits), its fill in screens (triangle area summed over the draw, overlap counted: stream draws from
//                their vertices, static draws from the kept copy of the static buffer), the shader class cost (ms a screen) and flags
//   IK_BREAK     any other draw (opaque, alpha tested, HUD, a blended draw that cannot go): it ends a run
//   IK_BOUNDARY  a scene boundary (a copy, an offscreen pass, the display copy): it ends a run and the stretch
// A RUN is a maximal sequence of routed IK_ROUTABLE items with nothing else between them (uploads and frees are not items).
//
// Decision rules (per stage; Reset() at a stage load):
//   - a key is routed when, over a window of `winObs` observed pictures, it was present in at least `minPresence` of them, its mean fill
//     (when present) is at least `thresholdScreens`, at least 80 % of that fill is far (not IF_NEAR) and not thin (IF_THIN), and the
//     BUDGET holds: the mean net of the runs it took part in (saving - cost, see RunSaveMs / RunCostMs) is at least `minNetMs`. The
//     runs it is judged in are formed with the keys that are routed or that qualify on the running mean (the provisional set), so
//     two keys that need each other to pay for a run are judged together.
//   - it must qualify in `confirm` consecutive windows (hysteresis against one noisy window).
//   - a routed key stays routed for the stage (a one-way transition, so it cannot flip-flop); a key that is not routed is judged again each
//     window and can still become routed once.
//   - the divisor (half or quarter size) is chosen with the first routing decision from the fill of the best tentative run (>= quarterScreens
//     -> quarter) and never changes in the stage.
// Round 2 (GXV_FXLOWRES_DEFER): the routed additive draws of a stretch can be DEFERRED to one run at the point where the picture's depth is final
// (Item::seg is the stretch, IF_DEFER says the back end proved that the draw commutes with everything between it and that point): FormRuns(.., defer)
// makes one run per stretch of them, so the run's cost is paid once. A key may then join a run that another key pays for when its own saving
// is positive (Cfg::joinMs). The size is half unless >= 90 % of the routed fill is soft (IF_SOFT: low-frequency texture or none) and the fill is
// at least quarterScreens (4).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vgx::fxauto {

constexpr float kScreenPx = 960.0f * 544.0f; // the cost model's unit: a screen at the console's full size

enum ItemKind : uint8_t { IK_ROUTABLE = 1, IK_BREAK = 2, IK_BOUNDARY = 3 };
enum ItemFlag : uint8_t {
    IF_DEP = 1,    // the shader reads a texture through an indirect (dependent) read
    IF_THIN = 2,   // projected width under the thin limit: stays at full size
    IF_NEAR = 4,   // nearer than the near limit (not flagged far): protected
    IF_DEPTHW = 8, // IK_BREAK: writes depth (the shared depth copy is no longer valid after it)
    IF_ADD = 16,   // IK_BREAK / unrouted IK_ROUTABLE: additive blend without depth write (it commutes with a run's light)
    IF_RECT = 32,  // x0..y1 hold the draw's NDC rectangle
    IF_DEFER = 64, // round 2: the back end proved the draw can be drawn at the stretch's depth-final point (additive, commutes with everything up to it)
    IF_SOFT = 128, // round 2: its texture (or none) is low-frequency and the shader has no dependent read: fit for quarter size
};
struct Item {
    uint32_t key = 0;   // fragment shader key (fh low 24 bits); 0 = unknown
    float fill = 0.0f;  // screens, overlap counted; < 0 not known
    float ms = 1.5f;    // ms a screen at full size of this draw's shader class
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t cmd = 0;
    uint16_t seg = 0;   // round 2: the stretch (between two barriers: an EFB capture, a pass, the display copy) the draw lies in
    uint8_t kind = IK_BREAK, flags = 0;
};

struct Cfg {
    float thresholdScreens = 0.75f;
    uint32_t winObs = 16;      // observed pictures a window
    uint32_t confirm = 2;      // consecutive qualifying windows
    float minPresence = 0.25f;
    float minNetMs = 0.3f;
    float quarterScreens = 4.0f;  // round 2 (was 8): quarter size needs this much fill AND softFrac of it soft
    float softFrac = 0.9f;
    float joinMs = 0.25f;         // round 2: a key joins a run that is paid for when its own saving is at least this (ms a picture)
    bool defer = false;           // round 2: GXV_FXLOWRES_DEFER
    float sceneMs = 0.6f;      // one extra GXM scene (3D floor; the vertex time of a scene is another ~0.5 ms the model does not add)
    float depthNsPx = 2.0f;    // the depth copy: a ns per target pixel
    float compNsPx = 1.3f;     // the composite, level 0 (bilinear): ns per covered full-size pixel
    float compNsPx1 = 3.2f;    // level 1 (nearest depth): 5 depth reads + 1..4 colour reads (an estimate; a console run replaces it)
    int level = 0;             // composite level (0 plain bilinear, 1 nearest depth)
    int div = 0;               // 0 chosen by fill, 2 or 4 forced
    int force = 0;             // compfix26: GXV_FXLOWRES_FORCE: 0 off; 1 / 2 route every key with unblocked fill >= forceMilli at once, half / quarter size, no windows, no budget
    float forceScreens = 0.05f;
};
constexpr int kMaxKeys = 48;

// ms a screen of a shader class at 960x544, from Freezeflame's drop runs (agents/compositor25/REPORT.txt, section 1): f9d97a8b 1 texture direct
// 5.8 ms / 3.86 screens = 1.50; 97443366 2 textures, dependent read 2.9 / 0.93 = 3.1; 6a2763ac 2.4 / 0.93 = 2.6
inline float ClassMsPerScreen(bool dependent, unsigned textures) {
    return dependent ? 3.0f : (textures >= 2 ? 1.8f : 1.5f);
}
inline bool IsThin(float meanAltitudePx, int div, float texels) { return texels > 0.0f && meanAltitudePx < texels * static_cast<float>(div); }
inline int ChooseDiv(float runFillScreens, float softFillScreens, const Cfg& c) {
    if (c.div == 2 || c.div == 4) return c.div;
    return (runFillScreens >= c.quarterScreens && softFillScreens >= c.softFrac * runFillScreens) ? 4 : 2;
}
inline float RectScreens(const float r[4]) {
    return std::max(0.0f, r[2] - r[0]) * std::max(0.0f, r[3] - r[1]) * 0.25f; // (NDC is 2 x 2)
}
inline float DepthPassMs(float rectScreens, int div, const Cfg& c) { return rectScreens * kScreenPx / static_cast<float>(div * div) * c.depthNsPx * 1.0e-6f; }
inline float CompositeMs(float rectScreens, const Cfg& c) { return rectScreens * kScreenPx * (c.level >= 1 ? c.compNsPx1 : c.compNsPx) * 1.0e-6f; }

struct Run {
    int first = 0, last = 0;       // item indices (inclusive)
    uint32_t draws = 0;
    float fill = 0.0f;             // screens
    float fillMs = 0.0f;           // ms the run's draws cost at full size
    float softFill = 0.0f;         // the part of `fill` that is soft (IF_SOFT)
    bool deferred = false;         // round 2: the run's draws are drawn together at the stretch's depth-final point
    float rect[4] = {1, 1, -1, -1};
    bool rectKnown = true;
    bool head = false;             // nothing was drawn in the EFB scene before it: it adds one scene, not two
    bool shareDepth = false;       // the previous run's depth copy is still valid: no depth pass
    uint32_t keys[4] = {};
    int nKeys = 0;
    uint32_t stretch = 0;          // the stretch (between two scene boundaries) it lies in
};
inline int ScenesAdded(const Run& r) { return r.head ? 1 : 2; }
inline float RunSaveMs(const Run& r, int div) { return r.fillMs * (1.0f - 1.0f / static_cast<float>(div * div)); }
inline float RunCostMs(const Run& r, int div, const Cfg& c) {
    const float rect = r.rectKnown ? std::clamp(RectScreens(r.rect), 0.0f, 1.0f) : 1.0f;
    return static_cast<float>(ScenesAdded(r)) * c.sceneMs + (r.shareDepth ? 0.0f : DepthPassMs(rect, div, c)) + CompositeMs(rect, c);
}

// The runs of one picture: `routed(item)` says which routable items go. Items between two routed draws that are not routed end the run.
// With `defer`, routed items flagged IF_DEFER join ONE run per stretch (Run::deferred) wherever they lie; the others form runs in place as before.
template <class Pred>
int FormRuns(const std::vector<Item>& it, Pred routed, std::vector<Run>& out, bool defer = false) {
    out.clear();
    bool open = false, drawnBefore = false, depthValid = false;
    uint32_t stretch = 0;
    int deferIdx[64];
    for (int& d : deferIdx) d = -1;
    auto add = [](Run& r, const Item& x, int i) {
        r.last = i;
        ++r.draws;
        if (x.fill > 0.0f) { r.fill += x.fill; r.fillMs += x.fill * x.ms; if (x.flags & IF_SOFT) r.softFill += x.fill; }
        if (x.flags & IF_RECT) {
            r.rect[0] = std::min(r.rect[0], x.x0); r.rect[1] = std::min(r.rect[1], x.y0);
            r.rect[2] = std::max(r.rect[2], x.x1); r.rect[3] = std::max(r.rect[3], x.y1);
        } else {
            r.rectKnown = false;
        }
        bool seen = false;
        for (int k = 0; k < r.nKeys; ++k) seen |= r.keys[k] == x.key;
        if (!seen && r.nKeys < 4) r.keys[r.nKeys++] = x.key;
    };
    for (size_t i = 0; i < it.size(); ++i) {
        const Item& x = it[i];
        if (x.kind == IK_ROUTABLE && routed(x)) {
            if (defer && (x.flags & IF_DEFER)) {
                int& di = deferIdx[x.seg & 63];
                if (di < 0) {
                    Run r;
                    r.first = static_cast<int>(i);
                    r.head = false;      // (it lies at the end of the stretch, after the depth writers: never the head of a scene)
                    r.shareDepth = false;
                    r.deferred = true;
                    r.stretch = x.seg;
                    di = static_cast<int>(out.size());
                    out.push_back(r);
                }
                add(out[static_cast<size_t>(di)], x, static_cast<int>(i));
                continue; // (it does not end an in-place run: it is not drawn there)
            }
            if (!open) {
                Run r;
                r.first = static_cast<int>(i);
                r.head = !drawnBefore;
                r.shareDepth = depthValid;
                r.stretch = stretch;
                out.push_back(r);
                open = true;
            }
            add(out.back(), x, static_cast<int>(i));
            drawnBefore = true;
            depthValid = true; // (this run's snapshot is valid until a depth write or a boundary)
            continue;
        }
        open = false;
        if (x.kind == IK_BOUNDARY) { drawnBefore = false; depthValid = false; ++stretch; continue; }
        drawnBefore = true;
        if (x.kind == IK_BREAK && (x.flags & IF_DEPTHW)) depthValid = false;
    }
    return static_cast<int>(out.size());
}

// How many runs a picture would have if the draws between runs could be moved out of the way exactly (census; not executed): two runs in
// one stretch merge when every draw between them either is additive without depth write (it commutes with a run's light: the sum does not
// depend on the order, apart from the clamp at 1), or has a known rectangle disjoint from the rectangle of both runs and from each other
// run's draws. Anything else (opaque draws, HUD, unknown rectangles, draws that write depth inside the rectangle) keeps the runs apart.
template <class Pred>
int RunsIfHoisted(const std::vector<Item>& it, Pred routed, std::vector<Run>& tmp) {
    FormRuns(it, routed, tmp);
    if (tmp.size() < 2) return static_cast<int>(tmp.size());
    int merged = 1;
    for (size_t r = 1; r < tmp.size(); ++r) {
        const Run& a = tmp[r - 1];
        const Run& b = tmp[r];
        bool ok = a.stretch == b.stretch && a.rectKnown && b.rectKnown;
        if (ok) {
            float u[4] = {std::min(a.rect[0], b.rect[0]), std::min(a.rect[1], b.rect[1]), std::max(a.rect[2], b.rect[2]), std::max(a.rect[3], b.rect[3])};
            for (int i = a.last + 1; i < b.first && ok; ++i) {
                const Item& x = it[static_cast<size_t>(i)];
                if (x.kind == IK_BOUNDARY) { ok = false; break; }
                if (x.flags & IF_ADD) { if (!(x.flags & IF_DEPTHW)) continue; }
                if (!(x.flags & IF_RECT)) { ok = false; break; }
                const bool disjoint = x.x1 < u[0] || x.x0 > u[2] || x.y1 < u[1] || x.y0 > u[3];
                if (!disjoint) ok = false;
            }
        }
        if (!ok) ++merged;
    }
    return merged;
}

struct Decision { uint32_t key; float fill; float netMs; int div; bool far_; };

class Model {
public:
    Cfg cfg;
    Model() { Reset(); }
    void Reset() {
        for (uint32_t& w : why_) w = 0;
        n_ = 0; div_ = 0; pics_ = 0; windows_ = 0; nDec_ = 0; winPics_ = 0; bestFillSum_ = bestSoftSum_ = 0.0f; bestN_ = 0;
        for (Slot& s : slot_) s = Slot{};
    }
    bool Routed(uint32_t key) const {
        for (int i = 0; i < n_; ++i) if (slot_[i].key == key) return slot_[i].routed;
        return false;
    }
    int Div() const { return div_ ? div_ : (cfg.div == 4 ? 4 : 2); }
    int RoutedKeys() const { int c = 0; for (int i = 0; i < n_; ++i) c += slot_[i].routed ? 1 : 0; return c; }
    uint32_t Pictures() const { return pics_; }
    uint32_t Windows() const { return windows_; }
    int KeysSeen() const { return n_; }
    // Round 3: why the keys not routed were not, in the last completed window: [0] absent in it, [1] present in too few pictures, [2] mean fill under the threshold,
    // [3] too much of the fill near or thin, [4] the budget net under the minimum, [5] qualifying, confirming.
    static constexpr int kWhy = 6;
    void WindowWhy(uint32_t out[kWhy]) const { for (int i = 0; i < kWhy; ++i) out[i] = why_[i]; }
    // Decisions made since the last call (for the log): returns the count and copies at most `cap`.
    int TakeDecisions(Decision* out, int cap) {
        const int n = std::min(nDec_, cap);
        for (int i = 0; i < n; ++i) out[i] = dec_[i];
        nDec_ = 0;
        return n;
    }
    // One observed picture (the back end observes one in every few).
    void Observe(const std::vector<Item>& it) {
        ++pics_;
        for (int i = 0; i < n_; ++i) { slot_[i].picFill = slot_[i].picBlocked = 0.0f; slot_[i].picDraws = 0; }
        for (const Item& x : it) {
            if (x.kind != IK_ROUTABLE || x.key == 0) continue;
            Slot* s = Find(x.key);
            if (!s) continue;
            const float f = x.fill > 0.0f ? x.fill : 0.0f;
            if (x.flags & (IF_NEAR | IF_THIN)) s->picBlocked += f; else s->picFill += f;
            s->cost = x.ms;
            ++s->picDraws;
        }
        // (the provisional set: routed, or qualifying on the running mean) -> tentative runs -> each key's share of the net
        std::vector<Run>& runs = runs_;
        auto prov = [this](const Item& x) { return !(x.flags & (IF_NEAR | IF_THIN)) && Provisional(x.key); };
        FormRuns(it, prov, runs, cfg.defer);
        const int div = ProvDiv(runs);
        {   // (the window's best tentative run: the size is chosen from it with the first decision)
            float best = 0.0f, soft = 0.0f;
            for (const Run& r : runs) if (r.fill > best) { best = r.fill; soft = r.softFill; }
            bestFillSum_ += best; bestSoftSum_ += soft; ++bestN_;
        }
        // (a tentative run whose own net is not positive would not be taken (the back end skips it): it adds nothing to its keys; a taken run's whole net counts for each key in it)
        for (int i = 0; i < n_; ++i) slot_[i].picNet = 0.0f;
        for (const Run& r : runs) {
            const float net = RunSaveMs(r, div) - RunCostMs(r, div, cfg);
            if (net < cfg.minNetMs) continue;
            for (int k = r.first; k <= r.last; ++k) {
                const Item& x = it[static_cast<size_t>(k)];
                if (x.kind != IK_ROUTABLE || !prov(x)) continue;
                if (r.deferred && !(x.flags & IF_DEFER)) continue;
                if (!r.deferred && r.stretch != 0xFFFFFFFFu && (x.flags & IF_DEFER) && cfg.defer) continue;
                Slot* s = Find(x.key);
                if (!s || s->noteStamp == pics_ * 64u + static_cast<uint32_t>(&r - runs.data())) continue;
                s->noteStamp = pics_ * 64u + static_cast<uint32_t>(&r - runs.data());
                s->picNet += net;
            }
        }
        for (int i = 0; i < n_; ++i) if (slot_[i].picDraws) { slot_[i].netSum += slot_[i].picNet; ++slot_[i].netN; }
        for (int i = 0; i < n_; ++i) {
            Slot& s = slot_[i];
            if (s.picDraws == 0) continue;
            const float tot = s.picFill + s.picBlocked;
            s.ema = s.emaN ? s.ema * 0.75f + tot * 0.25f : tot;
            ++s.emaN;
            s.sumFill += s.picFill;
            s.sumBlocked += s.picBlocked;
            ++s.pres;
        }
        if (cfg.force > 0) {   // compfix26: the A/B switch: no windows, no budget, no near/thin quorum (the back end's own near / thin / classification rules still apply)
            for (int i = 0; i < n_; ++i) {
                Slot& s = slot_[i];
                if (s.routed || s.picDraws == 0 || s.picFill < cfg.forceScreens) continue;
                s.routed = true;
                if (!div_) div_ = cfg.force >= 2 ? 4 : 2;
                if (nDec_ < 16) dec_[nDec_++] = Decision{s.key, s.picFill, 0.0f, div_, true};
            }
        }
        if (++winPics_ >= cfg.winObs) EndWindow();
    }

private:
    struct Slot {
        uint32_t key = 0;
        bool routed = false;
        float picFill = 0, picBlocked = 0, picNet = 0, cost = 1.5f;
        uint32_t picDraws = 0;
        float ema = 0; uint32_t emaN = 0;
        float sumFill = 0, sumBlocked = 0, netSum = 0;
        uint32_t pres = 0, netN = 0, streak = 0, noteStamp = 0;
    };
    Slot* Find(uint32_t key) {
        for (int i = 0; i < n_; ++i) if (slot_[i].key == key) return &slot_[i];
        if (n_ >= kMaxKeys) return nullptr;
        slot_[n_] = Slot{};
        slot_[n_].key = key;
        return &slot_[n_++];
    }
    bool Provisional(uint32_t key) const {
        for (int i = 0; i < n_; ++i)
            if (slot_[i].key == key) return slot_[i].routed || slot_[i].ema >= Threshold(slot_[i].cost) * 0.8f;
        return false;
    }
    // The fill a key needs: the threshold, or, once a run is paid for by another key (cfg.defer: a run is paid once), the fill whose own saving is cfg.joinMs.
    float Threshold(float msPerScreen) const {
        if (!cfg.defer || !AnyRouted()) return cfg.thresholdScreens;
        const int d = div_ ? div_ : (cfg.div == 4 ? 4 : 2);
        const float perScreen = msPerScreen * (1.0f - 1.0f / static_cast<float>(d * d));
        return std::min(cfg.thresholdScreens, perScreen > 0.0f ? cfg.joinMs / perScreen : cfg.thresholdScreens);
    }
    bool AnyRouted() const { for (int i = 0; i < n_; ++i) if (slot_[i].routed) return true; return false; }
    int ProvDiv(const std::vector<Run>& runs) const {
        if (div_) return div_;
        float best = 0.0f, soft = 0.0f;
        for (const Run& r : runs) if (r.fill > best) { best = r.fill; soft = r.softFill; }
        return ChooseDiv(best, soft, cfg);
    }
    void EndWindow() {
        ++windows_;
        for (uint32_t& w : why_) w = 0;
        for (int i = 0; i < n_; ++i) {
            Slot& s = slot_[i];
            if (!s.routed) {
                const float presence = static_cast<float>(s.pres) / static_cast<float>(std::max<uint32_t>(cfg.winObs, 1));
                const float mean = s.pres ? s.sumFill / static_cast<float>(s.pres) : 0.0f;
                const float all = s.sumFill + s.sumBlocked;
                const bool clean = all > 0.0f && s.sumFill >= 0.8f * all;
                const float net = s.netN ? s.netSum / static_cast<float>(s.netN) : -1.0e9f;
                const bool ok = presence >= cfg.minPresence && mean >= Threshold(s.cost) && clean && net >= cfg.minNetMs;
                s.streak = ok ? s.streak + 1 : 0;
                {
                    // (compfix26: "mostly near or thin" is tested BEFORE "fill under the threshold": a blocked draw's fill is not in `mean`, so the old order called every near or thin key "fill under the threshold")
                    const int r = s.pres == 0 ? 0 : presence < cfg.minPresence ? 1 : (all > 0.0f && !clean) ? 3 : mean < Threshold(s.cost) ? 2 : net < cfg.minNetMs ? 4 : 5;
                    ++why_[r];
                }
                if (s.streak >= cfg.confirm) {
                    s.routed = true;
                    if (!div_) {
                        const float bf = bestN_ ? bestFillSum_ / static_cast<float>(bestN_) : mean, bs = bestN_ ? bestSoftSum_ / static_cast<float>(bestN_) : 0.0f;
                        div_ = cfg.div == 2 || cfg.div == 4 ? cfg.div : ChooseDiv(std::max(bf, mean), bs, cfg);
                    }
                    if (nDec_ < 16) dec_[nDec_++] = Decision{s.key, mean, net, div_, clean};
                }
            }
            s.sumFill = s.sumBlocked = s.netSum = 0.0f;
            s.pres = s.netN = 0;
        }
        winPics_ = 0;
        bestFillSum_ = bestSoftSum_ = 0.0f;
        bestN_ = 0;
    }
    float bestFillSum_ = 0.0f, bestSoftSum_ = 0.0f;
    uint32_t bestN_ = 0;
    uint32_t why_[kWhy] = {};
    Slot slot_[kMaxKeys];
    int n_ = 0, div_ = 0, nDec_ = 0;
    uint32_t pics_ = 0, windows_ = 0, winPics_ = 0;
    Decision dec_[16];
    std::vector<Run> runs_;
};

} // namespace vgx::fxauto
