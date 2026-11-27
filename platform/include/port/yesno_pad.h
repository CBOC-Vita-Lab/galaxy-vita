#pragma once
// setfix31: the pad on the game's Yes / No window (YesNoController), for the windows the multiplayer menus and the in-game Settings open
// (MpUiSystem's SysInfoWindow: "Start the game again now?", Put Everything Back, a button swap, ...). The game's window answers only the star
// pointer (point at a button, then A), so a player with only buttons could not answer it (emulator run emu31b, N1). Pure logic, host-tested
// (tests/yesno_pad.cpp); the wiring is in decomp/src/Game/Screen/YesNoController.cpp (TARGET_PC).
//   - The focus starts on No (the left button): the safe answer.
//   - Left / Right move the focus to No / Yes.
//   - The pointer, when it comes onto a button, moves the focus there (as before: pointing is still how a touch or the pointer answers).
//   - Cross (A) answers the focused button; Circle (B) answers No.
namespace port::yesno {

enum : int { kNone = -1, kNo = 0, kYes = 1 };

struct PadIn {
    bool left = false, right = false, decide = false, back = false;
    int pointed = kNone;   // the button under the pointer now: kNo, kYes or kNone
};
struct PadState {
    int focus = kNo;
    int lastPointed = kNone;
};

// One frame. Returns kNone (no answer yet), kNo or kYes.
inline int Step(PadState& s, const PadIn& in) {
    if (in.pointed != kNone && in.pointed != s.lastPointed) s.focus = in.pointed;   // (the pointer came onto a button)
    s.lastPointed = in.pointed;
    if (in.left) s.focus = kNo;
    else if (in.right) s.focus = kYes;
    if (in.back) return kNo;
    if (in.decide) return s.focus;
    return kNone;
}

}  // namespace port::yesno
