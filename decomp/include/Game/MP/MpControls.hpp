// Galaxy 3.0 multiplayer: what every multiplayer action is on the handhelds (PS Vita), in one table. Pure code (no game headers):
// the host test tests/mp/vitabtn_test.cpp and docs/MULTIPLAYER.md / CONTROLS.md are written from it.
//
// How the pieces fit (nothing here draws or reads a button; it says what the other files do):
//  - The game reads Wii buttons (A, B, C, Z, +, -, the D-pad, a shake, the pointer). The port's input layer (vita/runtime/input.cpp,
//    platform/src/input/control_bindings.cpp) turns the Vita's buttons, sticks and touch into those reads; `reads` below is the Wii read
//    the multiplayer code makes, `vita` is what the player presses. The multiplayer code never reads a Vita button itself.
//  - Every glyph the multiplayer screens show is a picture of the game's picture font (MpMsgText::Pic, U+E000 + n in the texts, or a
//    button's `pic` in MpPanel::Model). The handheld UI (platform/src/port/handheld_ui.cpp) draws the Vita's own button pictures over the
//    font's Wii pictures (A, B, C, remote, Nunchuk, "1", "2", -, +, Z), following the player's button settings (ResolveGlyph). So the
//    texts name an action by the picture of the Wii button the game reads for it, and the player sees the Vita button that does it.
//  - Words never name a button when a glyph is shown (and never a Wii control): "Press L or R" became two glyphs.
#pragma once

#include <stdint.h>

#include "Game/MP/MpMessages.hpp"

namespace MpControls {

// The Wii reads (the bit values of the game's WPAD buttons) plus two the Wii has no bit for.
enum Wii : uint32_t {
    W_LEFT = 0x0001, W_RIGHT = 0x0002, W_DOWN = 0x0004, W_UP = 0x0008, W_PLUS = 0x0010, W_B = 0x0400, W_A = 0x0800, W_MINUS = 0x1000,
    W_Z = 0x2000, W_C = 0x4000,
    W_SWING = 0x10000,     // the remote shaken (Square or R on the handhelds)
    W_POINTER = 0x20000    // the Star Pointer on the screen (a finger on the front screen; the right stick while Circle is held)
};

// Pictures of the picture font that stand for the Wii buttons the handheld UI replaces (glyph index = picture + 1; handheld_ui.cpp kCells).
// The test compares this list with that file.
inline constexpr int kReplacedPics[] = {0, 1, 2, 3, 4, 5, 6, 23, 24, 25};
// Pictures of the font that are not controls (the Power Star, Star Bit, "x", coin, Mario's head, the open hand).
inline constexpr int kPlainPics[] = {7, 10, 16, 17, 18, 38};

enum Id : uint8_t {
    A_MOVE, A_VALUE, A_CONFIRM, A_BACK, A_EXTRA, A_SECOND, A_SESSION_MENU, A_QUICK_MESSAGE, A_TYPED_MESSAGE, A_EMOTE, A_POINTER,
    A_WATCH_PREV, A_WATCH_NEXT, A_WATCH_PICK, A_CHARACTER, A_TAB, A_SUPER_JUMP, A_AWAY, A_ROW_TOUCH, A_COUNT
};

struct Row {
    Id id;
    const char* what;     // the action
    uint32_t reads;       // the Wii reads of the game code (OR of Wii); every bit must come out of the Vita input layer
    int pic;              // the picture the screens show for it (MpMsgText::Pic), -1 when the screens show none
    const char* vita;     // what the player presses on a Vita (default settings)
    const char* touch;    // the touch way ("" when there is none)
};

inline constexpr Row kRows[] = {
    {A_MOVE, "Move the cursor", W_UP | W_DOWN, -1, "D-pad up / down, or the left stick", "touch a row, a big button or a head"},
    {A_VALUE, "Change a value, page, galaxy or star", W_LEFT | W_RIGHT, -1, "D-pad left / right, or the left stick", "tap the row"},
    {A_CONFIRM, "Choose / Join / Start / Continue / Yes", W_A, MpMsgText::PIC_CROSS, "Cross", "tap"},
    {A_BACK, "Back / Close / Leave / No / Cancel", W_B, MpMsgText::PIC_CIRCLE, "Circle", "tap Back in the guide band"},
    {A_EXTRA, "The third choice: Add / Retry / Tab / Players / Change character / By address / Details", W_C, MpMsgText::PIC_TRIANGLE, "Triangle",
     "tap it in the guide band"},
    {A_SECOND, "The fourth choice: Remove / Stamps / Kick / Test / Send report", W_SWING | W_MINUS, MpMsgText::PIC_SQUARE, "Square (or R, or SELECT)",
     "tap it in the guide band"},
    {A_SESSION_MENU, "Open / close the session menu in play", W_PLUS, MpMsgText::PIC_START, "START", ""},
    {A_QUICK_MESSAGE, "Quick message (the wheel)", W_MINUS | W_UP | W_RIGHT | W_DOWN | W_LEFT, MpMsgText::PIC_SELECT,
     "hold SELECT, then press a D-pad direction", "START, Stamps, tap a tile"},
    {A_TYPED_MESSAGE, "Typed message (if the host allows it)", W_PLUS | W_A, MpMsgText::PIC_START, "START, Stamps, Write a Message (the Vita's keyboard)",
     "tap the tile"},
    {A_EMOTE, "Say hello (emote)", W_DOWN, -1, "D-pad down", ""},
    {A_POINTER, "Show the others where you point; hold an enemy for Co-Star", W_B | W_POINTER, MpMsgText::PIC_CIRCLE,
     "hold Circle and aim with the right stick", "keep a finger on the screen"},
    {A_WATCH_PREV, "Watch the previous player", W_Z, MpMsgText::PIC_L, "L", ""},
    {A_WATCH_NEXT, "Watch the next player", W_SWING, MpMsgText::PIC_SQUARE, "Square (or R)", ""},
    {A_WATCH_PICK, "Watch a chosen player", W_A | W_POINTER, -1, "", "tap a head of the strip"},
    {A_CHARACTER, "Pick a character", W_A | W_UP | W_DOWN | W_LEFT | W_RIGHT, MpMsgText::PIC_CROSS, "D-pad or left stick, then Cross", "tap a head"},
    {A_TAB, "Next leaderboard tab", W_C, MpMsgText::PIC_TRIANGLE, "Triangle", "tap it in the guide band"},
    {A_SUPER_JUMP, "Super jump on a friend's head", W_A, MpMsgText::PIC_CROSS, "hold Cross while landing", ""},
    {A_AWAY, "Come back from Be right back", W_A | W_B | W_PLUS, -1, "any button", "touch"},
    {A_ROW_TOUCH, "Choose a list row, head or tile without buttons", W_A | W_POINTER, -1, "", "touch it (the tap chooses)"},
};
inline constexpr int kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// The guide band's button pictures and the input each one answers when touched (MpUiSystem).
enum GuideInput : uint8_t { G_NONE = 0, G_OK, G_BACK, G_TRI, G_SQ, G_START /* flow (one door): the gathering's START */ };
inline constexpr GuideInput guideInput(int pic) {
    return pic == MpMsgText::PIC_CROSS ? G_OK : pic == MpMsgText::PIC_CIRCLE ? G_BACK : pic == MpMsgText::PIC_TRIANGLE ? G_TRI :
           pic == MpMsgText::PIC_SQUARE ? G_SQ : pic == MpMsgText::PIC_START ? G_START : G_NONE;
}

}  // namespace MpControls
