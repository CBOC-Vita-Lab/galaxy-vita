# Stream Preparation Failure Fixture

This is a characterization test of a reproducible preparation failure, not a
runtime fix or a diagnosis of a particular player's loading screen.

Run `bash tools/test_jai_stream_prepare.sh`. It builds only the standalone host
fixture in the existing Linux ARM64 container, with networking disabled and the
source tree mounted read-only. It does not build or launch the game. Synthetic
audio bytes live in an owned temporary directory; no game assets or saves are
mounted. AddressSanitizer and UndefinedBehaviorSanitizer cover the fixture.

## Production Code Exercised

The test compiles the unchanged production JAIStream, JASAramStream, JAISound,
JAIStreamMgr, parameter, handle, and list implementation. It starts a real
JAIStream with the title sound ID `0x02000001`, attaches a real JAISoundHandle,
locks preparation, and drives the actual `prepare_()` state machine.

Only external dependencies are substituted: deterministic queued task dispatch,
OS message queues/interrupts, ARAM allocation/transfers, subframe registration,
and DVD access. The DVD seam preserves an existing FST entry when the synthetic
host file is absent, then performs a real POSIX open/read. It is not a test of
the port's DVD implementation; that contract is independently covered by the
DVD diagnostics fixture. Playback channel entry points abort if reached: these
tests must finish at locked-ready, never play audio.

## Expected Distinctions

| Case | JAI prepare state | Attached | Prepared | Header error latch | Reads |
| --- | --- | --- | --- | --- | --- |
| Valid synthetic stream | 3 | 1 | 1 | 0 | 3 |
| FST entry exists, host file absent | 2 | 1 | 0 | 1 | 1 failing read |
| New sound, file restored, error retained | 2 | 1 | 0 | 1 | 0 |
| Rejected DVD entry control | 1 | 1 | 0 | 0 | 0 |
| Valid stream after fresh-boot reset | 3 | 1 | 1 | 0 | 3 |

The missing-file case executes 4,096 additional production preparation updates.
It remains attached and unprepared, with 16 periodic prepare-count reports and
no prepared callback. Creating a new sound after restoring the file still
cannot prepare it: the retained global error prevents even another header read.
The recovery case resets the process-global DVD error
latches, as a fresh audio-system boot does; it is not evidence of hot recovery
after replacing a file in a running process.

`JASAramStream::headerLoad()` sets `UNK_BOOL_B` and returns on a negative header
read. `headerLoadTask()` discards that return and does not send the successful
prepare-finish callback. `JAIStream::prepare_prepareStream_()` consequently
stays in state 2. `AudSingleBgm::isPreparedPlay()` uses the tested attached
handle's `isPrepared()` predicate; `TitleSequenceProduct::exeBgmPrepare()`
requires that predicate before LogoFadein, which publishes title visibility.
The BGM wrapper and title scene are source-traced here, not executed by this
fixture.

## Full-Game Diagnostic Boundary

A separate existing-desktop-binary replay can retain the real FST and remove
only `files/AudioRes/Stream/SMG_title_strm.ast` from an isolated data clone, with
original data read-only and private saves/cache/output. Compare normal data,
missing-file data, then normal data again using the same executable and launch
environment. Do not use an empty file: the current DVD worker pads short reads,
so a zero header exercises unrelated invalid-header arithmetic instead.

Missing title audio predicts no title-visible hook while game turns may still
advance. The shader warm-up circular wait is different: title visibility has
already been published, pending shaders remain, and the warm budget is zero
under conflicting hold/after-title options. The Linux desktop host has no Vita
splash controller, so desktop captures cannot reproduce its overlay pixels.
