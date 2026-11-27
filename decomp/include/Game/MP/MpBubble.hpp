// Galaxy 3.0 multiplayer: Play Together "Bubbles" falls, New Super Mario Bros. style (owner: bubble). See MpBubbleLogic.hpp for
// the rules. Game side: MpBubble.cpp. Every entry point is a no-op without a session (the hooks test mp_active() first).
#pragma once

#include "port/mp_bridge.h"

class MarioActor;

namespace MpBubble {
    // MarioActor's game-over nerves (exeGameOver, ...Abyss, ...Fire, ...Sink, ...NonStop, ...BlackHole2), before anything of
    // the nerve runs: true = the fall became a bubble (the nerve returns at once; no miss demo, no reload). Only Play Together
    // with the "Bubbles" setting, in normal play; race defeats and falls during a demo stay the game's own.
    bool interceptDeath(MarioActor* actor);
    bool bypassKeepLives();          // MR::decPlayerLeft: true while a fall into a bubble takes its life (the real decrement)
    bool localBubbled();             // the local player floats in a bubble now
    // wave8: current local bubble pose for the bubble-follow camera (feet position, gravity up, velocity); false if not bubbled.
    bool localPose(float pos[3], float up[3], float vel[3]);
    // MP3 spectate: what the local player waits for. 0 not in a bubble, 1 floating (teammate it floats to -> *target, -1 none; its
    // distance from the bubble -> *distance, -1 unknown), 2 out of lives (only a teammate's touch pops it), 3 an arrival bubble (pops by itself).
    int localWait(int* target, float* distance);
    void popInPlace(const char* why);// out of the bubble where it is (a delivered star needs the player), if in one
    // "Arrive in a bubble" (drop-in arrival: MpDropInHooks; warp to a friend: MpFlow): the local player appears in a bubble beside
    // teammate `targetSlot` (-1: the nearest), no life lost; it pops by itself after ~2 s or at a touch. Play Together, either
    // Falls setting. False when it cannot (no such teammate playing, not in play, a demo, no bubble archive, ...): the caller
    // falls back to its own teleport.
    bool arriveInBubble(const char* why, int targetSlot = -1);

    // StarPiece::exeThrow (a Star Bit the local player shot), its flight this turn from `from` to `to` (3 floats each): true when it
    // passes through a teammate's bubble (the pop request is sent; the Star Bit hits it and is gone).
    bool shotThrough(const float* from, const float* to);
    // MpPuppets::handle, a BUMP: true when it is a pop request (kind MP_BUMP_POP; consumed whether valid or not).
    bool handlePopRequest(const MpMsg& m);

    void sceneInit();                // MpGame::onGameSceneInitAfterPlacement (scene init thread): the BigBubble holder
    void sceneEnd();                 // scene teardown: forget the scene's bubbles
    void update();                   // MpGame::onGameSceneUpdate after the flow, every game turn (also once after a session)
    void postCalcAnim();             // wave8: re-pin bubble camera state after MarioActor::calcAndSetBaseMtx() runs updateGroundInfo()
    void handle(const MpMsg& m);     // BUBBLE, RESTART (from MpFlow::handle)
    void onPeerJoined(int slot);     // rejoin: clear seen bit so a bubbled player re-announces to the returning peer
    bool warmBegin();                // MpWarm: one bubble of the pool appears at the camera (drawn once, out of sight); false: none
    void warmTick();                 // ... each picture while it is up: its appear sound (the nerve's first step) is stopped
    bool isWarmBubble(const void* bubble);   // the warm-up bubble (BigBubble::exeAppear makes no appear sound for it)
    void warmEnd();                  // ... and goes (kill, no effect, no pop)
    void addArchives(void* nameObjArchiveListCollector);   // SceneDataInitializer: BigBubble.arc (the bubble's textures)
    // MP3 demo (SMG_MP_DEMO=1, MpDemo.cpp): the remote players' bubbles from the canned poses' bubble flag, without a session.
    void demoStep();                 // once a turn: remoteStep only
    bool demoBubbleShown(int slot);  // that remote slot's bubble is up
}
