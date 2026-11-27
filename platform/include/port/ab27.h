// ab27 (13 Nov 2026): GXV_AB, the in-run A/B/A test harness. ONE console run answers many questions: inside a stage visit, with Mario standing still (or a scripted
// route), the driver flips ONE feature at a time off / on / off / on ... in fixed windows, measures every window, and prints per feature the change with a noise
// figure taken from the spread of the windows of the same arm (the run-to-run noise that made single runs useless is then inside the run).
// Pure and header-only: no clock, no globals but the freeze flag, no platform includes (the back end feeds frames and carries out the flips; tests/ab27_test.cpp runs this code).
//
//   GXV_AB_PLAN=<file | inline>    inline: entries separated by ';'. A file (ux0:data/smg/... or a path; recognised by a '/' or ':' or by no '=' in it) has one entry a line.
//       entry   KEY=off|on [+KEY2=off|on ...] [@settle=S] [@window=S]      e.g.  GXV_GLOW26=0|1      GXV_ACROSS=0|2+GXV_PASSHOIST=0|2      GXV_HSR_ORDER=1+GXV_HSR_ORDER_TIES=0|7
//                       a value without '|' is the same in both arms (a fixed companion setting). The entry's name is its text before the first '@'.
//       [Stage]         a section header (the game's stage name, e.g. [HoneyBeeKingdomGalaxy]; [*] = every stage without a section of its own). Entries before any header are [*].
//       # comment
//   GXV_AB_WINDOW_S (6)  GXV_AB_SETTLE_S (1.5, discarded after every flip)  GXV_AB_REPEATS (3 off/on pairs a feature)  GXV_AB_START_S (20 s of play before the first flip)
//   GXV_AB_WARM_S (6: extra settle for a feature whose state has to be rebuilt: knob class 'w', see port/stage_knob.h FlipClass)  GXV_AB_GOV_LEVEL (0: the governors' pinned level)
//   Per feature: off, on, off, on, off, on (REPEATS pairs), then the next feature. Everything else is held: the picture-size controller and the governors are frozen (Frozen()).
//   Output (port_log_summary):  ab: [Stage] KEY off a/b/c on a/b/c: fps +x.x (noise ±y.y), gpu -x.xx ms (±y.yy), game ..., fe ..., be ...: VERDICT acted|no effect|within noise
//   and one machine line a feature for tools/ab-sum.py:  ab-data: stage=S key=K fps=d/n gpu=d/n game=d/n fe=d/n be=d/n ta=d/n f3d=d/n p95=d/n hit=d/n verdict=V offfps=a,b,c onfps=a,b,c
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "stage_knob.h"

namespace port {
namespace ab {

constexpr int kMaxEntries = 20, kMaxAssign = 6, kMaxSections = 10, kMaxRepeats = 6, kMaxIv = 8192;
constexpr double kNaN = -1.0e30;  // "not measured"
inline bool Known(double v) { return v > -1.0e29; }

// ---- the freeze flag: read by the picture-size controller and the governors (one relaxed load) ---------------------------------------------------
inline std::atomic<int> g_frozen{0};
inline bool Frozen() { return g_frozen.load(std::memory_order_relaxed) != 0; }
inline void SetFrozen(bool on) { g_frozen.store(on ? 1 : 0, std::memory_order_relaxed); }

// ---- the plan ------------------------------------------------------------------------------------------------------------------------------
struct Assign {
    char key[44];
    float off = 0, on = 0;
};
struct Entry {
    char name[96];
    Assign a[kMaxAssign];
    int n = 0;
    float settleS = -1, windowS = -1;  // per-entry overrides (-1: the plan's)
    bool warm = false;                 // set by the back end: a knob of class 'w' (retained state) is named
};
struct Section {
    char stage[48];
    Entry e[kMaxEntries];
    int n = 0;
};
struct Params {
    double windowS = 6.0, settleS = 1.5, warmS = 6.0, startS = 20.0;
    int repeats = 3;
};
struct Plan {
    Section sec[kMaxSections];
    int nsec = 0;
};
struct Diag {
    int bad = 0;
    char first[160] = {};
};

inline bool IsSp(char c) { return c == ' ' || c == '\t' || c == '\r'; }
inline void Trim(char* s) {
    size_t n = std::strlen(s), b = 0;
    while (n > 0 && IsSp(s[n - 1])) s[--n] = 0;
    while (s[b] && IsSp(s[b])) ++b;
    if (b) std::memmove(s, s + b, n - b + 1);
}
inline Section* FindOrAdd(Plan& p, const char* stage) {
    for (int i = 0; i < p.nsec; ++i)
        if (!std::strcmp(p.sec[i].stage, stage)) return &p.sec[i];
    if (p.nsec >= kMaxSections) return nullptr;
    Section& s = p.sec[p.nsec++];
    s.n = 0;
    snprintf(s.stage, sizeof s.stage, "%.47s", stage);
    return &s;
}
inline void Bad(Diag& dg, const char* what, const char* text) {
    if (!dg.bad++) snprintf(dg.first, sizeof dg.first, "%s: %.100s", what, text);
}
// One entry's text ("KEY=a|b+KEY2=c [@settle=S] [@window=S]") into `e`. False (with Diag) when it is wrong.
inline bool ParseEntry(const char* text, Entry& e, Diag& dg) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", text);
    e.n = 0;
    e.settleS = e.windowS = -1;
    char* at = std::strchr(buf, '@');
    char opts[128] = {};
    if (at) { snprintf(opts, sizeof opts, "%s", at + 1); *at = 0; }
    Trim(buf);
    snprintf(e.name, sizeof e.name, "%.95s", buf);
    for (char* p = buf; *p;) {
        char* plus = std::strchr(p, '+');
        if (plus) *plus = 0;
        char* eq = std::strchr(p, '=');
        if (!eq || e.n >= kMaxAssign) { Bad(dg, "bad assignment", p); return false; }
        *eq = 0;
        char key[44];
        snprintf(key, sizeof key, "%.43s", p);
        Trim(key);
        char* val = eq + 1;
        Trim(val);
        Assign& a = e.a[e.n];
        snprintf(a.key, sizeof a.key, "%.43s", key);
        char* bar = std::strchr(val, '|');
        char* endp = nullptr;
        if (bar) {
            *bar = 0;
            a.off = static_cast<float>(std::strtod(val, &endp));
            if (endp == val) { Bad(dg, "bad off value", text); return false; }
            a.on = static_cast<float>(std::strtod(bar + 1, &endp));
            if (endp == bar + 1) { Bad(dg, "bad on value", text); return false; }
        } else {
            a.off = a.on = static_cast<float>(std::strtod(val, &endp));
            if (endp == val) { Bad(dg, "bad value", text); return false; }
        }
        if (!key[0]) { Bad(dg, "empty key", text); return false; }
        ++e.n;
        p = plus ? plus + 1 : p + std::strlen(p);
    }
    bool varies = false;
    for (int i = 0; i < e.n; ++i) varies = varies || e.a[i].off != e.a[i].on;
    if (!varies) { Bad(dg, "an entry needs one off|on pair", text); return false; }
    for (char* o = opts; *o;) {
        char* nx = std::strchr(o, '@');
        if (nx) *nx = 0;
        Trim(o);
        if (!std::strncmp(o, "settle=", 7)) e.settleS = static_cast<float>(std::atof(o + 7));
        else if (!std::strncmp(o, "window=", 7)) e.windowS = static_cast<float>(std::atof(o + 7));
        else if (*o) { Bad(dg, "unknown option", o); return false; }
        o = nx ? nx + 1 : o + std::strlen(o);
    }
    return true;
}
// Text (len bytes): lines or ';' separated entries and [Stage] headers. Returns the entries accepted.
inline int ParseText(Plan& p, const char* text, size_t len, Diag& dg) {
    int ok = 0;
    Section* cur = FindOrAdd(p, "*");
    size_t i = 0;
    while (i < len && text[i]) {
        size_t j = i;
        while (j < len && text[j] && text[j] != '\n' && text[j] != ';') ++j;
        char line[300];
        const size_t n = std::min(j - i, sizeof line - 1);
        std::memcpy(line, text + i, n);
        line[n] = 0;
        i = j + 1;
        char* hash = std::strchr(line, '#');
        if (hash) *hash = 0;
        Trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') {
            char* close = std::strchr(line, ']');
            if (!close) { Bad(dg, "bad section", line); continue; }
            *close = 0;
            cur = FindOrAdd(p, line + 1);
            if (!cur) Bad(dg, "too many sections", line);
            continue;
        }
        if (!cur || cur->n >= kMaxEntries) { Bad(dg, "too many entries", line); continue; }
        if (ParseEntry(line, cur->e[cur->n], dg)) { ++cur->n; ++ok; }
    }
    return ok;
}
// The section for a stage: its own, else "*".
inline const Section* Select(const Plan& p, const char* stage) {
    const Section* star = nullptr;
    for (int i = 0; i < p.nsec; ++i) {
        if (stage && !std::strcmp(p.sec[i].stage, stage) && p.sec[i].n > 0) return &p.sec[i];
        if (!std::strcmp(p.sec[i].stage, "*") && p.sec[i].n > 0) star = &p.sec[i];
    }
    return star;
}
inline double EntrySettle(const Entry& e, const Params& q) { return e.settleS >= 0 ? e.settleS : q.settleS + (e.warm ? q.warmS : 0.0); }
inline double EntryWindow(const Entry& e, const Params& q) { return e.windowS > 0 ? e.windowS : q.windowS; }
// Seconds a section takes from its first flip to its last window's end.
inline double TotalSeconds(const Section& s, const Params& q) {
    double t = 0;
    for (int i = 0; i < s.n; ++i) t += 2.0 * q.repeats * (EntrySettle(s.e[i], q) + EntryWindow(s.e[i], q));
    return t;
}

// ---- the measurement of a window -------------------------------------------------------------------------------------------------------------
// One shown picture. Every figure is that picture's own (the back end differences its cumulative counters); a field that is not known is left 0 with its flag false.
struct Frame {
    uint64_t presentUs = 0;
    uint32_t gpuBusyUs = 0, gpuN = 1;  // the GPU busy time of the pictures finished since the last present (gpuN of them)
    bool busyKnown = false;
    uint32_t taUs = 0, f3dUs = 0, scenePics = 0;  // the scene timers' TA and 3D of scenePics pictures (0: not known)
    uint32_t gameUs = 0, gameTurns = 0;           // game thread CPU of gameTurns drawn turns
    bool gameKnown = false;
    uint32_t frontUs = 0, frontPics = 0;
    bool frontKnown = false;
    uint32_t beUs = 0;  // the render thread's CPU for this picture
    bool beKnown = false;
};
struct Win {
    bool valid = false;
    uint32_t pics = 0, hitches = 0;
    double secs = 0, fps = 0, medMs = 0, p95Ms = 0, maxMs = 0;
    double busy = kNaN, ta = kNaN, f3d = kNaN, game = kNaN, fe = kNaN, be = kNaN;  // ms a picture (game: a drawn turn)
};
constexpr double kHitchMs = 50.0;

class Acc {
  public:
    void Clear() { *this = Acc(); }
    void Add(const Frame& f, uint64_t prevPresentUs) {
        const uint64_t iv = prevPresentUs && f.presentUs > prevPresentUs ? f.presentUs - prevPresentUs : 0;
        if (!iv) return;
        sumIv_ += iv;
        if (nIv_ < kMaxIv) iv_[nIv_++] = static_cast<uint32_t>(std::min<uint64_t>(iv, 0xFFFFFFFFull));
        ++pics_;
        if (iv >= static_cast<uint64_t>(kHitchMs * 1000.0)) ++hitches_;
        if (iv > maxIv_) maxIv_ = iv;
        if (f.busyKnown && f.gpuN) { busy_ += f.gpuBusyUs; busyN_ += f.gpuN; }
        if (f.scenePics) { ta_ += f.taUs; f3d_ += f.f3dUs; sceneN_ += f.scenePics; }
        if (f.gameKnown && f.gameTurns) { game_ += f.gameUs; gameN_ += f.gameTurns; }
        if (f.frontKnown && f.frontPics) { front_ += f.frontUs; frontN_ += f.frontPics; }
        if (f.beKnown) { be_ += f.beUs; ++beN_; }
    }
    uint32_t Pictures() const { return pics_; }
    Win Result() const {
        Win w;
        w.pics = pics_;
        w.hitches = hitches_;
        if (pics_ < 5 || sumIv_ == 0) return w;
        w.valid = true;
        w.secs = sumIv_ / 1e6;
        w.fps = pics_ / w.secs;
        uint32_t tmp[kMaxIv];
        std::memcpy(tmp, iv_, nIv_ * sizeof(uint32_t));
        std::sort(tmp, tmp + nIv_);
        w.medMs = tmp[nIv_ / 2] / 1000.0;
        w.p95Ms = tmp[std::min<uint32_t>(nIv_ - 1, static_cast<uint32_t>(nIv_ * 0.95))] / 1000.0;
        w.maxMs = maxIv_ / 1000.0;
        if (busyN_) w.busy = busy_ / 1000.0 / busyN_;
        if (sceneN_) { w.ta = ta_ / 1000.0 / sceneN_; w.f3d = f3d_ / 1000.0 / sceneN_; }
        if (gameN_) w.game = game_ / 1000.0 / gameN_;
        if (frontN_) w.fe = front_ / 1000.0 / frontN_;
        if (beN_) w.be = be_ / 1000.0 / beN_;
        return w;
    }

  private:
    uint32_t iv_[kMaxIv];
    uint32_t nIv_ = 0, pics_ = 0, hitches_ = 0;
    uint64_t sumIv_ = 0, maxIv_ = 0, busy_ = 0, busyN_ = 0, ta_ = 0, f3d_ = 0, sceneN_ = 0, game_ = 0, gameN_ = 0, front_ = 0, frontN_ = 0, be_ = 0, beN_ = 0;
};

// ---- the verdict -------------------------------------------------------------------------------------------------------------------------------
struct Delta {
    double d = kNaN, noise = kNaN;  // on - off (medians of the arms' windows); noise = the larger arm's half range
    int nOff = 0, nOn = 0;
};
enum Metric { kFps, kBusy, kGame, kFe, kBe, kTa, kF3d, kP95, kHit, kMetrics };
inline const char* const kMetricName[kMetrics] = {"fps", "gpu", "game", "fe", "be", "ta", "f3d", "p95", "hit"};
// The size of a change below which a metric did not move (whatever the noise).
inline double Floor(int m) { return m == kFps ? 0.4 : m == kHit ? 0.5 : 0.08; }
// A metric whose rise is good.
inline bool HigherIsBetter(int m) { return m == kFps; }
inline double Value(const Win& w, int m) {
    switch (m) {
    case kFps: return w.valid ? w.fps : kNaN;
    case kBusy: return w.busy;
    case kGame: return w.game;
    case kFe: return w.fe;
    case kBe: return w.be;
    case kTa: return w.ta;
    case kF3d: return w.f3d;
    case kP95: return w.valid ? w.p95Ms : kNaN;
    case kHit: return w.valid ? static_cast<double>(w.hitches) : kNaN;
    }
    return kNaN;
}
inline double MedianOf(double* v, int n) {
    std::sort(v, v + n);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
inline double HalfRange(const double* v, int n) {
    double lo = v[0], hi = v[0];
    for (int i = 1; i < n; ++i) { lo = std::min(lo, v[i]); hi = std::max(hi, v[i]); }
    return 0.5 * (hi - lo);
}
struct KeyResult {
    Entry entry;
    Win off[kMaxRepeats], on[kMaxRepeats];
    int nOff = 0, nOn = 0;  // windows recorded (valid or not) per arm
};
inline Delta DeltaOf(const KeyResult& k, int m) {
    double a[kMaxRepeats], b[kMaxRepeats];
    int na = 0, nb = 0;
    for (int i = 0; i < k.nOff; ++i) { const double v = Value(k.off[i], m); if (Known(v)) a[na++] = v; }
    for (int i = 0; i < k.nOn; ++i) { const double v = Value(k.on[i], m); if (Known(v)) b[nb++] = v; }
    Delta d;
    d.nOff = na;
    d.nOn = nb;
    if (!na || !nb) return d;
    double ca[kMaxRepeats], cb[kMaxRepeats];
    std::memcpy(ca, a, sizeof(double) * na);
    std::memcpy(cb, b, sizeof(double) * nb);
    d.d = MedianOf(cb, nb) - MedianOf(ca, na);
    d.noise = std::max(na > 1 ? HalfRange(a, na) : 0.0, nb > 1 ? HalfRange(b, nb) : 0.0);
    if (na < 2 || nb < 2) d.noise = kNaN;  // one window an arm: no noise figure
    return d;
}
enum Verdict { kActed, kNoEffect, kWithinNoise, kInconclusive };
inline const char* VerdictText(Verdict v) { return v == kActed ? "acted" : v == kNoEffect ? "no effect" : v == kWithinNoise ? "within noise" : "inconclusive (too few windows)"; }
// acted: some metric moved by more than both its floor and its noise; no effect: every metric moved by less than its floor; else within noise. The p95 / hitch / ta / 3d rows are
// reported but do not decide (they follow the others); the deciding set is fps, gpu, game, fe, be.
inline Verdict VerdictOf(const KeyResult& k, int* actedMetric = nullptr) {
    bool any = false, moved = false, noisy = false;
    for (int m = kFps; m <= kBe; ++m) {
        const Delta d = DeltaOf(k, m);
        if (!Known(d.d)) continue;
        any = true;
        const double ad = std::fabs(d.d);
        if (!Known(d.noise)) { noisy = true; continue; }
        if (ad > Floor(m) && ad > d.noise) { if (actedMetric) *actedMetric = m; return kActed; }
        if (ad > Floor(m)) moved = true;
    }
    if (!any) return kInconclusive;
    if (noisy && !moved) return kInconclusive;
    return moved ? kWithinNoise : kNoEffect;
}
inline int FormatDelta(char* out, size_t cap, const char* name, const Delta& d, bool ms) {
    if (!Known(d.d)) return snprintf(out, cap, "%s n/a", name);
    if (!Known(d.noise)) return snprintf(out, cap, ms ? "%s %+.2f ms (noise ?)" : "%s %+.1f (noise ?)", name, d.d);
    return snprintf(out, cap, ms ? "%s %+.2f ms (\xC2\xB1%.2f)" : "%s %+.1f (noise \xC2\xB1%.1f)", name, d.d, d.noise);
}
// "a/b/c" of the metric in the windows of an arm
inline int FormatArm(char* out, size_t cap, const Win* w, int n, int m) {
    int o = 0;
    for (int i = 0; i < n && o < static_cast<int>(cap) - 12; ++i) {
        const double v = Value(w[i], m);
        o += Known(v) ? snprintf(out + o, cap - o, "%s%.1f", i ? "/" : "", v) : snprintf(out + o, cap - o, "%s-", i ? "/" : "");
    }
    if (cap) out[o < static_cast<int>(cap) ? o : cap - 1] = 0;
    return o;
}
inline int FormatKeyLine(char* out, size_t cap, const char* stage, const KeyResult& k) {
    char a[96], b[96], f[5][80];
    FormatArm(a, sizeof a, k.off, k.nOff, kFps);
    FormatArm(b, sizeof b, k.on, k.nOn, kFps);
    const int ms[5] = {kFps, kBusy, kGame, kFe, kBe};
    for (int i = 0; i < 5; ++i) FormatDelta(f[i], sizeof f[i], kMetricName[ms[i]], DeltaOf(k, ms[i]), i != 0);
    int am = -1;
    const Verdict v = VerdictOf(k, &am);
    return snprintf(out, cap, "ab: [%s] %s off %s on %s: %s, %s, %s, %s, %s: %s%s%s", stage, k.entry.name, a, b, f[0], f[1], f[2], f[3], f[4], VerdictText(v), am >= 0 ? " by " : "",
                         am >= 0 ? kMetricName[am] : "");
}
inline int FormatDataLine(char* out, size_t cap, const char* stage, const KeyResult& k) {
    int o = snprintf(out, cap, "ab-data: stage=%s key=%s", stage, k.entry.name);
    for (int m = 0; m < kMetrics && o < static_cast<int>(cap) - 60; ++m) {
        const Delta d = DeltaOf(k, m);
        if (!Known(d.d)) o += snprintf(out + o, cap - o, " %s=-", kMetricName[m]);
        else if (!Known(d.noise)) o += snprintf(out + o, cap - o, " %s=%+.3f/?", kMetricName[m], d.d);
        else o += snprintf(out + o, cap - o, " %s=%+.3f/%.3f", kMetricName[m], d.d, d.noise);
    }
    char a[96], b[96];
    FormatArm(a, sizeof a, k.off, k.nOff, kFps);
    FormatArm(b, sizeof b, k.on, k.nOn, kFps);
    for (char* p = a; *p; ++p) if (*p == '/') *p = ',';
    for (char* p = b; *p; ++p) if (*p == '/') *p = ',';
    int am = -1;
    const Verdict v = VerdictOf(k, &am);
    const char* vt = v == kActed ? "acted" : v == kNoEffect ? "no_effect" : v == kWithinNoise ? "within_noise" : "inconclusive";
    o += snprintf(out + o, cap - o, " verdict=%s offfps=%s onfps=%s", vt, a, b);
    return o;
}

// ---- the driver -----------------------------------------------------------------------------------------------------------------------------------
enum ActKind { kNone, kFlip, kDone };
struct Action {
    ActKind kind = kNone;
    int entry = -1, arm = 0;  // kFlip: apply entry `entry` in arm 0 (off) / 1 (on)
};
class Driver {
  public:
    // A visit's start: the section (copied) and the parameters. Nothing happens until Run().
    void Begin(const Section& s, const Params& q, const char* stage) {
        sec_ = s;
        q_ = q;
        snprintf(stage_, sizeof stage_, "%s", stage ? stage : "?");
        nres_ = 0;
        for (int i = 0; i < sec_.n; ++i) { res_[i] = KeyResult(); res_[i].entry = sec_.e[i]; }
        nres_ = sec_.n;
        step_ = -1;
        state_ = kStart;
        prevPresent_ = 0;
        retries_ = 0;
        bad_ = false;
        discarded_ = 0;
        aborted_ = 0;
    }
    bool Running() const { return state_ != kIdle && state_ != kFinished; }
    bool Finished() const { return state_ == kFinished; }
    int Steps() const { return sec_.n * 2 * q_.repeats; }
    int Step() const { return step_; }
    const char* Stage() const { return stage_; }
    int Results() const { return nres_; }
    const KeyResult& Result(int i) const { return res_[i]; }
    int Entry() const { return step_ >= 0 ? step_ / (2 * q_.repeats) : -1; }
    int Arm() const { return step_ >= 0 ? (step_ % 2) : 0; }
    int Rep() const { return step_ >= 0 ? (step_ % (2 * q_.repeats)) / 2 : 0; }
    int Aborted() const { return aborted_; }
    // The windows that had to be thrown away (the stage left play, a hitch of the load): a window whose frames were not play restarts, three times at most.
    int Discarded() const { return discarded_; }
    // One shown picture. playOk: the stage is in play (no loading, no menu, no wipe, no movie). Returns what the back end must do now.
    Action OnFrame(const Frame& f, bool playOk) {
        Action a;
        if (state_ == kIdle || state_ == kFinished) return a;
        const uint64_t prev = prevPresent_;
        prevPresent_ = f.presentUs;
        if (state_ == kStart) {
            a = BeginStep(0, f.presentUs);
            return a;
        }
        if (!playOk) {  // the window or the settle is spoilt: begin the same step's settle again (the flip stays in force; the settle runs from here)
            if (state_ == kWindow || state_ == kSettle) {
                if (!bad_) { ++discarded_; if (++retries_ > 40) { aborted_ = 1; state_ = kFinished; Action d; d.kind = kDone; return d; } }
                bad_ = true;
                acc_.Clear();
                state_ = kSettle;
                settleEnd_ = f.presentUs + static_cast<uint64_t>(EntrySettle(sec_.e[Entry()], q_) * 1e6);
            }
            return a;
        }
        bad_ = false;
        if (state_ == kSettle) {
            if (f.presentUs >= settleEnd_) { state_ = kWindow; winStart_ = f.presentUs; winEnd_ = f.presentUs + static_cast<uint64_t>(EntryWindow(sec_.e[Entry()], q_) * 1e6); acc_.Clear(); }
            return a;
        }
        // kWindow
        acc_.Add(f, prev);
        if (f.presentUs >= winEnd_) {
            Record(acc_.Result());
            acc_.Clear();
            retries_ = 0;
            if (step_ + 1 >= Steps()) { state_ = kFinished; Action d; d.kind = kDone; return d; }
            return BeginStep(step_ + 1, f.presentUs);
        }
        return a;
    }
    // The stage ended or the run is stopping: the results so far stand.
    Action Abort() {
        Action d;
        if (!Running()) return d;
        aborted_ = 1;
        state_ = kFinished;
        d.kind = kDone;
        return d;
    }
    // Seconds the whole section needs from now on (a plan's length for the log line).
    double TotalS() const { return TotalSeconds(sec_, q_); }
    const Section& Sec() const { return sec_; }
    const Params& Prm() const { return q_; }

  private:
    enum State { kIdle, kStart, kSettle, kWindow, kFinished };
    Action BeginStep(int s, uint64_t now) {
        step_ = s;
        state_ = kSettle;
        settleEnd_ = now + static_cast<uint64_t>(EntrySettle(sec_.e[Entry()], q_) * 1e6);
        acc_.Clear();
        Action a;
        a.kind = kFlip;
        a.entry = Entry();
        a.arm = Arm();
        return a;
    }
    void Record(const Win& w) {
        KeyResult& k = res_[Entry()];
        if (Arm() == 0) { if (k.nOff < kMaxRepeats) k.off[k.nOff++] = w; }
        else if (k.nOn < kMaxRepeats) k.on[k.nOn++] = w;
    }
    Section sec_;
    Params q_;
    char stage_[48] = "?";
    KeyResult res_[kMaxEntries];
    int nres_ = 0, step_ = -1, retries_ = 0, aborted_ = 0, discarded_ = 0;
    bool bad_ = false;
    State state_ = kIdle;
    uint64_t prevPresent_ = 0, settleEnd_ = 0, winStart_ = 0, winEnd_ = 0;
    Acc acc_;
};

}  // namespace ab
}  // namespace port
