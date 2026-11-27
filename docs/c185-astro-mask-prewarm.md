# C185 Observatory Mask Prewarm

## Scope

`GXV_C185_ASTRO_MASK_WARM=1` adds exactly one targeted resource-plan item for
AstroGalaxy in Performance/60. It is not a global mask-prewarm policy. The runtime
must have HRW5, depth mode4, a successfully built mask program and a successfully
built D41 or keyed-box consumer, with the stage allowing HRW, no multisampling or
overscan, and a valid picture size. The size is derived from the current picture
and mask divisor, not hardcoded to 240x136. `GXV_PREWARM_RES=0` disables it too.

The mask item precedes generic guesses under the existing item count, byte cap,
known-VRAM reserve and closed-screen checks. Its first clear realizes the native
render target before play. No explicit depth attachment is requested; the
existing copy-target budget still conservatively includes native aligned depth
storage. At 960x544/div4 the texture is 130,560 bytes and the conservative budget
is 294,400 bytes, including 163,840 bytes of aligned depth. Native target metadata
is not included in that estimate.

Only MaskSnap can consume the reserved target. Generic texture takes and generic
FBO adoption cannot steal it. MaskSnap transfers its texture rather than making
a duplicate and never takes a DRS rung spare. An already-owned mask of the exact
dimensions satisfies the plan on later visits without a second allocation.
Unused targeted storage is canceled if stage, picture-size key or eligibility
changes; cancellation and expiry release PRIME/keepUntil before retiring it.
The ordinary pool owns any remaining idle FBO under its existing trim policy.

After the reserved spare is consumed or absent, live presents return before the
eligibility settings lookups or spare scans. Failures retain MaskSnap's original
on-demand allocation path and do not change the rendered image.

## Evidence And Limits

The C184 offline Observatory route made MaskSnap240x136 and SceneSnap960x544 at
3.8s after opening, with 36ms charged to target creation in a 37ms backend frame.
Its emulator VRAM query returned more free memory than the pool total, so the
existing safety guard intentionally did not pre-create anything. With the same
binary and a QA-only simulated64MiB reading, both generic targets were created
under cover and SceneSnap adopted its target; only the unplanned mask remained
made in play (one target in a 15ms backend frame). These are emulator allocation
and phase receipts, not physical Vita timings or an FPS prediction.

The old census counted only successful builds as "closed-screen frames" and
reported unknown VRAM as "no closed frame left". C185 counts actual admitted
closed-frame steps and reports `VRAM unknown` separately. Unknown VRAM, reserve
failure and allocation failure remove skipped work from the pending hold count
immediately instead of waiting for a hold cap. No shipped VRAM override is added.

fix31 (owner, 17 Nov): the unknown-VRAM half of that rule is behind `GXV_C185_HOLD_UNKNOWN_VRAM=1` and off
by default: with 0 an unknown reading stops the step with the item pending and the hold waits as in C184
(and a step that returns early keeps the last pending count, as in C184). Reserve and allocation failures
were already not pending in C184. The mask prewarm itself stays on (`GXV_C185_ASTRO_MASK_WARM=1`), but it
has a consumer only with `GXV_HRW41_DEPTH=1`, which is off by default until the console A/B
(`docs/RELEASE-3.0-CHECKLIST.md`, section 0b).

This does not eliminate first-use costs for other stages or sizes. It does not
change shader warming, scene rendering, cutout filtering, the HUD, or Mario.

## Verification

`tools/test_c185_astro_mask.sh` exercises the production backend on a GL stub,
including the targeted plan, off/no-mask cases, ownership, cancellation and
failure/hold behavior. The prewarm plan model is also covered by
`tools/test_prewarm_res.sh`, including ASan/UBSan on macOS. Real GLES depth-edge
tests remain a separate gate and do not stand in for target-allocation tests.

For a later explicitly authorized hardware rollback comparison, add
`GXV_C185_ASTRO_MASK_WARM=0` to `ux0:data/smg/env.txt` and fully relaunch. Remove the
line or set it to1 to restore the scoped optimization. Match all other settings,
caches and route. `GXV_AUTOQ_VRAM_KIB` is only a host-QA simulation hook and must
not be added to the shipped or physical-device settings.
