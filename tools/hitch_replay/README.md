# Offline Mac hitch replay

This is an **emulator experiment**, not a console preset, release shader pack,
or install tool. A warm host cache can help investigate Vita3K/MoltenVK pipeline
creation separately from the game's own first-use work. Do not use these host files on
a Vita. The no-input route does not reach the Queen or reproduce the original
player video.

Requirements: macOS/APFS; private, licensed game data, test saves and
firmware in the `c182_vita3k_smoke.py` template layout; the known Mac
Vita3K 4232 build and a pre-existing dedicated QA Metal-cache seed
(`--metal-seed-id`); and the verified C186.1 package, or an explicitly
specified `--vpk FILE --vpk-sha256 DIGEST` pair. The template's parent must
contain the input file expected by the smoke helper; the cloned script is
replaced with neutral input for 1,000 game seconds. Check free disk space
before starting. Run serially; don't operate the emulator window or
change clocks/power settings while timing. Never use your real save as
the QA fixture and never share the private profiles.

```sh
python3 -B tools/hitch_replay/replay.py reference --output /path/to/fresh-lab
python3 -B tools/hitch_replay/replay.py reference2 --output /path/to/fresh-lab
python3 -B tools/hitch_replay/replay.py full --output /path/to/fresh-lab --warm-from reference
python3 -B tools/hitch_replay/analyze_replay.py /path/to/fresh-lab/private/full
```

For a separate source-built checkpoint experiment, pass the same explicit
`--local-emulator FILE --local-emulator-sha256 HASH` pair to both reference
and treatment, and set `--checkpoint 0` or `--checkpoint 1` respectively.
Select `--route moving` in both: this replays the frozen C186 stick/jump/spin
sequence every 16 game seconds in Honeyhive scenario 1 for 85 host seconds
after first reaching control. The guest env and script match the archived
receipt byte-for-byte, without the stationary route's extra diagnostics.
Repeated stage deaths/reloads are recorded; clean moving windows exclude
the last second before each scene teardown through the next control marker.
Do not warm either arm with a donor. This changes only the new host emulator
process environment, not the game. It requires a build from the same pinned
revision displaying the same release banner. The reference must succeed
before comparing a treatment. Capture the original and per-run re-signed
executable hashes. Stock runs never inherit a checkpoint override.

The runner refuses an active Vita3K process or an existing run destination,
package/driver mismatches, unverified donors, and a non-private Metal-cache
ID. Each launch gets a fresh APFS clone, private app identifier and matching
initial Metal cache. The emulator's sandbox forbids networking and all writes
except to that experiment profile, its specifically named Metal cache and
`/dev/null`. The default package is the unmodified C186.1 executable. The
guest starts with **no writable shader cache**, no settings override file,
retail `GXV_VSOUT=1` and no forced packs in every arm. The presentation
mode is Performance, Assist at its normal setting. The two diagnostic
switches are `GXV_FIRSTUSE_TURN=1` and `GXV_HITCH25_TURN=1`.

The one intentional difference in a warmed run is the private host cache
overlay. The donor must be from a completed same-package, same-emulator
replay. The checked helper stages and reads back a new per-title directory
before it is used; the prior directory is preserved in that disposable
profile. Other titles and the user's original emulator cache are untouched.
The source fixture, local build archives and private caches aren't included.
Never substitute a trained pipeline .dat alone while retaining a different
shader index or SPIR-V set. That deliberately isolated negative control
aborted before game control; this runner refuses partial imports. Two raw
complete-cache pilot clones did finish, but a full checked import with
identical per-title bytes also aborted before control with a MoltenVK
pipeline link error. The cause is unknown. This is not a reliable startup
procedure, a player recommendation, or a verified cure. Never treat a
crash, timeout, or missing matched control marker as a timing sample.

The route stands still in Honeyhive scenario 1 for 2,400 playable turns,
then forces one re-entry. The supervisor waits for the second first-play
marker plus 18 host seconds before stopping its own child. Analysis splits
startup, first controlled play, forced re-entry/opening, and second controlled
play; a five-second reporting interval crossing a boundary is not counted
as clean play. A stage tagged `in play` by the renderer can still be before
the first controllable turn. The JSON includes independent host/guest
archive-open clock anchors; null means timing alignment was unavailable,
not zero events. The emulator's recorded host cache-save duration may omit
driver serialization preceding its first log line. Never add overlapping
game-thread and presentation waits to claim additional savings.
