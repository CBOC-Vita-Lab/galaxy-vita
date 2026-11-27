// deadfix27 (13 Nov 2026): the picture modes' per-mode key table, in ONE place. Pure logic (no game types, no clocks): host-tested by tests/mode_keys_test.cpp.
// The glue is platform/src/settings/settings_launch.cpp (ApplyStartup: the Picture mode is known there) and vita/vgx-game/entry.cpp (the snapshot of what env.txt set).
//
// Why. The console logs of build c86 showed keys that the Settings' Performance mode was meant to carry but that never took effect (GXV_COLSHADOW_CLIP lives only in the
// stage rows' "full file", which the shipped row set does not have; the S60 ladder keys act only with GXV_SMOOTH60=1). Every key the mode path writes now comes from this table,
// so the coordinator fills it from measurements and a console log proves what was written.
//
// Precedence (the same as GXV_QUALITY_SET / GXV_CPULOD): env.txt > this table > the code default. A key that env.txt set itself, even to its default, is never touched.
// "-" means "no change in this mode" (a value such as "-5" is a value). The census line says what was written, kept and left.
#pragma once

#include <cstdint>
#include <stdio.h>  // (unqualified snprintf: the Vita build maps it to port_snprintf with a macro)
#include <cstdlib>
#include <cstring>
#include <string>

namespace port {
namespace modekeys {

struct Row {
    const char* key;
    const char* perf;     // the value in Performance (60), "-" none
    const char* quality;  // the value in Quality (30), "-" none
    const char* note;     // the evidence, one line
};
// At most 128 rows (the user mask is 128 bits). Add a row: key, Performance value, Quality value, evidence. Only keys that take effect from the environment alone.
// q30aa (25 Nov, the owner: "whatever gives quality the highest quality ... while keeping it at 30fps"): the exact rows FIRSTCLEAR, TAILDEPTH, HOIST_GRID,
// VGL_VALIDREGION, EFBSWAP2_BACKOFF, COLSHADOW_CLIP and CORNERPASS_ALT now apply in Quality too (audit 25 Nov: no mode gate in code; same pixels). At the
// Quality's rung 6 (single-sampled) these run the same code as in Performance.
inline constexpr Row kRows[] = {
    {"GXV_COLSHADOW_CLIP", "1", "1",
     "level 1 is exact (stage_knob 'p': a Quality row clamps 2 to 1); console ladder runs 2000-2016 ran it from env: 'colshadow: ... STABLE'; the c86 mode path never wrote it (the shipped stage rows lack it)"},
    // reviewB (13 Nov, new/reviewB/TRIAGE.tsv C10): the code default is 3 (entry.cpp); c86 env and tourP used 2: removed the 3-vblank holds in 4 of 4 stages, no fps loss (HANDOFF s5). The mode path never applied it.
    // Quality: "-" on purpose. The fixed-30 modes do not use the legacy skip rule this caps (GXV_EVEN26 is ignored there, GXV_EVEN26_Q owns the rhythm), so a value there would change nothing.
    {"GXV_SKIP_MAXSTEP", "2", "-", "console c86/tourP env: 3-vblank holds removed in 4 of 4 stages, no fps loss; code default 3; no effect in the fixed-30 mode"},
    // strk27 (13 Nov): a turn that starts 80 ms or more after the one before it is a stall; without this the skip rules pay back up to 67 ms of "lateness" with 3-4 turns without a picture
    // (a visible jump of game time after a 100-250 ms hitch). frame_pace.h StallForgiveNs, tests/stall_forgive_model.cpp (decisions identical without a stall; after a stall of 80 ms .. 3 s at most
    // one turn without a picture). Both modes (the fixed 30's controller reads it too). NO console run yet: the "pace: stall forgiven" line proves it acted.
    {"GXV_STALL_FORGIVE", "80", "80", "model only (stall_forgive_model: identical decisions without a stall); the owner's hitch-repay symptom; switch existed since catchup25, never run on a console"},
    // reviewB OX6: exact (the EFB's buffer goes to the display; no previous picture to keep); ladder R1 "DIRECT_DISPLAY 15000/15000 shown direct" = 100% direct; est. 0.8-1.1 ms GPU (an estimate). Quality's edge pass blocks it, so no value there.
    // c163 (14 Nov, owner: the lives counter at the bottom-left flickers while Mario stands still; earlier the Game Over strip, the galaxy map's Back button, a blue box in
    // Bowser's galaxy): with the pairs of GXV_HRW=5 a flipped EFB buffer came back into use while still on the screen (its next picture's first scenes do not carry
    // the buffer's sync object). Console screenshots of the hub, Mario still, 30 each: direct display 12 of 30 counters whole, with GXV_DIRECT_DISPLAY_FLIGHT=4 22 of 30,
    // OFF 30 of 30; off costs ~0.8 ms GPU a picture (the plain 1:1 present copy: no edge pass at 960x544, the picture is the same), 59.5-59.9 fps, even 99-100.
    // sg12 (21 Nov): ON again in Performance. Console, Beach Bowl, Performance, plain 60 (60 Assist off, GXV_HRW=0), gameplay-only fps: control 41.8 / 41.5, GXV_DIRECT_DISPLAY=1 alone 43.8 fps (+2.1),
    // the "direct-display:" census shows the share shown directly. The c163 flicker was under the HRW pairs only; dd::EffectiveMode (vgx_directdisplay.h) now keeps the machine off for the whole run
    // while GXV_HRW != 0 (60 Assist on), so the row is harmless there (nothing is made, no cost). Quality: "-" (edge pass / q30 rungs block it; the decision falls back to the present anyway).
    {"GXV_DIRECT_DISPLAY", "1", "-", "sg12: Beach Bowl 60 Assist off 41.8/41.5 -> 43.8 fps; exact (the EFB's buffer is the picture); never runs under GXV_HRW != 0 (the c163 lives-counter flicker was under the HRW pairs, dd::EffectiveMode); every doubt (size, sharpen, hud, capture, msaa) presents as before"},
    // sg12 (21 Nov): the scene-load group, exact. Console, Beach Bowl, Performance, plain 60, the four together 43.7 fps against 41.8 / 41.5 (+2.0), no glitch in the log. FIRSTCLEAR: the picture's first EFB scene
    // loads no depth (its clear quad overwrites it whole); TAILDEPTH: the last scene stores none once 120 pictures in a row began that way (a miss switches it off); HOIST_GRID + VALIDREGION: Mario's
    // shadow-corner copy as a small pass on the EFB's own grid (+ GXM valid region) instead of an EFB split. Known limits: a uniform-pool rollover that has to wait re-begins a scene without its load
    // (rare wrong pixels for one picture); a TAILDEPTH miss may show stale depth for one picture. Quality: "-" (not measured there; Quality's stage-table / q30 ladder owns its scene structure).
    {"GXV_FIRSTCLEAR", "1", "1", "sg12: with TAILDEPTH, HOIST_GRID, VALIDREGION: Beach Bowl 43.7 vs 41.8/41.5 fps, no glitch in the log; exact on the host (test_vgx_firstclear_gl.sh); rare limit: uniform-pool rollover"},
    {"GXV_TAILDEPTH", "1", "1", "sg12: needs FIRSTCLEAR; the last EFB scene stores no depth once 120 pictures in a row began with a whole-depth overwrite; a miss switches it off"},
    {"GXV_HOIST_GRID", "1", "1", "sg12: the shadow-corner copy on the EFB's own grid: same draws / state / texture coordinates, only the 3 pixel margin; host tests/vgx_hoistgrid_gl.cpp"},
    {"GXV_VGL_VALIDREGION", "1", "1", "sg12: vitaGL patch 0028 (vp26+): the hoist pass's scene processes only its valid region; ignored (logged) when vitaGL lacks it"},
    // reviewB C6 / results29 fz-R3: -3.3 ms GPU and +5 fps at Freezeflame (38.8 -> 43.9) but the indirect warp stops wobbling. A per-STAGE candidate (a stage row's ind_flat=1), default OFF until the owner has seen it in motion: no value here.
    {"GXV_IND_FLAT", "1", "-", "defaults49 (22 Nov): owner approved by eye at Beach Bowl (water and underwater look good); Beach GPU 3D -1.9 ms, tail +4 fps. Was: PER-STAGE CANDIDATE (Freezeflame row ind_flat=1): fz-R3 GPU 25.8 -> 22.8 ms, 38.8 -> 43.9 fps measured; the warp wobble stops; OFF until the owner has seen it"},
    // reviewB X1 / scenes26: GXV_COLOR_LOADSKIP stays at code default 0 (retail Vita hardware in Strikers 2.0 verified
    // SCE_GXM_ERROR_INVALID_VALUE 0x805B0000 on 32x32 swizzled backgroundTex; still opt-in via env.txt with patch 0028 VP26_SURFACE_RETRY).
    // scenes27 (13 Nov): fewer GXM scenes a picture. Every scene costs ~1.0 ms of GPU vertex-phase time (ta27 fit) and ~0.8-1 ms of render thread, even with no draw.
    // INDCAP_SKIP=1: the game's whole-EFB "Indirect" capture is not made when no draw of the picture reads it and no earlier-picture reader was seen (pixcuts25: exact; refreshed on
    // first sight; it is the member the Quality umbrella already counts as exact). Console: CannonFleet R4 (INDCAP_SKIP=2 + ACROSS) took 2 scenes and 3.2 ms of TA off; tourP ran it at 0.
    {"GXV_INDCAP_SKIP", "1", "1", "exact (indcap: 'stale reads 0' must hold); CannonFleet: 2 scenes, -3.2 ms TA (ta27 on the c86 R4 run); no console run of =1 alone"},
    // OWNER DECISION 13 Nov 07:04 ("use the cheaper beach glow"): Performance folds both bloom chains (simple bloom 5 texture scenes + 1 EFB split -> 3 scenes; normal bloom 4 -> 3).
    // Level 1 keeps the blur's shape (host harness PSNR 36 dB, mean difference 1.1 levels); it is a stage-entry choice (stage knob bloom_fold, read when the stage loads), so
    // nothing switches in play. Quality keeps the original chain.
    {"GXV_BLOOM_FOLD", "3", "-", "g30 Wave 1/3: symmetric 4-fold Gaussian reduced-tap bloom (9 vs 18 draws = 36 vs 72 taps, <1% energy error, identical shape) in Performance; Quality untouched; env.txt GXV_BLOOM_FOLD=2 restores fold 2"},
    // Underwater (Beach Bowl, Ocean Ring) the NORMAL bloom is the water filter's glow: the existing reduced-glow chain (2 scenes) replaces 4; only where WaterAreaHolder::mUseBloom.
    {"GXV_WATER_BLOOMFOLD", "2", "-", "owner-approved coarser glow; underwater normal bloom 4 -> 2 scenes (est. -1.5..-2.5 ms while wet, dive27); never run on a console"},
    // scenes28 item 3: the silhouette alpha capture (cut A) is removed by the stencil path (stencil43); windows that cannot be classified fall back to the exact capture per picture
    // (census "stencil:" fallbacks). Exact by design; never run alone on a console; the existing ShadowStencilVerify pass (GXV_SHADOW_STENCIL=2) must read 0 mismatches on a console before this stays on.
    {"GXV_SHADOW_STENCIL", "1", "1", "c180: Quality too (console verify in Quality, c179 3130e GXV_SHADOW_STENCIL=2: 17 lines, windows 121-600, copy / fill / not safe / off the EFB / stale reads all 0). exact by construction (stencil43; per-picture fallback to the capture); est. -1 scene on pictures with Mario's see-through silhouette; no console run of =1 alone"},
    // scenes28 cut C: the Indirect capture (refraction / haze / heat source) is left out; its readers sample the previous picture's 3D scene (one end-of-3D copy, HUD excluded).
    // ---- pace33 PATCH8 (14 Nov 2026): the measured console candidate of the re-projected 60 as the Performance mode's defaults, so that a card with only env-user.txt plays it.
    // Each row is what the 46-line test env (env-c124-*-3a.txt) set by hand; a key of env.txt, even 0, still wins, so every row is switchable back. Quality: "-" on every row.
    {"GXV_HRW", "5", "-", "console c118-c135: 59.9-60.0 shown, even 100 at Honeyhive / Freezeflame / Dreadnought; the owner's decision of 14 Nov 11:50; GXV_HRW=0 = the plain 60 target as before"},
    // gov5 (21 Nov): 60 Assist is a step down from full 60, so the governor drives mode 5 too: plain 60 first; the GPU ladder (GXV_PERF_GPU896) where the GPU is the limit; 60 Assist where it still cannot hold
    // 60 (it keeps the camera and Mario at 60) and the pair fits; the even 30 only if neither works. It leaves for plain 60 when that would hold. GXV_HRW_GOV5=0 = mode 5's pairs always on after the settle, as before.
    // Needs a Vita check: the log lines "hrw: stage X: engaged because ..." / "disengaged because ..." (and "not engaged because the stage holds 60 natively").
    {"GXV_HRW_GOV5", "1", "-", "gov5 (21 Nov, the owner's intent: 60 Assist only where plain 60 cannot be held): real Vita, 60 Assist on = world at 30 everywhere; off = Battlerock / Melty Molten / Space Junk 52-56 plain; host-tested governor (tests/hrw_gov5_test.cpp); never run on a console; GXV_HRW_GOV5=0 restores the always-on pairs"},
    {"GXV_HRW_WARP_DEPTH", "4", "-", "rotation + the player's box (warp32 PATCH11): the console candidate since c118; code default 0"},
    // c169 (15 Nov, warp36): the player's box follows his body (8 px under his feet to above his head, 85 units either side, 10 px feather) instead of a 130-unit square
    // round his centre: the ground round his feet no longer moves with him on a jump (the owner's "bubble" in the Observatory). Console c165 Observatory: boxes 7.4-8.0k px
    // against 23-25k px for the square, 59.3-59.7 shown, even 98-100, warp pass 0.17-0.20 ms.
    {"GXV_HRW36_BOX", "1", "-", "the body-shaped player box (warp36): no ground dragged with him on a jump; code default 0"},
    // c173 (15 Nov, the owner's "dizzy" double path at Bowser's levels): the ground-plane warp. The second picture of a pair re-projects the ground under the player
    // by the camera's whole move (rotation and translation) instead of the rotation alone, so the near ground and what stands on it no longer stand still in every
    // second picture. Console c172b + PLANE=1, P60 stress z1 / z2: on the plane 55-79 % of the pairs at Beach Bowl, Honeyhive, Dreadnought, Battleship, 95 % at the
    // castle, 28-33 % at the Star Reactor (the rest: no ground data; hold36 keeps it through the air); shown fps and evenness as without, 0 crashes.
    {"GXV_HRW36_PLANE", "1", "-", "the ground-plane warp (warp36 + hold36): the near ground moves at 60 with the camera; code default 0"},
    // c178 (15 Nov): the plane trusted within 500 units of his ground point (gone by 1000) instead of 1500 (3000). Console flip-books, Honeyhive in play, half-size
    // pixels (tools-flip/flipmotion.py): ground band unevenness 3.1 (plane off) -> 0.81 (R 1500) / 0.68 (R 800) / 0.72 (R 500); the void past the platform edge 0.0 (off)
    // -> 2.39 (R 1500) / 2.01 (R 800) / 0.15 (R 500): the far background no longer swims with the ground. (plane37/38's depth gate stays 0: the game clears the depth
    // before the 2D, GameScene::draw3D MR::clearZBuffer, so A's depth is gone by the copy; 0f67fb0 on agent/plane38 makes a mask before the clear, not yet on a console.)
    {"GXV_HRW36_PLANE_R", "500", "-", "the ground plane's trusted radius round his ground point (units; it fades to infinity by twice that); code default 1500"},
    {"GXV_HRW_SETTLE", "100", "-", "c181: 100 (was 200, the candidate's value; code default 90). Drawn turns after a stage begins before pairs start: 200 kept the first ~6.7 s of every stage at 30; c178 console passes 3128b1/b2 with 100 (+RESUME 60): Beach Bowl's opening window 57.2 / 56.5 fps against 33.1-39.4 in five runs with 200, lost second pictures 1.36 / 1.29 a second (1.06-1.31 with 200), no dumps"},
    {"GXV_HRW_SETTLE_RESUME", "60", "-", "c181: drawn turns before pairs resume when the SAME scene had settled and was only covered (a pause, a closed fade); never more than GXV_HRW_SETTLE (stutter37, port/hrw_settle.h); console 3128b1/b2"},
    // stutter37 (15 Nov): CANDIDATE, no value yet. GXV_HRW_HUD_EDGE=2 (the code's default, c159: a HUD rectangle within 2 px of the picture's edge gives the pair's second
    // picture up) was the first answer to the lives-counter flicker; c163 then found the flicker's cause in the direct display (a flipped buffer drawn into while on the
    // screen: the counter broke in 18 of 30 screenshots of a build that had this rule, in 0 of 30 with GXV_DIRECT_DISPLAY=0), and the direct display has been off since. The rule is
    // still on and is the largest single reason a pair has no second picture: console c172b stress route, 123 of 234 lost second pictures in 115 s of pairs (runs of 5-35
    // pairs at 30 a second while a counter slides in). Performance takes "0" once a console run with GXV_HRW_HUD_EDGE=0 shows the counters whole (the hub, Mario still,
    // the lives counter up; a Game Over prompt; the galaxy map's Back button).
    // GXV_HRW_HUD_EDGE (row removed 26 Nov to keep table headroom: it set "-" in both modes; code default 2 applies only on pairs without the scene copy).
    // c171 (15 Nov 02:15): ON again, both crashes it exposed fixed: crash36 (the render job queue's storage in the scene heap: the library stage-change crash) and
    // replay36 (a partial draw that carried an EFB copy command without its record was executed by the warp's replay: c168's Honeyhive crash; such a partial draw is
    // now resources only, and a buffer whose indexed commands dangle is never executed). Console check on c171: library trips and Honeyhive with it on.
    {"GXV_HRW_WARP_FRESH", "1", "-", "the player drawn fresh on the second picture; with GXV_HRW_FRESH_AUTO (default 1) it is left out while the pairs have no room; c135 Honeyhive 59.9 / even 100 with it"},
    {"GXV_DRAIN33", "1", "-", "console c118: pad to flip 99 -> 70 ms at Honeyhive, 137 -> 70 at Freezeflame, 115 -> 88 at Beach Bowl"},
    {"GXV_LATENCY24", "1", "-", "the latency rule the drain needs (entry.cpp sets it only when env.txt asks for the drain); pad resample on with it"},
    {"GXV_LATENCY24_WINDOW", "45", "-", "L comes down every 3 s instead of 6: the value of every console drain run; code default 90"},
    {"GXV_FE_GOV", "3", "-", "the front end's governor up to level 3 (c161, owner 14 Nov: the hub's full far-cut step as a rung before the 60 gives way; was 1); its floor is the stage's static level less one; code default 0"},
    {"GXV_GAME_GOV", "3", "-", "the game thread's governor up to level 3 (c161, as GXV_FE_GOV; was 1); code default 0"},
    {"GXV_STAGE_PRESETS", "1", "1", "the compiled-in stage table and the optional row file: the candidate env; code default 0 (1 only with GXV_SMOOTH60). Quality 1 since c172 (15 Nov): with 0 the stage load never reached ApplySpec, so the Quality lock ladder (GXV_Q30_LOCK: far steps 3a-3l, beams, budget; the owner's 13 Nov request) never started; in 30 the rows are filtered to the exact / barely noticeable keys at the q level (vgx_stagepreset.h Resolve)"},
    {"GXV_Q30_LOCK_STEPS", "-", "28671", "Quality's lock ladder without step 3j (far flower / bush / plant groups beyond 45 m not drawn): it REMOVES far plants, against the owner's 13 Nov 11:50 rule (no far things appearing and going); the low-detail stand-ins (GXV_PLANT_IMP) never shipped. 32767 = every step (3j back)"},
    {"GXV_PAR_DL_VIEW", "0", "-", "the candidate env; code default 1"},
    {"GXV_UPSCALE", "1", "1", "entry.cpp turns it on when env.txt sets GXV_STAGE_PRESETS=1 (the candidate env did); the table sets GXV_STAGE_PRESETS after that point, so it is written here; idle at 960x544 shown 1:1"},
    {"GXV_UPSCALE_LITE", "1", "1", "as GXV_UPSCALE. Quality 1 since c173: the 896x512 step (GXV_Q30_MID) is shown through it; at 960x544 it is idle"},
    {"GXV_Q30_MID", "-", "1", "Quality's GPU step (owner 15 Nov 07:11: \"Yes, to 896x512 as a step for gpu\"): the ladder's step 6, the 3D at 896x512 with the HUD and 2D at 960x544, taken only behind a cover (a stage load, the pause menu, a wipe, a movie) once the GPU steps before it are not enough; the card remembers it per stage and gives it back when the GPU has room"},
    // cpu34d (14 Nov): five front-end / particle switches that existed exact but were never A/B'd. Console c146 (2932a verify, 2932b hub, 2932c Honeyhive):
    // tevcol verify 5.5 M checked 0 differing; reinitgx 61 k resets 0 mismatches; ptclcull 67 % of particle quads outside the clip volume not sent.
    // Hub 58.8 -> 59.7 fps, even 92 -> 99, hitch lines 274 -> 169, front end 20.9 -> 19.8 ms; Honeyhive pad to flip 86-92 -> 76-82 ms, the front-end governor no longer cuts (level 0, 10.6 ms a picture).
    {"GXV_HOTPROF_TEVCOL", "1", "1", "c179: Quality too (exact; an outside review 15 Nov: no reason was recorded for leaving Quality out). exact (GXV_HOTPROF_TEVCOL=2 verify 0 differing on the console, 2932a)"},
    {"GXV_HOTPROF_REINITGX", "1", "1", "c179: Quality too (exact; an outside review 15 Nov: no reason was recorded for leaving Quality out). exact (=2 verify 0 mismatches on the console, 2932a)"},
    {"GXV_HOTPROF_PTCLCULL", "1", "1", "c179: Quality too (exact; an outside review 15 Nov: no reason was recorded for leaving Quality out). particle quads wholly outside the clip volume not sent (console c128 2831a-c and 2932a/b/c)"},
    {"GXV_HOTPROF_NOFLUSHSTRIP", "1", "1", "c179: Quality too (exact; an outside review 15 Nov: no reason was recorded for leaving Quality out). console 2932a-c; GXV_FE_STATEFAST=2 run 2932d"},
    {"GXV_FE_STATEFAST", "1", "1", "c179: Quality too (exact; an outside review 15 Nov: no reason was recorded for leaving Quality out). console 2932b/c; =2 verify on the console 2932d"},
    // cpu34g / cpu34d / gpu34 (14 Nov), reviewed (fable-review34 + fixes c151), console c151 package runs with the owner's env (DLWELD=1):
    // hub 2940a: 59.9 fps, even 97-99, hitch lines 114 (274 before today), pad to flip 97 ms (101); Beach Bowl 2940b against its same-day control 2940c:
    // pad to flip ~80 ms against ~102, GPU 14.5-15.0 ms a picture against 16.0-16.7, game thread a pair ~2 ms less, even 100 in play in both.
    {"GXV_CLIP_PAIR", "1", "-", "the drawn turn of a pair does not judge clipping; the undrawn turn judges with the picture's camera (c147, 2934a / 2940a)"},
    {"GXV_PAIR_LAYOUT", "1", "-", "the drawn turn of a pair leaves out HUD layout calc-anim (the undrawn turn redoes it before the picture)"},
    {"GXV_PAIRPOSE", "1", "-", "non-player near models posed in the undrawn turn only (hub calc-anim 2.3 -> 1.4 ms a drawn turn, 2934a)"},
    {"GXV_SHAPECULL", "1", "1", "c179: Quality too (exact; an outside review 15 Nov: no reason was recorded for leaving Quality out). rigid shapes wholly outside the view left out on the game thread (c148 verify: 0 outside-plane failures in 4.9 M vertices; hub 38 of 243 a picture)"},
    // GXV_HRW_HUD_SHOWN taken back out of the defaults (c156, 14 Nov 18:51): the owner saw the dome's galaxy map "flicker a bit, especially on the buttons" on c153;
    // it is the one change of today that touches the HUD in the second picture. The HUD layer's own copy (one scene a pair more, ~1 ms GPU) is back.
    {"GXV_HRW_WARP_PROJ", "1", "-", "the warp as one projective texture read a pixel (gpu34; its program is compiled at first use until the next shader pack)"},
    // pace32 GXV_LATE_TURN (phase sync: the undrawn turn sleeps first, then reads the pad; adaptive, backs off after a missed retrace). Console Dreadnought: pad to flip 69.5 -> 59 ms,
    // even 100, 0 missed retraces (2931a, 2940d); Honeyhive about -3 ms (little slack). The fresh player's deadline moves with the sleep (c153; c150's after-sleep reset was too late).
    {"GXV_LATE_TURN", "1", "-", "late input read in the undrawn turn's slack; Performance only"},
    // defaults32 (20 Nov 2026, owner's rule: an improvement that helps is on by default): the Vita A/B of 19 Nov (new/verdict32/REPORT.md, console8 runs r2-700..718, Performance + 60 Assist) passed these.
    // Each is still switchable back from env.txt (a key of env.txt, even 0, wins and is listed under "kept from env.txt").
    {"GXV_CPULOD_MEMO", "1", "1", "perf32 (20 Nov): the detail-level table found by the name's address (confirmed against the name) instead of ~14 string compares, 300-450 lookups a turn in Quality; exact: host 70 M lookups and emulator verify mode 2.1 M lookups, 0 differ; strictly less work; GXV_CPULOD_MEMO=0 in env.txt restores the old search"},
    {"GXV_PTCL_PRE", "1", "1", "perf32 (Vita night32, 21 Nov): a particle quad far outside the view answered by one centre test before the full clip test; exact: Vita verify mode 97.2 % answered, 0 wrong; Quality pair at matched draws (Q0b 143 / Q1 142): game turn 1.57 -> 1.46 ms, 30.0 both; GXV_PTCL_PRE=0 in env.txt restores the old path"},
    // gpu896 (21 Nov, the owner's policy: "any time the gpu is the issue, dropping graphics to 896x512 in performance is fine and then dropping to 30fps"): Performance only. Owner 21 Nov 08:24 widened it: a GPU-limited stage walks cheaper picture -> 896x512 -> 848x480 -> edge smoothing off (then the even 30), and back up with room (Controller::LadderStep); CPU-limited stages give nothing up; HUD and 2D layers at 960x544. Needs a Vita check (log "stage-preset: GXV_PERF_GPU896=1", then "[autoq] rung 6 -> 4 ... Performance ladder"); GXV_PERF_GPU896=0 in env.txt restores 960x544 fixed
    {"GXV_PERF_GPU896", "0", "-", "gpu896 (21 Nov): Performance ladder when the GPU is the limit: cheaper picture, 896x512, 848x480, edge smoothing off, before the even 30, and back up with room; GPU-gated (GXV_AUTOQ_GPUGATE), HUD native; Quality untouched (960x544 fixed); GXV_PERF_GPU896=0 restores 960x544 fixed"},
    // GXV_PERF_HALFGLOW (row removed 26 Nov to make room: code default is already 0, opt-in via env.txt GXV_PERF_HALFGLOW=1 with GXV_PERF_GPU896=1).
    {"GXV_BOWL_POSE", "-", "1", "bowl31 (Vita night32, 21 Nov): a Beach Bowl fish's pose left out of the skip turn that nothing reads (pose age at draw 0, always); exact: Vita verify mode 0 mismatches; Beach pair at matched draws (B0 193 / B6 190): game turn 3.37 -> 2.52 ms; Quality only (verified there); GXV_BOWL_POSE=0 in env.txt restores the old path"},
    {"GXV_POSE_PHASE", "1", "1", "Vita QS1 vs QS0: stale poses (age 1+) 13.0 % -> 0.06 % (census 22.4 % -> 0.08 %), drawn turn +0.05 ms [-0.02,+0.11] (free), render thread -0.42 ms; fixes the lily / flower / Luma vibration; both modes (the rule reads the skip turns of a pair, harmless in a plain 30 / 60)"},
    {"GXV_HRW_MARIOTIME", "2", "-", "Vita console7, 2 pairs: stale-Mario pictures 268-611 (Honeyhive) / ~1,400 (Beach Bowl) -> 0, held 1.9 % / 0.4 %, shown fps -0.17 [-0.31,-0.07], 0 missed retraces; Performance only (it acts on the HRW pairs, which Quality does not use)"},
    {"GXV_PB_DEMO", "2", "2", "Vita r2-713 vs r2-707: port-made pop-ins in the flights Honeyhive 45 -> 4, Beach Bowl 7 -> 0; fps +0.09 [+0.02,+0.23] Honeyhive, -0.01 Beach, render +0.13 / +0.09 ms, 0 hitches; both modes (the flight rule and the camera-centred LOD distance do not depend on the mode)"},
    {"GXV_PB_MINPX_SHADOW", "3", "3", "Vita P7 vs P2: the last shadow pops of the flights 2-6 -> 0 ('port-made pops at or above the floor' 0); no frame cost seen (fps -0.12 [-0.34,0.00], render -0.04 ms); own price line ~58 shadows kept a turn x 6.5 us = ~0.37 ms a turn on the game thread (census estimate; accepted by the owner); the plant floor (_DECOR) stays off"},
    // fix31 (owner 17 Nov: off until the console A/B; single player as c181 until each passes on hardware): C182's Performance warp rows are out of the table, so the
    // code defaults (0) apply: GXV_HRW_STAGE_AUTO_RETRY (was 3), GXV_HRW41_DEPTH, GXV_HRW41_BOXKEY, GXV_HRW41_BOX_ROUND, GXV_HRW41_BUDGET (were 1). env.txt sets them
    // for the A/B arms (docs/RELEASE-3.0-CHECKLIST.md, the console A/B list).
    // GXV_HRW41_OCCLUDE (row removed 26 Nov to make room: it set "-" in both modes; C182 opt-in via env.txt).
    // defaults48 (22 Nov 2026, the confirm pass on the Vita, builds c36c / c36e; /home/claude/g31/DEFAULTS48.md and IMPROVE45.md Part 4). The owner's rule of 08:23:
    // "anything exact can be enabled for Quality as well".
    // GXV_PIX_ZERO: defaults46 put =1 in Performance. The confirm pass, with windows paired at the same PLACE (player within 200 units), not only the same draw count:
    // Honeyhive, 30 pairs twice: drawn turn +0.83 / +0.91 ms, GPU 3D -0.31 / -0.35 ms, 3.2 / 3.5 fps SLOWER (the game thread is the limit there); Beach Bowl drawn turn
    // +0.9 ms, GPU 3D -0.4..-0.6 ms, tail +1.3 fps. =1 is out. Its time went into the evaluation of every blended / alpha-tested draw's state; 13.0 of its 13.2 draws
    // a picture came from the draw's own vertex colours. =3 (the lean way, vgx_draw.cpp: only that proof; no evaluation at the resolution, no texture ranges) keeps
    // them: Vita3K c36e Beach Bowl "left out 13.0 draws ... evaluations 0.0"; host test_vgx_pixval_gl.sh arm z3 identical pictures. Never in a picture that opens a
    // 60 Assist pair (the owner saw the HUD flicker and fade there; PixDrop keeps the draw). NO VALUE HERE until the console A-B of =3 is read (night35 arms HZ0a
    // HZ3a HZ1a HZ0b HZ3b, BZ0a BZ3a BZ0b BZ3b; pass = drawn turn within +-0.3 ms of the control at the same place, GPU 3D lower): then "3" in BOTH modes (exact;
    // Quality has no pairs, no pipeline dependence: the proof is the draw's own blend state and vertex colours).
    {"GXV_PIX_ZERO", "3", "3", "defaults49 (22 Nov, owner: exact switches on in both modes): =3 lean, Vita c36e Honeyhive 58/51 pairs fps -0.3/-0.4, 3D -0.32/-0.29, drawn +0.12/+0.11; Beach 16+16 pairs 3D -0.51/-0.52, drawn +0.19/+0.14 (same-build controls); never drops in a 60 Assist pair picture (a4b72c26). Was: defaults48: =1 LOSES where the game thread limits (Vita c36c Honeyhive: drawn turn +0.83 / +0.91 ms, -3.2 / -3.5 fps, 30 same-place pairs twice; GPU 3D only -0.3 ms); =3, the lean way (vertex-colour proof only: the same 13 hidden layout panes a picture, no evaluation), is exact and waits for its console A-B: then 3 in both modes; GXV_PIX_ZERO=3 in env.txt tries it"},
    // GXV_CORNERPASS (imp45 + fix47; exact; the player's shadow corner copy from a small pass at the picture's start, one EFB split less): Vita c36c Beach Bowl,
    // CPa1 / CPb1 against CPa0 / CPb0 at the same place: "made from a pass at the picture's start 348 of 350, fell back 0", EFB scenes 3.5 -> 2.5, GPU 3D -0.94 /
    // -0.70 / -0.74 / -0.66 ms heavy (control pair -0.08), -0.34 / -0.34 ms in the tail with +0.9 / +0.9 fps there (11 / 10 pairs); render thread +0.14..+0.36 ms;
    // BUT the front end's alpha bounds cost the game thread draw +0.13 / +0.05 / +0.31 / +0.30 ms a drawn turn in the heavy part (fps +0.1 / +0.4 / -0.2 / -0.6).
    // Honeyhive (the game thread limits, 550 draws: more bounds work, no GPU gain to pay for it) did not run (HCP0 / HCP1 were cut). NO VALUE until HCP1 reads
    // within 0.5 fps of HCP0 at the same place: then "1" in both modes (Quality: it refuses a multisampled EFB by itself and then changes nothing). Never run under
    // 60 Assist pairs.
    // mask51: the 60 Assist depth mask takes the closest depth of 3x3 taps over its whole 4x4 block (was 2x2 of 16 pixels), so a character's edge pixels move with the character (less double image). Same single pass.
    // dm54 (22 Nov): the "double Mario" of 60 Assist (a copy of Mario a few px behind him when he runs fast, underwater zoomed in). Cause (Vita3K flip-book with the fresh player's pixels painted, GXV_DM54_MARK=1): the warp shows the first
    // picture's player in his OLD POSE and the fresh player is drawn over it in the new one; halo31's stamp (the fresh silhouette moved) and the box only cover the part where the poses overlap. This takes the old player out by the depth mask. Acts
    // only with GXV_HRW_HALO, GXV_HRW41_DEPTH, GXV_HRW41_BOXKEY and the stencil (the owner's 60 Assist env); GXV_HRW_INPAINT=0 = halo31 / box as before.
    {"GXV_HRW_INPAINT", "1", "-", "dm54: Vita3K swim window: Mario-red pixels outside the fresh player 3.3 -> 0.7 a picture (worst pair 26 -> 0), run window 0.97 -> 0.0; no console run yet"},
    // GXV_HRW_MASK_NEAR (row removed 26 Nov to make room: code default is already 0; opt-in via env.txt GXV_HRW_MASK_NEAR=1).
    {"GXV_CORNERPASS", "1", "1", "defaults49: exact; Beach Bowl 600/600 made, GPU 3D -0.70..-0.85 ms, drawn within bar; Honeyhive cost fixed by cp50 (bounds only near the corner, auto-off per stage when the grid hoist already took the copies). Was: defaults48 CANDIDATE (exact): Vita c36c Beach Bowl GPU 3D -0.66..-0.94 ms heavy, -0.34 ms tail (+0.9 fps twice), EFB scenes 3.5 -> 2.5, 0 fallbacks; game thread draw +0.05..+0.31 ms a drawn turn; Honeyhive c36e: marked 600, planned 0 (GXV_HOIST_GRID had taken the copies) and the alpha bounds cost +0.95 ms a drawn turn: cp50 works bounds only for draws whose scissor reaches the corner and switches the pass off for a stage after 600 marked pictures with none planned (back on at the next stage load; GXV_CORNERPASS_AUTOOFF); GXV_CORNERPASS=1 in env.txt tries it"},
    // night60 (25 Nov, w24f, Vita 498 MHz, A B A B pairs, Performance): verified on the console, exact or memory-only.
    {"GXV_EFBSWAP2_BACKOFF", "1", "1", "night60 2c Dreadnought: the EFB swap is used every picture again (made without a copy scene 0.00 -> 1.00), GXM scenes 4.00 -> 3.00 a picture, GPU 3D -0.2 / -1.6 ms, fps +0 / +1.1; exact (dd24 + fix24 table eviction). Quality not measured"},
    {"GXV_S68_PAIR_LAZY", "1", "-", "night60 3b Honeyhive: video memory free +8..10 MiB at stage load and in play (52 vs 42-44 MiB at load, 38-42 vs 28-32 in play), no hitch, no cost seen; 60 Assist pair targets are made at the next closed screen after the governor decides to engage (Assist may start a little later). Quality not measured"},
    {"GXV_PENDING_HOLD25", "1", "-", "pace25 (25 Nov): the skip rule's pending count allows the L-2 pictures a held picture keeps in the back end, so a stage that misses 60 no longer locks at 48. Console, Performance, w24h/w24k eboots: Beach Bowl 48.0 -> 59.9 / 59.7 fps median (mean 48.0 / 51.1 -> 53.2 / 52.8, windows stuck at 48: 7 / 3 -> 2 / 0, visible breaks 1447 / 912 -> 537 / 536); Good Egg 58.3 / 57.8 -> 59.0 / 58.9 (breaks 253 / 344 -> 92 / 117); Dreadnought 48.1 / 49.1 -> 50.1 / 50.6 (breaks 1868 / 1782 -> 1586 / 1513, stuck 7 / 5 -> 4 / 4); hitches unchanged; latency 5 vblanks unchanged. =2 (guarded, pace26/27) measured weaker: not the default. Quality not measured"},
    {"GXV_CORNERPASS_ALT", "1", "1", "night60 2d Dreadnought: EFB scenes 2.02 -> 1.89 a picture, fps +0.5 / +1.1, GPU 3D +0.28 / -0.45 ms (mixed, small); exact; ~100-230 KB per corner texture, refuses when VRAM is short; the parked texture goes through the RT_DEFER free (fix24 S1). Quality not measured"},
    // night73 (25 Nov, w24n eboot 63d35fc4, Vita 498 MHz, Performance, A B A B): the direct display also while 60 Assist is on but no pair is held (dd24: a pair opens only
    // after the display has given every buffer back, vgx_galaxy.cpp DirectDisplayDrained; the "hrw: dd-dyn:" line counts pairs opened undrained).
    {"GXV_DD_DYN", "1", "-", "night73: Dreadnought fps 50.4 / 50.3 -> 51.1 / 51.7, GPU 3D 18.88 / 18.61 -> 18.20 / 18.59 ms; Melty Molten 51.1 / 51.8 -> 53.0 / 53.1, GPU 3D 18.58 / 18.30 -> 18.08 / 17.80 ms, visible breaks 1491 / 1548 -> 1322 / 1315; direct 100 % of pictures; 0 dumps. Exact (the EFB buffer is the picture). Quality not measured"},
    // w25f exact wins (both Performance and Quality; identical pixels and deterministic in SP + MP lockstep).
    {"GXV_LIGHTVOL", "3", "3", "night60 2h-LVOL3: exact light-volume frustum cull (bit 1) + pass-major grouping (bit 2); measured -0.31..-0.57 ms 3D and -1.04..-2.26 ms TA; exact pixels in both modes"},
    {"GXV_FLUSH_OFFSCREEN", "1", "1", "gamepar exact (tests/view_cone.cpp): off-screen plants (mLogicFreeJoints != 0) skipped in flush; strictly render-only, deterministic in SP and MP lockstep, both modes"},
    {"GXV_PLANT_DRAWCULL", "1", "1", "gamepar exact (with GXV_FLUSH_OFFSCREEN=1): off-screen plants (mLogicFreeJoints != 0) also skipped in DrawBuffer; strictly render-only, deterministic in SP and MP lockstep, both modes"},
    // g30 Wave 3 exact zero-pack-refill wins (both Performance and Quality; identical pixels, zero shader-text change, 100% env.txt-overridable).
    {"GXV_VRAM24_HYST", "1", "1", "vram24 hysteresis: stops pool-trim urgent limit and seam68 low-memory limit from oscillating"},
    {"GXV_VRAM_STRICT", "1", "1", "spill24: drains deferred frees + one GC cycle before spilling a texture out of VRAM to LPDDR"},
    {"GXV_STAGEBUILD_FAST", "1", "1", "openstall26: merges shared-program stage queue from pre-sorted list instead of O(N log N) re-sort at every stage change"},
    {"GXV_TEXUP_LAZYFENCE", "1", "1", "texup: only syncs pending upload fence when touching an in-flight texture (exact)"},
    {"GXV_TEXUP_TINY_BYTES", "1024", "1024", "vram24: <=1 KiB single-level texture uploads stay linear instead of swizzled transfer (identical pixels)"},
    {"GXV_MP_CORE3", "1", "1", "mp3c24: tightens discretionary Core 3 worker budget during online MP sessions so Wi-Fi/net threads never starve"},
    {"GXV_HUDN_NODEPTH", "1", "1", "hud24: skips unused depth attachment on native-res HUD pass (HUD draws with depth test disabled; exact)"},
    {"GXV_SS_TILEBOUND", "1", "1", "ss24: clips shadow-stencil fill quad to volume screen bounds (exact pixels, cuts SGX fillrate)"},
    {"GXV_SS_ATRESET", "1", "1", "ss24: avoids redundant alpha-test resets on shadow-stencil paths (exact)"},
    {"GXV_SS_FILLFOG", "1", "1", "ss24: disables dead fog on shadow-stencil fill draws where fog has no effect (exact)"},
    {"GXV_SS_NOMASK", "1", "1", "mf25: skips redundant colour-write mask on shadow-stencil volume draws (exact)"},
    {"GXV_CLEARQ", "1", "1", "pixcuts25 + Wave 3 ClearQTry stencil-window unblock: replaces full-screen clearEfb draw quad (~0.62-0.77 ms) with fast clear (exact)"},
    // GXV_COLOR_LOADSKIP kept at code default 0 (removed from mode table after Strikers 2.0 retail Vita hardware verified
    // SCE_GXM_ERROR_INVALID_VALUE 0x805B0000 on 32x32 swizzled backgroundTex; opt-in via env.txt still safe via patch 0028 VP26_SURFACE_RETRY).
    {"GXV_INDCAP_PEEK", "1", "1", "peek25: omits dead Indirect EFB capture when next queued CmdBuf overwrites it before any read (exact, 0 stale reads)"},
    {"GXV_INDCAP_VIS", "1", "1", "indvis38 (Wave 3 decoupled from port_hrw_indcap_exact): omits Indirect EFB capture when every reader is invisible/culled (exact)"},
    {"GXV_BE_MERGE", "1", "1", "ta24 (Wave 3 boundary-hardened RunFrom): merges adjacent backend draws with identical program/state/uniforms/buffer and contiguous indices (exact)"},
    {"GXV_BE_USEPROG", "1", "1", "cpu24: skips redundant glUseProgram calls in backend draw loop (exact Core 1 CPU win)"},
    {"GXV_SEAM72", "1", "1", "seam72 (Wave 2 + Carbon fixes): parks spare EFB buffers per rung and defers size-down to covers (eliminates resolution-change hitches)"},
    {"GXV_PACE_PASSOFFER", "1", "1", "pace: fixed-30 skipped due turn passes picture to next turn (3-vblank hold instead of 4); harmless in 60 mode"},
    {"GXV_PAR_PRIO", "1", "1", "parprio31: lifts core-3 par workers two steps above game thread so finisher never starves parallel joins"},
    {"GXV_PAR_PAUSE", "1", "1", "parprio31: proportional serial fallback after a par-ptcl join stall instead of fixed 120-turn pause"},
    // g30 Wave 3 Quality-30 crispness & visual upgrades (Quality only; zero shader-pack change; 100% env.txt-overridable; GXV_EDGE_DEPTH kept opt-in until console A/B).
    {"GXV_RUNG_LODBIAS_EXTRA_AF", "-", "2", "Quality crispness: -0.25 extra mip bias while anisotropic filtering is active"},
    {"GXV_PLAYER_LODBIAS", "-", "-50", "mario24 Quality crispness: -0.5 mip bias on Mario's mipmapped textures (sharper character detail at 1.0x)"},
    {"GXV_PLAYER_DOF", "-", "0", "mario24 Quality crispness: starts DoF ramp behind Mario so Mario himself is never blurred by depth of field"},
    {"GXV_AUTOMIP", "-", "1", "automip25 Quality crispness: generates mip chains for mip-less 3D textures (stops far-surface shimmering/sparkling; 6 MiB cap)"},
    // g30 Wave 4: 60 Assist exact scissor + scoped AF (Performance only) + Quality-30 visual max-out (trilinear+AF automip, max player AF, auto 2x MSAA rung 7 -> 1x rung 6 floor + Vita MSAA stencil).
    {"GXV_HRW_OWN_SCISSOR", "1", "-", "g30 Wave 4 (04-hrw-gpucost): exact conservative scissoring/skipping of OwnMarkToAlpha quads in 60 Assist (0/255 host pixel oracle)"},
    {"GXV_VGL_ANISO_SCOPE", "1", "-", "g30 Wave 4 (vitaGL 0044, Strikers 0057): Performance-only scoped AF on opaque depth-writing draws (Quality 30 keeps full AF on blended/water/decal surfaces)"},
    {"GXV_AUTOMIP_FILTER", "-", "2", "g30 Wave 4 Quality max-out: trilinear auto-mipmaps (GL_LINEAR_MIPMAP_LINEAR), unlocking hardware SGX anisotropic filtering on originally mip-less 3D textures"},
    {"GXV_PLAYER_ANISO", "-", "3", "g30 Wave 4 Quality max-out: highest hardware anisotropic filtering on Mario/Luigi trilinear textures"},
    {"GXV_Q30_MSAA", "-", "1", "g30 Wave 4 Quality max-out: starts Quality 30 at rung 7 (960x544 hardware 2x MSAA, auto-disables post-process edge5 blur) with auto step-down to rung 6 (1x + edge5) before any detail cut"},
    {"GXV_Q30_MSAA_STENCIL", "-", "1", "g30 Wave 4 Quality max-out: keeps Vita shadow-stencil optimization active under 2x MSAA (Vita-only guard in MsaaStencilOn)"},
    {"GXV_MP_DET_LOD", "1", "1", "g30 Wave 8: deterministic lockstep-safe visual-only LOD (mLogicFreeJoints != 0) recovers CPU headroom during multiplayer"},
    // GXV_INDPREV (row removed 25 Nov to make room: it set nothing in either mode): candidate, off: refraction/haze source is one picture old; scene saving
    // unproven and never run on a console; needs console stills (GXV_INDPREV_AB=30 for A/B) before Performance takes it.
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
static_assert(kRowCount <= 128, "the user mask is 128 bits");   // (g30 Wave 3: 64 -> 128 bits for exact + Quality crispness rows)

struct Mask128 {
    uint64_t lo = 0, hi = 0;
    Mask128& operator=(uint64_t v) { lo = v; hi = 0; return *this; }
    bool Test(int i) const {
        if (i < 0 || i >= 128) return false;
        return i < 64 ? (((lo >> i) & 1ull) != 0) : (((hi >> (i - 64)) & 1ull) != 0);
    }
    void Set(int i) {
        if (i < 0 || i >= 128) return;
        if (i < 64) lo |= 1ull << i; else hi |= 1ull << (i - 64);
    }
};
inline bool NoChange(const char* v) { return !v || (v[0] == '-' && v[1] == 0); }
inline Mask128& UserMask() { static Mask128 m{}; return m; }
inline bool& Snapshotted() { static bool s = false; return s; }
inline bool& Applied() { static bool a = false; return a; }
// setnat31: did env.txt set this table key? With the snapshot (the console: entry.cpp takes it before its own defaults, e.g. setenv("GXV_HRW","0",0))
// the snapshot answers; without one (a host test) whatever is in the environment counts as env.txt's, as Apply does. A key outside the table: the environment.
template <class Get>
inline bool UserHas(const char* key, Get get) {
    if (Snapshotted())
        for (int i = 0; i < kRowCount; ++i)
            if (std::strcmp(kRows[i].key, key) == 0) return UserMask().Test(i);
    const char* v = get(key);
    return v && *v;
}
// get(key) -> the environment's text or null. Taken right after env.txt is read, before any default is set.
template <class Get>
inline void SnapshotUser(Get get) {
    Mask128 m{};
    for (int i = 0; i < kRowCount; ++i) {
        const char* v = get(kRows[i].key);
        if (v && *v) m.Set(i);
    }
    UserMask() = m;
    Snapshotted() = true;
}

// fmt1131: the census text is a std::string (heap, unbounded): nothing is cut and no 6 KiB array sits on a Vita thread's stack. Converts to const char* so
// callers that strstr / strlen / assign it keep working.
struct Text : std::string { operator const char*() const { return c_str(); } };
struct Result {
    int set = 0, kept = 0, none = 0;
    Text text;
};
// quality: the Quality (30) mode, else Performance. Writes with put(key, value). Without a snapshot whatever is in the environment counts as env.txt's.
template <class Get, class Put>
inline Result Apply(bool quality, Get get, Put put) {
    Result r;
    auto add = [&](const char* s) { r.text.append(s); };
    char buf[200];
    snprintf(buf, sizeof buf, "modekeys: %s mode:", quality ? "Quality" : "Performance");
    add(buf);
    std::string keptList, noneList;   // (fmt1131: were 400 / 300 byte arrays that dropped the rest without a word)
    char tmp[256];
    for (int i = 0; i < kRowCount; ++i) {
        const Row& w = kRows[i];
        const char* v = quality ? w.quality : w.perf;
        if (NoChange(v)) {
            ++r.none;
            noneList += ' '; noneList += w.key;
            continue;
        }
        bool user;
        if (Snapshotted()) user = UserMask().Test(i);
        else { const char* c = get(w.key); user = c && *c; }
        if (user) {
            ++r.kept;
            const char* cur = get(w.key);
            snprintf(tmp, sizeof tmp, " %s=%s(the mode would set %s)", w.key, cur ? cur : "?", v); keptList += tmp;
            continue;
        }
        put(w.key, v);
        ++r.set;
        snprintf(buf, sizeof buf, " %s=%s", w.key, v);
        add(buf);
    }
    snprintf(buf, sizeof buf, " (%d written, %d kept from env.txt, %d with no value in this mode)", r.set, r.kept, r.none);
    add(buf);
    if (!keptList.empty()) { add("; kept from env.txt:"); add(keptList.c_str()); }
    if (!noneList.empty()) { add("; no value:"); add(noneList.c_str()); }
    return r;
}

}  // namespace modekeys
}  // namespace port
