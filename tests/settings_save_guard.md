# Running Settings Save Guard

`GXV_SETTINGS_SAVE_GUARD=1` opts into deferring a running-settings request until
the game's existing save/reset-readiness query permits it. The value is read
once on the first otherwise eligible settings request; unset or zero preserves
the previous path without querying the save state. Settings itself must also be
enabled. This does not create an automatic save or preserve otherwise unsaved
gameplay.

The request stays pending and the game continues normally until ready. The
guard runs before any settings lease is acquired: freezing guest interrupts
first would prevent the save sequence from advancing. An outstanding native
write/close/fsync may finish on the helper thread, but the guest sequence must
also acknowledge completion before settings may take ownership.

Run `bash tools/test_settings_save_guard.sh [output-directory]` for the focused
ASan/UBSan host test. It compiles mechanically extracted production code for
the native boundary/lease/restart path, the game save-readiness query, and the
NAND manager write/close path. Device APIs, scheduler delivery, native file
syscalls, host-job dispatch and error readiness are fixtures. The test drives
the save-state transitions explicitly; it does not execute the real handler,
banner or error-sequence transition chain, nor prove native scheduler races
absent. Synthetic save operations use no file paths or files on disk.

The test covers disabled/unset/zero/invalid values, enabled behavior, both
read-once directions, save and preload states, idle-to-save transitions, write,
fsync and close still in progress, success/error completion, pending request
retention, failed acquisition, failed restart and a missing save sequence.
This is host correctness evidence, not a native build, device reproduction,
storage durability guarantee, or physical-Vita acceptance result.
