# C186.2: evidence boundaries

The `c1862-build` ref identifies the ARM TLS/release-PGO executable. The QA
child changes no part of the game. Its emulator tools are not deployed, not
Vita environment overrides and not a replacement for the original C181
hardware reference. Keep the five contributor PNGs and retail shader profile.

| Mac Vita3K, C186.1, retail VSOUT=1 (no forced packs) | Standard checkpoint | Background disk writes |
| --- | --- | --- |
| First checkpoint picture in the two completed runs | 108 / 118 ms | 59 / 59 ms |
| Second checkpoint | 49 / 83 ms | at most 34 / 35 ms (five-second upper bounds) |
| Shown rate, equal controlled windows, 100 seconds per condition | 54.99 FPS | 54.90 FPS |
| Pictures over 100 ms in those windows | 6 | 6 |

Same linked emulator, VPK, input, save, original host cache, Metal seed and
empty writable guest cache. Five attempted launches: OFF, ON, ON, OFF
(failed before control), OFF replacement. The four completed runs had two
independent matching archive events and 0 ms offset spread at timestamp
resolution. The closing save isn't a gameplay sample. This is evidence for
a specific checkpoint benefit with no general FPS gain. The emulator patch
is optional and defaults off; Vulkan snapshotting is still synchronous.

Separately, stationary Honeyhive re-entry maxima fell from 175 / 143 ms to
44 / 44 ms in two trained-cache pilots, but a checked full import aborted
with identical starting cache contents. An original-seed launch also
aborted. Do not import or distribute shader caches as a player fix. The
two near-second stalls (951 / 957 ms) were not aligned with a disk write;
the actual display synchronization cause and the Vita Queen Bee pause
remain unproven. Neither a host bench nor a reprojection FPS counter is a
GPU savings estimate.

Local handoff: `outputs/galaxy-c186.2/HITCH-HANDOFF.md`, with individual
receipts and raw-log hashes in `hitch-evidence/`. Cross-release history:
`claude-galaxy/integ31/codex-since-c181/README.md`. Data-free gate:
`bash tools/test_c186.sh`. The [replay](../tools/hitch_replay/README.md)
requires private inputs and the pinned isolated emulator; [optional
host trace](../tools/emu_hitch/long_trace/README.md) has exact-source and
fake-clock verification only. It was not included in the measured binary.
