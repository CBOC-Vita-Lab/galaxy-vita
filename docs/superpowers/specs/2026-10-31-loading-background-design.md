# Loading Background Repair

The owner authorized testing and improvements and reported a new blue band behind the loading bar. A fresh device screenshot reproduces the band. The current loader replaces the whole prompt rectangle with rows 58 pixels above it; the live title's cyan horizon makes the copied rectangle visibly different.

Goal: remove the captured START prompt without copying unrelated sky/horizon rows, retain the captured title, and keep the logo/copyright and pixels outside the prompt region unchanged. Keep the real progress bar and its layout unchanged. No game performance, save or control behavior changes.

Approaches: omitting prompt removal leaves text behind the bar; repainting the full rectangle creates another artificial panel; targeted prompt masking plus local same-row interpolation removes glyphs and their outlines while preserving unmasked sky/clouds. Codex chooses the targeted approach under the owner's delegation. Bright neutral clouds within the small prompt region remain a classification limitation.

Implementation: a bounded initialization-only helper in loading_screen.h identifies bright approximately neutral prompt pixels in the existing prompt bounds, expands the mask for the shadow/outline, and interpolates masked runs from neighboring clean pixels on the same row. No per-frame work, extra library, or persistent image mutation. Preserve the raw title capture.

Verification: fail first on the existing donor-strip algorithm using a clean cyan/vertical-gradient fixture; require unchanged unmasked background, local removal of synthetic white text with shadow, stable boundary gradients, unchanged logo/copyright/outside pixels, and existing neutral/progress tests. Render the owner's local capture into a local preview to inspect the actual reported band. Do not add that capture to git. Physical acceptance requires a separately built test candidate, not only host screenshots.
