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
#include "../../include/port/hot_knob.h"
#include "../../include/port/frame_pace.h"  // (kSteady30AboveFps: the steady 30's release, Config::cadenceReachFps)

namespace vgx::autoq {

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
    return kEmergencySize.w % 4 == 0 && kEmergencySize.h % 4 == 0 && kWideEmergencySize.w % 4 == 0 && kWideEmergencySize.h % 4 == 0;
}
static_assert(CornersOnPixels(), "every render size keeps the EFB's quarter on whole pixels");
inline bool WideRungs() {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h): every rung lookup of the back end and the controller
    return PORT_HOT_KNOB(bool, [] { const char* v = std::getenv("GXV_WIDE_RUNGS"); return v && std::atoi(v) == 1; }());
}
// Keep the public 0..8 rung IDs unchanged; the extra floor exists only in target mode.
inline const Rung& RungAt(int rung) {
    if (WideRungs()) return rung == kEmergency ? kWideEmergencySize : kWideLadder[rung];
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
    uint32_t frontRunUs = 0, frontPictures = 0; // target mode: the front end's cumulative CPU time and pictures (wrap intentional)
    bool frontKnown = false;   // ... known (a front end on its own thread that stamps its pictures)
    bool cadence30 = false;    // target 60: the pacing's steady 30 (frame_pace.h SteadyCadence) shows the pictures every second turn
    bool paced = false;        // paced display callback: the late frame fence alone cannot measure GPU busy time
    bool timers = false;       // GPU timers clock: gpuBusyUs is used, gpuDoneUs is not
    bool busyKnown = false;    // ... a frame finished on the GPU since the previous present, and this is its busy time
    uint32_t gpuBusyUs = 0;
    bool quiet = false;        // calm mode: a quiet moment (pause menu, closed fade or wipe): not measured, a step up may be taken
    bool wipe = false;         // ... the quiet moment is a closed fade or wipe (not the pause menu): Config::wipe
    bool movie = false;        // calm mode: a movie is playing: not measured, and no change of size (forkB, from Producdevity's fork)
    uint32_t scene = 0;        // calm mode: the scene's key (0 = unknown)
};

enum class Why : uint8_t { None, Headroom, Overload, Saturated, GpuBound, NoGain, NewScene, Forced, NoMemory, FxOn, FxOff, FxNoGain, AnisoOn, AnisoOff, Wipe, FreeDown, Severe,
                           HoldMiss, HoldClimb, HoldBack, HoldEdgeOff, HoldEdgeOn, HoldEdgeBack, CadenceUp, CadenceBack, FastDown };
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
    uint32_t cadence = 0;                  // pictures on Performance mode's steady 30 (judged against 33.3 ms, not 16.7)
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
    void Climb(int to, uint64_t now);           // a step up the room allowed (Why::Headroom, or Why::CadenceUp with its trial)
    void ForgetFxBlocks();
    void SetAniso(bool to, Why why, uint64_t now);
    void SetSceneAniso(bool to, uint64_t now);
    static constexpr unsigned kAnisoMem = 8u;
    unsigned Mem() const { return steps_ | (aniso_ ? kAnisoMem : 0u); } // what a scene remembers beside its rung (bit 3: anisotropic filtering)
    int FxRung() const { return cfg_.minRung > kToday ? cfg_.minRung : kToday; } // the rung the steps belong to (today's, or the lowest allowed)
    // (returns the late picture's stage: 0 GPU, 1 game, 2 renderer, 3 front end, 4 none of these, 5 not measured; -1 on time or not counted)
    int FloorCount(uint64_t interval, bool quiet, bool measured, bool gpuKnown, bool gameKnown, bool backendKnown,
                   uint64_t gpu, uint64_t game, uint64_t backend, bool frontKnown = false, uint64_t front = 0, bool cadence = false);
public:
    unsigned QuietUps() const { return quietUps_; }
    // Quality mode judged by the pictures (Config::hold30): the last closed hold window (diagnostics, tests): its pictures, those late,
    // those late for the GPU (the size could have helped), and the hold windows in a row that missed / were calm.
    struct HoldWindow { uint32_t pictures = 0, late = 0, lateGpu = 0; double gpuMs = 0; bool miss = false, calm = false; };
    const HoldWindow& LastHold() const { return lastHold_; }
    unsigned HoldWindows() const { return holdWindows_; }
    int HoldMissRun() const { return missRun_; }
    int HoldCalmRun() const { return calmRun_; }
    bool HoldTrial(uint64_t now) const { return holdTrialFrom_ != kNoRung && now < holdTrialUntil_; }
    // (Config::hold30 with edgeStep: the present's edge smoothing off by the rule above; its measured GPU saving, ms a picture, -1 unknown)
    bool HoldEdgeOff() const { return holdEdge_; }
    double HoldEdgeSavingMs() const { return edgeSave_; }
private:
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
    int WipeCap(int rung) const { return cfg_.wipe && rung > std::max(kToday, cfg_.minRung) ? std::max(kToday, cfg_.minRung) : rung; }
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
