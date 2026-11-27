// Galaxy 3.0 multiplayer: the 3D character heads (owner: heads). One head per character, drawn in 3D with the file select's own
// models and motion; the character select uses them, and so can every other screen that shows a player as a head (the gathering
// screen, the "played with" list, the party strip).
//
// ---- THE ONE CALL
//   MpHead3D::Spec s;                       // (defaults: a free Mario at the origin)
//   s.look = look; s.x = x; s.y = y; s.size = size;          // MpLook; the head's centre and box height, square units (y down)
//   s.owner = owner; s.mine = (owner == localSlot);          // -1 free, 0..3 taken by that slot: a badge "1P".."4P" on the head
//   MpHead3D::show(slot, s);                // every frame the head is on the screen, between beginFrame() and run()
//   MpHead3D::shake(slot);                  // (optional) a "no" shake: the player chose a head somebody else has
// That is all: the pop-in, the pop-out, the spin, the growth when pointed, the eyes and the badge follow from the state.
//   slot       this head's place (0 .. kSlots-1). A place keeps its head's motion from frame to frame; give every head on screen its
//              own place and keep it while the head stays (a gathering screen: the player's slot number).
//   look       which character (MpLook: Mario, Luigi, Toad, Cosmic Mario, Yellow Toad, Cosmic Luigi).
//   owner/mine free / taken by slot N / the local player's. A head keeps its full colour and its spin in every state; a taken or
//              owned head wears the owner's badge in the character's colour (MpHead3DLook.hpp).
//   selected   the cursor (or the pointer) is on it: it grows to 1.2 and turns to face the front (FileSelectItem's pointed motion).
//   dimmed     away / quiet: the whole head at 45 %.
//   arriving   a player joined: the head pops in from nothing (it overshoots, settles) with the game's appear sound, once. Set it on
//              the frame the player appears; a head that is first shown with the screen itself pops in without a sound.
//   leaving    a player left: the head shrinks away (8 frames) although the place is still being shown; or simply stop calling
//              show() for the place: it shrinks away by itself.
//   restFacing heads at rest face the front and ease back after a flick (the default is the file select's: they never stop turning).
//   alpha      the screen's own fade (0..1).
//
// ---- THE FRAME (MpPanelDraw::update and ::draw do these; another screen only calls show())
//   beginFrame(); show() for every head; run();      (the motion)
//   draw();                                          (the 2D pass: the heads in 3D, then their badges)
//
// ---- MEMORY (the Vita has little: this is what the heads cost)
//   models: SIX actors, one per character, made once at the file select's init in the scene heap and shared by every place: a place
//   is a Slot of about 150 bytes of numbers, not a model. 8 heads = 8 places = 1.2 KB plus the six models (about 0.9 MB of the scene
//   heap with the Cosmic archives; the number is logged at start: "head3d: heads made ... bytes"), the same six models whether 1 or
//   8 heads are on the screen. kSlots (8) heads can be alive at once; two heads of the SAME character share that character's eyes
//   and shimmer (they are drawn twice, turned and sized on their own).
//   No allocation per frame; the badges are drawn with immediate GX (no buffer).
//
// The file select's own heads are what these are made from: the FileSelectData models (Mario, Luigi, Toad, lit by the file select's
// light), turning the way FileSelectItem turns them (0.5 degrees a frame at least, a flick from the star pointer decays by 0.98 a
// frame), growing to 1.2 when pointed (its ScaleController: 30 frames), turning to face the front (turnToFront, 40 frames,
// squared), blinking (its BlinkController). Cosmic Mario / Cosmic Luigi are GhostMario / GhostLuigi, the Cosmic race's models:
// the head's shapes with the game's own materials (the starry window texture and its scroll, the noise rim, the register colours
// of its brk); Yellow Toad is the Toad head with the spot of its cap read as yellow (a TEV mask, as Kinopio's colorchange.brk
// frame 4 colours the NPC).
//
// The heads are actors of the file select scene (made at FileSelector::init in the scene heap, forgotten at the scene's end), run
// and drawn by hand only while a screen shows them: nothing per frame otherwise. Drawn in the 2D pass (MpPanelDraw::draw) with their
// own perspective camera over a depth-cleared picture. Outside the file select (no heads made) the layout's 2D heads stay.
#pragma once

#include <stdint.h>

namespace MpHead3D {

constexpr int kSlots = 8;   // heads alive (shown or shrinking away) at once
constexpr int kLooks = 6;   // characters (MpLook)

struct Spec {
    int look = 0;
    float x = 0.0f, y = 0.0f, size = 90.0f;   // the head's centre and box height, in square units of the picture (MpPanel::Screen, y down)
    int owner = -1;                           // -1 free; 0..3 the slot that has this character (a badge "1P".."4P")
    bool mine = false;                        // that slot is the local player
    bool selected = false;
    bool dimmed = false;
    bool arriving = false;
    bool leaving = false;
    bool restFacing = false;
    float alpha = 1.0f;
};

// SceneDataInitializer: the archives the file select does not mount itself (GhostMario, GhostLuigi), loaded with the scene's files.
void addArchives(void* collector);
// FileSelector::init (scene init, scene heap): the six heads. No-op with the multiplayer menu off.
void createForFileSelect();
// MpGame::onGameSceneEnd: every pointer into the scene is dropped.
void sceneEnd();

// One frame: begin, then show() per head shown, then run().
void beginFrame();
void show(int slot, const Spec& spec);
// The place's head shakes its head "no" (a taken head was chosen). Over after MpHead3DLook::kShakeFrames frames.
void shake(int slot);
void run();

// Whether the 3D head of this slot replaces the layout's 2D head this frame (made, and wanted or still shrinking away).
bool drawsSlot(int slot);
bool anyShown();
// The 2D pass: the heads, then their badges.
void draw();

// What one place is doing now (tests and logs): 0 nothing; else the head's size factor (pop x growth) in 1/100.
int debugScale(int slot);

}  // namespace MpHead3D
