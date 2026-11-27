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
//   1     592x424   1        0.86
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
// The floor first (the console, rc2, Beach Bowl: with that rule alone the stage stayed at 640x456 and showed 26 a second; each
// smaller size saves only 4-6 % of its GPU time there, and the old slide to 544x388 / 480x340 showed 31): while the floor is
// missed (the GPU-limited stretch before the step came slower than the target, or its GPU time alone was over the budget) a
// step is kept when it saved missGainFrac (3 %) of the GPU time, and the next one may follow; the stricter rule above applies
// to a step taken while the floor was met, which could only add pictures above the target. A step undone while the floor was
// missed is remembered with the scene only when it saved nothing (under nothingFrac, 1 %), so a saving hidden by one noisy check does not hold
// the scene above the size its floor needs on later visits. The same in both targets, each against its own budget.
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
// The Settings' Performance mode (settings_launch.cpp, entry.cpp) runs it as "the best picture at the best rate", and the console
// showed (1.2, 31 Oct) what a GPU-only reading of 60 does wrong: at the Comet Observatory the front end held the pictures at ~26 ms, the
// GPU at 20 ms looked like the limit against 16.7 ms, and the mode went down to 544x388 / 480x340 for fewer pictures than Quality. So:
//   - the front end is a stage (Config::frontStage, target 60 by default): the player thread stamps its CPU at each display copy
//     (port_autoq_front_picture), and the GPU must be the largest of four stages to count as the limit; the floor census names it;
//   - pictures, not GPU time: a series of steps below today's size is kept only when it shows pathRateFrac (3 %) more pictures; when the
//     GPU is under the budget again or the floor rung is reached without them, the whole series goes back (blocked, and remembered
//     with the scene for this visit and the next, maxBlockSec). No series starts when even the floor rung's GPU time (falling with the
//     pixels) and the other stages could not show pathRateFrac more pictures (throughput: the console's pipeline is not quantized to
//     whole retraces, a GPU at 24 ms a picture gives ~41 a second);
//   - below today's size a step up comes when another stage holds the pictures above the budget and the GPU at the next size stays
//     under freeClimbFrac (90 %) of it: a smaller picture that buys nothing is not kept;
//   - the present's edge smoothing (GXV_AUTOQ_EDGESTEP, Config::edgeStep) is the first step at today's rung, before the cheaper background
//     (GXV_AUTOQ_FXSTEP=3) and, for a severe overload only, half-size effects (GXV_AUTOQ_OVERLOAD=2), and the last one back; it costs
//     2.2 ms of GPU a picture (r61) whatever the size, needs no new render target and freezes nothing. Mario's own edge smoothing below
//     640x456 (GXV_CHARAA_AUTO) and the HUD are never touched;
//   - sizes run from the screen's 960x544 (rung 6, only with clear room at 16.7 ms and within GXV_AUTOQ_FREE of the picture interval) down
//     to 608x348 (rung 0 of the wide ladder, GXV_AUTOQ_MIN=0): no multisampling (GXV_AUTOQ_MSAA=0), no 536x304 emergency rung;
//   - a CPU-limited scene keeps its size, and when 60 is out of reach the pacing shows an even 30 (GXV_STEADY30, port/frame_pace.h
//     SteadyCadence; FrameTimes::cadence30): a window whose pictures came on that cadence is judged against 33.3 ms (DownBudgetMs), so a
//     GPU that fits 33.3 ms loses nothing for the CPU's limit; the steady 30 reads this controller's stage hint (port_autoq_stages: GPU,
//     the slowest per-picture stage, whether anything is left to lower) so that a scene the GPU limits is lowered first.
//   - the way back on the steady 30 (Config::cadenceUp, GXV_AUTOQ_CADENCE_UP=1|0, default on in the back end, off in Config). The console
//     (r98, Beach Bowl): the mode chased 60 down 720x404 -> 664x376 -> 608x348 (GPU 21-31 ms a picture), then the steady 30 engaged and
//     every later window was 150 pictures on it, none late for the GPU, the GPU at 24-28 ms: it stayed at 608x348 with the cheaper
//     background for the rest of the run, the same 30 a second as Quality (which holds 720x404 there) with a blurrier picture, because
//     every way back (the climb, a step's room to go off) is judged against 16.7 ms and upTarget, which a scene on the steady 30 never
//     meets. With the option a window mostly on the steady 30 (the test of DownBudgetMs) is also judged against 33.3 ms for the way back:
//     the cadence holds (the mean picture interval within 3 % of 33.3 ms), no picture of the window came late for the GPU on it, and the
//     GPU's predicted time at the next rung (busy x Cost(next) / Cost(now)) or without the step (busy x its ratio) and the other stages
//     within cadenceUpFrac (90 %) of 33.3 ms. Only when 60 is out of reach whatever the size: even the floor rung's predicted GPU time
//     (busy x Cost(minRung) / Cost(now)) or the other stages at 1000 / cadenceReachFps ms or more (22.2 ms: the steady 30 is released at an
//     estimate of 45 a second, frame_pace.h kSteady30AboveFps), so a scene that could reach 60 at a smaller size keeps its way down. Never
//     above today's rung (720x404 on the wide ladder): a size above it still needs clear room at 16.7 ms. The climb is the usual one (the
//     room in a row of calm mode and recovery, the blocks; Why::CadenceUp) and a trial for holdTrialSec (8 s): pictures late for the GPU on
//     the cadence (more than holdMissFrac of the trial's) or a GPU-limited window against 33.3 ms go back at once to the rung it came from
//     and block the rung tried (blockSec, doubling; Why::CadenceBack). Steps down on the cadence are as before (DownBudgetMs); anisotropic
//     filtering is not touched by it.
// Performance mode: fast60 (Config::fast60, GXV_AUTOQ_FAST60=1|0, default on in the back end and entry.cpp; target 60 only, Config::fast60 is
// forced off for any other target, so Quality and Locked 30 never run it). The console (r100): Good Egg zoom 2.0 held 720x404 at 48.6 a
// second, Honeyhive 41.8, Beach Bowl dropped to a steady 30, because a step below today's size waits for a quiet moment or 4-8 s of a
// GPU-limited stretch and then for a GPU-timed trial, and a spike took seconds to answer. With it, per picture (after the existing
// rejections: a movie, a quiet moment, a stall over gapResetMs, the settling after a change, a picture on the steady 30 are not looked at):
//   - a picture is "late for the GPU" as in the floor census (the budget + 3 %, the GPU the largest stage by causeShare and every other
//     measured stage within the budget + 3 %); a picture late for the game's CPU, the renderer or the front end never counts;
//   - fast60Late (8) of the last fast60Pics (12) pictures late for the GPU, those 12 within fast60SpanSec, and fast60GapSec since the
//     last change: one step down (about 0.2-0.4 s after the spike began). In this order: anisotropic filtering off (as the other rules),
//     the present's edge smoothing off (Config::edgeStep, with its GPU-timed trial, at today's rung), then ONE RUNG down. Above today's
//     rung the rung left is blocked as after an overload (blockSec, doubling to maxBlockSec: the hysteresis, no climb into a size that was
//     just too slow); at or below today's it joins the existing series of steps below (pathFrom_, checking_): the series is judged after
//     belowCheckSec by the GPU's time (any real saving, missGainFrac, because the floor is missed) and by the pictures (pathRateFrac), and
//     is undone as a whole, blocked and remembered with the scene when it buys nothing. The rung left is blocked for climbs as well.
//   - it goes on one rung at a time while the pictures stay late for the GPU (each step needs 12 fresh pictures after the change), down to
//     the floor rung, even when the predicted gain of a single step is small (the GPU's fixed per-scene cost makes each step a few percent);
//   - not when even the floor rung cannot hold ~fast60ReachFps: the floor rung's predicted GPU time (falling with the pixels) or the other
//     stages over 18.2 ms, or the existing "cannot gain" test: that is the steady 30's case, left to it (GXV_STEADY30) as before;
//   - the way back is the existing one (room at 16.7 ms for recoverHoldSec with the rung's block run out); nothing here steps up.
// The pacing hint (port_autoq_picture_work_us) stays a 30 Hz feasibility cost in both targets (two turns): the 20/30 pacing of
// GXV_PACE reads it against 30 Hz limits. The bounded slip (GXV_SLIP) is a 30-floor tool: it engages only when a pair of turns
// does not fit two retraces (a game slower than 30 at full speed), where 60 is out of reach anyway, and it only paces turns, so
// the two never pull the same lever.
//
// Floor protection by degrees (Config::fxStep / Config::bgStep, GXV_AUTOQ_FXSTEP=1|2, target mode, default off): the things
// that mean least to a player go first, the picture size and the emergency rung last. The order of degrading is
//   0. the multisampled rungs (7, 8), then the sizes above the game's own 640x456 (6..3): the ladder above, as before,
//   1. at today's rung (2, 640x456), after fxHoldSec (2 s) of a GPU-limited miss: cheaper background (Controller::BgStepOn();
//      only with GXV_AUTOQ_FXSTEP=2). Eligible depth-tested perspective draws without a recognized character/key-item texture
//      sample the nearest mip level, not the blend of two; orthographic/unknown projections and render targets keep theirs,
//   2. still GPU-limited, after fxHoldSec more: effects and blur at half size (the back end's GXV_FXHALF / GXV_PASSHALF as a
//      state this controller flips; Controller::FxStepOn()). GXV_AUTOQ_FXSTEP=1 still tries only this step,
//   3. then the picture size below the game's own (rungs 1, 0: 592x424, 544x388), by the old rules,
//   4. last the 480x340 emergency rung.
// Alongside (not a controller step): GXV_CHARAA_AUTO=1 draws the characters' edge smoothing (GXV_CHARAA's filter) for Mario's and
// Luigi's own textures only, and only while the picture is below 640x456 (rungs 1, 0, emergency): the player stays crisp when the
// rest of the picture softens, at the cost of the filter only where the picture is already cheaper (vgx_backend_gl.cpp PresentCharaAA).
// GXV_CHARAA_AUTO=2 draws it at every rung whose picture is shown larger than it was drawn (all but 960x544) and is not multisampled.
// Each step has a GPU-timed trial after checkSec. No-gain trials roll back and block retries for blockSec, doubling;
// a much heavier scene is treated as inconclusive and can retain the step. This is not a whole-pipeline CPU/cadence verdict.
// The way back is the reverse: 480x340 -> 544x388 -> 592x424 -> 640x456 by the old rules (the climb stops at today's rung while
// a step is on); then, at today's rung, half-size effects go off after fxOffHoldSec (8 s) of predicted room (the GPU's
// busy time times the measured saving of the step, or fxRatio / bgRatio until one was measured, within upTarget of the budget,
// and the other stages as well), then the cheaper background after another 8 s; only then may the picture size rise above
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
// Play-time quality (four options, target mode, each default off in the code; with all four off every decision is the one made
// without them; the Vita's entry.cpp turns them on). The console (r27, 4 Nov) showed the ladder's two failure modes in play:
// (a) a step up to 880x520 taken in a fade that closed the screen before a drop-in, then a 3.7 s load inside the same stage (so the
// scene memory kept the rung) and the drop-in's first seconds at 11-12 FPS before the overload stepped down; (b) after that, 592x422
// for the remaining 2.5 minutes at 55 FPS, because the front end's own late pictures (5-9 every 5 s, none of them the GPU's) ended
// every stretch of room before recoverHoldSec. And at 47 FPS a GPU allowed 70 % of the 33 ms budget (23 ms) is the limit itself.
//   free     (Config::freeFrac, GXV_AUTOQ_FREE=<percent>): quality costs GPU time only, never pictures. A step up (a rung, or anisotropic
//            filtering) must also leave the GPU's predicted busy time within freeFrac of the window's mean picture interval (the pace
//            the CPU stages set), not only within upTarget of the budget; above today's rung, a GPU busy freeDownFrac (95 %) of the
//            interval or more (the GPU is now what sets the pace) takes anisotropic filtering off after downHoldSec and the rung one
//            step down after freeDownSec (6 s), the rung left blocked as after an overload (Why::FreeDown).
//   wipe     (Config::wipe, GXV_AUTOQ_WIPE=1): a fade or wipe closing the screen (FrameTimes::wipe, not the pause menu) or a load (a gap
//            over longGapMs, also within the same stage) leads into an area nobody has measured: it never steps up there, the room
//            measured before it is forgotten, and a rung above today's goes back to today's while the screen is closed (Why::Wipe;
//            no block). A new scene starts at most at today's rung. The pause menu (the same area afterwards) still steps up.
//   lateRoom (Config::lateRoom, GXV_AUTOQ_LATEROOM=1): a single late picture ends a stretch of room only when its predicted work at the
//            next rung does not fit (the GPU's, the game's or the renderer's), not merely because it came late: the front end's late
//            pictures say nothing about the GPU. The window means must still keep the cadence (budget + 3 %).
//   anisoRung (Config::anisoRung, GXV_AUTOQ_ANISO_RUNG=<rung>): anisotropic filtering may come on at this rung and above (default: the
//            top rung only, as described above), by the same rules (room for anisoHoldSec, first given up, any change of rung has it
//            off, a scene remembers it). It costs no frozen picture, so it is the cheapest visible gain at the rungs normal play holds.
//
// Severe overload in play (Config::overload, GXV_AUTOQ_OVERLOAD=1|2, target mode, default off; on in the Vita's entry.cpp). The user's
// run on r41 (4 Nov 22:15, a drop-in at zoom 1.0): 8.9-9.8 FPS for 15 s and 20-37 FPS for a minute at 640x456, "late by GPU" in most
// pictures, and the controller logged nothing. Why: every picture over outlierMs (100 ms) is taken for a one-off hitch and left out of
// the windows, so at 9 FPS no window ever filled (and at 20-30 FPS the windows held only the lighter pictures); and the game's CPU
// estimate (two turns, 28 ms and more in those pictures: the GPU's memory traffic slows the CPU too) failed "the GPU is the largest
// stage by 5 %", so no window counted as GPU-limited either. With the option:
//   - a picture over outlierMs whose GPU busy time is at least 75 % of its interval is the GPU's work, not a hitch: it is measured;
//   - a window whose GPU time alone is over 110 % of the budget is GPU-limited whatever the game's estimate (the GPU alone misses it);
//   - per picture, not per window: pictures in a row whose GPU busy time each reaches the budget, for overloadSec (1 s) with a mean of
//     overloadFrac (125 %) of the budget or more (the 30 budget, 33.3 ms, in either target), take everything at once that the GPU
//     time predicts is needed to fit 90 % of it:
//     anisotropic filtering off, the cheaper background (if GXV_AUTOQ_FXSTEP has it), half-size effects (if FXSTEP has them, or
//     with =2 for such an overload only) and the rung as far down as the prediction says (to the emergency rung at most), in ONE
//     change (Why::Severe). A step below today is judged as any other (kept when it saved GPU time; undone otherwise). Another
//     second of it goes further. At least 1 s and 6 pictures between two such steps. The way back is the usual one.
//
// pace60 (Config::p60, GXV_PACE60=1, target 60 only, default off): 60 where the GPU can reach it, an even 30 at once where it cannot. The console
// (2.3 c5, Honeyhive, Performance): 720x404 on the steady 30 with the GPU at TA 12.7 ms (vertex work, the same at any size), 3D 18.9 ms (pixel
// work), busy 22.2 ms (the two overlap); the rules above judge "can a smaller size reach 60" by busy x Cost(floor) / Cost(now) (all of it
// falls with the pixels: 16.0 ms at 608x348) or p23Scale (60 %), and neither knows the quality steps, so 60 looked reachable where it is not
// and the controller chased it down the sizes (Beach Bowl r98: 720 -> 608 for nothing, then the steady 30 anyway). With the option:
//   model   per picture, from the scene timers (vgx_backend_gl.cpp gpuscenes, which GXV_PACE60 turns on): TA, the EFB and pass scenes' 3D, the
//           display scene's 3D (capped at its own minimum + 50 % + 0.5 ms, so a wait for the display is not cost) and busy (the timers' sum less
//           that wait), smoothed (1/24 a picture). The prediction at another size and steps: 3D(efb) x ((1 - p60PixShare) + p60PixShare x
//           Cost(to)/Cost(now)), x p60BgFrac / p60FxFrac for those steps (and + p60FxTaMs of TA for half-size effects); the display 3D with the
//           edge smoothing on / off as measured both ways (else +- p60EdgeMs, which the back end sets to the upscaler's cost when GXV_UPSCALE is
//           on); busy = max(TA, 3D) + k x min(TA, 3D), k the overlap measured now ((busy - max) / min). Kept through changes of size and
//           step (the smoothed values are moved to the new state by the same model; the first pictures after a change are not taken).
//   verdict "60 reachable" = the floor rung (minRung, 608x348) with every step the configuration has (edge smoothing off, cheaper background,
//           half-size effects if GXV_AUTOQ_FXSTEP has them) predicted within p60FitIn (100 %) of 16.7 ms AND the other stages (one game turn,
//           the renderer, the front end) as well; it is lost over p60FitOut (105 %: 17.5 ms, 57 a second, the steady 30's release with
//           GXV_PACE23's thresholds, which perf.cpp also takes under GXV_PACE60: engage below 50, release at 57); either change must hold
//           p60HoldSec (2 s). Until the model is filled the rules above decide as before.
//   uses    out of reach: the fast step does nothing (no edge step either); off the steady 30 a GPU-limited stretch takes no step and no smaller
//           size (Config::p60NoChase; the back end sets it only when the steady 30 is on); the stage hint tells the steady 30 nothing can be
//           lowered (it engages at today's size) and holds its release (the pipeline estimate is put just under the release rate); the
//           climb on the steady 30 (cadenceUp) counts it as "60 out of reach". On the steady 30 a GPU over 33.3 ms still steps down (to hold 30).
//           Reachable: the fast step's "can the floor reach 55" and its direct target, cannotGain, and the steady 30's release use the model's
//           prediction; the way back of a step (edge smoothing on again) needs room for the step's modelled cost too (with the upscaler the edge
//           step is ~3 ms, not edgeRatio's 12 %).
// Floor census (Controller::TakeFloor, target mode): for the basic log's "floor:" line, the pictures of a window, those later
// than the budget (+3 %), the time they took, and for each late picture which stage was the largest.
//
// Quality mode judged by the pictures (Config::hold30, GXV_AUTOQ_HOLD30=1, default on in the back end for target 30 under the fixed 30
// cap with a floor below 960x544, i.e. the Settings' Quality; off in Config, so every other configuration decides as before). The
// console (r82, 6 Nov): the lit Comet Observatory pinned at 960x544 showed 147-151 of 150 pictures as 2-vblank holds (a steady 30) with
// the GPU busy 32.5 ms against 33.3; unpinned, one window read the GPU at ~102 % and the overload rule took it to 784x440 for good
// (Bowser 3 960 -> 784, Sea Slide 960 -> 720 likewise). Under the fixed cap the GPU timer also counts waits as busy time (r57: "122 %" in a
// CPU-paced scene), so the GPU's busy share is no evidence of a miss. With the option, at target 30:
//   - evidence is gathered in hold windows of holdWindowSec (2 s, ~60 pictures): the share of pictures on time (not later than the budget
//     + 3 %, the floor census's test) and the share late for the GPU: the floor census blames the GPU (the largest stage, at least
//     causeShare of the budget) AND every other measured stage (two game turns, the renderer, the front end) fitted the budget + 3 %, so
//     a smaller picture could have brought that picture on time. A picture late for the game's CPU never counts against the size.
//   - down: only after holdMissWindows (2) hold windows in a row each with more than holdMissFrac (3 %) of its pictures late for the GPU,
//     one rung at a time (the rung left is blocked as after an overload). A window with >= 97 % on time can never count as a miss, whatever
//     the GPU's busy share reads. The same evidence stands in for the GPU-limited test everywhere a step down reads it (anisotropic
//     filtering off, the quality steps at today's rung, sizes below it); the "GPU sets the pace" rule (GXV_AUTOQ_FREE) does not step down,
//     and a severe overload counts only pictures that were late for the GPU.
//   - up: below the start rung, after holdClimbWindows (10, 20 s) hold windows in a row with no picture late for the GPU, at least
//     holdOnTimeFrac (97 %) on time and the GPU's mean busy time within holdClimbGpuFrac (95 %) of the budget, a trial one rung up (the
//     quality steps at today's rung go off first the same way). Every climb under the option is a trial for holdTrialSec (8 s): ONE miss
//     window in it goes back to the rung it came from and blocks the rung tried (blockSec, doubling to maxBlockSec), so a failed climb is
//     retried after 60, 120, 240, 480 s at most; the room-based climbs of the other rules stay as they were.
//   - the present's edge smoothing first (with Config::edgeStep, GXV_AUTOQ_EDGESTEP=1, which Settings now sets in Quality too): at 960x544
//     its pass costs ~3.4-3.7 ms of GPU a picture against ~0.9 ms for a plain blit (gpu960, r82), of ~32.5 ms in the lit hub. Under the
//     option a GPU miss (the same holdMissWindows windows in a row) at ANY rung first turns it off at that rung (after anisotropic
//     filtering, before any step or size), and only further misses with it off move the size. Its saving is measured: the GPU's mean
//     over the miss windows before, against the first full hold window after. It comes back on only after holdClimbWindows calm windows
//     AND when the GPU's mean now plus that saving fits holdClimbGpuFrac of the budget, and only at the start rung or where no climb is
//     possible (the way back is the reverse of the way down: size first, edge smoothing after). Back on is a trial for holdTrialSec: ONE
//     miss window turns it off again and blocks the next try for blockSec, doubling to maxBlockSec. The state is the controller's own
//     (not a bit of Steps(): it survives changes of rung, so a climb back to 960x544 arrives with it off); a new scene starts with it on.
//     Without the option the edge step stays one of the steps at today's rung, as in Performance mode.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "../../include/port/hot_knob.h"
#include "../../include/port/frame_pace.h"  // (kSteady30AboveFps: the steady 30's release, Config::cadenceReachFps)

namespace vgx::autoq {
// seam68: the back end answers whether every piece of a picture size (EFB target, render textures, the 60 Assist pair path's targets) is made ahead, so that the change to it
// makes nothing in play. A ladder size step is taken only to such a rung. No hook set (host tests, the desktop): every rung is ready.
using RungReadyFn = bool (*)(int rung);
void SetRungReadyHook(RungReadyFn fn);
bool RungReadyNow(int rung);


struct Rung {
    uint16_t w, h;   // render size for a 640x456 EFB
    uint8_t samples; // the EFB's samples a pixel (1: no multisampling)
};
inline constexpr Rung kLadder[] = {{544, 388, 1}, {592, 424, 1}, {640, 456, 1}, {720, 480, 1}, {800, 504, 1},
                                   {880, 520, 1}, {960, 544, 1}, {960, 544, 2}, {960, 544, 4}};
inline constexpr int kRungs = static_cast<int>(sizeof kLadder / sizeof kLadder[0]);
inline constexpr int kToday = 2;      // 640x456: the size of every build before automatic quality
inline constexpr int kTopSingle = 6;  // 960x544, the highest rung without multisampling
inline constexpr int kBaseW = 640, kBaseH = 456;
inline constexpr int kEmergency = -1, kNoRung = -2;
inline constexpr Rung kEmergencySize{480, 340, 1};
// GXV_WIDE_RUNGS=1: the rungs below 960x544 in the screen's shape (about 16:9) for the same number of pixels, instead of the
// GameCube EFB's narrower shape that the screen then stretches sideways (640x456 -> 720x404): more detail across, fewer lines
// down. The GPU cost follows the pixels (and, here, mostly the draws), so it is about the same. Every size the back end derives
// already scales x and y separately (the top rung is 1.5 x 1.19 of the EFB), so nothing else changes. Same IDs, same samples.
inline constexpr Rung kWideLadder[] = {{608, 348, 1}, {664, 376, 1}, {720, 404, 1}, {784, 440, 1}, {848, 480, 1},
                                       {896, 512, 1}, {960, 544, 1}, {960, 544, 2}, {960, 544, 4}};
inline constexpr Rung kWideEmergencySize{536, 304, 1};
// aa25, GXV_ANAMORPHIC=1 (with GXV_WIDE_RUNGS=1; default 0: the wide ladder above): rungs 4 and 5 keep all 544 lines and give up width
// only, at the same pixel count as the rungs they replace (848x480 = 407k -> 752x544 = 409k; 896x512 = 459k -> 848x544 = 461k), so the
// controller's cost model and every decision built on it stay as they are. The picture is enlarged along x alone (a one-dimensional
// filter: the upscaler's 3-read form, GXV_UPSCALE); every vertical detail (text rows, horizon, thin horizontal edges) stays native.
// The EFB -> render target mapping is already non-uniform on every wide rung (960/640 across, 544/456 down), so the game's projection,
// copies, captures, HUD and pointer need nothing: the present maps the picture back to 960x544 and circles stay round.
inline constexpr Rung kAnaLadder[] = {{608, 348, 1}, {664, 376, 1}, {720, 404, 1}, {784, 440, 1}, {752, 544, 1},
                                      {848, 544, 1}, {960, 544, 1}, {960, 544, 2}, {960, 544, 4}};
static_assert(sizeof kWideLadder == sizeof kLadder, "the wide ladder has the same rungs");
// The image effects work in a corner of the EFB (a half or a quarter of it: 320x228, 160x114) and draw the saved corner back; the
// corner must end on whole render pixels. At 592x422 the quarter ended at row 105.5 and at 608x346 at row 86.5, a pixel's centre,
// where the copy that saves it and the draws over it can round to different rows (a line under Beach Bowl's star counter; fix
// from Producdevity's fork, b89e421). So every size is a multiple of 4 both ways: 592x424, 608x348.
constexpr bool CornersOnPixels() {
    for (const Rung& r : kLadder)
        if (r.w % 4 != 0 || r.h % 4 != 0) return false;
    for (const Rung& r : kWideLadder)
        if (r.w % 4 != 0 || r.h % 4 != 0) return false;
    for (const Rung& r : kAnaLadder)
        if (r.w % 4 != 0 || r.h % 4 != 0) return false;
    return kEmergencySize.w % 4 == 0 && kEmergencySize.h % 4 == 0 && kWideEmergencySize.w % 4 == 0 && kWideEmergencySize.h % 4 == 0;
}
static_assert(CornersOnPixels(), "every render size keeps the EFB's quarter on whole pixels");
inline bool WideRungs() {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h): every rung lookup of the back end and the controller
    return PORT_HOT_KNOB(bool, [] { const char* v = std::getenv("GXV_WIDE_RUNGS"); return v && std::atoi(v) == 1; }());
}
inline bool AnamorphicRungs() { // (aa25, GXV_ANAMORPHIC=1: rungs 4 and 5 full height; only with the wide rungs)
    return PORT_HOT_KNOB(bool, [] { const char* v = std::getenv("GXV_ANAMORPHIC"); return v && std::atoi(v) == 1; }());
}
// Keep the public 0..8 rung IDs unchanged; the extra floor exists only in target mode.
inline const Rung& RungAt(int rung) {
    if (WideRungs()) return rung == kEmergency ? kWideEmergencySize : (AnamorphicRungs() ? kAnaLadder[rung] : kWideLadder[rung]);
    return rung == kEmergency ? kEmergencySize : kLadder[rung];
}
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
    bool fxStep = false;         // bit 1: effects and blur at half size (the only step in mode 1)
    bool bgStep = false;         // bit 2: cheaper background, tried before bit 1 in mode 2
    bool glowStep = false;       // halfglow38 (GXV_PERF_HALFGLOW): the Performance ladder may take the half-rate glow + shadow texture step (the back end must also say it is allowed)
    // GXV_AUTOQ_EDGESTEP=1 (target mode only; default off, and then every decision is the one made without it): the whole-picture edge
    // smoothing of the present (GXV_EDGE_AA, the FXAA-style pass; ~2.2 ms of GPU time a picture with GXV_PRESENT_FAST) as the first
    // step at today's rung, before the cheaper background and half-size effects, and the last one back (see "Performance mode").
    bool edgeStep = false;       // bit 4: the present without edge smoothing
    double fxHoldSec = 2.0;      // GPU-limited at today's rung this long: a step goes on
    double fxOffHoldSec = 8.0;   // predicted room in a row before a step goes off
    double fxRatio = 1.3;        // GPU time without the step / with it, assumed until the step's own saving is measured
    double bgRatio = 1.1;
    double edgeRatio = 1.12;     // (edge smoothing off: the display scene 4.5 -> 2.3 ms of about 18 ms, r61; assumed until measured)
    double fxOffBlockSec = 20.0; // after a switch on within fxRepeatSec of a switch off, no switching off for this long (doubling)
    double fxRepeatSec = 60.0;
    // GXV_AUTOQ_ANISO (target mode only; default off, and then every decision is the one made without it): anisotropic filtering above the
    // top rung (see above).
    bool aniso = false;
    double anisoHoldSec = 8.0;   // predicted room in a row (at the top rung) before it goes on
    double anisoRatio = 1.15;    // GPU time with it / without it, assumed (not measured on a console yet)
    // Play-time quality (see above; all off by default, and then every decision is the one made without them)
    double freeFrac = 0.0;       // GXV_AUTOQ_FREE: > 0, a step up keeps the predicted GPU busy time within this share of the picture interval
    double freeDownFrac = 0.95;  // ... and above today's rung a GPU busy this share of the interval or more ...
    double freeDownSec = 6.0;    // ... for this long steps one rung down (anisotropic filtering goes after downHoldSec)
    bool newStage = false;       // GXV_AUTOQ_NEWSTAGE: a stage first named after its load's gap, with no remembered rung, starts at the start rung
    bool wipe = false;           // GXV_AUTOQ_WIPE: fades, wipes and loads lead into an unmeasured area (no step up, back to today's rung)
    bool lateRoom = false;       // GXV_AUTOQ_LATEROOM: a late picture whose work fits does not end a stretch of room
    int anisoRung = kNoRung;     // GXV_AUTOQ_ANISO_RUNG: the lowest rung anisotropic filtering may be on at (kNoRung: maxRung only)
    // Severe overload in play (see above; off by default, and then every decision is the one made without it)
    bool overload = false;       // GXV_AUTOQ_OVERLOAD=1
    bool overloadFx = false;     // GXV_AUTOQ_OVERLOAD=2: half-size effects for such an overload even when fxStep is off
    double overloadSec = 1.0;    // pictures in a row each at the budget or over, this long ...
    double overloadFrac = 1.25;  // ... with a mean GPU busy time this share of the budget or more
    double overloadFit = 0.9;    // the share of the budget the prediction aims the GPU time at
    // A late picture (interval over budgetMs + 3 %) is blamed on a stage when that stage's work is at least this share of the budget
    double causeShare = 0.85;
    // Target mode: a step below today's size is kept only when the GPU's busy time fell by this share of the saving the sizes
    // predict (and by betterFrac at least); a step that bought nothing waits at most noGainUndoSec for a quiet moment to be undone.
    double gainShare = 0.5;
    double missGainFrac = 0.03;  // ... but while the floor is missed (pictures slower than the target), any saving of this much is kept
    // The front end's CPU per picture (FrameTimes::frontRunUs) as a stage of the whole-picture work in the decisions (GXV_AUTOQ_FRONT):
    // -1 (the default) in target 60 only, 1 in either target, 0 never. The floor census blames late pictures on it whenever it is measured.
    int frontStage = -1;
    double pathRateFrac = 0.03;  // target 60: a series of steps below must show this many more pictures, or it is undone as a whole
    double freeClimbFrac = 0.9;  // below today, a step up while another stage holds the pictures and the GPU stays under this share of it
    double nothingFrac = 0.01;   // ... and a step undone then is remembered with the scene only when it saved less than this (nothing)
    double noGainUndoSec = 5.0;
    double belowCheckSec = 4.0;  // ... judged over this long (checkSec in the other modes): the GPU time of a water stage wanders
    // Quality mode judged by the pictures (GXV_AUTOQ_HOLD30, target 30 only; off here, so every other configuration decides as before)
    bool hold30 = false;
    double holdWindowSec = 2.0;     // a hold window: the pictures of at least this long of play
    double holdMissFrac = 0.03;     // a window misses when more than this share of its pictures were late for the GPU
    int holdMissWindows = 2;        // misses in a row that step one rung down (one within a climb's trial)
    double holdOnTimeFrac = 0.97;   // a calm window: at least this share on time, none late for the GPU ...
    double holdClimbGpuFrac = 0.95; // ... and the GPU's mean busy time within this share of the budget
    int holdClimbWindows = 10;      // calm windows in a row (20 s, as GXV_AUTOQ_RECOVER_SEC) before a climb trial below the start rung
    double holdTrialSec = 8.0;      // after any climb: one miss window in this long goes back and blocks the rung tried
    // The way back on Performance mode's steady 30 (GXV_AUTOQ_CADENCE_UP, target 60 only; off here, so every other configuration decides as
    // before; see "Performance mode")
    bool cadenceUp = false;
    double cadenceUpFrac = 0.90;    // the predicted GPU time and the other stages within this share of 33.3 ms
    double cadenceReachFps = port::pace::kSteady30AboveFps; // 60 out of reach: the floor rung's GPU or the other stages miss this rate
    // GXV_STEADY30_GPUSCALE=<percent> (default 100 = as before): the share of the GPU's busy time that is taken to fall with the pixels when the
    // steady 30's release estimate (port_autoq_stages) is made with the GPU at the smallest size the controller may take. 100 scales all of
    // it (busy x Cost(min) / Cost(now)). The console says much of the picture's GPU time is fixed per picture, not per pixel (Honeyhive
    // r107, 720x404 -> 664x376: 25.3/22.3/21.4 ms -> 23.4/22.5 ms against the 14 % the pixels predict), so the estimate released the
    // cadence for 54 pictures a second at a GPU of 22 ms and the pictures then came at 37-45 with the GPU late (p107hh10 18:36:11-18:36:23).
    double cadenceGpuScale = 1.0;
    // s1117b (Strikers 1117b), GXV_AUTOQ_GPUGATE=1|0 (default ON, a bug fix): no step down is taken for a picture rate the GPU does not hold.
    // Strikers' controller stepped live play down on a "GPU mean" that was really the back end's CPU-starved wait (26.9 ms reported against
    // ~16 ms of real GPU time a lane); Galaxy's 12 Nov lesson 7 (a stage's time inflated by a backlog) is the same trap. The gate is an AND on
    // every rule that lowers anything for the GPU's sake (the windows' over / bound, the free-down, the severe overload, fast60, the table's
    // steps): over the last gateWindow pictures (the GPU timers' own busy time of the pictures that carry one against those pictures'
    // periods: the interval, at most the budget, so a GPU a little over the budget whose pictures then take two retraces still reads over 100 %)
    // the GPU's busy share must be gateBusy (85 %) or more, or gateBusyLate (70 %) or more with gateLateFrac (8 %) of the pictures
    // late; or the last gateRecent (12) pictures alone say so (a GPU that has just become the limit is not held back by the mean). Less than gateMinPics measured pictures, or no GPU timers (the fence clock cannot tell the wait from work): the gate is open, i.e.
    // today's rules. It only ever removes step downs, so it can never lower the picture more than the rules without it.
    bool gpuGate = true;
    int gateWindow = 90;            // pictures looked at (at most GpuGate::kMax)
    int gateRecent = 12;            // ... or the last this many pictures alone (at least gateMinPics of them measured) satisfy the same
    int gateMinPics = 6;            // measured pictures needed before the gate may close
    double gateBusy = 0.85;         // the GPU's share of the picture interval that is clearly the limit
    double gateBusyLate = 0.70;     // ... or this much with gateLateFrac of the pictures late
    double gateLateFrac = 0.08;
    // GXV_AUTOQ_FAST60=1|0 (target 60 only; off here, on in the back end): Performance mode steps down within about half a second of
    // sustained pictures late for the GPU, one size at a time, and keeps going while they stay late (see "Performance mode: fast60").
    bool fast60 = false;
    int fast60Pics = 12;            // the last this many pictures are looked at ...
    int fast60Late = 8;             // ... and this many of them late for the GPU (every other stage fitted) is a sustained miss
    double fast60SpanSec = 1.0;     // ... when they took at most this long (a stall is not a burst)
    double fast60GapSec = 0.75;     // changes at least this far apart (settleSec, then fast60Pics fresh pictures, are needed anyway)
    double fast60ReachFps = 55.0;   // no step when even the floor rung's predicted GPU time, or another stage, is over 1000 / this ms
    // GXV_AUTOQ_EDGECHECK=1|0 (target 60 with edgeStep only; off here, on in the back end): the edge step's GPU-timed trial is judged over
    // edgeCheckSec instead of checkSec and kept when the GPU fell by the smaller of half its assumed saving (1 - 1 / edgeRatio) and
    // edgeBetterFrac, instead of betterFrac (5 %): its ~12 % saving read as 5 % in a 2 s trial through the camera's noise (Good Egg,
    // console)
    bool edgeCheck = false;
    double edgeCheckSec = 4.0;
    double edgeBetterFrac = 0.03;
    // GXV_AUTOQ_SHED=1 (gpuheavy25; target 60 only; off here and by default, and then every decision is the one made without it; see "shed" in
    // vgx_autoq.cpp): the order in which a GPU-limited scene gives things up is a TABLE of levels (Controller::ShedBuild), cheapest to the picture first:
    //   level 0 full picture (edge pass on), level 1 edge pass off (and, at the native rung with shedDirectMs > 0, direct display), level 2 the
    //   half-rate captures (Config::shedCuts), then ONE RUNG per level down to today's rung. A burst of pictures late for the GPU (fast60's
    //   detector, or the severe overload) moves to the first level whose PREDICTED GPU time fits shedFit of the budget (a level that saves less than
    //   shedMinGainMs is passed over: stepping 960 -> 896 loses the direct display and gains about nothing); a level that was entered is judged by
    //   the GPU time it really bought (shedTrialSec) and undone and blocked if it bought less than shedGainShare of the prediction. The way back is
    //   the reverse, one level at a time or as far as the prediction allows, only after shedUpHoldSec of calm and when the GPU after it is predicted
    //   within shedUpFrac of the budget; a level re-entered is a trial for shedBackSec and a late burst in it goes back and blocks it (doubling).
    //   60 out of reach (the last level of the table predicted over 1000 / fast60ReachFps) gives up NOTHING: the steady 30 is the way, as before.
    //   A new kind of step (a multisampled or width-only rung such as 896x544, a geometry-only edge pass) is one more row of the table: its
    //   `cost` (pixels relative to 640x456), `edgeOff`, `cut` and an extra saving; nothing else changes.
    bool shed = false;
    bool shedCuts = false;         // the table has the half-rate captures level (the back end arms the captures with GXV_INDCAP_HALFRATE=2 / PETARI_*_HALFRATE=2)
    bool shedEdgeAtNative = true;  // the edge pass runs at the native rung (GXV_EDGE_AA_NATIVE=1); with 0 the native rung is already without it
    double shedFit = 0.99;         // a level is chosen when its predicted GPU time is within this share of the budget (16.5 ms at 60: on time is 16.68)
    double shedPixShare = 0.5;     // the share of the GPU's busy time (less the display's own terms) that falls with the pixels: busy holds the fixed TA (HH 3D fit 0.65, EFB 0.75, busy of a TA-heavy scene ~0.45)
    double shedEdgeMs = 3.0;       // the edge pass's GPU time over the plain present, until measured (HH 960x544: 4.58 -> 1.26 ms of the display scene)
    double shedDirectMs = 0.0;     // direct display's extra saving over the plain present at the native rung with no edge pass (0: not available)
    double shedCutMs = 1.0;        // the half-rate captures' saving until measured (gpu23: 3D 0.6-1.0, TA 0.25-0.35 at 960)
    double shedMinGainMs = 0.6;    // a level must be predicted to save this much to be taken
    double shedGainShare = 0.3;    // ... and must have bought this share of the prediction (measured over shedTrialSec) or it is undone
    double shedTrialSec = 4.0;
    double shedUpFrac = 0.90;      // the way back: the GPU predicted after it within this share of the budget ...
    double shedUpHoldSec = 10.0;   // ... for this long with every picture on time
    double shedBackSec = 8.0;      // a level re-entered is a trial for this long
    double shedBlockSec = 60.0;    // a level that failed is blocked this long, doubling to maxBlockSec
    double shedReach = 1.03;       // 60 is out of reach when even the table's last level is predicted over this share of the budget: nothing is given up
    bool shedBelow = false;        // GXV_AUTOQ_SHED_BELOW=1: the older rules may still step below today's rung (720x404) under shed; by default the table ends there
    double shedDwellSec = 3.0;     // minimum time between two changes of the table's level (the severe overload excepted)
    double shedCycleSec = 0.0;     // GXV_AUTOQ_SHED_CYCLE=<s> (stress): walk the table up and down, one level every <s> s, whatever the load
    // GXV_PACE23=1 (target 30 and 60; off here and by default; see the "pace23" notes in vgx_autoq.cpp): the GPU's load measured without the
    // waits for a CPU-paced picture, a late picture blamed on the GPU only when the GPU really held it, the game's CPU judged over the
    // whole picture span, no size change for p23SettleSec after a load, and Performance mode's size changes few and each one direct.
    bool pace23 = false;
    double p23GpuMiss = 0.95;    // a late picture is the GPU's only when the GPU's own busy time is this share of the budget or more (alone it misses it)
    double p23GameShare = 0.85;  // ... and not while the game thread's CPU over the picture's whole span is this share of the interval or more
    int p23ElimPictures = 40;    // the GPU timers silent for this many pictures in a row: only then may "late, nothing else measured" mean the GPU
    double p23SettleSec = 6.0;   // after a load, a stall or a new scene: no size or step change that the GPU's load decides (not a new scene's own rung)
    double p23GapSec = 4.0;      // size changes in Performance mode at least this far apart ...
    int p23MaxChanges = 3;       // ... and at most this many in p23WindowSec: the steady 30 is the way out after that
    double p23WindowSec = 30.0;
    double p23Scale = 0.6;       // the share of the GPU's time that falls with the pixel count (the rest is per picture: scenes, display pass)
    double p23Fit = 0.92;        // a direct step is the smallest size whose predicted GPU time is within this share of the budget
    // GXV_PACE60=1 (target 60 only; off here and by default; see "pace60" in vgx_autoq.cpp): the GPU's time per size and per quality step
    // modelled from the scene timers' split (vertex TA, the EFB/pass scenes' 3D, the display scene's 3D, their overlap), and ONE verdict, with
    // hysteresis, of whether 60 can be reached at the floor rung with every quality step on. The verdict gates the fast step, the slow steps
    // below today, the steady 30's stage hint (release and "lowerable") and the climb on the steady 30, so 60 is chased only where it can be
    // had and the even 30 is taken at once (at today's size) where it cannot.
    bool p60 = false;
    double p60PixShare = 0.7;    // the share of the EFB/pass scenes' 3D that falls with the pixel count (the rest is per scene / per tile)
    double p60FitIn = 1.0;       // 60 becomes reachable when the predicted floor (and the other stages) fit this share of the budget ...
    double p60FitOut = 1.05;     // ... and stops being reachable over this share (57 a second: the steady 30's release); each must hold p60HoldSec
    double p60HoldSec = 2.0;
    double p60EdgeMs = 2.2;      // the present's edge smoothing (or the upscaler in its place) over the plain present, until measured
    double p60BgFrac = 0.95;     // the EFB/pass 3D with the cheaper background / without it
    double p60FxFrac = 0.85;     // ... with half-size effects / without
    double p60FxTaMs = 2.0;      // ... and the TA half-size effects add (more scenes: 0.5-0.9 ms TA each on the console)
    int p60MinPics = 20;         // GPU pictures with a split before the model is used
    double p60StepSec = 1.0;     // reachable, off the steady 30, GPU-limited this long: the steps and size the model says, in one change
    double p60RepeatSec = 30.0;  // a "reachable" lost within this long: the next one must hold twice as long (p60HoldSec doubling) ...
    double p60MaxHoldSec = 64.0; // ... up to this; forgotten after p60ForgetSec without such a loss
    double p60ForgetSec = 120.0;
    bool p60NoChase = true;      // 60 out of reach: no step down off the steady 30 for pictures' sake (the back end: only with GXV_STEADY30)
    // GXV_AUTOQ_QGUARD=1 (quality23; target 60 only; off here, so every other configuration decides as before; see "qguard" in vgx_autoq.cpp):
    // the owner's rule "if giving up graphics doesn't help anything, we shouldn't be doing it". (a) No verdict and no step from pictures that
    // are not play (FrameTimes::loading / noStage, quiet moments, movies): after them, and after a load or a new scene, qgPlayWindows windows
    // and qgPlaySec of play first; pace60's verdict smoothed (qgNeedSec) and "reachable" held qgReachHoldSec at least. (b) Every reduction (a
    // smaller size, a quality step on, anisotropic filtering off) records the picture rate, the GPU and the other stages before it; after
    // qgTrialSec of play it is kept only when the pictures came qgRateFrac faster AND the GPU was the stage that limited them, else everything
    // it took is restored ("quality: ... restored: bought no speed") and that reduction blocked qgBlockSec (doubling). (c) On a 30 cadence (the
    // steady 30, or cap30) with 60 out of reach: room for qgRestoreSec restores, in one change, every size up to today's and every step that the
    // measured GPU time says fits cadenceUpFrac of 33.3 ms, sharpest first (size, half-size effects, background, edge smoothing, anisotropic).
    bool qg = false;
    bool cap30 = false;          // (with qg) target 60 under the fixed 30 cap (GXV_FPS_LIMIT=30): every picture is on a 30 cadence, 60 never reachable
    int qgPlayWindows = 8;
    double qgPlaySec = 3.0;
    double qgNeedSec = 2.0;
    double qgReachHoldSec = 6.0;
    double qgTrialSec = 4.0;
    double qgRateFrac = 0.03;
    double qgBlockSec = 20.0;
    // (gov77, GXV_AUTOQ_QGUARD_STAGEBLOCK) a step that bought nothing is blocked longer each time within a stage even when the GPU was the limit (20, 40, 80, ... to maxBlockSec): Dreadnought took edge
    // smoothing and the cheaper background off, put them back as no gain and took them again 20 s later, three times in a minute (console night57). Off: the old 20 s every time.
    bool qgStageBlock = false;
    double qgRestoreSec = 2.0;
    double qgGpuHoldFrac = 1.45; // off a 30 cadence: pictures this much slower than the GPU's own time (and the other stages') are not the GPU's to buy
    // GXV_SEAMLESS_CUT=1 (seamless23; off here, so every other configuration decides as before): the owner's "no visible quality step in play".
    // Every step that RAISES the quality the player sees (a bigger size: Headroom, CadenceUp, HoldClimb, the undo of a smaller size that bought
    // nothing, QGUARD's restores; a quality step off: FxOff; anisotropic filtering on; edge smoothing back under hold30) and FreeDown (a smaller
    // size that no late picture asks for) waits until a camera cut, a closed fade or the pause menu (FrameTimes::cut, or quiet) is at most
    // seamCutMemSec old, or seamWaitSec have passed since the first such step was held. Reductions that late pictures ask for (an overload, a
    // GPU-limited stretch, fast60, pace60, the trials' backs) are not held: the overload has already persisted for the rule's own hold time.
    bool seam = false;
    double seamWaitSec = 15.0;
    double seamCutMemSec = 0.35;
    // GXV_SEAM72=1 (default off): a size step DOWN taken by the ladder also waits for a quiet moment (a cut, a closed fade, the pause menu: the same notes as seam);
    // when none comes for seam72WaitSec the step is not taken (the size is kept; the wish is asked again) and one line says so.
    bool seam72 = false;
    double seam72WaitSec = 20.0;
    // The rules before calm mode (GXV_AUTOQ_CALM=0).
    static Config Legacy() {
        Config c;
        c.calm = false;
        c.gpuGate = false;   // (s1117b: Legacy is "the rules above unchanged")
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
// s1117b: the GPU gate's ring (Config::gpuGate). One entry per picture of play: its period (the picture interval, at most the budget), the GPU timers' busy time when the picture carried
// one, and whether it was late. Pure arithmetic, testable alone (tests/vgx_autoq_gpugate.cpp).
struct GpuGate {
    static constexpr unsigned kMax = 90;
    struct Pic { uint32_t periodUs = 0, busyUs = 0; bool known = false, late = false; };
    Pic ring[kMax];
    unsigned head = 0, n = 0;
    void Clear() { head = n = 0; }
    void Add(uint64_t periodUs, uint64_t busyUs, bool known, bool late) {
        Pic& p = ring[head];
        p.periodUs = static_cast<uint32_t>(periodUs > 0xFFFFFFFFu ? 0xFFFFFFFFu : periodUs);
        p.busyUs = static_cast<uint32_t>(busyUs > 0xFFFFFFFFu ? 0xFFFFFFFFu : busyUs);
        p.known = known;
        p.late = late;
        head = (head + 1) % kMax;
        if (n < kMax) ++n;
    }
    // The last `window` pictures: measured ones, the busy share of their periods, the share of all of them that was late.
    void Stats(unsigned window, unsigned& measured, double& busyShare, double& lateFrac) const {
        if (window > kMax) window = kMax;
        if (window > n) window = n;
        uint64_t busy = 0, span = 0;
        unsigned late = 0;
        measured = 0;
        for (unsigned i = 0; i < window; ++i) {
            const Pic& p = ring[(head + kMax - 1 - i) % kMax];
            late += p.late ? 1u : 0u;
            if (!p.known) continue;
            ++measured;
            busy += p.busyUs;
            span += p.periodUs;
        }
        busyShare = span ? static_cast<double>(busy) / static_cast<double>(span) : 0.0;
        lateFrac = window ? static_cast<double>(late) / window : 0.0;
    }
};

struct FrameTimes {
    uint64_t startUs = 0;      // the back end began executing the frame
    uint64_t presentUs = 0;    // its present (swap) returned: all of its GPU work has been submitted
    uint64_t gpuDoneUs = 0;    // frame fence clock: the GPU had finished all of it (0: not known)
    uint64_t backendRunUs = 0; // the back-end thread's CPU time so far (0: not known)
    uint32_t gameRunUs = 0, gameTurns = 0; // target mode: cumulative CPU time/turns, unsigned wrap is intentional
    uint32_t gameFaults = 0;   // cumulative failed game-clock samples; a later valid turn must not hide one
    bool gameKnown = false;
    uint32_t frontRunUs = 0, frontPictures = 0; // target mode: the front end's cumulative CPU time and pictures (wrap intentional)
    bool frontKnown = false;   // ... known (a front end on its own thread that stamps its pictures)
    bool cadence30 = false;    // target 60: the pacing's steady 30 (frame_pace.h SteadyCadence) shows the pictures every second turn
    bool paced = false;        // paced display callback: the late frame fence alone cannot measure GPU busy time
    bool timers = false;       // GPU timers clock: gpuBusyUs is used, gpuDoneUs is not
    bool busyKnown = false;    // ... a frame finished on the GPU since the previous present, and this is its busy time
    uint32_t gpuBusyUs = 0;
    uint32_t gpuRawUs = 0;     // GXV_PACE23: the timers' own busy time when gpuBusyUs is the corrected one (0: the same, or not known)
    uint32_t gpuFrames = 1;    // GXV_PACE23: GPU frames finished since the last present that carried a busy time (their count)
    bool quiet = false;        // calm mode: a quiet moment (pause menu, closed fade or wipe): not measured, a step up may be taken
    bool wipe = false;         // ... the quiet moment is a closed fade or wipe (not the pause menu): Config::wipe
    bool movie = false;        // calm mode: a movie is playing: not measured, and no change of size (forkB, from Producdevity's fork)
    uint32_t scene = 0;        // calm mode: the scene's key (0 = unknown)
    bool loading = false;      // Config::qg: not play: a stage's loading thread runs (the loading screen, a transition) or a menu layer (star select) drew
    bool noStage = false;      // Config::qg: not play: no stage yet (the title and its menus before the first stage)
    bool cut = false;          // Config::seam: a camera cut (or a quiet moment) at this picture: a visible step may be taken now
};

enum class Why : uint8_t { None, Headroom, Overload, Saturated, GpuBound, NoGain, NewScene, Forced, NoMemory, FxOn, FxOff, FxNoGain, AnisoOn, AnisoOff, Wipe, FreeDown, Severe,
                           HoldMiss, HoldClimb, HoldBack, HoldEdgeOff, HoldEdgeOn, HoldEdgeBack, CadenceUp, CadenceBack, FastDown, Pace60, QgNoGain, QgRestore, QgBack,
                           ShedEdgeOff, ShedEdgeOn, ShedCutOn, ShedCutOff, ShedDown, ShedUp, ShedBack, LadderDown, LadderUp };
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
    uint32_t front = 0;                    // ... the front end's CPU (when it is measured)
    uint32_t frontPics = 0;                // (fe26, GXV_FE_GOV) pictures of the window with a measured front-end time ...
    uint64_t frontSumUs = 0;               // ... and the sum of those times (the front end's CPU microseconds a picture, held when a present brought none)
    uint32_t cadence = 0;                  // pictures on Performance mode's steady 30 (judged against 33.3 ms, not 16.7)
    uint32_t stalls = 0;                   // intervals over gapResetMs (a load, a shader build): not in `late`
    uint64_t stallUs = 0;
    uint32_t quiet = 0;                    // pictures of quiet moments (pause menu, fades): not counted
    // (GXV_FLOOR_SPAN, census only, no decision reads these) The late pictures counted in `other` ("none of these"), told apart: the
    // pictures that spanned two or more game turns (the skip rule left turns out, so the picture interval is several turns long and
    // the one-turn game stage cannot reach its share of the budget), and of those or any, the ones whose game thread CPU over ALL the
    // turns since the last picture was gameShare (0.85) of the interval or more: the game thread itself was the limit.
    uint32_t p23GameSpan = 0, p23CpuPaced = 0; // (GXV_PACE23) late pictures blamed on the game's span CPU; late pictures the GPU's busy time reached the share on but did not hold
    uint32_t otherMulti = 0, otherGameSpan = 0;
    uint64_t otherGameSpanUs = 0, otherSpanUs = 0; // ... the CPU and the interval of those, for the log's mean share
};

class Controller {
public:
    struct ShedRowView { double cost; bool cut; double extraMs; };
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
    double WindowFrontMs() const { return winFrontMs_; } // the front end's CPU per picture (-1: not measured)
    unsigned Windows() const { return windows_; }
    // Floor protection by degrees: whether the back end should draw effects and blur passes at half size now (always false
    // without Config::fxStep), the last change of it, and how often it changed.
    bool FxStepOn() const { return (steps_ & 1) != 0; }
    bool BgStepOn() const { return (steps_ & 2) != 0; }
    bool EdgeStepOn() const { return (steps_ & 4) != 0 || holdEdge_; } // (Config::edgeStep: the present without edge smoothing now)
    unsigned Steps() const { return steps_; }
    const Change& LastFxChange() const { return lastFx_; }
    unsigned FxChanges() const { return fxChanges_; }
    // GXV_PACE23: what the rules did (diagnostics, log lines and tests), counted since the start.
    struct Pace23Stats {
        uint32_t noElim = 0;        // late pictures NOT blamed on the GPU by elimination (timers silent or the other stages not cleared)
        uint32_t lowUtil = 0;       // late pictures not blamed on the GPU: its busy time alone was under p23GpuMiss of the budget (it waited for a CPU-paced picture)
        uint32_t gameSpan = 0;      // late pictures not blamed on the GPU: the game thread's span CPU was p23GameShare of the interval or more
        uint32_t settleHeld = 0;    // pictures judged inside a settle window (a load, a stall, a new scene)
        uint32_t rateHeld = 0;      // size changes the gap or the rate limit refused
        uint32_t direct = 0;        // direct steps taken (more than one rung at once)
        double util = 0;            // the smoothed GPU share of the picture interval now (-1 not known)
    };
    const Pace23Stats& Pace23() const { return p23_; }
    // GXV_PACE60: one finished GPU picture's split from the scene timers (us): vertex (TA) of all its scenes, pixel (3D) of the EFB and pass
    // scenes, 3D of the display scene, and the timers' busy time (the sum of the scenes' spans). Called by the back end in order, any time.
    void AddSplit(uint32_t taUs, uint32_t efbUs, uint32_t dispUs, uint32_t busyUs);
    struct Pace60Stats {
        bool ready = false;         // the model has been filled once
        bool reach = true;          // the verdict: 60 can be reached at the floor rung with every step on
        double taMs = 0, efbMs = 0, dispMs = 0, busyMs = 0, overlap = 0; // the model now (display 3D without its wait; busy likewise)
        double floorMs = 0;         // the predicted GPU busy time at the floor rung with every step on
        double otherMs = -1;        // the other stages of the last window
        double edgeMs = -1;         // the display scene's measured cost of the edge smoothing (-1: not measured both ways)
        double shareMeasured = -1;  // the share of the EFB/pass 3D that fell with the pixels at the last measured change of size (-1: none)
        int shareFrom = 0, shareTo = 0;
        uint32_t verdicts = 0;      // verdict changes
        uint32_t pictures = 0;      // pictures with a split, in all
        uint32_t heldSteps = 0;     // windows in which a GPU-limited stretch was not acted on (60 out of reach: the steady 30's case)
        uint32_t direct = 0;        // direct changes to the size and steps the model says (P60Step)
        double reachHoldSec = 0;    // what a "reachable" verdict must hold now (p60HoldSec, doubling after a quick loss)
    };
    const Pace60Stats& Pace60() const { return p60_; }
    // Config::qg: what the guard did (diagnostics, log lines and tests), counted since the start
    struct QualityStats {
        uint32_t nonPlay = 0;       // pictures not play (loading, no stage, menu layer, quiet, movie)
        uint32_t unsettles = 0;     // times the play had to settle again (a load, a new scene, a stretch of non-play pictures)
        uint32_t trials = 0;        // reductions judged
        uint32_t kept = 0;          // ... kept (faster pictures, the GPU the limit)
        uint32_t reverts = 0;       // ... restored: bought no speed
        uint32_t restores = 0;      // restores on the 30 cadence with 60 out of reach
        uint32_t restoreBacks = 0;  // ... taken back (pictures late for the GPU on the cadence)
    };
    const QualityStats& Quality() const { return qgStats_; }
    // GXV_SEAMLESS_CUT (Config::seam): quality-up steps held for a cut, taken at one, and taken when the wait ran out.
    struct SeamStats { uint32_t held = 0, atCut = 0, timedOut = 0, cuts = 0; };
    const SeamStats& Seam() const { return seamStats_; }
    struct Seam72Stats { uint32_t quiet = 0, kept = 0, asked = 0; };
    const Seam72Stats& Seam72() const { return seam72Stats_; }
    bool SeamHoldDown(uint64_t now);
    bool SeamWaiting() const { return cfg_.seam && seamSince_ != 0; } // (a quality-up step is held for a cut now: the back end makes the rung above ahead)
    bool QualitySettled() const { return QgSettled(); }
    // GXV_PACE60's prediction of the GPU's busy time (ms) at `rung` with the quality steps `steps` (Steps() bits; 4 = edge smoothing off)
    double Pace60Predict(int rung, unsigned steps) const;
    unsigned Pace60AllSteps() const; // every step the configuration has
    bool Pace23Settling(uint64_t now) const { return cfg_.pace23 && now < p23SettleUntil_; }
    // The floor census since the last call (target mode): returns it and starts a new window.
    FloorWindow TakeFloor();
    // Anisotropic filtering (Config::aniso): whether the back end should have it on now (only at the top rung), the last change of it,
    // and how often it changed.
    bool AnisoOn() const { return aniso_; }
    const Change& LastAnisoChange() const { return lastAniso_; }
    unsigned AnisoChanges() const { return anisoChanges_; }
    // The GPU's busy time a picture over the windows of checkSec after anisotropic filtering last came on, against the window before
    // (diagnostics for Config::anisoRatio, which is assumed): returns true once per measurement.
    bool TakeAnisoMeasure(double& beforeMs, double& afterMs);
    // Play-time quality: the lowest rung anisotropic filtering may be on at.
    int AnisoMinRung() const { return cfg_.anisoRung == kNoRung ? cfg_.maxRung : std::max(cfg_.anisoRung, cfg_.minRung < kToday ? kToday : cfg_.minRung); }
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
    uint32_t prevFrontRun_ = 0, prevFrontPictures_ = 0;
    bool prevFrontKnown_ = false;
    uint64_t frontRun_ = 0, frontPictures_ = 0; // window: the front end's CPU and pictures
    uint32_t lastFrontUs_ = 0;                  // its last known CPU per picture (0: not known)
    double winFrontMs_ = -1;
    unsigned cadenceFrames_ = 0;                // window: pictures shown on the steady 30 (FrameTimes::cadence30)
    int pathFrom_ = kNoRung;                    // target 60: the rung a series of steps below began at (kNoRung: none)
    double pathInterval_ = 0;                   // ... and the mean picture interval before it (ms)
    double belowInterval_ = 0;                  // the same for a step below waiting for a quiet moment
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
    double boundIntervalSum_ = 0;    // target mode: the frame intervals of the GPU-limited stretch (us), to tell a missed floor
    unsigned boundFrames_ = 0;
    uint64_t settleUntil_ = 0;
    unsigned sinceChange_ = 1u << 30; // frames since the last change (minChangeFrames)
    unsigned sinceLoad_ = 1u << 30;   // frames since the last load's gap (Config::newStage)
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
    bool belowMissing_ = false, checkMissing_ = false; // target mode: the floor was missed before the step below today
    uint64_t roomAt_ = 0;            // when that window ended (0: no room seen since the last change or overload)
    uint32_t scene_ = 0;             // the key of the scene being played
    struct SceneRung { uint32_t key; int8_t rung; uint8_t fx; int8_t floor; };
    SceneRung sceneMem_[32] = {};
    unsigned sceneNext_ = 0;
    void Remember(uint32_t key, int rung, unsigned fx = 0, int floor = kNoRung);
    int Recall(uint32_t key, unsigned* fx = nullptr, int* floor = nullptr) const;  // kNoRung: unknown (the emergency rung is a valid remembered value)
    void RecallFloor(uint32_t key, uint64_t now);
    unsigned NextStep(uint64_t now) const;
    unsigned RecoveryStep() const;
    void SetSteps(unsigned to, Why why, uint64_t now);
    void SetSceneSteps(unsigned to, uint64_t now);
    bool StepsEnabled() const { return cfg_.fxStep || cfg_.bgStep || cfg_.edgeStep || cfg_.overloadFx; }
    bool Target() const { return cfg_.targetFps != 0; }                  // a whole-picture target (30 or 60)
    bool FrontStage() const { return Target() && (cfg_.frontStage > 0 || (cfg_.frontStage < 0 && cfg_.targetFps == 60)); }
    unsigned PictureTurns() const { return cfg_.targetFps == 60 ? 1u : 2u; } // 60 Hz game turns per picture of the target
    unsigned StepMask() const { return (cfg_.fxStep ? 1u : 0u) | (cfg_.bgStep ? 2u : 0u) | (cfg_.edgeStep ? 4u : 0u); }
    static int StepIdx(unsigned bit) { return bit == 1u ? 0 : bit == 2u ? 1 : 2; } // (the step's slot in the arrays below)
    double StepRatio(int idx) const { return idx == 0 ? cfg_.fxRatio : idx == 1 ? cfg_.bgRatio : cfg_.edgeRatio; }
    // (target 60: the budget a window is judged against for a lower picture: 33.3 ms while the pacing shows a steady 30)
    double DownBudgetMs() const;
    // (target 60: most pictures of the window were on the steady 30, FrameTimes::cadence30)
    bool CadenceWindow() const { return cfg_.targetFps == 60 && frames_ && cadenceFrames_ * 2 > frames_; }
    // Config::cadenceUp: the stretch of room includes a window whose step up only the steady 30's 33.3 ms allowed; a climb so made is on
    // trial
    bool roomCadence_ = false;
    bool picCadLateGpu_ = false;                // the picture handed to Add last: on the steady 30, late against 33.3 ms for the GPU
    unsigned winCadLateGpu_ = 0;                // decision window: such pictures
    int cadTrialFrom_ = kNoRung;                // a climb on the cadence on trial: the rung it came from ...
    uint64_t cadTrialUntil_ = 0;                // ... until then
    unsigned cadTrialPics_ = 0, cadTrialLate_ = 0; // ... its pictures on the cadence and those late for the GPU
    bool cadTrialMiss_ = false;                 // ... it missed: back at the next change allowed
    bool Climb(int to, uint64_t now);           // a step up the room allowed (Why::Headroom, or Why::CadenceUp with its trial); false: held (Config::seam)
    void ForgetFxBlocks();
    void SetAniso(bool to, Why why, uint64_t now);
    void SetSceneAniso(bool to, uint64_t now);
    static constexpr unsigned kAnisoMem = 8u;
    unsigned Mem() const { return steps_ | (aniso_ ? kAnisoMem : 0u); } // what a scene remembers beside its rung (bit 3: anisotropic filtering)
    // (gpu896) the Performance ladder: levels {rung, steps}, see LadderStep in vgx_autoq.cpp
    struct LadLevel { int rung; unsigned steps; bool glow = false; };   // glow: GXV_PERF_HALFGLOW's step (half-rate glow + shadow texture), a flag beside the steps' bits
    bool LadderOn() const;
    int LadderTable(LadLevel* t) const;
    double LadderCost(const LadLevel& l) const;
    bool LadderEntryBlocked(const LadLevel* t, int m, uint64_t now) const;
    void LadderBlockEntry(const LadLevel* t, int m, uint64_t now);
    bool LadderStep(uint64_t now, double dt, bool mayChange, bool over, bool known, double gpuMs);
    bool LadderRoomDown() const;   // (gov5)
    struct LadChk { bool on = false; unsigned bits = 0; int fromLevel = 0; double before = 0, beforeIv = 0, pred = 0, t = 0, gpu = 0, iv = 0; unsigned frames = 0; char what[24] = ""; };
    LadChk ladChk_;
    double ladEmaGpu_ = 0, ladEmaIv_ = 0;
    double winOtherMs_ = -1;   // (seam49: the longest of the game thread, back end and front end in the last window: what a smaller picture does not shorten)
    double ladOtherLogAt_ = 0;
    unsigned ladEmaN_ = 0, ladRefused_ = 0, ladUndone_ = 0, ladMemNext_ = 0;
    uint32_t ladMemScene_[16] = {};
    unsigned ladMemMask_[16] = {};
    // seam68: what the stage's earlier size trials really bought, as a share of what was predicted (0 = no trial yet), kept per scene for the session beside the refused mask:
    // the next prediction for the stage is scaled by it, so a stage whose GPU time did not fall with the pixels (Beach Bowl: 848x480 bought 0.5 ms of a predicted 2.2) is not tried again.
    float ladMemRatio_[16] = {};
    float ladSceneRatio_ = 1.0f;        // (the current scene's)
    double ladMinMs_ = 1.0;             // seam68: a size step's predicted saving must be at least this many ms (GXV_AUTOQ_LADDER_MINMS)
    double ladSizeOver_ = 1.10;         // seam68: a size step only while the GPU time is this far over the budget (GXV_AUTOQ_LADDER_SIZE_OVER; 0 = off)
    bool ladRefuseAllSizes_ = true;     // seam68: a size trial that was undone refuses every size step of the stage, not only the rung it tried (GXV_AUTOQ_LADDER_REFUSE_ALL)
    double ladSeamLogAt_ = 0;
    double ladMinGain_ = 0.05, ladChkSec_ = 1.0, ladKeepFrac_ = 0.6;
    double LadderStepF(unsigned steps) const;
    double LadderStepF(const LadLevel& l) const { return LadderStepF(l.steps) * (l.glow ? 1.0 - ladGlowFrac_ : 1.0); }
    bool glow_ = false, glowAllowed_ = false;   // (halfglow38) the step is on / may be offered (set by the back end: GXV_PERF_HALFGLOW, no HRW pairs, neither knob already on)
    double ladGlowFrac_ = 0.06, glowBlockLen_ = 0;   // the saving the step is expected to give (a share of the GPU time; Beach Bowl measured 8 %), the block's length
    uint64_t glowBlockedUntil_ = 0;
    void SetGlow(bool on, Why why, uint64_t now);
    double LadderPredict(const LadLevel& a, const LadLevel& b, double gpuMs) const;
    unsigned LadderEntryBit(const LadLevel* t, int m) const;
    uint32_t ladScene_ = ~0u;
    int ladTrial_ = -1, ladTrialFrom_ = 0;
    uint64_t ladTrialUntil_ = 0;
    double ladDownT_ = 0, ladRoomT_ = 0;
    unsigned ladDowns_ = 0, ladUps_ = 0;
    double ladDownHold_ = 0.6, ladUpHold_ = 2.0, ladUpMargin_ = 0.85, ladTrialSec_ = 4.0;   // (justified in LadderStep's note)
    int FxRung() const { const int b = presetActive_ && preset_.ladder ? preset_.maxRung : cfg_.minRung; return b > kToday ? b : kToday; } // the rung the steps belong to (today's, or the lowest allowed)
    // (returns the late picture's stage: 0 GPU, 1 game, 2 renderer, 3 front end, 4 none of these, 5 not measured; -1 on time or not counted)
    // (gameSpanUs / spanTurns: the game thread's CPU over every turn since the last picture and their number, 0 when not known; they only
    // feed FloorWindow::otherMulti / otherGameSpan, GXV_FLOOR_SPAN, and never change the returned stage)
    int FloorCount(uint64_t interval, bool quiet, bool measured, bool gpuKnown, bool gameKnown, bool backendKnown,
                   uint64_t gpu, uint64_t game, uint64_t backend, bool frontKnown = false, uint64_t front = 0, bool cadence = false,
                   uint64_t gameSpanUs = 0, uint32_t spanTurns = 0);
public:
    unsigned QuietUps() const { return quietUps_; }
    unsigned LadderDowns() const { return ladDowns_; }   // (gpu896)
    unsigned LadderUps() const { return ladUps_; }
    bool LadderActive() const { return LadderOn(); }
    // halfglow38 (GXV_PERF_HALFGLOW): the Performance ladder's step "half-rate glow + shadow texture" (PETARI_BLOOM_HALFRATE=1 + PETARI_SHADOWTEX_HALFRATE=1, switched live by the back end), after the cheap
    // picture steps and BEFORE the first resolution step. HalfGlowOn: the step is taken. SetHalfGlowAllowed: the back end says whether it may be offered at all (switch on, not under 60 Assist pairs
    // (GXV_HRW_CAPFIX refuses every half rate there), neither knob already on). Off by default: Config::glowStep.
    bool HalfGlowOn() const { return glow_; }
    void SetHalfGlowAllowed(bool on) { glowAllowed_ = on; }
    void LadderGlowFrac(double f) { ladGlowFrac_ = f; }
    unsigned LadderUndone() const { return ladUndone_; }
    unsigned LadderRefused() const { return ladRefused_; }
    void LadderTune(double downHold, double upHold, double upMargin, double trialSec) { ladDownHold_ = downHold; ladUpHold_ = upHold; ladUpMargin_ = upMargin; ladTrialSec_ = trialSec; }
    // Quality mode judged by the pictures (Config::hold30): the last closed hold window (diagnostics, tests): its pictures, those late,
    // those late for the GPU (the size could have helped), and the hold windows in a row that missed / were calm.
    struct HoldWindow { uint32_t pictures = 0, late = 0, lateGpu = 0; double gpuMs = 0; bool miss = false, calm = false; };
    // GXV_AUTOQ_SHED (Config::shed): the table's level now (-1: not on the table: shed off, or a rung/step the table does not own), whether the
    // half-rate captures are engaged, the levels' count and what the table holds (tests, log)
    bool ShedOn() const { return cfg_.shed && cfg_.targetFps == 60; }
    int ShedLevelNow() const { return ShedFind(); }
    bool ShedCutOn() const { return shedCut_; }
    unsigned ShedChanges() const { return shedChanges_; }
    unsigned ShedHitches() const { return shedHitches_; }
    unsigned ShedTransitions() const { return shedTxCount_; }
    const char* ShedRowName(int i) const { return i >= 0 && i < ShedLevels() ? shedTab_[static_cast<size_t>(i)].name : ""; }
    int ShedLevels() const { return static_cast<int>(shedTab_.size()); }
    double ShedPredictMs(double gpuMs, int fromLevel, int toLevel) const { return ShedPredict(gpuMs, fromLevel, toLevel); }
    // ---- stagepreset25 (GXV_STAGE_PRESETS; platform/vgx/src/vgx_stagepreset.h, agents/stagepreset25/REPORT.txt): a stage's preset, applied by the back end
    // when the game starts loading the stage (the screen is closed). It only sets the controller's frame: the rungs it may use, the one a new scene starts
    // at (the stage's own, never the rung remembered from a visit and never the wipe cap, because the preset knows the stage), what the shed ladder does,
    // and the effect steps engaged from the first picture. The rung itself changes where the controller already changes it for a new scene (the load's gap,
    // the scene key), so a visible picture is never resized. With no preset (or after ClearStagePreset) every rule is the one it was.
    struct StagePreset {
        int start = kNoRung, minRung = kNoRung, maxRung = kNoRung; // kNoRung: the global default (the configuration's)
        bool fixedSize = false;  // min = max = start: no automatic change of size in this stage
        bool noSizeUp = false;   // size may go down in play (a boss), the ceiling then comes down to that rung: never up again until the stage ends
        bool edgeOff = false;    // the present's edge pass is off from the start (the shed ladder's first level, or the edge step)
        bool cuts = false;       // the half-rate captures are engaged from the start (needs Config::shedCuts)
        bool twoSize = true;     // only the floor and the ceiling are sizes: no rung between (the owner's two sizes); the steps between are jumps
        bool ladder = false;     // (gpu896, 21 Nov) the Performance ladder (Controller::LadderStep): cheaper picture, 896x512, 848x480, edge smoothing off, GPU-limited only, two ways
        int fx = 0;              // the effect steps: 0 as the controller decides, 1 down only (no way back up), 2 hold (no step either way: the preset is the whole answer)
    };
    void ApplyStagePreset(const StagePreset& p);
    void ClearStagePreset() { ApplyStagePreset(StagePreset()); }
    // GXV_SMOOTH60 (policy26): the per-stage controller moves the picture to `to` now (after ApplyStagePreset gave a frame that holds only that rung). True when the
    // rung changed; the back end then takes Current() as the wanted rung, as for any other change (Why::Forced is urgent: no wait for a pre-built size).
    bool ForceRung(int to, uint64_t now) {
        const int before = rung_;
        SetRung(to, Why::Forced, now);
        return rung_ != before;
    }
    bool StagePresetActive() const { return presetActive_; }
    bool StagePresetTwoSize() const { return presetActive_ && preset_.twoSize; }
    const StagePreset& CurrentStagePreset() const { return preset_; }
    int StagePresetMinRung() const { return cfg_.minRung; }
    int StagePresetMaxRung() const { return cfg_.maxRung; }
    const HoldWindow& LastHold() const { return lastHold_; }
    unsigned HoldWindows() const { return holdWindows_; }
    int HoldMissRun() const { return missRun_; }
    int HoldCalmRun() const { return calmRun_; }
    bool HoldTrial(uint64_t now) const { return holdTrialFrom_ != kNoRung && now < holdTrialUntil_; }
    // (Config::hold30 with edgeStep: the present's edge smoothing off by the rule above; its measured GPU saving, ms a picture, -1 unknown)
    bool HoldEdgeOff() const { return holdEdge_; }
    double HoldEdgeSavingMs() const { return edgeSave_; }
private:
    // ---- GXV_AUTOQ_SHED state (see Config::shed). The table: one row per level, from the full picture down.
    struct ShedRow { int rung; bool edgeOff; bool cut; double cost; double extraMs; const char* name; };
    std::vector<ShedRow> shedTab_;
    bool shedCut_ = false;                  // the half-rate captures are engaged now (the table's cut rows)
    unsigned shedChanges_ = 0;
    double shedCalmT_ = 0;                  // seconds of calm in a row (every picture on time, the next level up predicted to fit)
    int shedTrialFrom_ = kNoRung;           // a level entered on the way UP: the level index it came from (kNoRung: none)
    uint64_t shedTrialUntil_ = 0;
    int shedJudge_ = -1;                    // a level entered on the way DOWN is being judged: the level it came from (-1 none) ...
    double shedJudgeBefore_ = 0, shedJudgePred_ = 0, shedJudgeT_ = 0, shedJudgeSum_ = 0;
    uint32_t shedJudgeN_ = 0;
    int shedJudgeTo_ = -1;
    uint64_t shedBlockedUntil_[16] = {};    // per level: not entered before then (after a failed trial)
    double shedBlockLen_[16] = {};
    double shedEdgeSave_ = -1;              // measured edge saving (ms, -1 unknown)
    double shedCutSave_ = -1;
    // the transition log: the pictures before, at and after every change of level (GXV_AUTOQ_SHED)
    double shedRing_[8] = {};
    unsigned shedRingN_ = 0, shedRingHead_ = 0;
    struct ShedTx { bool active = false; int from = 0, to = 0; double before = 0, median = 0, v[4] = {}; unsigned n = 0; const char* what = ""; };
    ShedTx shedTx_;
    unsigned shedTxCount_ = 0, shedHitches_ = 0;
    uint64_t shedLastAt_ = 0, shedCycleNext_ = 0;
    int shedCycleDir_ = 1;
    void ShedNotePicture(double intervalMs);
    bool ShedCycleStep(uint64_t now);
    void ShedBlock(int level, uint64_t now);
    void ShedBuild();
    int ShedFind() const;
    double ShedPredict(double gpuMs, int from, int to) const;
    double ShedPredictState(double gpuMs, int to) const; // from the state as it is now (which may be off the table's path)
    double ShedEdgeMs() const { return shedEdgeSave_ >= 0.0 ? shedEdgeSave_ : cfg_.shedEdgeMs; }
    double ShedCutMs() const { return shedCutSave_ >= 0.0 ? shedCutSave_ : cfg_.shedCutMs; }
    bool ShedEdgeActive(const ShedRow& r) const { return !r.edgeOff && (r.rung != kTopSingle || cfg_.shedEdgeAtNative); }
    bool ShedDirect(const ShedRow& r) const { return cfg_.shedDirectMs > 0.0 && r.rung == kTopSingle && !ShedEdgeActive(r); }
    int ShedDownStep(uint64_t now, double gpuMs, double otherMs, bool severe); // 0 nothing (decided elsewhere), 1 rung changed, 2 other level, 3 handled: out of reach
    bool ShedUpStep(uint64_t now, double gpuMs, double otherMs, double intervalMs, double dt);
    void ShedEnter(int to, Why why, uint64_t now, double gpuMs, double predicted, bool judge);
    void ShedJudgeAdd(uint64_t now, double gpuMs, double dt, uint32_t frames);
    bool ShedOwnsRung() const { return ShedOn() && ShedFind() >= 0; }
    bool Hold30() const { return cfg_.hold30 && cfg_.targetFps == 30; }
    // the picture handed to Add last (Config::hold30): measured, late, late for the GPU (every other stage fitted)
    bool picLate_ = false, picLateGpu_ = false;
    uint32_t winPics_ = 0, winLate_ = 0, winLateGpu_ = 0;      // decision window (ResetWindow)
    double holdT_ = 0, holdGpuSum_ = 0;                         // hold window (ResetHolds)
    uint32_t holdPics_ = 0, holdLate_ = 0, holdLateGpu_ = 0, holdGpuN_ = 0;
    int missRun_ = 0, calmRun_ = 0;                             // hold windows in a row (ResetHolds)
    HoldWindow lastHold_;
    unsigned holdWindows_ = 0;
    int holdTrialFrom_ = kNoRung;                               // a climb on trial: the rung it came from ...
    uint64_t holdTrialUntil_ = 0;                               // ... until then
    void HoldAccumulate(double dt, double gpuMs);
    // the present's edge smoothing under Config::hold30 (see above)
    bool holdEdge_ = false;                                     // off now
    double missGpuSum_ = 0;                                     // GPU ms x pictures over the current run of miss windows (ResetHolds)
    uint32_t missGpuN_ = 0;
    double edgeBefore_ = 0;                                     // the GPU's mean over the miss windows before it went off
    bool edgeMeasuring_ = false;                                // the first full hold window after it went off measures the saving
    double edgeSave_ = -1;                                      // measured saving (ms; -1: not measured)
    uint64_t edgeTrialUntil_ = 0;                               // back on: a trial until then
    uint64_t edgeBlockedUntil_ = 0;                             // back on not before then (after a failed trial)
    double edgeBlockLen_ = 0;
    bool HoldEdgeRule() const { return Hold30() && cfg_.edgeStep; }
    void SetHoldEdge(bool off, Why why, uint64_t now);
    unsigned quietUps_ = 0;
    // GXV_PACE23 state
    bool P23() const { return cfg_.pace23 && Target(); }
    Pace23Stats p23_;
    uint64_t p23SettleUntil_ = 0;               // no change the GPU's load decides before this (a load, a stall, a new scene)
    uint64_t p23Chg_[8] = {};                   // the times of the last size changes (ring), p23ChgN_ of them counted
    unsigned p23ChgHead_ = 0, p23ChgN_ = 0;
    double p23PendSpan_ = 0;                    // picture intervals since the last picture that carried a busy time (us)
    double p23UtilBusy_ = 0, p23UtilSpan_ = 0;  // smoothed: the GPU's busy time and the picture intervals (us), 7/8 per picture
    unsigned p23BusyAge_ = 1u << 30;            // pictures since a busy time was known
    uint32_t p23LastBusyUs_ = 0;                // ... and that busy time
    bool P23ChangeAllowed(uint64_t now, bool count, double gapScale = 1.0);
    void P23NoteChange(uint64_t now);
    double P23Pred(double gpuMs, int from, int to) const; // the GPU's time at size `to` from its time at `from`: p23Scale of it falls with the pixels
    // GXV_PACE60 state: the model's smoothed split, the state it describes (rung, steps), the verdict
    bool P60() const { return cfg_.p60 && cfg_.targetFps == 60; }
    bool P60Ready() const { return P60() && p60_.ready; }
    Pace60Stats p60_;
    double mTa_ = 0, mEfb_ = 0, mDisp_ = 0, mBusy_ = 0;  // ms, smoothed
    double mDispOn_ = -1, mDispOff_ = -1;               // the display scene's 3D with edge smoothing on / off (smoothed; -1 unknown)
    unsigned mN_ = 0, mSkip_ = 0;                        // samples since the last reset; samples still to skip after a change
    int mRung_ = kNoRung;                                // the state the model describes
    unsigned mSteps_ = 0;
    uint32_t mDispRing_[64] = {};                        // the display scene's 3D of the last pictures: its cost without a wait
    unsigned mDispHead_ = 0, mDispN_ = 0;
    bool mGap_ = false;                                  // a stall / load since the last sample: the next one starts the model afresh
    double reachT_ = 0;                                  // seconds the opposite verdict has held
    double shareEfbBefore_ = -1;                         // the EFB 3D before the last change of size (the measured share)
    int shareFrom_ = kNoRung;
    unsigned StepsNow() const { return steps_ | (holdEdge_ ? 4u : 0u); }
    bool mFirst_ = true;                                 // the first verdict is taken at once (the middle of the two thresholds)
    double shareSum_ = 0;                                // the EFB 3D summed since that change (raw), over shareN_ pictures
    unsigned shareN_ = 0;
    void P60Parts(int rung, unsigned steps, double& ta, double& efb, double& disp) const; // the model's parts moved to (rung, steps)
    double P60Busy(double ta, double efb, double disp) const;                              // ... and their busy time (the overlap now)
    double P60Floor() const { return Pace60Predict(cfg_.minRung, Pace60AllSteps()); }
    void P60Verdict(double dt, double otherMs);
    bool P60Holds() const { return P60Ready() && !p60_.reach; } // 60 out of reach by the model
    double p60OverT_ = 0;                                // seconds GPU-limited off the steady 30 while reachable
    uint64_t p60ReachAt_ = 0, p60LossAt_ = 0;            // when the verdict last became reachable / was lost soon after
    int P60Step(uint64_t now, double gpuMs);            // the size and steps the model says fit, in one change (0 none, 1 the rung, 2 steps only)
    // Config::qg state ("qguard" in vgx_autoq.cpp)
    bool QG() const { return cfg_.qg && cfg_.targetFps == 60; }
    bool QgSettled() const { return !QG() || (qgPlayWins_ >= static_cast<unsigned>(cfg_.qgPlayWindows) && qgPlayT_ >= cfg_.qgPlaySec); }
    void QgUnsettle();                                   // a load, a new scene, pictures that are not play: settle again, verdict anew
    static bool QgJudged(Why w);                         // a reduction of this kind is judged (not a new scene's, memory's, or an undo)
    void QgNoteDown(uint64_t now);                       // a reduction is about to be made: record the state and the play before it
    int QgJudge(uint64_t now, double dt, bool mayChange, double gpuMs, double otherMs); // the open trial after a window (0 nothing, 1 the rung, 2 steps)
    bool QgOutOfReach(double gpuMs, double otherMs) const; // 60 out of reach (cap30, pace60's verdict, or the floor rung's GPU / the others)
    double QgPredict(int rung, unsigned steps, double gpuMs) const; // the GPU at (rung, steps) from the measured time now
    int QgRestore(uint64_t now, double gpuMs, double otherMs); // the 30 cadence's restore (0 nothing, 1 the rung changed, 2 steps only)
    int QgBack(uint64_t now);                            // a restore that lost pictures to the GPU on the cadence: back (as QgRestore)
    QualityStats qgStats_;
    // Config::seam
    bool SeamHold(uint64_t now);       // true: a quality-up step must wait (no cut lately, the wait not run out)
    uint64_t seamLastCutUs_ = 0, seamSince_ = 0, seamLastAsk_ = 0, seamEvalCut_ = 0;
    SeamStats seamStats_;
    Seam72Stats seam72Stats_;
    uint64_t seam72Since_ = 0, seam72LastAsk_ = 0;
    bool qgNonPlay_ = false;                             // the last picture handed to Add was not play
    unsigned qgPlayWins_ = 0;                            // windows of play since the last unsettling (and their seconds)
    double qgPlayT_ = 0;
    double qgSettleOtherSum_ = 0;                        // the other stages over those windows (ms x windows): the first verdict's
    unsigned qgSettleOtherN_ = 0;
    double qgNeedOther_ = -1;                            // the other stages smoothed over qgNeedSec (pace60's verdict; -1 not yet)
    double qgIv_ = 0, qgGpu_ = 0, qgOther_ = 0;          // the recent play, smoothed over ~2 s of windows (picture interval, GPU, others)
    bool qgRecent_ = false;
    double qgLastIv_ = 0, qgLastGpu_ = 0;                // ... and the last window's own (a scene that just got heavier: the EMA lags)
    struct QgTrial {
        bool open = false;
        int rung = kToday;                               // the state before the reduction(s)
        unsigned steps = 0;
        bool aniso = false;
        double ivBefore = 0, gpuBefore = 0, otherBefore = 0;
        double t = 0, ivSum = 0, gpuSum = 0, otherSum = 0;
        unsigned frames = 0;
        unsigned extends = 0;                            // trials extended: the GPU fell but is still over the budget (a series under way)
    } qgTrial_;
    bool qgInRevert_ = false;                            // the guard's own changes are not reductions to judge
    double qgBlockLen_ = 0;                              // a reduction that bought nothing: its block (doubling)
    unsigned qgGpuNoGainN_ = 0;                          // (gov77) reductions undone as no gain in this stage while the GPU was the limit (Config::qgStageBlock)
    uint64_t qgAnisoKeepUntil_ = 0;                      // ... anisotropic filtering off that bought nothing: not off again before this
    double qgRoomT_ = 0;                                 // the 30 cadence's room for the restore, in a row
    int qgBackRung_ = kNoRung;                           // a restore on trial: the state before it ...
    unsigned qgBackSteps_ = 0;
    uint64_t qgBackUntil_ = 0;                           // ... until then
    bool qgBackMiss_ = false;                            // ... it lost pictures to the GPU: back at the next change allowed
    uint64_t qgRestoreBlockedUntil_ = 0;                 // a restore taken back: none again before this (doubling)
    double qgRestoreBlockLen_ = 0;
    // s1117b: the GPU gate (Config::gpuGate)
    GpuGate gate_;
    unsigned gateClosedPics_ = 0, gateVetoes_ = 0;   // pictures judged with the gate closed; step-down rules it stopped (Evaluate, severe)
    void GateNote(const FrameTimes& f, uint64_t interval);
public:
    // false: the GPU's busy share of the last pictures is below what makes it the limit (the picture rate is held by something else: the game's
    // CPU, a wait for the front end, the pacing): no rule lowers the picture for the GPU's sake. True with too few measured pictures.
    bool GpuGateOpen() const {
        if (!cfg_.gpuGate) return true;
        const unsigned minPics = static_cast<unsigned>(std::max(1, cfg_.gateMinPics));
        unsigned measured = 0;
        double share = 0, lateFrac = 0;
        gate_.Stats(static_cast<unsigned>(std::max(1, cfg_.gateWindow)), measured, share, lateFrac);
        if (measured < minPics) return true;
        if (share >= cfg_.gateBusy || (share >= cfg_.gateBusyLate && lateFrac >= cfg_.gateLateFrac)) return true;
        // (the last gateRecent pictures alone, when enough of them were measured: a GPU that has just become the limit opens the gate at once,
        // as the fast rules' own 12 pictures do, instead of after the 90 pictures' mean has caught up)
        gate_.Stats(static_cast<unsigned>(std::max(1, cfg_.gateRecent)), measured, share, lateFrac);
        return measured >= minPics && (share >= cfg_.gateBusy || (share >= cfg_.gateBusyLate && lateFrac >= cfg_.gateLateFrac));
    }
    unsigned GateClosedPictures() const { return gateClosedPics_; }
    unsigned GateVetoes() const { return gateVetoes_; }
private:
    // Performance mode, Config::fast60: the last pictures (ring), the step and its state
    bool Fast60() const { return cfg_.fast60 && cfg_.targetFps == 60; }
    struct FastPic { uint32_t intervalUs = 0, gpuUs = 0, otherUs = 0; bool lateGpu = false; };
    static constexpr unsigned kFastRing = 32;
    FastPic fastRing_[kFastRing];
    unsigned fastN_ = 0, fastHead_ = 0;            // pictures in the ring (since the last change, quiet moment, stall ...), next slot
    unsigned fastSteps_ = 0;
    void FastNote(const FrameTimes& f, uint64_t interval, bool lateGpu, uint64_t gpuUs, uint64_t otherUs);
    int FastStep(uint64_t now);                     // 0: nothing, 1: the rung changed, 2: a step or anisotropic filtering changed
public:
    unsigned FastSteps() const { return fastSteps_; }
private:
    // floor protection by degrees (Config::fxStep)
    unsigned steps_ = 0;                    // bit 0 half-size effects and blur, bit 1 cheaper background
    Change lastFx_;
    unsigned fxChanges_ = 0;
    uint64_t lastFxAt_ = 0;
    bool fxEver_ = false;                   // a change was made: changes are spaced from lastFxAt_
    unsigned trial_ = 0;                    // the step (1, 2) whose trial is running; 0: none
    double fxBefore_ = 0, fxCheckT_ = 0, fxCheckSum_ = 0; // GPU ms before; trial time and sum of GPU ms x frames
    double stepGain_[3] = {0, 0, 0};        // measured saving of each step (0: not measured; StepIdx)
    unsigned fxCheckFrames_ = 0;
    double fxRoomT_ = 0, fxBoundKept_ = 0;
    double boundGpuSum_ = 0;                // GPU ms x frames over the unbroken GPU-limited stretch
    unsigned boundGpuFrames_ = 0;
    uint64_t onBlockedUntil_[3] = {0, 0, 0}, fxOffBlockedUntil_ = 0, lastFxOffAt_ = 0;
    double onBlockLen_[3] = {0, 0, 0}, fxOffBlockLen_ = 0;
    FloorWindow floor_;
    // anisotropic filtering at the top rung (Config::aniso)
    bool aniso_ = false;
    Change lastAniso_;
    unsigned anisoChanges_ = 0;
    uint64_t anisoAt_ = 0;                  // when it last changed (0: never): changes are spaced from here
    uint64_t anisoBlockedUntil_ = 0;
    double anisoBlockLen_ = 0;
    double anisoRoomT_ = 0;                 // seconds of predicted room in a row for it
    // its GPU cost as measured (diagnostics only: no decision reads these)
    double anisoBefore_ = 0, anisoSum_ = 0, anisoT_ = 0;
    unsigned anisoFrames_ = 0;
    bool anisoMeasuring_ = false, anisoMeasured_ = false;
    double anisoAfterMs_ = 0, anisoBeforeMs_ = 0;
    double lastGpuMs_ = 0;                  // the GPU's busy time a picture in the last window (target mode)
    // play-time quality
    double freeOverT_ = 0;                  // seconds the GPU has set the pace (Config::freeFrac)
    // severe overload (Config::overload): the unbroken run of pictures whose GPU busy time reaches the budget
    double ovT_ = 0, ovGpuSum_ = 0;
    unsigned ovN_ = 0;
    uint64_t ovLastAt_ = 0;                 // when the last such step was taken
    bool SevereStep(uint64_t now, double meanGpuMs);
    // (a severe overload is one against the 30 floor in either target: Performance mode's 16.7 ms is not what makes it severe)
    double SevereBudgetMs() const { return std::max(cfg_.budgetMs, 1000.0 / 30.0); }
    bool AnisoRungOk(int rung) const { return rung >= AnisoMinRung() && rung <= cfg_.maxRung; }
    // (a change of rung keeps it only when it is the old top-rung option and the rung is the top: any change has it off otherwise)
    bool AnisoKeptAt(int rung) const { return cfg_.anisoRung == kNoRung && rung == cfg_.maxRung; }
    int WipeCap(int rung) const { return !presetActive_ && cfg_.wipe && rung > std::max(kToday, cfg_.minRung) ? std::max(kToday, cfg_.minRung) : rung; }
    // ---- stagepreset25 state (ApplyStagePreset)
    StagePreset preset_;
    bool presetActive_ = false, presetSaved_ = false, presetForcedEdge_ = false, presetForcedCuts_ = false;
    int savedMin_ = 0, savedMax_ = 0, savedStart_ = 0;
    void ApplyPresetStart(uint64_t now); // at a new scene: the preset's edge pass and captures (or the defaults back), and its steps
    unsigned PresetSteps() const { return presetActive_ && !ShedOn() && preset_.edgeOff && cfg_.edgeStep ? 4u : 0u; }
};

// The log of automatic quality: the Vita's port_log (the console's log file; the back end's stderr goes nowhere there),
// stderr elsewhere. One line, no newline needed.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// The same, into the basic log (the Vita's port_log_summary, which every build writes; stderr elsewhere): the "floor:" line.
void LogSummary(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// GXV_PACE23: the back end's scene timers' GPU busy time of a finished picture, the display scene's wait for its buffer taken out, and the
// timers' raw busy time beside it. Enabled once the scene timers run; the GPU clock then reports these instead of its own figure.
void HonestBusyEnable(bool on);
bool HonestBusyEnabled();
void HonestBusyPublish(uint32_t busyUs, uint32_t rawUs);
// GXV_PACE60: the scene timers' split of a finished GPU picture (us: TA, EFB/pass 3D, display 3D, busy), handed from the scene reader to the
// controller (Controller::AddSplit) through a small ring; both ends on the back end's thread. SplitNext: false when none is waiting.
void SplitPublish(uint32_t taUs, uint32_t efbUs, uint32_t dispUs, uint32_t busyUs);
bool SplitNext(uint32_t out[4]);
int TargetFps();       // GXV_AUTOQ_TARGET (30 or 60; anything else 0), read once for both the controller and game CPU sampling
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
