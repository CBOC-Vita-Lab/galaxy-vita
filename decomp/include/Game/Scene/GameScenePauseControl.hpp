#pragma once

#include "Game/System/NerveExecutor.hpp"

class GameScene;
class PauseButtonCheckerInGame;

class GameScenePauseControl : public NerveExecutor {
public:
    GameScenePauseControl(GameScene*);

    virtual ~GameScenePauseControl();

    void registerNervePauseMenu(const Nerve*);
    void requestPauseMenuOff();
    void exeNormal();
    bool tryStartPauseMenu();
#ifdef TARGET_PC
    bool tryStartPauseMenuMp();  // MP3: the session menu in front of tryStartPauseMenu
#endif

    GameScene* mScene;                        // 0x08
    PauseButtonCheckerInGame* mPauseChecker;  // 0x0C
    bool mPauseMenuOff;                       // 0x10
    const Nerve* mPauseMenuNerve;             // 0x14
};
