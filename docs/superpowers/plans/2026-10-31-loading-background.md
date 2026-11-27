# Loading Background Implementation Plan

> **For agentic workers:** Use test-driven development and a fresh whole-change review. Keep all changes in this isolated candidate.

**Goal:** Remove the blue donor-strip artifact without changing the title artwork or the actual progress bar.
**Architecture:** Initialization-only masked removal of prompt glyphs, filled from clean same-row neighbors. No renderer or game-logic changes.
**Tech Stack:** Existing C++17 header renderer and ASan/UBSan host tests.
**Spec:** ../specs/2026-10-31-loading-background-design.md

## Global Constraints

- No raw title images or game data in git; use the downloaded capture only as local evidence.
- Leave the verified final source/package checkout untouched.
- No per-frame extra work or new library dependencies. Task 1 does not install on-device; Task 2 permits the verified private candidate for physical acceptance. No commit, push or public release.
- Preserve all pixels outside the established captured-prompt bounds, including logo and copyright.

## Review Focus

- Bright cyan horizon must not be misclassified as white prompt text.
- Text shadows/outlines must not remain as dark lettering.
- Runs touching a mask boundary must not read outside the image.
- A title with no visible prompt must not acquire a rectangle.
- Missing/short/oversized captures must retain neutral fallback behavior.

## Task 1: Repair Prompt Removal

Files: platform/vgx/vita/loading_screen.h; tests/loading_artwork.cpp.

- [x] Add clean cyan/vertical-gradient, masked prompt/shadow and boundary regressions; show that existing donor-copy behavior fails the intended continuity/preservation assertion.
- [x] Add a bounded initialization-only prompt-mask removal helper and replace the donor-row copy. Keep progress rendering unchanged.
- [x] Run existing and new loading artwork tests with sanitizers and the downloaded local capture. Keep preview output outside git for inspection.
- [x] Review the diff for false-positive masking, bounds, stack/initialization cost and all preservation requirements.
- [x] Run the repository Python suite; record any source-inventory changes required by deliberate candidate edits without misrepresenting the final release hashes.

## Task 2: Device Acceptance

- [x] Determine whether the local pinned SDK/dependencies can reproduce the candidate build before altering the installed runtime.
- [x] If available, build a clearly labeled candidate, verify its identity/content, preserve rollback material, and test its loading screen on the physical Vita.
- [x] Confirm the real bar remains correct, the donor band is gone and saves/settings are unchanged. No public release.

## Ledger

- Native managed-worktree tool rejected the nested source repository because it is not the top-level chat workspace. Used git worktree fallback at review-1.2-20261031/fixes, branch codex/galaxy-12-gameplay-fixes, based on exact final fc78e5e. Original checkout remains clean.
- Clarified the original no-install constraint as Task 1-only; Task 2 explicitly requires the owner-authorized physical acceptance step.
- Actual-font testing required five-pixel dilation after a three-pixel implementation left shadows. Added a failing thicker-outline regression before refinement. Independent final review found no actionable issue.
- All 1,834 linked game units were compiled as ThinLTO. Native-arm64 LLVM equivalence was established with byte-identical object/bitcode/archive samples; the unused plain archive build was stopped after configuration. Final SDK GCC link remained amd64. No linked game unit omitted.
- Physical loading acceptance: 10,240 pixels around the actual bar match the original captured title exactly (old donor-copy screenshot differs at all 10,240). Title and Observatory smoke tests passed; not an all-galaxy acceptance claim.
- Candidate runtime 12af5e2e, official rollback b2aa158d and all protected game-save/legacy-settings/HD-pack/title-capture bytes were read back and verified. Stage/fixed-rung test overrides removed; character smoothing off is the deliberate final preference change. No-sleep remains enabled as requested.
