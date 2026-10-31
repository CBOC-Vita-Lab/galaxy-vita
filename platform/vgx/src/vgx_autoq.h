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
//         blocked (a lower picture that buys no speed is not kept). In target mode the check is on the GPU's busy time
//         instead (see "Below-today trials" further down).
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
// Recovery (Config::recover, GXV_AUTOQ_RECOVER=1, calm mode, default off). Console reading of the rules above, for a game that
// has to hold 30 and should go back to its full size afterwards: (1) a step up in play needs 120 s of room in a row, and in
// target mode ONE frame whose predicted work at the next rung exceeds 70 % of the budget (23 ms) ends the stretch, so a scene
// with noise of 15-25 % around a mean that fits stays at a lower rung for minutes; (2) the doubling of the blocks (60 s up to
// 480 s) is forgotten only at a new scene, so a few overloads in one long stage keep the full size away for most of it.
// With the option: the stretch needed in play is recoverHoldSec (20 s, the smallest step any window of it allowed, one
// rung at least); a frame ends the stretch only when its cadence missed the budget or its predicted work exceeds
// recoverFrameFrac (100 %) of it, the window means still having to fit upTarget; and the blocks' doubling starts again
// blockForgetSec (300 s) after the last block. Step downs are untouched: they still need the same overload for the same
// time, so one spike (or an overload shorter than downHoldSec, 1.5 s) costs nothing, and the quiet-moment paths are as before.
//
// Opt-in 30 Hz target (GXV_AUTOQ_TARGET=30, default 0): the throughput estimate
// is max(two game turns' CPU work, back-end CPU per picture, GPU busy/picture),
// never the sum of overlapping stages or a deliberate present wait. A step up
// must leave room in that whole-picture budget AND measured cadence must fit:
// unmeasured front-end/shared-core work is not permission to raise quality.
// A GPU-limited miss can step down
// at quiet moments after the usual hold, or after 8 s without one. Target-only
// rung -1 is 480x340, tried only after the old 544x388 floor itself misses.
// Below-today trials are judged on what a smaller picture is for: the GPU's busy
// time per picture, so deliberate 20 Hz pacing does not undo a real GPU saving.
// Changes are at least 3 s apart (except scene restores). Paced presentation
// needs exact GPU timers; a late frame fence cannot distinguish callback waiting
// from GPU work, so it holds the rung.
// Below-today trials in target mode (the console, Beach Bowl, 31 Oct 2026: the GPU's busy time per picture hardly changed
// from 640x456 to 480x340, 38 ms against 36-38 ms, yet the controller went down every rung to the emergency one, blurrier
// for nothing): the GPU's mean busy time over checkSec after the step is compared with its mean over the GPU-limited stretch
// before it, against the saving the sizes predict (1 - Cost(to) / Cost(from): 14 % for 640 -> 592, 23 % for 544 -> 480). The
// step is kept only when the GPU time fell by at least gainShare (half) of that, and by betterFrac at least, measured over
// belowCheckSec (4 s): the busy time of a half-second window wanders from 103 % to 117 % of the budget in the water galaxy,
// so a bare 5 % over 2 s let a step that bought nothing pass by chance. A stall during the check (the change's own freeze is often just over gapResetMs) no longer ends it
// unjudged (that kept the step without a verdict). A step that bought nothing is undone at the next quiet moment, or after
// noGainUndoSec (5 s) without one, and steps below are blocked for blockSec (doubling) from the undo, not from the verdict
// (a long wait for a quiet moment used up most of the block). A scene remembers, with its rung, the rung below which a step
// bought nothing; the next visit takes no step below it for maxBlockSec (a stage whose GPU time does not scale with the
// picture does not try the useless drop again on every visit; after that it may try once more, in case it was the moment).
//
// Performance mode (GXV_AUTOQ_TARGET=60, Config::targetFps 60; the default stays 30, Quality mode: never below 30, then the
// sharpest picture): as close to 60 pictures a second as the GPU allows. Everything of target mode above, against a budget of
// 16.7 ms (a picture every retrace) instead of 33.3 ms, and with ONE game turn per picture instead of two in the whole-picture
// work: max(one game turn's CPU, back-end CPU per picture, GPU busy/picture). So:
//   - only a GPU-limited miss lowers anything (the GPU at 95 % of 16.7 ms and the largest stage by 5 %); a game-CPU-limited scene
//     keeps its size, a smaller picture would buy it nothing;
//   - the order of giving up is the same: anisotropic filtering (at the top rung only, so off in practice), the multisampled rungs
//     and the sizes above the game's own (they come only with room at 16.7 ms, upTarget of it, which a Wii scene rarely leaves),
//     half-size effects and the cheaper background (GXV_AUTOQ_FXSTEP), the sizes down to 544x388, and 480x340 last. The emergency
//     rung is allowed: 60 is the point of the mode, and a step below today that buys no GPU time is undone as in target 30;
//   - the way back is the same (room at 16.7 ms for recoverHoldSec with GXV_AUTOQ_RECOVER).
// The pacing hint (port_autoq_picture_work_us) stays a 30 Hz feasibility cost in both targets (two turns): the 20/30 pacing of
// GXV_PACE reads it against 30 Hz limits. The bounded slip (GXV_SLIP) is a 30-floor tool: it engages only when a pair of turns
// does not fit two retraces (a game slower than 30 at full speed), where 60 is out of reach anyway, and it only paces turns, so
// the two never pull the same lever.
//
// Floor protection by degrees (Config::fxStep / Config::bgStep, GXV_AUTOQ_FXSTEP=1|2, target mode, default off): the things
// that mean least to a player go first, the picture size and the emergency rung last. The order of degrading is
//   0. the multisampled rungs (7, 8), then the sizes above the game's own 640x456 (6..3): the ladder above, as before,
//   1. at today's rung (2, 640x456), after fxHoldSec (2 s) of a GPU-limited miss: effects and blur at half size (the back
//      end's GXV_FXHALF / GXV_PASSHALF as a state this controller flips; Controller::FxStepOn()),
//   2. still GPU-limited, after fxHoldSec more: cheaper background (Controller::BgStepOn(); the back end samples the
//      textures of everything that is not a character or key item with the nearest mip level between the mip levels, not
//      the blend of two: half the texture fetches of a trilinear surface; only with GXV_AUTOQ_FXSTEP=2),
//   3. then the picture size below the game's own (rungs 1, 0: 592x422, 544x388), by the old rules,
//   4. last the 480x340 emergency rung.
// Alongside (not a controller step): GXV_CHARAA_AUTO=1 draws the characters' edge smoothing (GXV_CHARAA's filter) for Mario's and
// Luigi's own textures only, and only while the picture is below 640x456 (rungs 1, 0, emergency): the player stays crisp when the
// rest of the picture softens, at the cost of the filter only where the picture is already cheaper (vgx_backend_gl.cpp PresentCharaAA).
// GXV_CHARAA_AUTO=2 draws it at every rung whose picture is shown larger than it was drawn (all but 960x544) and is not multisampled.
// Each step is judged like a step below today: after checkSec the GPU's mean busy time must be betterFrac shorter, else it is
// switched off again (the next step is tried at once; this one is not tried again for blockSec, doubling), so a step that buys
// nothing in a scene costs two seconds, not a lasting loss.
// The way back is the reverse: 480x340 -> 544x388 -> 592x422 -> 640x456 by the old rules (the climb stops at today's rung while
// a step is on); then, at today's rung, the cheaper background goes off after fxOffHoldSec (8 s) of predicted room (the GPU's
// busy time times the measured saving of the step, or fxRatio / bgRatio until one was measured, within upTarget of the budget,
// and the other stages as well), then the half-size effects after another 8 s; only then may the picture size rise above
// today's. A step switched on again within a minute of being switched off blocks the next switching off for fxOffBlockSec
// (20 s), doubling. Changes are at least 3 s apart, as for rungs. A CPU-limited stretch never starts a step (the GPU must be
// the largest of the three stages by 5 % and near the budget: the rules for the emergency rung). A scene remembers the steps
// with its rung.
//
// Anisotropic filtering as the top of the ladder (Config::aniso, GXV_AUTOQ_ANISO, target mode, default off): the picture extra above the
// last rung. It is on only at the highest rung the platform allows (Config::maxRung: 960x544 with 4x multisampling where the EFB can
// have it, else the highest size), so the order of giving up is
//   1. anisotropic filtering (Controller::AnisoOn(): the back end sets vitaGL's anisotropic filter for trilinear textures),
//   2. the multisampled rungs, then the sizes above the game's own, ... (the ladder and steps above, as before).
// and the way back is the reverse: it comes on last, only at the top rung, after anisoHoldSec (8 s) of predicted room in a row (the
// GPU's busy time times anisoRatio, 1.15 until measured on a console, and the other stages, within upTarget of the budget; the cadence
// must hold). Under load it goes first: the GPU is the limit (the same test as for any step down) for downHoldSec (1.5 s) takes
// it off, before the rung is touched, and switching it on is blocked for blockSec (60 s, doubling to maxBlockSec) like a rung that
// overloaded. It costs no frozen picture (no new render targets), so it needs no quiet moment; changes are at least 3 s apart.
// Any change of rung, or a step below the top, has it off; a scene remembers it with its rung. Nothing else of the controller moves
// for it: with the option off every decision is the one made without it.
//
// Floor census (Controller::TakeFloor, target mode): for the basic log's "floor:" line, the pictures of a window, those later
// than the budget (+3 %), the time they took, and for each late picture which stage was the largest.
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
    int targetFps = 0;        // 30 / 60: whole-picture throughput target (Quality / Performance mode); 0: the old GPU-only controller
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
    // GXV_AUTOQ_RECOVER=1 (calm mode only; default off, and then every decision is the one made without these four):
    // the way back up after an overload is quicker and not held down by single frames.
    bool recover = false;
    double recoverHoldSec = 20.0;   // room in a row that steps up without a quiet moment (instead of upLongHoldSec)
    double blockForgetSec = 300.0;  // the blocks' doubling starts again after this long without a block
    double recoverFrameFrac = 1.0;  // target mode: one frame cancels a stretch of room only if its predicted work at the next rung
                                    // exceeds this share of the budget (instead of upTarget), or its cadence missed the budget;
                                    // the window means still have to fit upTarget
    // GXV_AUTOQ_FXSTEP (target mode only; default off, and then every decision is the one made without these): half-size effects
    // and blur before the emergency rung, back off first when room returns (see above).
    bool fxStep = false;         // step 1: effects and blur at half size
    bool bgStep = false;         // step 2: cheaper background (after step 1 or, when that bought nothing, on its own)
    double fxHoldSec = 2.0;      // GPU-limited at today's rung this long: a step goes on
    double fxOffHoldSec = 8.0;   // predicted room in a row before a step goes off
    double fxRatio = 1.3;        // GPU time without the step / with it, assumed until the step's own saving is measured
    double bgRatio = 1.1;
    double fxOffBlockSec = 20.0; // after a switch on within fxRepeatSec of a switch off, no switching off for this long (doubling)
    double fxRepeatSec = 60.0;
    // GXV_AUTOQ_ANISO (target mode only; default off, and then every decision is the one made without it): anisotropic filtering above the
    // top rung (see above).
    bool aniso = false;
    double anisoHoldSec = 8.0;   // predicted room in a row (at the top rung) before it goes on
    double anisoRatio = 1.15;    // GPU time with it / without it, assumed (not measured on a console yet)
    // A late picture (interval over budgetMs + 3 %) is blamed on a stage when that stage's work is at least this share of the budget
    double causeShare = 0.85;
    // Target mode: a step below today's size is kept only when the GPU's busy time fell by this share of the saving the sizes
    // predict (and by betterFrac at least); a step that bought nothing waits at most noGainUndoSec for a quiet moment to be undone.
    double gainShare = 0.5;
    double noGainUndoSec = 5.0;
    double belowCheckSec = 4.0;  // ... judged over this long (checkSec in the other modes): the GPU time of a water stage wanders
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

enum class Why : uint8_t { None, Headroom, Overload, Saturated, GpuBound, NoGain, NewScene, Forced, NoMemory, FxOn, FxOff, FxNoGain, AnisoOn, AnisoOff };
const char* WhyText(Why w);

struct Change {
    int from = kToday, to = kToday;
    Why why = Why::None;
    double load = 0, cpu = -1, intervalMs = 0; // the window that decided it
    uint64_t atUs = 0;
};

// The floor census of a window (target mode): see Controller::TakeFloor.
struct FloorWindow {
    uint64_t spanUs = 0;                   // time the pictures of the window took (without quiet moments)
    uint32_t pictures = 0;                 // pictures counted
    uint32_t late = 0;                     // ... later than the budget + 3 % (not counting stalls)
    uint64_t lateUs = 0;                   // the time those late pictures took: seconds below 30
    uint64_t worstUs = 0;                  // the longest late picture
    uint32_t gpu = 0, game = 0, renderer = 0, other = 0, unknown = 0; // late pictures by their largest stage
    uint32_t stalls = 0;                   // intervals over gapResetMs (a load, a shader build): not in `late`
    uint64_t stallUs = 0;
    uint32_t quiet = 0;                    // pictures of quiet moments (pause menu, fades): not counted
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
    // Floor protection by degrees: whether the back end should draw effects and blur passes at half size now (always false
    // without Config::fxStep), the last change of it, and how often it changed.
    bool FxStepOn() const { return (steps_ & 1) != 0; }
    bool BgStepOn() const { return (steps_ & 2) != 0; }
    unsigned Steps() const { return steps_; }
    const Change& LastFxChange() const { return lastFx_; }
    unsigned FxChanges() const { return fxChanges_; }
    // The floor census since the last call (target mode): returns it and starts a new window.
    FloorWindow TakeFloor();
    // Anisotropic filtering (Config::aniso): whether the back end should have it on now (only at the top rung), the last change of it,
    // and how often it changed.
    bool AnisoOn() const { return aniso_; }
    const Change& LastAnisoChange() const { return lastAniso_; }
    unsigned AnisoChanges() const { return anisoChanges_; }
    unsigned Changes() const { return changes_; }
    const Config& Cfg() const { return cfg_; }
    // When the block on `rung` ends (microseconds on the frames' clock; 0: not blocked): diagnostics and tests.
    uint64_t BlockedUntilUs(int rung) const { return blockUntil_[rung + 1]; }

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
    double checkT_ = 0;              // target mode: seconds the check has measured (checkSpan_ is a sum of GPU time there)
    unsigned checkFrames_ = 0;
    uint64_t undoDueAt_ = 0;         // target mode: an undo still waiting for a quiet moment is made in play from here
    int sceneFloor_ = kNoRung;       // target mode: in this scene a step below this rung bought nothing (kNoRung: none known)
    uint64_t sceneFloorUntil_ = 0;   // ... and, remembered from an earlier visit, no step below it is taken until then
    void UndoBelow(uint64_t now);
    // blocks
    uint64_t blockUntil_[kRungs + 1] = {};
    double blockLen_[kRungs + 1] = {};
    uint64_t belowBlockedUntil_ = 0;
    double belowBlockLen_ = 0;
    uint64_t lastBlockAt_ = 0; // GXV_AUTOQ_RECOVER: when a rung (or a step below today) was last blocked
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
    struct SceneRung { uint32_t key; int8_t rung; uint8_t fx; int8_t floor; };
    SceneRung sceneMem_[32] = {};
    unsigned sceneNext_ = 0;
    void Remember(uint32_t key, int rung, unsigned fx = 0, int floor = kNoRung);
    int Recall(uint32_t key, unsigned* fx = nullptr, int* floor = nullptr) const;  // kNoRung: unknown (the emergency rung is a valid remembered value)
    void RecallFloor(uint32_t key, uint64_t now);
    unsigned NextStep(uint64_t now) const;
    void SetSteps(unsigned to, Why why, uint64_t now);
    void SetSceneSteps(unsigned to, uint64_t now);
    bool StepsEnabled() const { return cfg_.fxStep || cfg_.bgStep; }
    bool Target() const { return cfg_.targetFps != 0; }                  // a whole-picture target (30 or 60)
    unsigned PictureTurns() const { return cfg_.targetFps == 60 ? 1u : 2u; } // 60 Hz game turns per picture of the target
    unsigned StepMask() const { return (cfg_.fxStep ? 1u : 0u) | (cfg_.bgStep ? 2u : 0u); }
    void ForgetFxBlocks();
    void SetAniso(bool to, Why why, uint64_t now);
    void SetSceneAniso(bool to, uint64_t now);
    unsigned Mem() const { return steps_ | (aniso_ ? 4u : 0u); } // what a scene remembers beside its rung (bit 2: anisotropic filtering)
    int FxRung() const { return cfg_.minRung > kToday ? cfg_.minRung : kToday; } // the rung the steps belong to (today's, or the lowest allowed)
    void FloorCount(uint64_t interval, bool quiet, bool measured, bool gpuKnown, bool gameKnown, bool backendKnown,
                    uint64_t gpu, uint64_t game, uint64_t backend);
public:
    unsigned QuietUps() const { return quietUps_; }
private:
    unsigned quietUps_ = 0;
    // floor protection by degrees (Config::fxStep)
    unsigned steps_ = 0;                    // bit 0 half-size effects and blur, bit 1 cheaper background
    Change lastFx_;
    unsigned fxChanges_ = 0;
    uint64_t lastFxAt_ = 0;
    bool fxEver_ = false;                   // a change was made: changes are spaced from lastFxAt_
    unsigned trial_ = 0;                    // the step (1, 2) whose trial is running; 0: none
    double fxBefore_ = 0, fxCheckT_ = 0, fxCheckSum_ = 0; // GPU ms before; trial time and sum of GPU ms x frames
    double stepGain_[2] = {0, 0};           // measured saving of each step (0: not measured)
    unsigned fxCheckFrames_ = 0;
    double fxRoomT_ = 0, fxBoundKept_ = 0;
    double boundGpuSum_ = 0;                // GPU ms x frames over the unbroken GPU-limited stretch
    unsigned boundGpuFrames_ = 0;
    uint64_t onBlockedUntil_[2] = {0, 0}, fxOffBlockedUntil_ = 0, lastFxOffAt_ = 0;
    double onBlockLen_[2] = {0, 0}, fxOffBlockLen_ = 0;
    FloorWindow floor_;
    // anisotropic filtering at the top rung (Config::aniso)
    bool aniso_ = false;
    Change lastAniso_;
    unsigned anisoChanges_ = 0;
    uint64_t anisoAt_ = 0;                  // when it last changed (0: never): changes are spaced from here
    uint64_t anisoBlockedUntil_ = 0;
    double anisoBlockLen_ = 0;
    double anisoRoomT_ = 0;                 // seconds of predicted room in a row for it
};

// The log of automatic quality: the Vita's port_log (the console's log file; the back end's stderr goes nowhere there),
// stderr elsewhere. One line, no newline needed.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// The same, into the basic log (the Vita's port_log_summary, which every build writes; stderr elsewhere): the "floor:" line.
void LogSummary(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int TargetFps();        // GXV_AUTOQ_TARGET (30 or 60; anything else 0), read once for both the controller and game CPU sampling
bool TargetEnabled();   // a whole-picture target (30 or 60): game CPU sampling on, complete GPU timings required
bool Target30Enabled(); // exactly the 30 target (Quality mode)

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
