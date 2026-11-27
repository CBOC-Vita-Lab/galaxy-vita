// Galaxy 3.0 multiplayer: shows MpPanel scenes with the game's own layouts and draws the GX plans over them (owner: ui).
// Game side (LayoutActor, GX, nw4r fonts). System level: created once after the resources are loaded, updated and drawn by
// MpUiSystem from GameSequenceDirector in every scene.
#pragma once

#include "Game/MP/MpPanelPlan.hpp"

namespace MpPanelDraw {
// memfix: the panel's layouts are scene-scoped groups (MpUiLayouts.hpp): MpUi registers them once at boot; nothing is created here.
void registerLayouts();
bool fonts(MpPanel::FontMetrics out[MpPanel::FONT_COUNT]);   // false: the fonts are not there yet
MpPanel::Screen screen();
// Once a frame: the layouts follow the scenes (appear / disappear with their animations, texts, places, the selected button),
// then their nerves and animations run.
// `strip` is the party strip beside the life counter (its heads; the Power Star flashes are in its plan).
void update(const MpPanel::Scene& menu, const MpPanel::Scene& toasts, const MpPanel::Scene& strip);
// The menu (frame, windows, buttons, its GX, heads), then the toasts (windows, GX).
void draw(const MpPanel::PlanBase& menuPlan, const MpPanel::PlanBase& toastPlan, const MpPanel::PlanBase& stripPlan);
// MP3 heads: triangles and words of the 2D pass outside a plan (the 3D heads' badges). Vertices in square units, as a plan's.
void drawMini(const MpPanel::Vertex* vertices, int vertexCount, const MpPanel::TextRun* runs, int runCount, const char16_t* text, int textCount);
// The star pointer is over a big button: its slot (0..3), else -1.
int pointedButton();
bool allDead();   // (perf) no layout of ours is alive or closing: nothing to run or draw
bool onlyHudSteady();   // (perf) only steady in-play HUD heads are alive (no menu/toast/closing animations): safe to skip update on undrawn turns
void tickSkipped();   // advances sShimmerFrame on undrawn turns so Cosmic HUD shimmer stays at 60 Hz in 30 fps modes
}  // namespace MpPanelDraw
