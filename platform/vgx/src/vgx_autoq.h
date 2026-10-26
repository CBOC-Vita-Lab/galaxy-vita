// Automatic picture quality (dynamic resolution): the controller that picks the back end's internal
// render size from what the GPU has to spare, and the GPU clock that feeds it.
//
// The ladder: render sizes for the game's usual 640x456 EFB; another EFB size is scaled by the same
// factors (rung w / 640 across, rung h / 456 down). Rung 2 (640x456, factors 1 x 1) is the picture of
// every build before this one; 6 is the screen's own 960x544. Above it the EFB is multisampled (2 and 4
// samples a pixel, resolved into the EFB's texture when a scene stores it). Cost is the pixel count against
// rung 2, times the multisampling's factor.
//
//   rung  size      samples  cost
//   0     544x388   1        0.72   floor: only when the GPU is the limit AND a lower size shortens the frames
//   1     592x422   1        0.86
//   2     640x456   1        1.00   start (today's picture)
//   3     720x480   1        1.18
//   4     800x504   1        1.38
//   5     880x520   1        1.57
//   6     960x544   1        1.79   the screen's pixels: no upscale blur
//   7     960x544   2        2.33   anti-aliased edges (x 1.3)
//   8     960x544   4        3.40   (x 1.9)
//
// Multisampling's cost factor (MsaaCost) is deliberately high: on the Vita's GPU the shading is per pixel, but the
// depth test and the depth buffer are per sample, and the EFB's depth is loaded and stored at every scene break of
// the picture (STORE_DEPTH_STENCIL; several per picture in the castle plaza), so 4x moves four times the depth
// bytes. An over-estimate only makes the controller climb later; a rung that turns out dearer than predicted is
// left at the first overload and blocked like any other. Multisampling at a lower size (640x456 2x: 1.30) is not
// on the ladder: on the 960x544 screen the upscale blur of a smaller picture costs more sharpness than the edges'
// steps, and 720x480 without it is cheaper (1.18). The back end leaves rungs 7 and 8 out (maxRung 6) when the
// vitaGL has no per-framebuffer multisampling (FBO_MSAA) or the start-up GXV_MSAA already multisamples everything.
//
// The signal (Controller::Add, one call per presented frame, in order), from one of two GPU clocks:
//   GPU timers (a vitaGL built with GPU_TIMERS, tools/vitagl/gpu_timers.py; after the Strikers port): every scene
//           has its own fragment notification and a watcher times its completion; a frame's busy time is the sum
//           over its scenes of done - max(submitted, previous done): the GPU's own work, without the gaps in
//           which it waited for the back end. load = mean busy / mean frame interval.
//   the frame fence (any vitaGL): a watcher times when the GPU passes the fence of the frame's last scene;
//           busy = gpuDone(n) - max(gpuDone(n-1), start(n)), an UPPER bound of the GPU's work on frame n (none of
//           it can start before the back end begins the frame or before the GPU has finished frame n-1; it
//           includes the back end's submission gaps). load = busy / time between GPU completions.
//   Either way: 1.0 = the GPU never waited (it may be the limit), lower = it idled.
//   cpu   = the back-end thread's CPU time over the window divided by the window's time: near 1 = the back
//           end itself never waited (its own CPU work is the limit), lower = it was blocked (on the GPU when
//           the load is 1.0).
// Decisions, once per window of `windowFrames` frames:
//   up    when load x cost(next)/cost(now) <= upTarget for upHoldSec (default 78 % for 3 s): the next rung
//         is predicted to leave the GPU idle part of every frame, so it cannot be what limits the frames.
//         (Conservative: busy includes the back end's own submission time, which does not grow with pixels.)
//         The step goes as many rungs up (at most maxUpStep, 3) as the same prediction allows.
//   down  above rung 2, when load >= downAbove for downHoldSec (90 % for 0.5 s): the GPU may be the limit,
//         so a picture above today's quality gives way at once (to rung 2 directly when the load is
//         saturated, >= 97 %). The rung left is blocked for blockSec (30 s, doubling on each repeat).
//   below at or below rung 2, only when load >= downBelow (95 %) AND cpu <= cpuMax (85 %) for belowHoldSec
//         (1.5 s): the GPU is the limit, not the back end's CPU. After checkSec (2 s) the mean frame interval
//         must be at least betterFrac (5 %) shorter than before, or the step is undone and steps below are
//         blocked (a lower picture that buys no speed is not kept).
//   fast  above rung 2, fastFrames (3) frames in a row at a saturated load (>= 97 %) step one rung down at once,
//         without waiting for a window, and three more such frames after the change step again (the Strikers
//         port's rule): a picture above today's costs only a few slow frames when an effect overloads the GPU.
// A frame interval over gapResetMs (250 ms: a load, a shader compile) restarts the windows; over longGapMs
// (1.5 s: a new scene) the blocks are forgotten as well and the rung goes back to the start rung (today's).
// After every change the first settleSec (0.25 s) are not measured (the change itself costs GPU time).
//
// Calm mode (Config::calm, the default; GXV_AUTOQ_CALM=0 gives the rules above unchanged, Config::Legacy()).
// The first console run with the controller (23 Oct 2026, 7 minutes, Comet Observatory and Good Egg) changed the rung 24
// times, and every change froze the picture for 100-270 ms (new render targets, the resample of the old ones; a change
// of multisampling alone 35-90 ms): those freezes were the stutter the player felt, 5.7 s of them. The rungs also went
// up and down: the Observatory went 640 -> 880 -> 960 2x -> 800 -> 640 -> 800 -> ... in two minutes, because its frames
// are often limited by the game's CPU (72 ms a frame just after a load) and the GPU load, busy / interval, then looks
// low although the GPU's busy time already fills most of a frame budget. So in calm mode:
//   up      the prediction is against the frame budget as well: busy x cost(next)/cost(now) <= upTarget x
//           min(window interval, budgetMs) (33.3 ms: a 30 fps picture), so a CPU-limited frame does not hide a GPU
//           that would be the limit at the next rung. And a step up waits for a quiet moment (FrameTimes::quiet: the
//           pause menu, a fade or wipe closing the screen, set by the back end from the game), where a frozen picture
//           is not seen: it is taken there when the last windows of play showed the room (within roomMemorySec).
//           Without a quiet moment a step up comes only after upLongHoldSec of room in a row.
//   down    after downHoldSec (1.5 s) of a saturated GPU (downAbove 97 %: 95 % at 60 fps is still 60 fps), as far as the
//           load says (not straight to today), and the fast path needs fastFrames (12) saturated frames in a row: a GPU
//           at its limit for a second costs a few slow frames, a change a 150-300 ms freeze. The rung left is blocked for
//           blockSec (60 s, doubling to 480 s).
//   below   (GPU-limited at or below today, belowHoldSec 4 s) and the undo of a step below that bought nothing wait for a
//           quiet moment too: they buy little speed and cost a freeze.
//   margins upTarget 70 % (78 % legacy); a quiet moment steps up only after upQuietHoldSec (8 s) of room in a row that
//           ended within roomMemorySec (5 s); without a quiet moment only after upLongHoldSec (120 s).
// The second console run (thp53, 23 Oct 22:30, 9 minutes): 11 changes, 5 of them below today in play and 2 steps up in a
// pause undone 2-3 s later by an overload at 0.95; each change cost 90-150 ms in ApplyRung (EFB target ~50 ms, about 8 ms
// for each of up to 11 render textures) plus a slow next frame. These rules answer that run.
//   scenes  over longGapMs (a load) the rung of the scene being left is remembered under its key (FrameTimes::scene,
//           a hash of the stage's name from the game, 0 = unknown), and the new scene starts at its remembered rung
//           (or the start rung): the change falls in the load, not in play. A long gap within the same scene keeps
//           the rung. Up to 32 scenes are remembered.
//   Quiet frames are not measured (a black screen or a still menu says nothing about play).
//
// Opt-in 30 Hz target (GXV_AUTOQ_TARGET=30, default 0): the throughput estimate
// is max(two game turns' CPU work, back-end CPU per picture, GPU busy/picture),
// never the sum of overlapping stages or a deliberate present wait. A step up
// must leave room in that whole-picture budget AND measured cadence must fit:
// unmeasured front-end/shared-core work is not permission to raise quality.
// A GPU-limited miss can step down
// at quiet moments after the usual hold, or after 8 s without one. Target-only
// rung -1 is 480x340, tried only after the old 544x388 floor itself misses.
// Below-today trials are judged on throughput work, so deliberate 20 Hz pacing
// does not undo a real GPU saving. Changes are at least 3 s apart (except scene
// restores). Paced presentation needs exact GPU timers; a late frame fence
// cannot distinguish callback waiting from GPU work, so it holds the rung.
#pragma once
#include <cstdint>

namespace vgx::autoq {

struct Rung {
    uint16_t w, h;   // render size for a 640x456 EFB
    uint8_t samples; // the EFB's samples a pixel (1: no multisampling)
};
inline constexpr Rung kLadder[] = {{544, 388, 1}, {592, 422, 1}, {640, 456, 1}, {720, 480, 1}, {800, 504, 1},
                                   {880, 520, 1}, {960, 544, 1}, {960, 544, 2}, {960, 544, 4}};
inline constexpr int kRungs = static_cast<int>(sizeof kLadder / sizeof kLadder[0]);
inline constexpr int kToday = 2;      // 640x456: the size of every build before automatic quality
inline constexpr int kTopSingle = 6;  // 960x544, the highest rung without multisampling
inline constexpr int kBaseW = 640, kBaseH = 456;
inline constexpr int kEmergency = -1, kNoRung = -2;
inline constexpr Rung kEmergencySize{480, 340, 1};
// Keep the public 0..8 rung IDs unchanged; the extra floor exists only in target mode.
inline const Rung& RungAt(int rung) { return rung == kEmergency ? kEmergencySize : kLadder[rung]; }
inline double MsaaCost(int samples) { return samples >= 4 ? 1.9 : (samples >= 2 ? 1.3 : 1.0); }
inline double Cost(int rung) {
    const Rung& r = RungAt(rung);
    return static_cast<double>(r.w) * r.h / (static_cast<double>(kBaseW) * kBaseH) * MsaaCost(r.samples);
}

struct Config {
    int targetFps = 0;        // 30: whole-picture throughput target; 0: the old GPU-only controller
    int minRung = 0, maxRung = kRungs - 1, startRung = kToday;
    double upTarget = 0.78;    // predicted load after a step up must stay at or below this
    double downAbove = 0.90;   // above today: step down when the load reaches this ...
    double saturated = 0.97;   // ... straight to today when it reaches this
    double downBelow = 0.95;   // at or below today: step down only with this load ...
    double cpuMax = 0.85;      // ... and the back end's CPU busy at most this fraction of the time
    double betterFrac = 0.05;  // a step below today must shorten the frame interval by this much
    int maxUpStep = 3;         // rungs one step up may climb when the prediction allows
    int windowFrames = 12;     // frames per decision window
    int fastFrames = 3;        // frames in a row at a saturated load that step down at once (above today)
    int minChangeFrames = 20;  // frames between two changes at least (each makes new targets while the old are still held;
                               // the Strikers port, 160c3f9); a new scene's return to the start rung is not held back
    double upHoldSec = 3.0, downHoldSec = 0.5, belowHoldSec = 1.5, checkSec = 2.0, settleSec = 0.25;
    double blockSec = 30.0, maxBlockSec = 240.0;
    double gapResetMs = 250.0, longGapMs = 1500.0, outlierMs = 100.0;
    // calm mode (see above)
    bool calm = true;
    double budgetMs = 33.3;       // the frame budget a step up must fit in (upTarget of it)
    double upLongHoldSec = 120.0; // room in a row that steps up without a quiet moment
    double upQuietHoldSec = 8.0;  // room in a row before a quiet moment may step up
    double roomMemorySec = 5.0;   // how old the last window with room (or a GPU limit below today) may be at the quiet moment
    // The rules before calm mode (GXV_AUTOQ_CALM=0).
    static Config Legacy() {
        Config c;
        c.calm = false;
        c.downAbove = 0.90;
        c.upTarget = 0.78;
        c.downHoldSec = 0.5;
        c.fastFrames = 3;
        c.belowHoldSec = 1.5;
        c.blockSec = 30.0;
        c.maxBlockSec = 240.0;
        return c;
    }
    Config() {
        // (the calm defaults; Legacy() puts back the old ones)
        downHoldSec = 1.5;
        downAbove = 0.97;  // (calm: only when the GPU really makes the frames longer; 0.95 at 60 fps is still 60 fps)
        upTarget = 0.70;   // (calm: more margin, a step up taken in a pause must hold after it)
        fastFrames = 12;
        belowHoldSec = 4.0;
        blockSec = 60.0;
        maxBlockSec = 480.0;
    }
};

// One presented frame (microseconds on one clock).
struct FrameTimes {
    uint64_t startUs = 0;      // the back end began executing the frame
    uint64_t presentUs = 0;    // its present (swap) returned: all of its GPU work has been submitted
    uint64_t gpuDoneUs = 0;    // frame fence clock: the GPU had finished all of it (0: not known)
    uint64_t backendRunUs = 0; // the back-end thread's CPU time so far (0: not known)
    uint32_t gameRunUs = 0, gameTurns = 0; // target mode: cumulative CPU time/turns, unsigned wrap is intentional
    uint32_t gameFaults = 0;   // cumulative failed game-clock samples; a later valid turn must not hide one
    bool gameKnown = false;
    bool paced = false;        // paced display callback: the late frame fence alone cannot measure GPU busy time
    bool timers = false;       // GPU timers clock: gpuBusyUs is used, gpuDoneUs is not
    bool busyKnown = false;    // ... a frame finished on the GPU since the previous present, and this is its busy time
    uint32_t gpuBusyUs = 0;
    bool quiet = false;        // calm mode: a quiet moment (pause menu, closed fade or wipe): not measured, a step up may be taken
    uint32_t scene = 0;        // calm mode: the scene's key (0 = unknown)
};

enum class Why : uint8_t { None, Headroom, Overload, Saturated, GpuBound, NoGain, NewScene, Forced, NoMemory };
const char* WhyText(Why w);

struct Change {
    int from = kToday, to = kToday;
    Why why = Why::None;
    double load = 0, cpu = -1, intervalMs = 0; // the window that decided it
    uint64_t atUs = 0;
};

class Controller {
public:
    explicit Controller(const Config& c = Config());
    // One presented frame, in order. True when the wanted rung changed with this frame (LastChange()).
    bool Add(const FrameTimes& f);
    // The step up just chosen cannot be made in full (the back end has no video memory for it): the rung becomes
    // `highest` (the highest that fits; the rung before the step when none does), and every rung above it is blocked
    // like an overloaded one (blockSec, doubling).
    void Limit(int highest, uint64_t now);
    // The back end is short of video memory: the rung becomes `to` at once (if lower) and every rung above it is blocked.
    void Drop(int to, uint64_t now);
    // The back end could not make the targets of rung `failed` (no memory): the rung is `back` again, `failed` is blocked
    // (and, when it was a step up, every rung above it).
    void Refused(int back, int failed, uint64_t now);
    int Current() const { return rung_; }
    const Change& LastChange() const { return last_; }
    // The last finished window (diagnostics): GPU load, back-end CPU fraction (-1: unknown), mean frame interval.
    double WindowLoad() const { return winLoad_; }
    double WindowCpu() const { return winCpu_; }
    double WindowIntervalMs() const { return winIntervalMs_; }
    double WindowWorkMs() const { return winWorkMs_; }
    double WindowGameMs() const { return winGameMs_; }
    double WindowBackendMs() const { return winBackendMs_; }
    unsigned Windows() const { return windows_; }
    unsigned Changes() const { return changes_; }
    const Config& Cfg() const { return cfg_; }

private:
    void ResetWindow();
    void ResetHolds();
    bool Evaluate(uint64_t now, bool mayChange);
    void SetRung(int to, Why why, uint64_t now);
    bool Blocked(int rung, uint64_t now) const { return now < blockUntil_[rung + 1]; }
    void Block(int rung, uint64_t now);

    Config cfg_;
    int rung_;
    Change last_;
    // previous frame
    bool havePrev_ = false;
    uint64_t prevDone_ = 0, prevPresent_ = 0, prevRun_ = 0;
    uint32_t prevGameRun_ = 0, prevGameTurns_ = 0, prevGameFaults_ = 0;
    bool prevGameKnown_ = false;
    // window accumulators
    uint64_t busy_ = 0, span_ = 0, present_ = 0, run_ = 0;
    unsigned frames_ = 0, busyN_ = 0;
    unsigned overFrames_ = 0; // frames in a row over downAbove (fast path)
    bool runKnown_ = true;
    uint64_t gameRun_ = 0, gameTurns_ = 0;
    bool gameKnown_ = true;
    bool hintKnown_ = true;
    uint32_t currentWorkUs_ = 0;
    // holds (seconds the condition has lasted)
    double overT_ = 0, boundT_ = 0, roomT_ = 0;
    double boundSpan_ = 0;
    unsigned boundFrames_ = 0;
    uint64_t settleUntil_ = 0;
    unsigned sinceChange_ = 1u << 30; // frames since the last change (minChangeFrames)
    // a step below today waiting to be judged
    bool checking_ = false;
    int checkFrom_ = kToday;
    double checkBefore_ = 0, checkSpan_ = 0;
    unsigned checkFrames_ = 0;
    // blocks
    uint64_t blockUntil_[kRungs + 1] = {};
    double blockLen_[kRungs + 1] = {};
    uint64_t belowBlockedUntil_ = 0;
    double belowBlockLen_ = 0;
    // diagnostics
    double winLoad_ = 0, winCpu_ = -1, winIntervalMs_ = 0;
    double winWorkMs_ = 0;
    double winGameMs_ = -1, winBackendMs_ = -1;
    unsigned windows_ = 0, changes_ = 0;
    // calm mode
    int pendingUp_ = 0;              // rungs the stretch of room allowed (the smallest step of its windows)
    int minUp_ = 0;
    int pendingUndo_ = -1;           // calm: a step below today that bought nothing, undone at the next quiet moment
    bool pendingBelow_ = false;      // calm: a step below today is due (the GPU is the limit at today's size)
    uint64_t belowAt_ = 0;
    double belowBefore_ = 0;
    uint64_t roomAt_ = 0;            // when that window ended (0: no room seen since the last change or overload)
    uint32_t scene_ = 0;             // the key of the scene being played
    struct SceneRung { uint32_t key; int8_t rung; };
    SceneRung sceneMem_[32] = {};
    unsigned sceneNext_ = 0;
    void Remember(uint32_t key, int rung);
    int Recall(uint32_t key) const;  // kNoRung: unknown (the emergency rung is a valid remembered value)
public:
    unsigned QuietUps() const { return quietUps_; }
private:
    unsigned quietUps_ = 0;
};

// The log of automatic quality: the Vita's port_log (the console's log file; the back end's stderr goes nowhere there),
// stderr elsewhere. One line, no newline needed.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool Target30Enabled(); // GXV_AUTOQ_TARGET, read once for both the controller and game CPU sampling

// ---- the GPU clock (the Vita: a watcher thread that time-stamps the GPU's completion of every presented
// frame through vitaGL's scene-end notification). Elsewhere it reports nothing and the controller never
// moves. All calls are on the back end's thread except the watcher's own.
namespace clock {
bool Start();                                 // false: no GPU clock on this platform / library
void FrameBegin();                            // the back end starts a frame
void FramePresented();                        // the present (swap) returned
bool NextDone(FrameTimes& out);               // the oldest frame whose GPU work is known to be done, in order
void Counters(char* out, unsigned size);      // the clock's own counters for the status line
uint64_t NowUs();
} // namespace clock

} // namespace vgx::autoq
