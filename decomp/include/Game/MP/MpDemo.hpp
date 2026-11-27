// Galaxy 3.0 multiplayer: the puppet demo (owner: demo). A local recording aid for the shader pack: it puts everything a second player
// would bring on screen without one (remote players of all six characters and their forms, a bubble, name plates, the party strip,
// another player's pointer, toasts, a card), or tours the multiplayer menus from the title screen. Nothing runs unless
// SMG_MP_DEMO is set (see port/mp_demo.h and share/heavy/PUPPET-DEMO.md); the hooks in game files test enabled() (one int compare)
// and do nothing else while it is off.
//
//   SMG_MP_DEMO=1      the first stage that loads: after the scene starts, canned remote players around the player for SMG_MP_DEMO_SECS
//   SMG_MP_DEMO=menus  the title screen: entry screen -> Multiplayer -> character select -> multiplayer menu -> lobby -> Connection Test
//                      -> back to the title (no input from the player; nothing is chosen, started, sent or saved)
//
// No session is faked: mp_active() stays false, no socket opens, mp_core is not started. The puppets are the real puppets (MpPuppets
// makes and steps them) fed with canned STATE messages through MpPuppets::handle; the names, characters and connection bars of the
// mock players come from the mock roster behind the bridge queries (port/mp_demo.h).
#pragma once

#include "port/mp_demo.h"

namespace MpMenu {
    struct Input;
    class Sequence;
}
namespace MpEntry {
    struct In;
}

namespace MpDemo {
    extern int gMode;           // -1 = not read yet
    extern bool gLive;          // a puppet demo runs in this scene: the modules' gates (mp_active() || live()) let it through
    int readMode();
    inline int mode() { return gMode >= 0 ? gMode : readMode(); }
    inline bool enabled() { return mode() != MP_DEMO_OFF; }   // the flag check of every hook
    inline bool live() { return gLive; }

    // ---- SMG_MP_DEMO=1 (MpGame.cpp hooks)
    void addArchives(void* nameObjArchiveListCollector);   // SceneDataInitializer: the archives of all six characters (and the bubble's)
    void sceneInit();     // MpGame::onGameSceneInitAfterPlacement: after the scene's placement
    void update();        // MpGame::onGameSceneUpdate, every logic turn
    void sceneEnd();      // MpGame::onGameSceneEnd

    // ---- SMG_MP_DEMO=menus
    bool titleConfirm();                             // TitleSequenceProduct: the tour presses START on the title once
    void entryInput(MpEntry::In* in, int state, int cursor);   // MpEntryScreen: the tour's presses on the two big buttons
    bool menuInput(MpMenu::Input* in, MpMenu::Sequence& seq);  // MpUiSystem::updateSystem: true = the tour drives the menu this frame
    bool persistOk();                                // false while the demo runs: nothing of the demo may write mp.cfg / a save
}
