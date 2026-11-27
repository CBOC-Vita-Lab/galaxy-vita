# mariolag31: console arms for "Mario one picture behind" in 60 Assist

Branch `mariolag31`. Switch `GXV_HRW_MARIOTIME` (default 0 = today's code), census `GXV_HRW_MARIOTIME_CENSUS=1` (any mode; on by itself in modes 1 and 2).
The reasoning and the host numbers are in `reports/mariolag31.md` (cloud workspace) and in the header `platform/include/port/mario_time.h`.

## What the arms measure

In a pair, the first picture A shows the state of turn T-1 and the generated picture B shows turn T, when Mario is drawn fresh for it. When the fresh draw
is left out, B shows his pixels from A, moved by his centre's move: his pose is one turn old. On a landing he is still in his jump pose for one picture.

Tonight's console logs (r2-601 K0a, the default env, Honeyhive) show how often that happens: of 7,726 second pictures, only 1,727 had the fresh Mario.
The auto rule (`GXV_HRW_FRESH_AUTO`) had him off for three minutes in a row. That rule reads a pair's time including the deliberate sleep of `GXV_LATE_TURN`,
and comes back only after 90 pairs in a row under 27 ms.

- **Mode 1:** a second picture that would show his old pixels is not made. The real picture stays for both refreshes. The fresh draw uses the camera of
  the turn whose pose it draws.
- **Mode 2:** mode 1, plus the auto rule decides by the fresh draw's own cost, with the late-turn sleep left out.

## Build

One build of `mariolag31` merged on integ-3.1 (the coordinator's build). All arms run on the same eboot.

## Jobs (`tools/console31/make_jobs.py`, numbers 8400-8408)

| Job | env.txt lines | What |
|---|---|---|
| 8400-c31-MT-0 | `GXV_HRW_MARIOTIME_CENSUS=1` | Control with the census: today's stale pictures by reason |
| 8401-c31-MT-1 | `GXV_HRW_MARIOTIME=1` | No stale Mario, old auto rule. Expect many holds (30 a second) where the rule is off |
| 8402-c31-MT-2 | `GXV_HRW_MARIOTIME=2` | The candidate |
| 8403-c31-MT-0b | `GXV_HRW_MARIOTIME_CENSUS=1` | Control again, for a state-matched pair |
| 8404-c31-MT-2b | `GXV_HRW_MARIOTIME=2` | Candidate again |
| 8405-c31-MT-F0 | census + `GXV_HRW_FLIPBOOK=240 GXV_HRW_FLIPBOOK_FROM=600` | Flip-book, control. Pull `ux0:data/smg/hrw31/`, then clear it |
| 8406-c31-MT-F2 | `GXV_HRW_MARIOTIME=2` + the flip-book | Flip-book, candidate |
| 8407a/b-c31-MT-O2 | `GXV_HRW_MARIOTIME=2` | The owner plays (start / end jobs) |
| 8408a/b-c31-MT-O0 | census only | The owner plays the control the same way |

- **Route:** Performance + 60 Assist on, Honeyhive 120 s then Beach Bowl 90 s. The flip-book arms use Honeyhive only. The scripted seedwalk route jumps (A),
  spins and ground-pounds.
- **Shots:** 3 mid-run screenshots per unattended arm. A screenshot is A or B, whichever is on screen at that moment.
- **Run order:** MT-0, MT-2, MT-0b, MT-2b, then MT-1 if time allows, then the flip-books.
- **State match:** compare only visits whose draws per picture match within about 10 % (the Beach Bowl "states" lesson of console31run2). Prefer Honeyhive.

## What to read in log.txt (every 5 s, in play)

- **`hrw: mariotime |`**
  - pairs; asked with the fresh Mario; merged.
  - **STALE Mario shown N (by reason: auto off, speed guard, given up, deadline missed, not merged)**, and his move on those pictures.
  - **held as a real 30 picture instead N (by reason)**.
  - No Mario in the picture; drawn nothing.
  - **fresh camera:** pairs; the camera moved in the update before the draws (deg, units); the pose's camera used.
  - Auto rule switches and the cost estimate.
- `hrw: fresh auto |`: pairs with and without the fresh player; switches.
- `hrw: fresh deadline |`: in time / missed / no partial draw.
- `hrw: pictures (10 s) |`: re-projected, of which with the fresh draw merged.
- `late-turn:`: **turns that missed their retrace** (the price of a fresh draw that is late), sleep, idle left.
- `fps:`: shown fps, hitches; `even:` scores; `hrw: mode 5 |`: game thread per pair.

## Pass rule

1. **MT-0 / MT-0b confirm the cause.** STALE Mario shown is above 0 at Honeyhive or Beach Bowl, mostly "auto off". STALE ≈ re-projected - merged in the same window.
2. **MT-2 / MT-2b pass when, in play, every window:**
   - **STALE Mario shown = 0.**
   - Held is at most 3 % of pairs, summed over each stage visit.
   - "pose's camera used" = "fresh camera: pairs".
   - Against the state-matched control:
     - shown fps is not lower by more than 0.5 at Honeyhive and Beach Bowl;
     - `late-turn` missed retraces are at most 2 per 10 s window;
     - hitches over 100 ms are no more;
     - `hrw: pictures` "with a fresh partial draw merged" is at least 95 % of re-projected.
   - No new crash dump.
3. **MT-1** is informational: the price of strictness with the old rule (held ≈ today's stale count; shown fps lower where the rule is off).
4. **Flip-books (MT-F0 / MT-F2).** In the B pictures right after a landing:
   - in MT-F0, some show his jump pose where the A after them shows him landed;
   - in MT-F2, none does (every B shows a pose between the A before and the A after).
   - Count the landings looked at and the ones that hang.
5. **Owner (MT-O2 against MT-O0):** does Mario "hang in the air for a picture" after a landing in O0 and not in O2? Any new stutter in O2?

## Decision

- **MT-2 passes 2 and 4 (and 5 if run):** `GXV_HRW_MARIOTIME=2` becomes a Performance row in `mode_keys.h` (coordinator).
- **STALE = 0 but fps or missed retraces fail:** keep mode 2 off. Send the logs: the fresh draw's own cost (`hrw: fresh |` game-thread mean and max) shows where
  the time went.
- **STALE above 0 in MT-2:** a reason not modelled. The census names it; send the log.
- **The late-turn sleep alone:** a separate candidate bug fix (the old rule reading work, not sleep) can be cut from mode 2 if the coordinator wants it on
  its own. Ask.
