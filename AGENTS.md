# Galaxy Vita packaging

Keep the five contributor-supplied PNGs in `vita/vgx-game/sce_sys/` as the
default Galaxy Vita bubble and LiveArea. The contributor offered free use in
this game according to the owner on 2026-11-17. Provenance and archive hash:
`vita/vgx-game/sce_sys/ARTWORK.md`. Do not regenerate them during a Claude,
Codex or release handoff. Rebuild the SFO and use this branch's template.
Keep title captures for in-game use separate from shell artwork.

For C186, general shader coverage, pop-in policy and performance tuning belong
to the integration branch. Do not add game- or scene-specific shader recipes
or special-case hashes in the renderer. Measure first use only with retail
vertex outputs (`GXV_VSOUT=1`) and the unmodified pack policy. Label the evidence
as host, emulator/retail, or console/retail. Keep speculative visual changes
off until a matched console comparison passes. Do not bind L+R+Select.

Before modifying a console, back up saves and settings; follow
`docs/console-tests-C186.md`. Keep unpublished VPKs local. Do not push, deploy
or replace the player's cache as a side effect of building or testing.
