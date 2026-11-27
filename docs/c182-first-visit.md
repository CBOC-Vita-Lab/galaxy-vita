# First-visit shader work in C182

Historical note: C186 removes the compiled-in, stage-specific priority list
described below. The donor stage records are unchanged; C186 prioritizes
records tagged by the existing learning mechanism. These older physical timings
are not findings from C186 or proof about the first visit to Queen Bee.
On integ-3.1 (seedrun31, kept by c186merge) the compiled-in list is gone as
well; its nine records are slow tags in the packaged stage list
(`tools/seedrun/stagetags.py`), so the priority is data, not hashes in code.

This addresses frozen-looking first appearances, not a demonstrated deadlock.
Earlier physical logs contain the following delayed frames. Neither log records
that the player was looking at Queen Bee at the instant of the pause.

| Earlier console log (relay/outbox) | Frame | Back end | Compiler inside that frame |
| --- | ---: | ---: | ---: |
| `2290-c106-look-m5/10/log.txt:2197`, Observatory | 7665 | 887 ms | 3 runs, 753 ms |
| `3227-w181-P-M08/149/log.txt:1126`, Honeyhive | 2350 | 287 ms | 1 run, 235 ms |
| `3227-w181-P-M08/149/log.txt:1173`, Honeyhive | 2380 | 1,750 ms | 3 runs, 1,451 ms |

The seven linked programs across those three frames all have exact 64-bit
vertex/fragment records in the C181 test19 VPK. The four Observatory records
were 648-651 of 697 in the room list and the three Honeyhive records were
497-499 of 499. C182 prioritizes these *existing* records if the program is not
resident. A known program pair is not a promise that the driver's patched
binary is cached, nor that there are free program slots. The work is moved
under an existing loading cover where possible, not eliminated.

* The loader deduplicates the shader IDs, complete vertex layout and complete
  blend key together across the room/player lists. Existing test19 archive:
  35 redundant queue entries removed for Observatory; 51 for Honeyhive.
* If either registered source is missing at queue construction, the room or
  player record waits for that specific source. An unrelated shader no longer
  causes an entire replay of the warm queue. When both are present, only newly
  ready variants are appended to the front under cover. No shader is compiled by
  the source notification and stage compile/eviction budgets are not reset.
* A successful in-play link taking at least 100 ms can mark the actual draw
  variant for priority on a later visit to that stage. Up to 12 such markers
  share its existing capped, append-only stage file. Bits used for GPU state are
  unchanged; older readers ignore the priority bit. `GXV_FIRSTVISIT_PRIORITY=0`
  disables historical promotion and recording. `GXV_STAGEWARM_REBUILD=0`
  disables new registration watches.
* A genuinely unseen pair still needs its real source and a first compile/link
  on the render thread. Never substitutes another shader or drops a real draw.
  Stage binary lookup/compact packaging is separate from draw prediction.

`bash tools/test_c182_firstvisit.sh` tests exact deduplication, late registration
and priority boundaries. An additional argument containing a VPK replays its
actual stage archive. On the offline Linux/EGL build host,
`bash tools/test_c182_firstvisit_backend.sh` also runs the production backend through
queue construction, source registration, a live frame, a closed frame and slow
link learning with no GL context. The original queue progress and the compile
budget both survive late resolution. These are host and archive checks; there
is no C182 Vita before/after timing for these frames.

## C183: two additional Observatory variants

The same Observatory log has two later frames containing **one** new draw
program and **one** compiler run. Frame 7669 at line 2246 took 741 ms in the
backend (666 ms compiler); frame 7728 at line 2275 took 272 ms (214 ms).
Their exact 64-bit shader IDs and 25-bit layout/blend match test19 Observatory
entries 669 and 671 of 697. C183 promotes only these additional variants; all
nine historical priorities now match that full layout/blend value. The queue
limits and covered-screen budgets are unchanged. Reordering two existing
variants can displace other preparation inside the same budget; no assertion
that the cited time is saved in the candidate.

Frame 7668 bundled eight draw links around one compile. It and other stalls
without a unique traceable variant are not new hardcoded priorities. This older
log used Performance settings with a 30 fps limit override; it isn't a matched
steady-state C183 FPS comparison. The baseline VPK replay now asserts the
six Observatory entry positions and the three unchanged Honeyhive positions,
rejecting a wrong layout, wrong blend, and a candidate from the bundled frame.

## seedrun31: the priorities are data

`Historical()` (the nine records above) and the built-in player seed (`kPlySeed`, eleven low-32-bit entries whose fragment hashes no longer
matched anything) are gone from the renderer. A record is queued first when its stage-list word carries the slow tag (bit 28) for the scenario
being loaded (`vgx::firstvisit::Priority`). `tools/seedrun/stagetags.py legacy` puts the tag on the nine records of a packaged list made before
seedrun31; `bash tools/test_c182_firstvisit.sh [VPK]` replays the build's head with the old rule against the tagged list and requires identical
queues. Bits 29-31 carry the scenario a record was drawn in (0 = any): the scenario being loaded is queued first, other scenarios' own records go
behind the player list. Bit 25 marks a "can draw" record (a harvest's enumeration): a tier of its own behind the whole queue, run only under the
game's own load cover, never compiled for, never counted for the hold. The records learned in play are also written to `<stage file>.jnl` by the
player list's writer thread, so a session closed in play keeps them (`GXV_STAGEJOURNAL`).

### seedrun31b: the real list

On test19 / seed 1117's list (15,583 records; the VPK's and seed1117.tgz's are the same file, sha 732feb1a...) the first seedrun31 tool tagged 14 records: the
nine, and five copies of the same draws recorded under other galaxies (the three Honeyhive draws under ReverseKingdomGalaxy 3c2b6fbe, the Observatory 16400 draw
under BattleShipGalaxy 9545aad9 and EggStarGalaxy 099e9a42). `Historical()` was per stage, so those five were never priorities; the tool now matches the stage key
too. The old player seed was not inert there: all 11 entries resolve to exactly one key each (also among the key list's 3,899 key hashes); 2 were player records
already, 9 are now PLYR records at the end of the list. The replay compares every stage (47, 24,846 queue entries) with the old rule (hashes and seed) and requires
identical queues; `tools/test_c182_firstvisit_backend.sh <VPK>` loads the tagged list through the production loader and checks the head of both galaxies.
