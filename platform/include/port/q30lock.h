// q30lock26 (12 Nov 2026): GXV_Q30_LOCK, the lock ladder of the Quality (30) mode. Pure logic: no game types, no GL, no threads of its own (host-tested by tests/q30lock_test.cpp).
// The glue is in platform/src/port/perf.cpp (the turn feed, the cpulod / far-draw actuators, the log), platform/vgx/src/vgx_autoq.cpp (Controller::Add: the picture feed),
// platform/vgx/vita/vgx_host_galaxy_vita.cpp (PaceDisplay: the shown pictures' holds), platform/vgx/src/vgx_backend_gl.cpp (the GPU actuators 4 / 5 / 6, the stage hook, the card file).
//
// WHY. The owner's promise for Quality is an even, never-dropping 30 with the best picture that allows it. The picture mode keeps the exact items and the 'b' (barely noticeable) class
// (q30-26); that is not enough for every stage (the locked-30 tour: Honeyhive 23.6 fps, Beach Bowl 28.2, Gusty Garden 28.8, six near; Ice Volcano and Dreadnought have no GPU room once the
// edge smoothing is back on). This ladder spends MORE quality, one small step at a time, least visible first, ONLY while the measured picture time threatens the 33.3 ms deadline, and gives it
// back when the stage allows it again.
//
// THE BUDGET (per picture at 30: two game turns, one drawn and one undrawn)
//   game thread: the drawn turn + the undrawn turn after it (wall time from the turn's start to its retrace wait) within 33.3 ms. The undrawn turn is NOT a constant 6 ms: the tours' turn-prof
//                lines give 4.4 (TriLeg, Floater) to 10.2 (Honeyhive) ms; it is the game's logic and animation without the draw. At Honeyhive 21.5 + 9.7 = 31.2 ms of 33.3.
//   front end:   its CPU ms a picture within 33.3 ms (it runs beside the game thread; only a CPU-bound stage with a 20 ms front end plus a full pipeline reaches it)
//   GPU:         busy ms a picture within 33.3 ms less the present's margin (default 2.3 ms: 31.0)
//   The decision measure is the 95th percentile of the last N pictures (N = 48), not the mean: a mean of 31 with a spread of 3 ms misses one picture in four.
// THE STEPS (bit s of the mask; step 2, the 'b' set, is the baseline and always on; the static 'b' keys of the stage row are part of it)
//   3a  far models' material lists and far small characters' / marked families' joints refreshed every 3rd turn instead of every 2nd (beyond 3000 units from camera AND player)
//   3b  far specks of pure decoration under 6 lines are not drawn (cpugov level 1)
//   3c  far particles thinned (60 % kept at 7000 units; none thinned under 2500)
//   3d  far star bits one in two (beyond 6000 units; still collectible), each one SHRUNK away over ~0.4 s (port/piecefade.h), never cut in a picture
//   3e  [game thread only] the strongest still-subtle CPU member (q30fin26): far models / small characters / marked families (not star bits and coins) refreshed every 4th turn instead of
//       the 3rd, from 2200 units, up to 24 lines tall (the cpulod level 2 values the owner allowed in Performance; FARLOGIC stays out)
//   4   (q30fin26: replaced) far small light beams fade (GXV_LIGHTVOL's LOD bit, each volume slews 1/8 a picture: a fade, never a jump); with GXV_EDGE_DEPTH=1 set by hand, also the
//       depth-gated edge pass off (q30lock26). Quality does not run the gated pass by default (it costs MORE than edge5), so by default step 4 is the beams only. [GPU, front end]
//   5   large soft effect layers at reduced resolution (the compositor's mild glow26 level 1 set, ONE layer at a time, each ramped in over 12 pictures, its target and programs made at the
//       stage load) [GPU]. Allowed by the back end only when GXV_Q30_FX=1 and the prebuild is done.
//   6   internal 3D size 896x512 with the HUD at the native 960x544 [GPU, the last resort, never lower]. Taken only behind a natural cover (stage load, closed screen, pause menu) or, in
//       the stage being played, not at all: the ladder then WANTS it and the card remembers it so the NEXT visit starts there.
// Each step helps some of the three resources (kHelps); a threat names the resources over their limit and the next step is the first inactive one that helps one of them.
//
// recover30 (13 Nov): RECOVERY. The waits of the rules below (calm 20 s, hold 30 s, gap 10 s, proof 20 s) are the WORST CASE, used at a margin of 0; with room they shrink (GXV_Q30_RECOVER=1): the
// margin is the smallest (limit - p95) of the resources with data, and a wait w runs from wMin (at fullMarginMs of room) to w (at no room). A step is given back when p95 + its LEARNED cost fits
// under the limit less band; the cost is learned both ways (at the take: before - after; at the give-back: after - before, kept per stage on the card, v4 file). Several steps go back together
// when the margin covers the sum of their learned costs. A step given back and needed again within retakeSec doubles its own wait AND its required margin; the doubling halves again after
// halveAfterSec without a re-take. Sustained slip (the game clock below slipPct of real time for slipSec) is a threat like a p95 over the limit: the game never runs slow to hide an overload.
// THE RULES. A threat: the p95 of a resource over its limit, or a picture shown for 3 or 4 vblanks within the last 6 s, in play, after the stage's first 6 s. A step is taken after the threat has
// lasted 1.5 s, never within 10 s of another change; a step taken stays at least 30 s; steps are given back, the most visible (highest) first, after 20 s without any threat and when the resource it
// helps would still fit with its measured gain added; a step given back that the stage needs again within 30 s doubles its own calm requirement (up to 5 min): no limit cycle. A step whose measured
// gain is under 0.25 ms is marked "no gain" and swapped out for the next one. The set in force when a stage was calm for 20 s is remembered per stage (a text file on the card, decaying one
// step per visit that needed nothing more), so a known heavy stage starts where it held.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <cmath>

namespace port {
namespace q30lock {

// q30-28: 3f and 3g are APPENDED (the glue and the game units name steps 4 / 5 / 6 by number: VolumeModelDrawer asks step 5 = bit 5): they are the Quality answer for a stage the game thread
// cannot hold (Honeyhive: pair 41 ms): the Performance budget's far-detail parts at their mildest, ramped in. Their place in the escalation order is kOrder (after 3e, before 4).
enum Step : uint8_t { k3a, k3b, k3c, k3d, k3e, k4, k5, k6, k3f, k3g, k3h, k3i, k3j, k3k, k3l, kStepN };   // (q30fin26: 3e inserted: the memory file is v2; q30-28: 3f, 3g appended, masks are 16 bits, file v3)
using StepMask = uint16_t;
// the order a threat takes steps in (least visible first); a step given back goes in the reverse order
constexpr uint8_t kOrder[kStepN] = {k3a, k3b, k3c, k3d, k3f, k3e, k3g, k3h, k3i, k3j, k3k, k3l, k4, k5, k6};   // (recover30: 3f, far beyond 70 m / 40 m, before 3e, which reaches in to 25 m: the far and small go first, the near come back first)
enum Res : uint8_t { kGame = 1, kFront = 2, kGpu = 4 };
constexpr uint8_t kHelps[kStepN] = {kGame | kFront, kGame | kFront, kGame | kFront | kGpu, kGame | kFront, kGame, kGpu | kFront, kGpu, kGpu, kGame | kFront, kGame | kFront, kGame | kFront, kGame | kFront | kGpu, kGame | kFront | kGpu, kGame | kFront | kGpu, kGame};
constexpr float kDefaultGainMs[kStepN] = {0.6f, 0.5f, 0.6f, 0.5f, 0.8f, 0.8f, 1.5f, 3.0f, 2.5f, 2.0f, 1.2f, 0.8f, 0.6f, 1.5f, 1.0f};   // (E, all guesses; the first measurement replaces them)
// seconds after a step before its gain is measured (step 5 needs the compositor's decision windows first)
constexpr float kGainDelaySec[kStepN] = {5, 5, 5, 5, 5, 5, 14, 5, 6, 6, 6, 6, 6, 6, 6};
inline const char* StepLabel(int s) {
    static const char* const t[kStepN] = {"3a", "3b", "3c", "3d", "3e", "4", "5", "6", "3f", "3g", "3h", "3i", "3j", "3k", "3l"};
    return s >= 0 && s < kStepN ? t[s] : "?";
}
inline const char* StepWhat(int s) {
    static const char* const t[kStepN] = {"far models and small characters refreshed every 3rd turn", "far specks of pure decoration not drawn", "far particles thinned, mild",
                                          "far star bits one in two beyond 60 m, shrunk away", "far models and small characters refreshed every 4th turn from 22 m (game thread)",
                                          "far small light beams fade (and the gated edge pass off when it is on)", "large soft effects at reduced resolution, one layer at a time",
                                          "3D at 896x512, HUD at 960x544",
                                          "far flower / bush groups beyond 7000 units and far shadows beyond 4000 not drawn (the Performance budget's mildest decor and shadow cut), swept in",
                                          "the game's own LOD models switch at 0.8 of their distance and far fur layers thin (the budget's mildest LOD cut), swept in",
                                          "far3 rate: models, characters and marked families beyond 60 m (never nearer than 25 m while anything farther is uncut) posed every 2nd turn, the horizon swept in",
                                          "far LOD deeper: the game's LOD models switch at 0.6 of their distance (far3: the far end first)",
                                          "far density: flower / bush / plant groups beyond 45 m not drawn, swept in",
                                          "far faded draw distance: tiny decoration (under 12 px tall) beyond 90 m shrunk away through the existing fade, the horizon swept in",
                                          "far logic: ambient actors (pure decoration, honey bees) beyond 100 m at half pace; never collectibles, enemies or anything that can reach the player"};
    return s >= 0 && s < kStepN ? t[s] : "?";
}
// How long a step's change is walked in (ms): the fader's / ramp's length. A step never starts while the previous walk runs; the gap (10 s) is far longer.
constexpr uint32_t kTransMs[kStepN] = {3000, 1000, 500, 1000, 3000, 1000, 5000, 300, 4000, 4000, 4000, 4000, 4000, 4000, 4000};   // (3d: the shrink is 0.4 s per bit, bits start one after the other; 6: the picture changes behind a cover)
// far3 (the Quality far-field steps 3h..3l): their mask, and the plan of what each carries (the horizons are in farfield.h; the carriers in perf.cpp). GXV_Q30_FAR=0 clears them from `allowed`.
constexpr StepMask kFarMask = static_cast<StepMask>((1u << k3h) | (1u << k3i) | (1u << k3j) | (1u << k3k) | (1u << k3l));
constexpr int kFarSteps = 5;
inline int FarIndex(int s) { return s >= k3h && s <= k3l ? s - k3h : -1; }
inline const char* ResName(uint8_t r) { return r == kGame ? "game thread" : r == kFront ? "front end" : r == kGpu ? "GPU" : r == 0 ? "missed pictures" : "several"; }

// recover30: the census of one controller (a ladder step, a governor level): how long it was taken, how often given back, how long it took to give back, how often re-taken soon.
struct Meter {
    bool on = false;
    uint64_t since = 0, begin = 0, lastGive = 0;
    double takenUs = 0, giveUs = 0;
    uint32_t takes = 0, gives = 0, retaken = 0;
    void Start(uint64_t now) { *this = Meter(); begin = now; }
    void Take(uint64_t now) { if (on) return; on = true; since = now; ++takes; if (lastGive && now - lastGive < 10000000u) ++retaken; }
    void Give(uint64_t now) { if (!on) return; const double d = static_cast<double>(now - since); takenUs += d; giveUs += d; ++gives; lastGive = now; on = false; }
    double TakenSec(uint64_t now) const { return (takenUs + (on ? static_cast<double>(now - since) : 0.0)) / 1e6; }
    bool Used() const { return takes > 0; }
    // "recover: <name>: taken N s of M s, given back K times, mean time to give back X s, re-taken within 10 s Y times"
    int Format(char* out, size_t cap, const char* name, uint64_t now) const {
        return std::snprintf(out, cap, "recover: %s: taken %.0f s of %.0f s, given back %u times, mean time to give back %.1f s, re-taken within 10 s %u times", name, TakenSec(now),
                             now > begin ? static_cast<double>(now - begin) / 1e6 : 0.0, static_cast<unsigned>(gives), gives ? giveUs / 1e6 / gives : 0.0, static_cast<unsigned>(retaken));
    }
};

struct Config {
    bool memory = true;
    float p95Ms = 31.5f;       // game-thread pair and front-end limit (33.3 less 1.8 ms)
    float gpuMs = 31.0f;       // GPU limit (33.3 less the present's margin)
    float bandMs = 0.5f;       // a resource is calm under its limit less this
    float gapSec = 10.0f;      // between two changes
    float holdSec = 30.0f;     // a step taken stays this long
    float calmSec = 20.0f;     // without a threat before a step is given back
    float calmMaxSec = 300.0f; // the longest a backed-off calm requirement gets
    float settleSec = 6.0f;    // after a stage's start (loads, shader builds)
    float confirmSec = 1.5f;   // a threat must last this long before it is acted on
    // q30-27 (GXV_Q30_LOCK_FAST_OVER_MS / _FAST_GAP_S / _FAST_CONFIRM_S): a stage that is far over (a resource's p95 over its limit by at least fastOverMs)
    // does not wait 1.5 s + 10 s per step: the next step comes after fastConfirmSec / fastGapSec (a walk is at most 5 s and a new step never starts
    // while one runs, so every step is still faded in one at a time). fastOverMs 0 = off (the old timing). Dreadnought needs 3-4 steps: 40 s became ~15 s.
    float fastOverMs = 1.0f;
    float fastGapSec = 4.0f;
    float fastConfirmSec = 0.75f;
    float missSec = 6.0f;      // a 3 or 4 vblank picture is a threat for this long
    float noGainMs = 0.25f;    // a step that bought less than this is marked "no gain"
    float proofSec = 20.0f;    // q30-28: this long in play with no missed picture (and at least 15 pictures a second shown) is PROOF of headroom: a p95 over its limit is then not a threat
    float probeSlackMs = 1.0f; // q30-28: with that proof a step is given back if its resource's p95 + the step's gain is within the limit + this
    // recover30 (GXV_Q30_RECOVER and the keys named below): the waits above are the worst case, at no room; with room they shrink to these
    bool recover = true;       // GXV_Q30_RECOVER (0 = today's flat waits)
    float calmMinSec = 3.0f;   // GXV_Q30_CALM_MIN_S: the calm wait at fullMarginMs of room
    float holdMinSec = 8.0f;   // GXV_Q30_HOLD_MIN_S: a step taken stays this long at fullMarginMs of room (a fade is at most 4 s)
    float gapMinSec = 4.0f;    // GXV_Q30_GAP_MIN_S: between two changes at fullMarginMs of room (never inside a fade: the walk blocks the next change)
    float proofMinSec = 6.0f;  // GXV_Q30_PROOF_MIN_S: the proof of headroom when the p95 is not over its limit; it grows to proofSec at proofOverMs over
    float proofOverMs = 3.0f;  // GXV_Q30_PROOF_OVER_MS
    float fullMarginMs = 4.0f; // GXV_Q30_MARGIN_FULL_MS: the room at which every wait is at its minimum
    bool multi = true;         // GXV_Q30_MULTI: several steps together when the room covers their learned costs
    float retakeSec = 30.0f;   // GXV_Q30_RETAKE_S: a step needed again within this doubles its wait and margin
    float halveAfterSec = 60.0f;   // GXV_Q30_HALVE_S: the doubling halves after this long without a re-take
    float peakSec = 10.0f;     // GXV_Q30_PEAK_S: a give-back is judged against the heaviest load (steps taken out) of the last peakSec seconds: a load that swings with a period up to this is seen at its top
    float peakMinSec = 3.0f;   // GXV_Q30_PEAK_MIN_S: the least data (seconds) the peak needs; before that the p95 of the moment judges
    float slipPct = 98.0f;     // GXV_Q30_SLIP_PCT: game speed (% of real time) under which the game is slipping (0 = slip is no signal)
    float slipSec = 1.0f;      // GXV_Q30_SLIP_S: slipping this long is a threat
    float slipFastPct = 95.0f; // under this the next step comes after fastGapSec
    // speed31 (GXV_Q30_GUARD, default 1; 0 = today's rules exactly): the speed guard's dropped pictures are a FIRST-CLASS threat. A picture the guard leaves out is a fault to be removed by cutting, not a
    // slow motion to live with: the guard says (NoteGuard) when its repayment is sustained (the game thread's natural loss over the leak, for its own 0.3-0.5 s); the ladder acts guardConfirmSec after that,
    // with the fast gap, and when one step cannot cover the need (the learned gains) takes several steps in ONE change (at most guardMaxSteps), each faded in by its own walk. Counted in the speed census.
    bool guard = true;
    float guardConfirmSec = 0.25f;   // GXV_Q30_GUARD_CONFIRM_S
    int guardMaxSteps = 4;           // GXV_Q30_GUARD_STEPS: 1 = one step a change as before
    float logSec = 60.0f;      // the running census line
    uint32_t minSamples = 24;
    // q30-28 (owner 13 Nov 07:34: mild cuts are approved outright): the steps ON FROM THE STAGE'S ENTRY (a load is a cover: no sweep), per stage, "Key=hexmask,Key2=hexmask" (Key = the stage name with
    // or without ":scenario"). The ladder stays above them as the safety net. GXV_Q30_START overrides; "-" = none.
    char startSpec[320] = "HoneyBeeKingdomGalaxy=31F,HeavenlyBeachGalaxy=115";
    // far30: the OPENING set (per stage, same syntax): steps in force from the stage's entry (a load is a cover: no sweep) that the ladder MAY give back by its usual rules, unlike startSpec's. The far-field steps
    // a stage is known to need from its first second (the numbers: REPORT.txt of q30far) are listed here; GXV_Q30_OPEN overrides, "-" = none. Bits outside `allowed` (GXV_Q30_FAR=0) fall away.
    char openSpec[160] = "HoneyBeeKingdomGalaxy=7400";
    StepMask allowed = 0x7FFF;    // steps the build/env permits (bit s); a step without an actuator is cleared by the glue
};

constexpr int kRing = 48;
class Ring {
public:
    void Clear() { n_ = 0; head_ = 0; }
    void Push(float v) { v_[head_] = v; head_ = (head_ + 1) % kRing; if (n_ < kRing) ++n_; }
    int Count() const { return n_; }
    float Mean() const { if (!n_) return 0.0f; double s = 0; for (int i = 0; i < n_; ++i) s += v_[i]; return static_cast<float>(s / n_); }
    float P95() const {
        if (!n_) return 0.0f;
        float t[kRing];
        for (int i = 0; i < n_; ++i) t[i] = v_[i];
        const int k = (n_ * 95 + 99) / 100 - 1;
        std::nth_element(t, t + (k < 0 ? 0 : (k >= n_ ? n_ - 1 : k)), t + n_);
        return t[k < 0 ? 0 : (k >= n_ ? n_ - 1 : k)];
    }
private:
    float v_[kRing] = {};
    int n_ = 0, head_ = 0;
};

// ---- the card file: stage -> the set that held (text, ux0:data/smg/q30lock.txt) ------------------------------------------------------------
constexpr int kMemEntries = 64;
struct Memory {
    struct Entry { char key[48] = ""; StepMask mask = 0; uint16_t visits = 0; uint32_t seq = 0; int16_t gain[kStepN] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; };   // (recover30: learned gains in 0.01 ms, -1 unknown)
    Entry e[kMemEntries];
    int n = 0;
    uint32_t seq = 0;
    bool dirty = false;
    int Find(const char* key) const { for (int i = 0; i < n; ++i) if (!std::strcmp(e[i].key, key)) return i; return -1; }
    bool Lookup(const char* key, StepMask* mask) const { const int i = Find(key); if (i < 0) return false; if (mask) *mask = e[i].mask; return true; }
    void Store(const char* key, StepMask mask, bool visit = true) {
        int i = Find(key);
        if (i < 0) {
            if (n < kMemEntries) i = n++;
            else { i = 0; for (int k = 1; k < n; ++k) if (e[k].seq < e[i].seq) i = k; }   // the least recently stored goes
            std::snprintf(e[i].key, sizeof e[i].key, "%s", key);
            e[i].visits = 0;
        }
        if (e[i].mask != mask) dirty = true;
        e[i].mask = mask;
        if (visit && e[i].visits < 65000) ++e[i].visits;
        e[i].seq = ++seq;
    }
    // recover30: the learned gains of a stage's steps (known ones only), kept across visits
    void StoreGains(const char* key, const float* gain, const bool* known) {
        const int i = Find(key);
        if (i < 0) return;
        for (int s = 0; s < kStepN; ++s) {
            const int16_t v = known[s] ? static_cast<int16_t>(std::min(2000.0f, std::max(0.0f, gain[s] * 100.0f + 0.5f))) : static_cast<int16_t>(-1);
            if (e[i].gain[s] != v) { e[i].gain[s] = v; dirty = true; }
        }
    }
    // "# q30lock v2\n<stage> <hex mask> <visits> [gains...]\n..." ; a line that does not parse is skipped. Returns the entries read. A v1 file (before 3e: bits 4.. meant other steps) is not read.
    int Parse(const char* text, size_t len) {
        n = 0;
        size_t p = 0;
        if (len >= 12 && std::strncmp(text, "# q30lock v1", 12) == 0) { dirty = false; return 0; }
        while (p < len && n < kMemEntries) {
            size_t q = p;
            while (q < len && text[q] != '\n') ++q;
            char line[220];
            const size_t m = q - p < sizeof line - 1 ? q - p : sizeof line - 1;
            std::memcpy(line, text + p, m);
            line[m] = 0;
            p = q + 1;
            if (line[0] == '#' || !line[0]) continue;
            char key[48];
            unsigned mask = 0, visits = 0;
            int used = 0;
            if (std::sscanf(line, "%47s %x %u%n", key, &mask, &visits, &used) < 2 || mask > 0xFFFFu) continue;
            std::snprintf(e[n].key, sizeof e[n].key, "%s", key);
            e[n].mask = static_cast<StepMask>(mask);
            e[n].visits = static_cast<uint16_t>(visits > 65000 ? 65000 : visits);
            e[n].seq = ++seq;
            for (int s = 0; s < kStepN; ++s) e[n].gain[s] = -1;
            if (used > 0) {   // (v4: the learned gains, 0.01 ms, -1 unknown)
                const char* q = line + used;
                for (int s = 0; s < kStepN; ++s) {
                    char* endp = nullptr;
                    const long v = std::strtol(q, &endp, 10);
                    if (endp == q) break;
                    e[n].gain[s] = static_cast<int16_t>(v < -1 ? -1 : (v > 2000 ? 2000 : v));
                    q = endp;
                }
            }
            ++n;
        }
        dirty = false;
        return n;
    }
    int Format(char* out, size_t cap) const {
        size_t used = static_cast<size_t>(std::snprintf(out, cap, "# q30lock v5: stage, steps in force when it held (hex, bit 0 = 3a ... bit 4 = 3e, 5 = 4, 6 = 5, 7 = 6, 8 = 3f, 9 = 3g, 10..14 = 3h..3l), visits, learned gain of each step in 0.01 ms (-1 unknown)\n"));
        for (int i = 0; i < n && used + 190 < cap; ++i) {
            used += static_cast<size_t>(std::snprintf(out + used, cap - used, "%s %04x %u", e[i].key, static_cast<unsigned>(e[i].mask), static_cast<unsigned>(e[i].visits)));
            for (int s = 0; s < kStepN; ++s) used += static_cast<size_t>(std::snprintf(out + used, cap - used, " %d", static_cast<int>(e[i].gain[s])));
            used += static_cast<size_t>(std::snprintf(out + used, cap - used, "\n"));
        }
        return static_cast<int>(used);
    }
};

inline uint64_t NowUs() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline int PopCount(unsigned m) { int n = 0; for (; m; m >>= 1) n += static_cast<int>(m & 1u); return n; }
inline int HighestBit(unsigned m) { int h = -1; for (int i = 0; m; ++i, m >>= 1) if (m & 1u) h = i; return h; }
// the step in the mask that comes LAST in the escalation order (the most visible one: the first to go back, the one the memory's decay drops)
inline int LastStep(unsigned m) { for (int oi = kStepN - 1; oi >= 0; --oi) if (m >> kOrder[oi] & 1u) return kOrder[oi]; return -1; }

// The shared, lock-free outputs (the actuators read these).
struct Out {
    std::atomic<uint32_t> mask{0};      // bit s = step s in force
    std::atomic<uint32_t> rev{0};       // changes by one at every change of the mask
    std::atomic<int> enabled{0};        // the ladder is armed (Quality mode and GXV_Q30_LOCK)
    std::atomic<uint32_t> extra{0};     // anticip32 (GXV_ANTICIPATE): steps asked for ahead of a heavy moment, on top of the ladder's own; 0 = none = the ladder's mask exactly
};
inline Out& O() { static Out o; return o; }
inline bool Active() { return O().enabled.load(std::memory_order_relaxed) != 0; }
inline uint32_t Mask() { return O().mask.load(std::memory_order_relaxed) | O().extra.load(std::memory_order_relaxed); }   // (anticip32: the actuators and their fades see the overlay as steps in force)
inline bool StepOn(int s) { return Active() && (Mask() >> s & 1u) != 0; }
// The level number the cpulod tables are given in Quality while the ladder is armed: bit 8 marks it, bit 0 is step 2 (the baseline), bits 1.. the steps 3a..3d (port/cpulod.h TableValue).
constexpr int kLevelFlag = 0x100;
inline int CpuLodLevel() { return Active() ? (kLevelFlag | 1 | static_cast<int>((Mask() & 0x1Fu) << 1)) : 0; }   // (steps 3a..3e = bits 1..5)

class Ladder {
public:
    struct Sink {   // a one-line log queue the glue drains
        char text[8][400];
        int n = 0;
    };
    void Configure(const Config& c) { std::lock_guard<std::mutex> lk(m_); c_ = c; }
    Config config() const { return c_; }
    StepMask mask() const { return mask_; }
    // Step 6 (the internal size) waits for its other render target to exist and for a natural cover (a load, a wipe, a camera cut): the glue says when. Until then it is not offered.
    void SetStep6Ready(bool r) { std::lock_guard<std::mutex> lk(m_); ready6_ = r; }
    // GXV_Q30_MSAA: while 2x MSAA is active (rung 7), suppress GPU-only q30lock escalation so autoq steps 7 -> 6 first.
    void SetMsaaActive(bool on) { std::lock_guard<std::mutex> lk(m_); msaaActive_ = on; }
    // q30fin26: the back end opens / closes a step at run time (step 4: light beams were seen; step 5: the compositor's target and programs are made). A closed step is never offered;
    // one in force stays until it is given back as usual.
    void SetAllowed(int s, bool on) { std::lock_guard<std::mutex> lk(m_); if (on) c_.allowed = static_cast<StepMask>(c_.allowed | (1u << s)); else c_.allowed = static_cast<StepMask>(c_.allowed & ~(1u << s)); }
    // q30fin26: a natural cover is open (a closed screen, a load, the pause menu, a movie) or not. Step 6 is taken (and given back) only while one is open; a step 6 the ladder wanted
    // before is taken at the first cover. Call at every picture (also the non-play ones).
    void NoteCover(uint64_t now, bool open) {
        std::lock_guard<std::mutex> lk(m_);
        CoverLocked(now, open);
    }
    void CoverLocked(uint64_t now, bool open) {
        cover_ = open;
        if (!active_ || !open) return;
        if (pending6_ && ready6_ && (c_.allowed >> k6 & 1u) && !(mask_ >> k6 & 1u)) { Take6(now); return; }
        if ((mask_ >> k6 & 1u) && calmSince_ != 0 && trans_.on == false) {
            const View v = Look();
            if (v.warm == 0 && v.n >= static_cast<int>(c_.minSamples)) TryRelease(now, v, static_cast<double>(now - calmSince_) / 1e6, true, false);
        }
    }
    bool Step6Pending() const { return pending6_; }
    // anticip32: the next `n` steps a threat would take (least visible first; allowed, not in force, not the back end's 4 / 5 / 6): what GXV_ANTICIPATE asks for ahead of a heavy moment.
    // The ladder's own state does not change: the glue puts them in O().extra for the window and takes them out after; while they are there the ladder gives nothing back (Decide).
    StepMask NextSteps(int n) const {
        std::lock_guard<std::mutex> lk(m_);
        uint32_t set = 0;
        if (!active_ || menu_) return 0;
        for (int oi = 0; oi < kStepN && n > 0; ++oi) {
            const int s = kOrder[oi];
            if ((mask_ >> s & 1u) || !(c_.allowed >> s & 1u) || s == k4 || s == k5 || s == k6) continue;
            set |= 1u << s;
            --n;
        }
        return static_cast<StepMask>(set);
    }
    // far30: a flight (the opening camera, a demo) is on: the ladder takes and gives back nothing until it ends (the camera races past far things: a change then would show as pop-in)
    void SetHold(bool h) { std::lock_guard<std::mutex> lk(m_); hold_ = h; }

    // ---- stage boundaries ----------------------------------------------------------------------------------------------------------------
    // A new stage (or the first): the previous one's census and memory entry are closed, the new one starts from the remembered set (decayed already by StageEnd) or from none.
    void StageBegin(uint64_t now, const char* key, Memory* mem) {
        std::lock_guard<std::mutex> lk(m_);
        EndLocked(now, mem);
        mem_ = mem;
        std::snprintf(key_, sizeof key_, "%s", key ? key : "?");
        stageAt_ = now;
        StepMask start = 0;
        bool remembered = false;
        menu_ = std::strncmp(key_, "FileSelect", 10) == 0;   // (q30-28: the title's file select: 23 draws, the display slot's wait read as 32 ms of GPU; no step, no memory)
        if (c_.memory && mem && !menu_) {
            const int i = mem->Find(key_);
            if (i >= 0) { start = static_cast<StepMask>(mem->e[i].mask & c_.allowed); remembered = true; }
        }
        if (!menu_) { baseStart_ = StartFor(key_) & c_.allowed; start = static_cast<StepMask>(start | baseStart_ | (SpecFor(c_.openSpec, key_) & c_.allowed)); } else baseStart_ = 0;
        ResetStage(now, start);
        if (c_.memory && mem && !menu_) {   // (recover30: the gains learned in earlier visits)
            const int gi = mem->Find(key_);
            if (gi >= 0) for (int s = 0; s < kStepN; ++s) if (mem->e[gi].gain[s] >= 0) { gain_[s] = static_cast<float>(mem->e[gi].gain[s]) * 0.01f; gainKnown_[s] = true; }
        }
        Publish();
        if (start) Logf("q30lock: stage %s: starts with steps %s (remembered from the card: %s held there last time)", key_, MaskText(start), MaskText(start));
        else if (remembered) Logf("q30lock: stage %s: starts at best quality (the card remembers nothing needed)", key_);
        (void)remembered;
    }
    // The run ends (or the mode is left): close the census and the memory entry.
    void Shutdown(uint64_t now, Memory* mem) { std::lock_guard<std::mutex> lk(m_); EndLocked(now, mem); }

    // ---- feeds (any thread) ----------------------------------------------------------------------------------------------------------------
    // q30-28: `drawn` = the turn made a picture. The pair is a DRAWN turn plus the undrawn turn(s) that follow it (the picture's cost to the game thread at 30). It was fed from
    // a flag that was already cleared for the undrawn turn (GameSystem.cpp), so every turn counted as drawn and the pair was one turn: Honeyhive read ~30 ms for a real 41.3.
    void NoteTurn(uint64_t now, uint32_t workUs, bool drawn) {
        std::lock_guard<std::mutex> lk(m_);
        (void)now;
        if (!play_ || !active_ || menu_ || workUs > 100000u) return;
        const float ms = static_cast<float>(workUs) / 1000.0f;
        if (drawn) {
            if (havePair_) { PushPairLocked(pairAcc_); }
            pairAcc_ = ms;
            havePair_ = true;
            drawnR_.Push(ms);
            win_.d += ms; ++win_.nd; st_.d += ms; ++st_.nd;
        } else if (havePair_) {
            pairAcc_ += ms;
            undrawnR_.Push(ms);
            win_.u += ms; ++win_.nu; st_.u += ms; ++st_.nu;
        }
    }
    // recover30: the game's speed. One call per turn start with the wall time since the previous turn start (SpeedGuard's rule: a turn that is not counted, or a gap over 80 ms, is no sample).
    // The speed is the share of real time the game clock advanced over the last ~1.5 s (an exponentially forgotten sum). Sustained under slipPct it is a threat (Decide).
    void NoteSpeed(uint64_t now, uint32_t gapUs, bool counting) {
        std::lock_guard<std::mutex> lk(m_);
        if (!counting || gapUs > 80000u || !active_ || menu_ || !play_) { spdA_ = spdB_ = 0; slipSince_ = 0; speedPct_ = 100.0f; return; }
        const double f = 1.0 - std::min(1.0, static_cast<double>(gapUs) / 1500000.0);
        spdA_ = spdA_ * f + 16683.35;
        spdB_ = spdB_ * f + static_cast<double>(gapUs);
        if (spdB_ < 700000.0) return;   // (under about half a second of data: no verdict)
        speedPct_ = static_cast<float>(100.0 * spdA_ / spdB_);
        if (c_.slipPct > 0 && now >= settleUntil_ && speedPct_ < c_.slipPct) { if (!slipSince_) slipSince_ = now; lastSlipAt_ = now; }
        else slipSince_ = 0;
    }
    float SpeedPct() const { std::lock_guard<std::mutex> lk(m_); return speedPct_; }
    bool Slipping(uint64_t now) const { std::lock_guard<std::mutex> lk(m_); return SlipLocked(now); }
    // speed31: once per turn: the speed guard's pressure (sustained: its repayment of the game thread's natural loss has lasted; needMs = that loss a picture, over the leak). Not sustained / not in play: cleared.
    void NoteGuard(uint64_t now, bool sustained, float needMs) {
        std::lock_guard<std::mutex> lk(m_);
        if (!c_.guard || !sustained || !active_ || menu_ || !play_ || now < settleUntil_) { guardSince_ = 0; return; }
        if (!guardSince_) guardSince_ = now;
        guardNeed_ = needMs;
        lastGuardAt_ = now;
    }
    // cuts taken because of the guard since the last call (the speed census counts them)
    uint32_t TakeGuardCuts() { std::lock_guard<std::mutex> lk(m_); const uint32_t n = guardCuts_; guardCuts_ = 0; return n; }
    bool GuardThreat(uint64_t now) const { std::lock_guard<std::mutex> lk(m_); return GuardLocked(now); }
    // the stage is a menu (FileSelect: a handful of draws; its GPU time is the display wait): nothing is judged and no step is taken
    bool Menu() const { return menu_; }
    // census access for tests: the pair and its parts over the last 48 pairs
    float PairP95() const { std::lock_guard<std::mutex> lk(m_); return pair_.P95(); }
    float PairMean() const { std::lock_guard<std::mutex> lk(m_); return pair_.Mean(); }
    float DrawnMean() const { std::lock_guard<std::mutex> lk(m_); return drawnR_.Mean(); }
    float UndrawnMean() const { std::lock_guard<std::mutex> lk(m_); return undrawnR_.Mean(); }
    bool Proven(uint64_t now) const { std::lock_guard<std::mutex> lk(m_); return ProvenLocked(now); }
    // One picture on the render thread: its GPU busy time and the front end's CPU (each when known); play: a picture of play (not a load, a menu, a fade, a movie). Runs the decision.
    void NotePicture(uint64_t now, uint32_t gpuUs, bool gpuKnown, uint32_t frontUs, bool frontKnown, bool play) {
        std::lock_guard<std::mutex> lk(m_);
        play_ = play;
        if (!active_) return;
        if (!play || menu_) { lastPlayAt_ = now; CoverLocked(now, true); return; }   // (a picture that is not play is a cover; q30-28: a menu stage is judged by nothing)
        cover_ = false;
        if (gpuKnown && gpuUs < 200000u) gpu_.Push(static_cast<float>(gpuUs) / 1000.0f);
        WatchTrans(now, std::max(gpuKnown ? static_cast<float>(gpuUs) / 1000.0f : 0.0f, lastPair_));
        if (frontKnown && frontUs != 0 && frontUs < 200000u) front_.Push(static_cast<float>(frontUs) / 1000.0f);
        if (++tickN_ % 3u != 0u) return;   // (the percentiles every third picture)
        Decide(now);
    }
    // One picture shown (display thread): the vblanks it was held (0 = not known). 2 is the cadence; 3 or 4 a miss; 6 or more a stall (a load, a shader build): neither.
    void NoteShown(uint64_t now, uint32_t hold, bool play) {
        std::lock_guard<std::mutex> lk(m_);
        if (!active_ || !play || !play_ || hold == 0) return;
        if (menu_) return;
        ++pictures_;
        if (now >= settleUntil_) ++sinceMiss_;
        if (hold == 2) ++onCadence_;
        else if (hold >= 3 && hold <= 5) {
            ++misses_;
            if (now >= settleUntil_) {
                lastMissAt_ = now; ++recentMisses_;
                missAt_[missHead_++ % kMissRing] = now;
                sinceMiss_ = 0;
            }
        } else if (hold >= 6) ++stalls_;
        else if (hold == 1) ++short_;
    }
    // recover30: the census lines of the controllers that took anything ("recover: ..."), for the glue's 5-second tick and at the stage's end (pushed by EndLocked)
    int Census(uint64_t now, char (*out)[160], int maxLines) {
        std::lock_guard<std::mutex> lk(m_);
        int n = 0;
        for (int s = 0; s < kStepN && n < maxLines; ++s) {
            if (!meter_[s].Used()) continue;
            char name[24];
            std::snprintf(name, sizeof name, "q30lock step %s", StepLabel(s));
            meter_[s].Format(out[n++], 160, name, now);
        }
        return n;
    }
    bool PopCensus(char* out, size_t cap) {
        std::lock_guard<std::mutex> lk(m_);
        if (!clogN_) return false;
        std::snprintf(out, cap, "%s", clog_[0]);
        for (int i = 1; i < clogN_; ++i) std::memcpy(clog_[i - 1], clog_[i], sizeof clog_[0]);
        --clogN_;
        return true;
    }
    bool PopLog(char* out, size_t cap) {
        std::lock_guard<std::mutex> lk(m_);
        if (!logN_) return false;
        std::snprintf(out, cap, "%s", log_[0]);
        for (int i = 1; i < logN_; ++i) std::memcpy(log_[i - 1], log_[i], sizeof log_[0]);
        --logN_;
        return true;
    }
    // test / census access
    uint32_t changes() const { return changes_; }
    uint32_t pictures() const { return pictures_; }
    uint32_t misses() const { return misses_; }
    uint32_t onCadence() const { return onCadence_; }
    bool Exhausted() const { return exhaustedLogged_; }

private:
    bool StepReady(int) const { return true; }   // (q30fin26: step 6's readiness gates its TAKING at a cover, not its being wanted)
    // step 6 taken now (a cover is open)
    void Take6(uint64_t now) {
        Accrue(now);
        const StepMask before = mask_;
        mask_ = static_cast<StepMask>(mask_ | (1u << k6));
        takenAt_[k6] = now;
        meter_[k6].Take(now);
        pending6_ = false;
        pend_.step = k6; pend_.at = now; pend_.res = kGpu; pend_.before[0] = pair_.P95(); pend_.before[1] = front_.P95(); pend_.before[2] = gpu_.P95();
        lastChangeAt_ = now;
        firstDecision_ = false;
        escalated_ = true;
        ++changes_;
        Logf("q30lock: stage %s: step 6 (%s) taken behind a cover (it was wanted since %.0f s)", key_, StepWhat(k6), static_cast<double>(now - pend6At_) / 1e6);
        BeginTrans(now, before, mask_);
        Reset3();
        Publish();
    }
    // the per-stage always-on set from Config::startSpec
    StepMask StartFor(const char* key) const { return SpecFor(c_.startSpec, key); }
    static StepMask SpecFor(const char* spec, const char* key) {
        const char* p = spec;
        const size_t kl = std::strlen(key);
        while (*p) {
            const char* e = std::strchr(p, '='); if (!e) break;
            const size_t nl = static_cast<size_t>(e - p);
            unsigned m = 0;
            char* endp = nullptr;
            m = static_cast<unsigned>(std::strtoul(e + 1, &endp, 16));
            const bool hit = (nl == kl && !std::strncmp(p, key, nl)) || (nl < kl && !std::strncmp(p, key, nl) && key[nl] == ':');
            if (hit) return static_cast<StepMask>(m & 0x7FFFu);
            p = endp && *endp == ',' ? endp + 1 : (endp ? endp : e + 1);
            if (!endp || *endp == 0) break;
        }
        return 0;
    }
    StepMask baseStart_ = 0;
    uint32_t extraSeen_ = 0;   // (anticip32: the overlay as Decide last saw it)
    bool hold_ = false;
    void ResetStage(uint64_t now, StepMask start) {
        active_ = true;
        mask_ = start;
        startMask_ = start;
        heldMask_ = start;
        heldConfirmed_ = false;
        escalated_ = false;
        pair_.Clear(); gpu_.Clear(); front_.Clear(); drawnR_.Clear(); undrawnR_.Clear();
        win_ = Acc{}; st_ = Acc{}; sinceMiss_ = 0; missHead_ = 0; for (uint64_t& t : missAt_) t = 0;
        havePair_ = false;
        pairAcc_ = 0;
        settleUntil_ = now + static_cast<uint64_t>(c_.settleSec * 1e6);
        lastChangeAt_ = now;   // (the gap counts from the stage's start, except for a first decision after the settle: see Decide)
        firstDecision_ = true;
        threatSince_ = 0;
        calmSince_ = 0;
        lastMissAt_ = 0;
        recentMisses_ = 0;
        pictures_ = onCadence_ = misses_ = stalls_ = short_ = 0;
        changes_ = 0;
        for (int s = 0; s < kStepN; ++s) { takenAt_[s] = start >> s & 1u ? now : 0; gain_[s] = kDefaultGainMs[s]; gainKnown_[s] = false; noGain_[s] = false; usedUs_[s] = 0; calmNeed_[s] = c_.calmSec; restoredAt_[s] = 0; }
        lastStepTick_ = now;
        for (int s = 0; s < kStepN; ++s) { meter_[s].Start(now); if (start >> s & 1u) meter_[s].Take(now); }
        for (int i = 0; i < kPeakBins; ++i) peakBin_[i] = -1;
        peakFirst_ = 0; relSetLast_ = 0; relAtLast_ = 0; multiFailAt_ = 0; multiCap_ = kStepN;
        margin_ = 0; speedPct_ = 100.0f; spdA_ = spdB_ = 0; slipSince_ = 0; lastSlipAt_ = 0; relPend_ = RelPend{}; takePend_ = RelPend{}; guardSince_ = lastGuardAt_ = 0; guardNeed_ = 0;
        bestUs_ = 0;
        totalUs_ = 0;
        lastTick_ = now;
        pend_.step = -1;
        trans_.on = false;
        exhaustedLogged_ = false;
        pending6_ = false;
        want6_ = false;
        exhaustCount_ = 0;
        exhaustWorst_ = 0;
        exhaustRes_ = 0;
        nextCensusAt_ = now + static_cast<uint64_t>(c_.logSec * 1e6);
        stalledBefore_ = false;
    }
    void EndLocked(uint64_t now, Memory* mem) {
        if (!active_) return;
        Accrue(now);
        // census
        const double secs = static_cast<double>(totalUs_) / 1e6;
        char used[200] = "";
        for (int s = 0; s < kStepN; ++s)
            if (usedUs_[s] > 0) {
                const size_t u = std::strlen(used);
                std::snprintf(used + u, sizeof used - u, "%s%s:%.0fs", u ? " " : "", StepLabel(s), static_cast<double>(usedUs_[s]) / 1e6);
            }
        Logf("q30lock: stage %s: pictures %u, on 2-vblank cadence %.1f %%, misses %u, stalls %u, steps used {%s}, time at best quality %.1f %% of %.0f s, changes %u", key_, pictures_,
             pictures_ ? 100.0 * onCadence_ / pictures_ : 0.0, misses_, stalls_, used[0] ? used : "none", secs > 0 ? 100.0 * static_cast<double>(bestUs_) / static_cast<double>(totalUs_) : 100.0, secs, changes_);
        if (st_.np) Logf("q30lock: stage %s: %s (stage totals)", key_, PairText(st_));
        for (int s = 0; s < kStepN && clogN_ < 12; ++s) if (meter_[s].Used()) {   // (recover30)
            char name[24];
            std::snprintf(name, sizeof name, "q30lock step %s", StepLabel(s));
            meter_[s].Format(clog_[clogN_++], 160, name, now);
        }
        if (exhaustCount_ || want6_)
            Logf("q30lock: stage %s: exhausted %u time%s (worst: still over by %.1f ms on the %s)%s", key_, exhaustCount_, exhaustCount_ == 1 ? "" : "s", static_cast<double>(exhaustWorst_), ResName(exhaustRes_),
                 want6_ ? "; step 6 (896x512) was wanted and not taken (no cover): the card starts the next visit at it" : "");
        // memory: the lowest set that held; a stage that held with no change at its start set is tried one step lighter next time (the decay)
        if (c_.memory && mem && secs >= 26.0 && !menu_) {
            StepMask store;
            if (heldConfirmed_) {
                store = heldMask_;
                if (!escalated_ && heldMask_ == startMask_ && heldMask_ != 0) store = static_cast<StepMask>(heldMask_ & ~(1u << LastStep(heldMask_)));
            } else {
                store = mask_;   // it never calmed down: what was tried last is the best known
            }
            if (want6_) store = static_cast<StepMask>(store | (1u << k6));   // (the stage wanted the 896 picture: the next visit starts there, at the load, which is a cover)
            mem->Store(key_, store);
            mem->StoreGains(key_, gain_, gainKnown_);
        }
        active_ = false;
    }
    void Accrue(uint64_t now) {
        if (now > lastTick_ && now - lastTick_ < 2000000u && play_) {
            const uint64_t d = now - lastTick_;
            totalUs_ += d;
            if (mask_ == 0) bestUs_ += d;
            for (int s = 0; s < kStepN; ++s) if (mask_ >> s & 1u) usedUs_[s] += d;
        }
        lastTick_ = now;
    }
    void PushPairLocked(float v) { pair_.Push(v); lastPair_ = v; win_.p += v; ++win_.np; st_.p += v; ++st_.np; }
    // misses (3-5 vblank holds in play after the settle) in the last missSec
    int MissesIn(uint64_t now) const {
        int n = 0;
        for (int i = 0; i < kMissRing; ++i) if (missAt_[i] && now - missAt_[i] < static_cast<uint64_t>(c_.missSec * 1e6)) ++n;
        return n;
    }
    // PROOF of headroom: proofSec of play since the settle with no missed picture, at least 15 pictures a second shown in that time: whatever a p95 says, the stage holds the cadence
    struct View { float game, gpu, front, gameMean, gpuMean, frontMean; int n; uint8_t hot, warm; };
    // recover30: the proof's length follows how far the p95 is over its limit (the estimate is wrong by about that much): at proofOverMs over or more the full proofSec, at no excess proofMinSec
    float ProofSec() const {
        if (!c_.recover) return c_.proofSec;
        const float over = margin_ < 0 ? -margin_ : 0.0f;
        const float f = c_.proofOverMs > 0 ? std::min(1.0f, over / c_.proofOverMs) : 1.0f;
        return std::min(c_.proofSec, c_.proofMinSec + (c_.proofSec - c_.proofMinSec) * f);
    }
    bool GuardLocked(uint64_t now) const { return c_.guard && guardSince_ != 0 && now - lastGuardAt_ < 400000u && now - guardSince_ >= static_cast<uint64_t>(c_.guardConfirmSec * 1e6); }
    bool SlipLocked(uint64_t now) const { return c_.slipPct > 0 && slipSince_ != 0 && now - slipSince_ >= static_cast<uint64_t>(c_.slipSec * 1e6); }
    bool ProvenLocked(uint64_t now) const {
        const float ps = ProofSec();
        if (!active_ || menu_ || now < settleUntil_ + static_cast<uint64_t>(ps * 1e6)) return false;
        if (lastMissAt_ != 0 && now - lastMissAt_ < static_cast<uint64_t>(ps * 1e6)) return false;
        if (lastSlipAt_ != 0 && now - lastSlipAt_ < static_cast<uint64_t>(ps * 1e6)) return false;   // (recover30: a game that slipped did not hold its speed)
        if (lastGuardAt_ != 0 && now - lastGuardAt_ < static_cast<uint64_t>(ps * 1e6)) return false;  // (speed31: nor one whose pictures the guard had to drop)
        return sinceMiss_ >= static_cast<uint32_t>(15.0f * ps);
    }
    // the learned gains of the steps of `m` that help resource `r`
    float GainsOf(uint32_t m, uint8_t r) const { float g = 0; for (int s = 0; s < kStepN; ++s) if ((m >> s & 1u) && (kHelps[s] & r)) g += gain_[s]; return g; }
    static int ResIndex(uint8_t r) { return r == kGame ? 0 : r == kFront ? 1 : 2; }
    void PeakNote(uint64_t now, const View& v) {
        if (trans_.on || v.n < static_cast<int>(c_.minSamples)) return;
        const int64_t bin = static_cast<int64_t>(now / 500000u);
        float* b = peakV_[bin % kPeakBins];
        if (peakBin_[bin % kPeakBins] != bin) { peakBin_[bin % kPeakBins] = bin; b[0] = b[1] = b[2] = 0; }
        if (!peakFirst_) peakFirst_ = now;
        for (uint8_t r : {kGame, kFront, kGpu}) b[ResIndex(r)] = std::max(b[ResIndex(r)], P95Of(r, v) + GainsOf(mask_, r));
    }
    // the heaviest load without steps over the last peakSec: false until peakMinSec of data exist
    bool PeakL0(uint64_t now, float out[3]) const {
        out[0] = out[1] = out[2] = 0;
        if (!c_.recover || !peakFirst_ || now - peakFirst_ < static_cast<uint64_t>(c_.peakMinSec * 1e6)) return false;
        const int64_t nb = static_cast<int64_t>(now / 500000u), span = static_cast<int64_t>(c_.peakSec * 2.0f);
        for (int64_t k = nb - span; k <= nb; ++k) {
            if (k < 0 || peakBin_[k % kPeakBins] != k) continue;
            for (int i = 0; i < 3; ++i) out[i] = std::max(out[i], peakV_[k % kPeakBins][i]);
        }
        return true;
    }
    // recover30: the room: the smallest (limit - p95) of the resources that have data (negative = over); with the peak: against the heaviest recent load with the steps now in force
    float MarginOf(const View& v, uint64_t now) const {
        float pk[3];
        const bool havePk = PeakL0(now, pk);
        float m = 1e9f;
        const struct { uint8_t r; int n; } rs[3] = {{kGame, pair_.Count()}, {kFront, front_.Count()}, {kGpu, gpu_.Count()}};
        for (const auto& x : rs) {
            if (x.n < static_cast<int>(c_.minSamples)) continue;
            float p = P95Of(x.r, v);
            if (havePk && pk[ResIndex(x.r)] > 0) p = std::max(p, pk[ResIndex(x.r)] - GainsOf(mask_, x.r));
            m = std::min(m, LimitOf(x.r) - p);
        }
        return m > 1e8f ? 0.0f : m;
    }
    // a wait from `lo` (room >= fullMarginMs) to `hi` (no room)
    float Wait(float lo, float hi) const {
        if (!c_.recover || c_.fullMarginMs <= 0) return hi;
        const float f = 1.0f - std::min(1.0f, std::max(0.0f, margin_) / c_.fullMarginMs);
        return lo + (hi - lo) * f;
    }
    // a step's backoff factor: 1 normally, doubled by each re-take within retakeSec (up to 16)
    float Mult(int s) const { return calmNeed_[s] / c_.calmSec; }
    View Look() const {
        View v{};
        v.n = pair_.Count();
        v.game = pair_.P95(); v.gpu = gpu_.P95(); v.front = front_.P95();
        v.gameMean = pair_.Mean(); v.gpuMean = gpu_.Mean(); v.frontMean = front_.Mean();
        if (pair_.Count() >= static_cast<int>(c_.minSamples) && v.game > c_.p95Ms) v.hot |= kGame;
        if (front_.Count() >= static_cast<int>(c_.minSamples) && v.front > c_.p95Ms) v.hot |= kFront;
        if (gpu_.Count() >= static_cast<int>(c_.minSamples) && v.gpu > c_.gpuMs) v.hot |= kGpu;
        if (pair_.Count() >= static_cast<int>(c_.minSamples) && v.game > c_.p95Ms - c_.bandMs) v.warm |= kGame;
        if (front_.Count() >= static_cast<int>(c_.minSamples) && v.front > c_.p95Ms - c_.bandMs) v.warm |= kFront;
        if (gpu_.Count() >= static_cast<int>(c_.minSamples) && v.gpu > c_.gpuMs - c_.bandMs) v.warm |= kGpu;
        return v;
    }
    float P95Of(uint8_t r, const View& v) const { return r == kGame ? v.game : r == kFront ? v.front : v.gpu; }
    float LimitOf(uint8_t r) const { return r == kGpu ? c_.gpuMs : c_.p95Ms; }
    void Decide(uint64_t now) {
        Accrue(now);
        if (now >= nextCensusAt_) {
            nextCensusAt_ = now + static_cast<uint64_t>(c_.logSec * 1e6);
            // the set that held so far is put in the card's table at every census line (a run that is stopped, not left, still leaves it); the stage's end replaces it (with the decay)
            if (c_.memory && mem_ && heldConfirmed_ && totalUs_ >= 26000000u) { mem_->Store(key_, heldMask_, false); mem_->StoreGains(key_, gain_, gainKnown_); }
            const double secs = static_cast<double>(totalUs_) / 1e6;
            Logf("q30lock: stage %s: running: pictures %u, on cadence %.1f %%, misses %u, steps now %s, time at best quality %.1f %% of %.0f s; p95 game pair %.1f ms, front end %.1f, GPU %.1f (without the display wait when the back end says so); %s; headroom %s",
                 key_, pictures_, pictures_ ? 100.0 * onCadence_ / pictures_ : 0.0, misses_, MaskText(mask_), totalUs_ ? 100.0 * static_cast<double>(bestUs_) / static_cast<double>(totalUs_) : 100.0, secs,
                 static_cast<double>(pair_.P95()), static_cast<double>(front_.P95()), static_cast<double>(gpu_.P95()), PairText(win_), ProvenLocked(now) ? "proven (cadence held)" : "not proven");
            win_ = Acc{};
        }
        if (now < settleUntil_ || hold_) return;   // (far30: hold_ = a flight is on)
        const View v = Look();
        const uint32_t ex = O().extra.load(std::memory_order_relaxed);
        if (ex != extraSeen_) { extraSeen_ = ex; pend_.step = -1; takePend_ = RelPend{}; relPend_ = RelPend{}; }   // (anticip32: a gain / cost measured across a change of the overlay would be the overlay's: void)
        if (!ex) PeakNote(now, v);   // (anticip32: the "load without steps" is not known while steps the ladder did not take are in force)
        margin_ = MarginOf(v, now);
        // recover30: the COST of the steps given back last: the p95 after minus the p95 before, shared by their gains; replaces the gain (a blend when it was measured at a take)
        if (relPend_.set && now >= relPend_.at + static_cast<uint64_t>(5e6) && v.n >= static_cast<int>(c_.minSamples)) {
            float cost = 0;
            int cnt = 0;
            for (uint8_t r : {kGame, kFront, kGpu})
                if (relPend_.res & r) { cost += P95Of(r, v) - relPend_.before[r == kGame ? 0 : r == kFront ? 1 : 2]; ++cnt; }
            if (cnt && relPend_.sumGain > 0) {
                cost = std::min(6.0f, std::max(0.0f, cost / static_cast<float>(cnt)));
                for (int s = 0; s < kStepN; ++s)
                    if (relPend_.set >> s & 1u) {
                        const float share = cost * gain_[s] / relPend_.sumGain;
                        gain_[s] = gainKnown_[s] ? 0.5f * gain_[s] + 0.5f * share : share;
                        gainKnown_[s] = true;
                        noGain_[s] = false;
                    }
            }
            relPend_ = RelPend{};
        }
        // speed31: the gain of the steps taken together: the game pair's p95 before minus after, shared by their guesses
        if (takePend_.set && now >= takePend_.at + static_cast<uint64_t>(6e6) && v.n >= static_cast<int>(c_.minSamples)) {
            float g = 0;
            int cnt = 0;
            for (uint8_t r : {kGame, kFront, kGpu})
                if (takePend_.res & r) { g += takePend_.before[r == kGame ? 0 : r == kFront ? 1 : 2] - P95Of(r, v); ++cnt; }
            if (cnt && takePend_.sumGain > 0) {
                g = std::min(8.0f, std::max(0.0f, g / static_cast<float>(cnt)));
                for (int s = 0; s < kStepN; ++s)
                    if (takePend_.set >> s & 1u) {
                        const float share = g * gain_[s] / takePend_.sumGain;
                        gain_[s] = share;
                        gainKnown_[s] = true;
                        noGain_[s] = share < c_.noGainMs;
                    }
            }
            takePend_ = RelPend{};
        }
        // the gain of the step taken last: measured once, a few seconds after it, against the p95 it was taken at
        if (pend_.step >= 0 && now >= pend_.at + static_cast<uint64_t>(kGainDelaySec[pend_.step] * 1e6f) && v.n >= static_cast<int>(c_.minSamples)) {
            const int s = pend_.step;
            float g = 0;
            int cnt = 0;
            for (uint8_t r : {kGame, kFront, kGpu})
                if ((kHelps[s] & r) && (pend_.res & r)) { g += pend_.before[r == kGame ? 0 : r == kFront ? 1 : 2] - P95Of(r, v); ++cnt; }
            if (cnt) {
                g /= static_cast<float>(cnt);
                gain_[s] = g < 0 ? 0 : g;
                gainKnown_[s] = true;
                noGain_[s] = g < c_.noGainMs;
            }
            pend_.step = -1;
        }
        // q30-28: a lone missed picture is a stall (a load, a shader build), not a trend: two within missSec are a threat. And a p95 over its limit is a threat only without PROOF that the stage
        // holds its cadence (proofSec of play with no miss): the estimates (the GPU's busy time includes the display slot's wait; the game pair's p95) were wrong at Dreadnought and the menu.
        const bool slip = SlipLocked(now);
        const bool guardP = GuardLocked(now);   // (speed31)
        const bool missRecent = MissesIn(now) >= 2;
        const bool lone = lastMissAt_ != 0 && now - lastMissAt_ < static_cast<uint64_t>(c_.missSec * 1e6);
        const bool proven = ProvenLocked(now);
        const uint8_t hotEff = proven ? uint8_t(0) : (msaaActive_ ? static_cast<uint8_t>(v.hot & ~kGpu) : v.hot);
        const bool threat = hotEff != 0 || missRecent || slip || guardP;
        if (threat) {
            calmSince_ = 0;
            if (!threatSince_) threatSince_ = now;
            // a step given back that is needed again soon: it waits longer next time
            for (int s = 0; s < kStepN; ++s)
                if (restoredAt_[s] && now - restoredAt_[s] < static_cast<uint64_t>(c_.retakeSec * 1e6)) { calmNeed_[s] = std::min(calmNeed_[s] * 2.0f, c_.calmMaxSec); restoredAt_[s] = 0; }
            float over = 0;   // how far the worst resource's p95 is over its limit (q30-27)
            for (uint8_t r : {kGame, kFront, kGpu})
                if (hotEff & r) over = std::max(over, P95Of(r, v) - LimitOf(r));
            const bool fast = (c_.fastOverMs > 0 && over >= c_.fastOverMs) || (slip && speedPct_ < c_.slipFastPct) || guardP;
            const float gapS = fast ? std::min(c_.gapSec, c_.fastGapSec) : c_.gapSec, confS = fast ? std::min(c_.confirmSec, c_.fastConfirmSec) : c_.confirmSec;
            const bool gapOk = (firstDecision_ || now - lastChangeAt_ >= static_cast<uint64_t>(gapS * 1e6)) && !trans_.on;
            if (now - threatSince_ >= static_cast<uint64_t>(confS * 1e6) && gapOk) {
                Escalate(now, v, hotEff, missRecent, slip, guardP);
                if (ex) { pend_.step = -1; takePend_ = RelPend{}; }   // (anticip32: a step taken on top of the overlay is kept by the usual rules, its gain is not learned from this moment)
            }
            return;
        }
        threatSince_ = 0;
        const bool calmNow = (v.warm == 0 || proven) && !lone && !slip && !guardP && v.n >= static_cast<int>(c_.minSamples)
                             && O().extra.load(std::memory_order_relaxed) == 0;   // (anticip32: no give-back inside an announced heavy moment; the calm time starts after it)
        if (multiCap_ < kStepN && now - multiFailAt_ >= static_cast<uint64_t>(c_.halveAfterSec * 1e6)) { multiCap_ = std::min<int>(kStepN, multiCap_ * 2); multiFailAt_ = now; }
        // recover30: a doubling halves again after halveAfterSec without a re-take
        for (int s = 0; s < kStepN; ++s)
            if (restoredAt_[s] && now - restoredAt_[s] >= static_cast<uint64_t>(c_.halveAfterSec * 1e6)) { calmNeed_[s] = std::max(c_.calmSec, calmNeed_[s] * 0.5f); restoredAt_[s] = 0; }
        if (!calmNow) { calmSince_ = 0; return; }
        if (!calmSince_) calmSince_ = now;
        const double calmFor = static_cast<double>(now - calmSince_) / 1e6;
        if (calmFor >= c_.calmSec && (!heldConfirmed_ || heldMask_ != mask_)) { heldMask_ = mask_; heldConfirmed_ = true; escalatedSinceHeld_ = false; }
        if (mask_ == 0 || trans_.on || now - lastChangeAt_ < static_cast<uint64_t>(Wait(c_.gapMinSec, c_.gapSec) * 1e6)) return;
        TryRelease(now, v, calmFor, false, proven);
    }
    // The step to give back: a "no gain" one that has been held long enough, else the highest. Step 6 only behind a cover (cover_); `only6`: nothing else (the cover's own call).
    // recover30: the waits scale with the room (Wait), the fit test uses the LEARNED gain and a margin that a flapping step has doubled, and with `multi` further steps (the next lower ones in the
    // reverse escalation order) go back in the same change while the room still covers the SUM of their learned gains.
    void TryRelease(uint64_t now, const View& v, double calmFor, bool only6, bool proven = false) {
        if (mask_ == 0 || trans_.on || now - lastChangeAt_ < static_cast<uint64_t>(Wait(c_.gapMinSec, c_.gapSec) * 1e6)) return;
        const float holdS = Wait(c_.holdMinSec, c_.holdSec);
        uint32_t set = 0;
        float acc[3] = {0, 0, 0};   // the learned gains of the chosen steps by resource (game, front, GPU)
        float sum = 0;
        float pk[3];
        const bool havePk = PeakL0(now, pk);
        for (int oi = kStepN - 1; oi >= 0; --oi) {
            const int s = kOrder[oi];
            if (!(mask_ >> s & 1u) || (baseStart_ >> s & 1u)) continue;   // (q30-28: the stage's always-on set is not given back)
            if (only6 && s != k6) continue;
            if (s == k6 && !cover_) continue;
            const float mult = Mult(s);
            // the calm this step needs: its doubling factor times the wait at today's room
            const float need = c_.recover ? mult * Wait(c_.calmMinSec, c_.calmSec) : calmNeed_[s];
            const bool eligible = now - takenAt_[s] >= static_cast<uint64_t>(holdS * 1e6) && calmFor >= need;
            if (!eligible) { if (set) break; else continue; }
            if (noGain_[s]) { if (!set) { set = 1u << s; sum = 0; } break; }   // (a step that bought nothing goes alone, at once)
            // would the resources it helps still fit with its learned gain (and those already chosen) back? A flapping step wants a larger margin (its doubling, at most 4 times the band)
            const float room = proven ? c_.probeSlackMs : -c_.bandMs * std::min(4.0f, c_.recover ? mult : 1.0f);
            bool fits = true;
            for (uint8_t r : {kGame, kFront, kGpu})
                if (kHelps[s] & r) {
                    const int cnt = r == kGame ? pair_.Count() : r == kFront ? front_.Count() : gpu_.Count();
                    const int ri = r == kGame ? 0 : r == kFront ? 1 : 2;
                    if (cnt < static_cast<int>(c_.minSamples)) continue;
                    float pred = P95Of(r, v) + acc[ri] + gain_[s];
                    // recover30: against the heaviest recent load (steps taken out) with only the steps that stay: a swinging load is judged at its top
                    if (havePk && pk[ri] > 0) pred = std::max(pred, pk[ri] - GainsOf(mask_, r) + acc[ri] + gain_[s]);
                    if (pred > LimitOf(r) + room) fits = false;
                }
            if (!fits) break;
            if (set && (s == k6 || s == k5 || (set >> k5 & 1u) || (set >> k6 & 1u))) break;   // (steps 5 and 6 are never changed together with another: one layer / one cover at a time)
            set |= 1u << s;
            for (uint8_t r : {kGame, kFront, kGpu}) if (kHelps[s] & r) acc[r == kGame ? 0 : r == kFront ? 1 : 2] += gain_[s];
            sum += gain_[s];
            if (!c_.multi || !c_.recover || s == k5 || s == k6 || PopCount(set) >= multiCap_) break;
        }
        if (!set) return;
        ReleaseSet(now, set, v, calmFor, sum);
    }
    void Escalate(uint64_t now, const View& v, uint8_t hot, bool missRecent, bool slip = false, bool guardP = false) {
        uint8_t res = hot;
        const char* by = "";
        if (!res && guardP) {   // (speed31) the guard repays lost game time with dropped pictures: the game thread's pair is the cost
            res = kGame;
            by = " (by the speed guard's dropped pictures)";
        } else if (!res) {   // misses with every p95 under its limit: the resource closest to its limit is blamed
            const float g = v.gameMean / c_.p95Ms, f = v.frontMean / c_.p95Ms, p = v.gpuMean / c_.gpuMs;
            res = g >= f && g >= p ? kGame : (p >= f ? kGpu : kFront);
            by = missRecent ? " (by the missed pictures)" : " (by the game's speed under its slip limit)";
        }
        // GXV_Q30_MSAA: while 2x MSAA is active (rung 7), let autoq step 7 -> 6 before q30lock cuts Quality GPU detail
        if (msaaActive_ && res == kGpu) return;
        // recover30: a give-back that did not hold (the threat came within retakeSec of it) is undone AT ONCE, all its steps in one change: its exposure is one confirm, not one step a gap;
        // the next give-back is smaller (multiCap) and each of its steps has doubled its wait and margin (the threat path above)
        if (c_.recover && relSetLast_ && now - relAtLast_ < static_cast<uint64_t>(c_.retakeSec * 1e6)) {
            const uint32_t back = relSetLast_ & ~static_cast<uint32_t>(mask_) & c_.allowed & ~((1u << k5) | (1u << k6));
            multiCap_ = std::max(1, PopCount(relSetLast_) / 2);
            multiFailAt_ = now;
            relSetLast_ = 0;
            if (back) {
                Accrue(now);
                const StepMask before = mask_;
                mask_ = static_cast<StepMask>(mask_ | back);
                for (int s = 0; s < kStepN; ++s) if (back >> s & 1u) { takenAt_[s] = now; meter_[s].Take(now); }
                pend_.step = -1;
                relPend_ = RelPend{};
                lastChangeAt_ = now;
                firstDecision_ = false;
                escalated_ = true;
                ++changes_;
                Logf("q30lock: stage %s: steps %s taken again at once: the give-back did not hold (%s %.1f ms); the next give-back is at most %d step%s", key_, MaskText(static_cast<StepMask>(back)),
                     ResName(res), static_cast<double>(res & kGame ? v.game : (res & kFront ? v.front : v.gpu)), multiCap_, multiCap_ == 1 ? "" : "s");
                BeginTrans(now, before, mask_);
                Reset3();
                Publish();
                return;
            }
        }
        // swap out a held step that bought nothing, then take the first inactive step that helps
        int drop = -1;
        for (int oi = 0; oi < kStepN; ++oi) {
            const int s = kOrder[oi];
            if ((mask_ >> s & 1u) && noGain_[s] && now - takenAt_[s] >= static_cast<uint64_t>(c_.holdSec * 1e6)) { drop = s; break; }
        }
        int pick = -1;
        const uint32_t inForce = mask_ | O().extra.load(std::memory_order_relaxed);   // (anticip32: a step the overlay already has in force would buy nothing now: the next one is taken; 0 extra = mask_)
        for (int oi = 0; oi < kStepN; ++oi) {
            const int s = kOrder[oi];
            if (!(inForce >> s & 1u) && (c_.allowed >> s & 1u) && StepReady(s) && (kHelps[s] & res) && !noGain_[s]) { pick = s; break; }
        }
        if (pick < 0)
            for (int oi = 0; oi < kStepN; ++oi) {   // (a step that bought nothing before may be the only one left that helps)
                const int s = kOrder[oi];
                if (!(inForce >> s & 1u) && (c_.allowed >> s & 1u) && StepReady(s) && (kHelps[s] & res) && s != drop) { pick = s; break; }
            }
        if (pick == k6 && !(cover_ && ready6_)) {
            // step 6 changes the picture's size: not in play. It is WANTED now; the first cover takes it, and the card starts the next visit at it.
            if (!pending6_) {
                pending6_ = true;
                want6_ = true;
                pend6At_ = now;
                Logf("q30lock: stage %s: step 6 (%s) wanted because %s %.1f ms: waits for a cover (a load, a closed screen, the pause menu); the next visit starts at it", key_, StepWhat(k6),
                     ResName(res & kGpu ? static_cast<uint8_t>(kGpu) : res), static_cast<double>(v.gpu));
            }
            return;
        }
        if (pick < 0) {
            if (pending6_) return;   // (waiting for a cover is not running out)
            if (!exhaustedLogged_ || now - exhaustedAt_ > static_cast<uint64_t>(30e6)) {
                exhaustedLogged_ = true;
                exhaustedAt_ = now;
                // by how much the worst resource is still over its limit (p95 against the limit; misses with every p95 under: 0)
                float over = 0;
                uint8_t worst = res;
                for (uint8_t r : {kGame, kFront, kGpu}) if ((res & r) && P95Of(r, v) - LimitOf(r) > over) { over = P95Of(r, v) - LimitOf(r); worst = r; }
                ++exhaustCount_;
                if (over >= exhaustWorst_) { exhaustWorst_ = over; exhaustRes_ = worst; }
                Logf("q30lock: stage %s: exhausted, still over by %.1f ms: no step left for %s (steps now %s): game pair p95 %.1f ms (limit %.1f), front end %.1f, GPU %.1f (limit %.1f)%s", key_,
                     static_cast<double>(over), ResName(worst), MaskText(mask_), static_cast<double>(v.game), static_cast<double>(c_.p95Ms), static_cast<double>(v.front), static_cast<double>(v.gpu),
                     static_cast<double>(c_.gpuMs), missRecent ? ", pictures still missed" : "");
            }
            return;
        }
        Accrue(now);
        const StepMask before = mask_;
        if (guardP && c_.guardMaxSteps > 1 && pick >= 0 && pick != k5 && pick != k6 && drop < 0) {
            // speed31: the drops need more than one step's learned gain: several steps in ONE change (each has its own fade walk), the least visible first, until the gains cover the need
            uint32_t set = 1u << pick;
            float sum = gain_[pick];
            const float need = guardNeed_ + c_.bandMs;
            for (int oi = 0; oi < kStepN && sum < need && PopCount(set) < c_.guardMaxSteps; ++oi) {
                const int s = kOrder[oi];
                if ((inForce >> s & 1u) || (set >> s & 1u) || !(c_.allowed >> s & 1u) || !StepReady(s) || !(kHelps[s] & res) || noGain_[s] || s == k5 || s == k6) continue;
                set |= 1u << s;
                sum += gain_[s];
            }
            if (PopCount(set) > 1) {
                mask_ = static_cast<StepMask>(mask_ | set);
                float sumG = 0;
                for (int s = 0; s < kStepN; ++s) if (set >> s & 1u) { takenAt_[s] = now; meter_[s].Take(now); sumG += gain_[s]; }
                relPend_ = RelPend{};
                pend_.step = -1;
                takePend_ = RelPend{};
                takePend_.set = set; takePend_.at = now; takePend_.res = res; takePend_.sumGain = sumG;
                takePend_.before[0] = v.game; takePend_.before[1] = v.front; takePend_.before[2] = v.gpu;
                lastChangeAt_ = now;
                firstDecision_ = false;
                escalated_ = true;
                ++changes_;
                guardCuts_ += static_cast<uint32_t>(PopCount(set));
                Logf("q30lock: stage %s: steps %s together (several: the speed guard drops pictures, it needs %.1f ms a picture and one step's learned gain is %.1f ms; the gains add up to %.1f ms) because %s %.1f ms%s", key_,
                     MaskText(static_cast<StepMask>(set)), static_cast<double>(guardNeed_), static_cast<double>(gain_[pick]), static_cast<double>(sum), ResName(res & kGame ? static_cast<uint8_t>(kGame) : res),
                     static_cast<double>(res & kGame ? v.game : v.front), by);
                BeginTrans(now, before, mask_);
                Reset3();
                Publish();
                return;
            }
        }
        if (guardP) ++guardCuts_;
        if (drop >= 0) { mask_ = static_cast<StepMask>(mask_ & ~(1u << drop)); takenAt_[drop] = 0; }
        mask_ = static_cast<StepMask>(mask_ | (1u << pick));
        takenAt_[pick] = now;
        meter_[pick].Take(now);
        if (drop >= 0) meter_[drop].Give(now);
        relPend_ = RelPend{};
        pend_.step = pick;
        pend_.at = now;
        pend_.res = res;
        pend_.before[0] = v.game; pend_.before[1] = v.front; pend_.before[2] = v.gpu;
        lastChangeAt_ = now;
        firstDecision_ = false;
        escalated_ = true;
        ++changes_;
        float val = v.gpu;
        if (res & kGame) val = v.game;
        else if (res & kFront) val = v.front;
        Logf("q30lock: stage %s: step %s (%s) because %s %.1f ms%s", key_, StepLabel(pick), StepWhat(pick), ResName(res & kGame ? static_cast<uint8_t>(kGame) : (res & kGpu ? static_cast<uint8_t>(kGpu) : res)), static_cast<double>(val), by);
        BeginTrans(now, before, mask_);
        if (drop >= 0) Logf("q30lock: stage %s: step %s (%s) off: it bought %.2f ms (no gain)", key_, StepLabel(drop), StepWhat(drop), static_cast<double>(gain_[drop]));
        Reset3();
        Publish();
    }
    void ReleaseSet(uint64_t now, uint32_t set, const View& v, double calmFor, float sumGain) {
        Accrue(now);
        const StepMask before = mask_;
        relSetLast_ = set; relAtLast_ = now;
        relPend_ = RelPend{};
        relPend_.set = set; relPend_.at = now; relPend_.sumGain = sumGain;
        relPend_.before[0] = v.game; relPend_.before[1] = v.front; relPend_.before[2] = v.gpu;
        for (int s = 0; s < kStepN; ++s)
            if (set >> s & 1u) {
                mask_ = static_cast<StepMask>(mask_ & ~(1u << s));
                takenAt_[s] = 0;
                restoredAt_[s] = now;
                meter_[s].Give(now);
                for (uint8_t r : {kGame, kFront, kGpu}) if (kHelps[s] & r) relPend_.res |= r;
                Logf("q30lock: stage %s: step %s (%s) off because calm for %.0f s (game pair p95 %.1f ms, front end %.1f, GPU %.1f; its gain %.1f ms%s; %s)", key_, StepLabel(s), StepWhat(s), calmFor,
                     static_cast<double>(v.game), static_cast<double>(v.front), static_cast<double>(v.gpu), static_cast<double>(gain_[s]), gainKnown_[s] ? " measured" : " assumed",
                     PopCount(set) > 1 ? "several steps together: the room covers their gains" : "one step");
            }
        lastChangeAt_ = now;
        ++changes_;
        pend_.step = -1;
        BeginTrans(now, before, mask_);
        Reset3();
        Publish();
    }
    // A change of the mask is walked in by the actuators (lockfade.h); the picture time during the walk is watched and one line says what it cost.
    void BeginTrans(uint64_t now, StepMask from, StepMask to) {
        uint32_t ms = 0;
        for (int s = 0; s < kStepN; ++s) if ((from ^ to) >> s & 1u) ms = std::max(ms, kTransMs[s]);
        trans_ = Trans{true, from, to, now, now + static_cast<uint64_t>(ms) * 1000u + 500000u, 0.0f};
    }
    void WatchTrans(uint64_t now, float pictureMs) {
        if (!trans_.on) return;
        if (pictureMs > trans_.worst) trans_.worst = pictureMs;
        if (now < trans_.until) return;
        Logf("q30lock: transition %u->%u took %.0f ms, worst picture %.1f ms", static_cast<unsigned>(trans_.from), static_cast<unsigned>(trans_.to), static_cast<double>(trans_.until - trans_.at) / 1000.0,
             static_cast<double>(trans_.worst));
        trans_.on = false;
    }
    void Reset3() { pair_.Clear(); gpu_.Clear(); front_.Clear(); havePair_ = false; pairAcc_ = 0; threatSince_ = 0; calmSince_ = 0; }
    void Publish() {
        O().mask.store(mask_, std::memory_order_relaxed);
        O().rev.fetch_add(1, std::memory_order_release);
    }
    const char* MaskText(StepMask m) const {
        static thread_local char buf[4][48];
        static thread_local int k = 0;
        char* b = buf[k++ & 3];
        b[0] = 0;
        size_t u = 0;
        for (int s = 0; s < kStepN; ++s)
            if (m >> s & 1u) u += static_cast<size_t>(std::snprintf(b + u, 48 - u, "%s%s", u ? "+" : "", StepLabel(s)));
        if (!u) std::snprintf(b, 48, "none");
        return b;
    }
    void Logf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (logN_ >= 8) return;
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(log_[logN_], sizeof log_[0], fmt, ap);
        va_end(ap);
        ++logN_;
    }

    mutable std::mutex m_;
    Config c_{};
    char key_[48] = "?";
    bool active_ = false, play_ = false, firstDecision_ = true, havePair_ = false, heldConfirmed_ = false, escalated_ = false, escalatedSinceHeld_ = false, exhaustedLogged_ = false, stalledBefore_ = false;
    StepMask mask_ = 0, startMask_ = 0, heldMask_ = 0;
    uint64_t stageAt_ = 0, settleUntil_ = 0, lastChangeAt_ = 0, threatSince_ = 0, calmSince_ = 0, lastMissAt_ = 0, lastPlayAt_ = 0, lastTick_ = 0, bestUs_ = 0, totalUs_ = 0, nextCensusAt_ = 0, exhaustedAt_ = 0,
             lastStepTick_ = 0;
    uint64_t takenAt_[kStepN] = {}, restoredAt_[kStepN] = {}, usedUs_[kStepN] = {};
    float gain_[kStepN] = {}, calmNeed_[kStepN] = {};
    bool gainKnown_[kStepN] = {}, noGain_[kStepN] = {};
    Ring pair_, gpu_, front_, drawnR_, undrawnR_;
    // q30-28: the real game pair = drawn turn + the undrawn turn after it (means over the census window / the stage, and the p95 of the last 48)
    struct Acc { double d = 0, u = 0, p = 0; uint32_t nd = 0, nu = 0, np = 0; } win_, st_;
    const char* PairText(const Acc& a) const {
        static thread_local char buf[4][220];
        static thread_local int k = 0;
        char* b = buf[k++ & 3];
        std::snprintf(b, 220, "game pair (drawn + undrawn) mean %.1f ms = drawn %.1f + undrawn %.1f, p95 %.1f (%u pairs)", a.np ? a.p / a.np : 0.0, a.nd ? a.d / a.nd : 0.0, a.nu ? a.u / a.nu : 0.0,
                      static_cast<double>(pair_.P95()), a.np);
        return b;
    }
    static constexpr int kMissRing = 8;
    uint64_t missAt_[kMissRing] = {};
    uint32_t missHead_ = 0, sinceMiss_ = 0;
    bool menu_ = false;
    float pairAcc_ = 0, lastPair_ = 0;
    uint32_t tickN_ = 0, pictures_ = 0, onCadence_ = 0, misses_ = 0, stalls_ = 0, short_ = 0, recentMisses_ = 0, changes_ = 0;
    struct Trans { bool on = false; StepMask from = 0, to = 0; uint64_t at = 0, until = 0; float worst = 0; } trans_;
    bool ready6_ = false, cover_ = false, pending6_ = false, want6_ = false;
    bool msaaActive_ = false;
    uint64_t pend6At_ = 0;
    uint32_t exhaustCount_ = 0;
    float exhaustWorst_ = 0;
    uint8_t exhaustRes_ = 0;
    Memory* mem_ = nullptr;
    struct Pend { int step = -1; uint64_t at = 0; uint8_t res = 0; float before[3] = {}; } pend_;
    char log_[8][400];
    int logN_ = 0;
    char clog_[12][160];
    int clogN_ = 0;
    // recover30 state
    Meter meter_[kStepN];
    float margin_ = 0, speedPct_ = 100.0f;
    double spdA_ = 0, spdB_ = 0;
    uint64_t slipSince_ = 0, lastSlipAt_ = 0;
    // recover30: the load WITHOUT the ladder's steps (p95 + the learned gains of the steps in force), the largest in each half second, for the last 24 s
    static constexpr int kPeakBins = 48;
    float peakV_[kPeakBins][3] = {};
    int64_t peakBin_[kPeakBins];
    uint64_t peakFirst_ = 0;
    uint32_t relSetLast_ = 0;
    uint64_t relAtLast_ = 0, multiFailAt_ = 0;
    int multiCap_ = kStepN;
    struct RelPend { uint32_t set = 0; uint64_t at = 0; uint8_t res = 0; float before[3] = {}; float sumGain = 0; } relPend_;
    // speed31
    RelPend takePend_;
    uint64_t guardSince_ = 0, lastGuardAt_ = 0;
    float guardNeed_ = 0;
    uint32_t guardCuts_ = 0;
};

}  // namespace q30lock
}  // namespace port
