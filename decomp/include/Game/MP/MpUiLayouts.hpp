// Galaxy 3.0 multiplayer: WHEN and WHERE the multiplayer UI's layouts are created (memfix). Pure code: no game, no SDK.
//
// Before: MpUi::createSystem made all 38 panel layouts (frame, buttons, windows, heads, characters, pointer, arrows, scroll) and the
// SysInfoWindow at boot, in the stationed Napa heap (9 MiB, never freed), even when multiplayer is never used.
// Now every piece of layout-based UI belongs to a GROUP in this registry, and a group is created in the SCENE heap, in a scene that
// needs it, and dies with that scene (the scene heap is solid and freed wholesale; the group's `forget` only drops the pointers):
//   KIND_MENU     the file select scene: created when the first thing of ours has to be shown (the Multiplayer menu opens, a toast,
//                 a dialog). Never opened = nothing created. The scene heap is nearly empty there; still the free size is checked.
//   KIND_SESSION  a game scene that starts with a session: created at scene load (scene init thread), never mid-play.
//   KIND_SETTINGS a single-player game scene in which the pause menu opened the native Settings (settingsnative): the panel and the dialog only,
//                 created on the main thread at the first open (EnsureLayouts), refused safely when the scene heap has no room.
//   KIND_NONE     any other scene (single player; intermission; logo): nothing is created and nothing can be shown.
// A group says which kinds want it (`kinds`). A new screen with layouts of its own adds ONE group (MpPanelDraw.cpp registerLayouts()
// is the model): its `create` makes the layouts with `new` (the current heap is the scene heap while it runs) and its `forget` nulls
// the pointers. Nothing in a group's `create` may be called from anywhere else.
//
// The allocation rule: a game-side `new` that fails is a crash (OSPanic), so `ensure` first compares the scene heap's free bytes with
// the groups' budgets plus a reserve, and refuses (R_NO_MEMORY) when they do not fit: the caller then shows nothing and says so once.
#pragma once

#include <stdint.h>

namespace MpUiLayouts {

enum Kind : uint8_t { KIND_NONE = 0, KIND_MENU = 1, KIND_SESSION = 2, KIND_SETTINGS = 3, KIND_TOAST = 4 };   // KIND_TOAST: the Settings' refusal (no room for its panel): the frame and the windows only, so its toast can draw

struct Group {
    const char* name;
    uint8_t kinds;               // bit mask: 1 << KIND_MENU, 1 << KIND_SESSION (kMenu, kSession)
    uint16_t layouts;            // how many layouts `create` makes (the log)
    uint32_t bytes;              // budget: bytes of the scene heap `create` takes (measured; with margin) for the free-space check
    void (*create)();
    void (*forget)();
    // integ: the layout archives (LayoutData prefixes: the name given to initLayoutManager) its layouts need. A scene whose layouts are
    // made on the scene init thread must have them MOUNTED before: asked for in the scene's archive-request phase (MpUi::addArchives).
    // `ensure` given a ready test refuses a group whose archive is not mounted (R_NO_ARCHIVE) instead of letting the game build a layout
    // holder on a null archive (a crash). Optional: null / 0 = none (the group's layouts need nothing the scene lacks).
    const char* const* archives;
    uint8_t archiveCount;
};
constexpr uint8_t kMenu = 1 << KIND_MENU, kSession = 1 << KIND_SESSION, kSettings = 1 << KIND_SETTINGS, kToast = 1 << KIND_TOAST;
constexpr int kMaxGroups = 16;

bool add(const Group* group);   // at boot; false when the table is full or the group is already in
int groupCount();
const Group* group(int index);
bool created(int index);

void beginScene(Kind kind);     // a scene starts (the groups of the last scene must have been dropped: endScene)
Kind sceneKind();
void setSceneKind(Kind kind);   // the file select found out it is the menu scene (after beginScene)

enum Result : uint8_t { R_READY = 0, R_CREATED, R_NO_KIND, R_NO_MEMORY, R_NO_ARCHIVE };
typedef bool (*ArchiveReadyFn)(const char* archive);   // true: the layout archive is mounted
// Make the groups this scene's kind wants and does not have yet, if `freeBytes` (the scene heap's free size) covers their budgets
// plus `reserve`. R_READY: nothing to do (all there); R_CREATED: made now (`*layouts` = how many); R_NO_KIND: this scene has none;
// R_NO_MEMORY: refused, nothing created. The caller has made the scene heap the current heap.
// integ: with `ready` (the scene init thread: nothing may be mounted from there), a wanted group with an archive that is not mounted
// refuses everything (R_NO_ARCHIVE, `*missing` = that archive's name); the check comes before the memory check. Without `ready`
// (the main thread: the game mounts a missing layout archive itself while the layout is built) no archive is tested.
Result ensure(uint32_t freeBytes, uint32_t reserve, int* layouts = nullptr, ArchiveReadyFn ready = nullptr, const char** missing = nullptr);
// The distinct archive names the groups of `kind` that are not made yet need, at most `cap` of them (the archive-request phase).
int archivesWanted(Kind kind, const char** out, int cap);
uint32_t bytesNeeded();         // of the groups of this kind still to create
bool ready();                   // the scene's kind has all its groups (false for KIND_NONE)
// SMG_MP_UI_EAGER (measuring, the old behaviour): every group at once, never dropped by endScene.
Result ensureAll(int* layouts = nullptr);
void endScene();                // forget every group made for the scene (except the eager ones) and the scene's kind

}  // namespace MpUiLayouts
