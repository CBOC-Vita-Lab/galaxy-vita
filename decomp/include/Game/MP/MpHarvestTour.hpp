// Galaxy 3.x (harvprep31): the shader harvest's menu tour. Harvest runs only (tools/seedwalk: GXV_SEEDWALK=1 together with SMG_MP_DEMO=menus, the walk's
// "mpmenus" leg); nothing of it runs in the release, where neither variable is set.
//
// The puppet demo's menus tour (MpDemo.cpp) shows the Multiplayer menu, the character select, the gathering screen and the Connection Test. The harvest walk
// records what every screen draws, so on a harvest it then also shows, each once, the Multiplayer menu's other screens that can be shown without their flow,
// and the native Settings' pages (opened the way the entry screen's Settings pill opens them). It never chooses anything, never changes a value (no Left /
// Right on a Settings page, no Cross anywhere: the Settings are not dirty, so closing them writes nothing) and asks nothing of a service: the screens below
// were each checked by tests/mp/ui_menu/harvest_test.cpp to make no service, network, keep-safe, save or settings call in 600 frames without input.
// Pure code (the host tests run it): the caller (MpDemo.cpp) gives the time and the menu.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Game/MP/MpMenuSequence.hpp"

namespace MpHarvestTour {

// The Multiplayer menu screens shown with Sequence::demoShow (a screen without its flow) beyond the demo's own. Not here, and why:
//   S_FRIENDS, S_REQUESTS, S_REQUEST, S_BLOCKED, S_PLAYED, S_BOARD: their screen signs in to the online service (tickFriends: SignIn, requestBoard);
//   S_BUSY, S_RPWAIT: they poll a request that is under way (a session, a report, a check); S_CONNTEST, S_GATHER, S_CHARS, S_MAIN: the demo's own tour;
//   the S_SET_* pages: below, with the Settings open.
inline constexpr MpMenu::Scr kMpScreens[] = {
    MpMenu::S_SAVE, MpMenu::S_SLOT, MpMenu::S_SPFILE, MpMenu::S_FILEUSE, MpMenu::S_KEEP, MpMenu::S_KEEPLIST, MpMenu::S_KEEPCOPY,
    MpMenu::S_FALLS, MpMenu::S_HELP, MpMenu::S_REPORT, MpMenu::S_SESSION, MpMenu::S_MESSAGES, MpMenu::S_WARP, MpMenu::S_PLAYERS, MpMenu::S_PLAYER,
};
// The Settings' pages, shown over the Settings' main page (S_SET_MAIN, where the Settings open). Not S_SET_CAPTURE ("Press the button you want": it binds the
// first button held) and not S_SET_PICK (its button list is made by the Cross on a move, which the tour never presses).
inline constexpr MpMenu::Scr kSetScreens[] = {
    MpMenu::S_SET_CAMERA, MpMenu::S_SET_CONTROLS, MpMenu::S_SET_BUTTONS, MpMenu::S_SET_MORE, MpMenu::S_SET_LEAVE,
};

// A walk over one of the lists: each screen is pushed (demoShow), held for holdMs, then popped (demoBack). `in` is cleared every turn: the tour presses nothing.
// The screen actually on top is reported to `shown` (a screen whose own tick pops it at once is reported as not reached). Returns true when the list is done.
struct Walk {
    int i = -1;            // the list entry on screen (-1: none yet)
    int64_t at = 0;        // when it was pushed (or when the walk started)
    int depth = 0;         // the menu's depth before the push
    bool started = false;
    unsigned reached = 0, missed = 0;
    template <size_t N>
    bool step(MpMenu::Sequence& seq, MpMenu::Input* in, const MpMenu::Scr (&list)[N], int64_t nowMs, int64_t holdMs, bool* shown = nullptr) {
        *in = MpMenu::Input();
        if (!started) { started = true; at = nowMs; i = -1; }
        if (nowMs - at < holdMs) {
            if (i >= 0 && shown && seq.screen() == list[i]) shown[list[i]] = true;
            return false;
        }
        if (i >= 0) {   // the shown screen goes (only when it is still the top one: a screen that left by itself is not popped twice)
            if (seq.screen() == list[i] && seq.depth() == depth + 1) { seq.demoBack(); ++reached; } else ++missed;
        }
        if (++i >= static_cast<int>(N)) return true;
        depth = seq.depth();
        seq.demoShow(list[i], 0);
        at = nowMs;
        return false;
    }
};

}  // namespace MpHarvestTour
