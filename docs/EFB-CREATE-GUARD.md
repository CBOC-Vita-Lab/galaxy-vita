# EFB Name Exhaustion Guard

`GXV_EFB_CREATE_GUARD=1` opts into rejecting exhausted colour-texture or
framebuffer names before a new EFB target replaces the current one. Unset or
zero retains the previous path. The switch is read once by `EnvInt`, normally
during renderer initialization.

## Confirmed Failure

The native GL name generators can leave their output unchanged when their fixed
slot tables are full. `CreateTarget` initialized those names to zero but did not
check them. During a dynamic-resolution change, it could publish framebuffer
zero, clear the display framebuffer, and report success. Later quality changes
then returned early because there was no EFB framebuffer. Texture-name failure
could likewise publish colour texture zero.

The guard refuses a zero colour name before texture binding/storage calls and
reserves a required framebuffer before changing dimensions or retiring the old
target. Failed reservation retires only the temporary colour. The existing
`ApplyRung` failure path restores a stashed target and the old quality rung;
later changes can retry. Guarded initialization now propagates target failure
instead of continuing with an invalid EFB. A bounded basic-log message identifies
which name was unavailable.

Preallocation temporarily needs one extra framebuffer name while an old target
is still owned. Under name pressure this can refuse a change that destructive
replacement might have accommodated; keeping the known-good picture is preferred.

This is not comprehensive GPU allocation recovery. Depth names/storage, native
lazy depth allocation, and native scene creation remain separate paths. It does
not establish that slot exhaustion occurred in any reported player freeze.

## Host Test

Run `bash tools/test_vgx_efb_create_guard.sh`. It requires the existing local
`codex-galaxy-native:20261007-arm64` image and runs offline with software GLES,
ASan and UBSan. The test compiles the actual Vita-conditional backend. Device
queries are stubs; GL name-generation failure is injected. This is not a native
driver build or a physical GPU allocation test.

The 36 processes cover unset/OFF/ON, both render-target lifetimes, failed colour
or framebuffer names, successful quality changes, startup refusal, and both
read-once directions. Guarded failures retain target identity, dimensions, rung,
and a real red pixel without clearing the default framebuffer; subsequent
allocation recovery succeeds. OFF cases deliberately assert the legacy failure
as a parity control. The same guarded framebuffer case failed before the repair.

## Device Validation

Build the native game with this patch, then compare unset/zero against one on the
same save, route and graphics settings. Exercise repeated resolution changes,
settings return, and the affected water/floor transitions. Compare the picture
and basic log; normal healthy runs should be unchanged. Do not intentionally
exhaust native GPU memory on a player's device. This guard makes no FPS claim.
