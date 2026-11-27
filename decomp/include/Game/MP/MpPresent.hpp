// Galaxy 3.0 multiplayer: the in-play presentation's hooks between modules (owner: ingame). Rules: MpPresentLogic.hpp.
#pragma once

#include <stdint.h>

namespace MpPanel { struct Edge; struct Badge; struct StarRow; struct Prompt; }

namespace MpOverlay {
// The toast rule (MpPresentLogic.hpp: a toast only for what the player cannot see). On in the game; off (the default) every toast
// shows, as the ui host tests expect.
void setPolicy(bool on);
bool policy();
}  // namespace MpOverlay

namespace MpNameTags {
// The edge heads of off-screen players (square units), built by update(); false: none this frame.
bool edgeModel(MpPanel::Edge* out);
// That slot's edge head this picture (square units of the panel); false: none (the player is on screen or not here).
bool edgeOf(int slot, float* x, float* y);
}  // namespace MpNameTags

namespace MpChat {
// A slot's quick-message balloon is up (its name tag shows meanwhile).
bool balloonUp(int slot);
// A greeting (stamp 0..3 = the Messages page's phrases 0..3) as the balloon over that player, no toast.
void greetingBalloon(int slot, int stamp);
}  // namespace MpChat

namespace MpSpectate {
// The watch badge (the watched player's head in a ring in a corner, the L and R pictures); false: not watching.
bool badgeModel(MpPanel::Badge* out);
}  // namespace MpSpectate

namespace MpBubble {
// The button picture over the local player's bubble while their own press pops it; false: none.
bool promptModel(MpPanel::Prompt* out);
}  // namespace MpBubble

namespace MpTeam {
// The game shows its own HUD now (PlayerLeft lives and is not hidden): false in cutscenes, star get, the galaxy's fly-in.
bool hudShown();
// Play Together's Power Star: the row of faces under the game's "You got a Star!" (no card); false: none this frame.
bool starRowModel(MpPanel::StarRow* out);
}  // namespace MpTeam
