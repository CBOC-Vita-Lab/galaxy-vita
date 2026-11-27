// Galaxy 3.0 multiplayer: internal interface between the puppet files (owner: puppet). Not for other modules: they use
// MpPuppets:: in MpGame.hpp.
#pragma once

#include "Game/MP/MpGame.hpp"
#include "Game/MP/MpPoseMath.hpp"
#include <JSystem/JGeometry/TVec.hpp>

class LiveActor;
class MarioActor;
class MpPuppetBase;
class XanimeResourceTable;

// MP_BUMP_POP: a pop request for the target's bubble (spin / Star Bit; MpBubbleLogic readPopRequest, handled by MpBubble)
enum MpBumpKind : uint8_t { MP_BUMP_STOMP = 1, MP_BUMP_SPIN = 2, MP_BUMP_PUSH = 3, MP_BUMP_POP = 4 };
enum MpPuppetStyle { MP_PUPPET_SOLID = 0, MP_PUPPET_GHOST = 1, MP_PUPPET_HIDDEN = 2 };

namespace MpPuppetsInt {
    uint32_t turn();                                  // puppet clock: logic turns counted by MpPuppets::update
    uint32_t pictures();                              // (perf) pictures drawn with puppets (MpPuppetDrawer): the far LOD's phase
    void notePicture();
    MarioActor* localActor();                         // null outside a game scene / before Mario exists
    MpPuppetBase* puppet(int slot);                   // null when none (only valid while the scene lives); the local slot
                                                      // holds the self puppet when our character is not the MarioActor's
    MpSampleResult samplePose(int slot, MpPoseF* pOut);
    const MpAnimNameMap& names();                     // resource-name hash -> name (MarioAnime motion table)
    const char* groupBckName(uint32_t groupHash);     // first bck of a Mario animation group (null when unknown)
    XanimeResourceTable* xanimeTable(bool luigiSwap);  // Mario's animation tables, with or without the Luigi swap
    bool localInFlyInOrDemo();                        // wave8: true during opening/starter/demo/bind
    bool pushHeld(uint16_t remoteFlags);              // mppause: the arrival / demo window: the bodies do not push (MpCoopLogic::pushHeld)
    bool interactionOn();                             // Together and setting != off
    int style();                                      // MpPuppetStyle (race rivals setting; solid in Together)
    void sendBump(int targetSlot, uint8_t kind, const TVec3f& rDir);
    uint16_t localSquashTurns();                      // our own cosmetic squash after being stomped (sent in STATE flags)
    void logOnce(int slot, const char* what, uint32_t value);
    void nameTagsEndScene();          // MpNameTag.cpp: forget the plates (scene objects)
    int spareCount();                 // drop-in spares this scene made (0 outside a scene)
    void warmPuppets(int variant);     // MpWarm: every pool puppet draws once out of sight (0 solid, 1 see-through list)
    bool arrivalHidden();             // we dropped into this scene and are not beside the others yet (our STATE says invisible)
}

// Drop-in arrival on the joiner's console (MpPuppets.cpp). By default a player who drops into a scene the others already play
// is hidden from them until it can warp beside the host (or, host unavailable, the first player found) through
// MpFlowHooks::requestWarpTo. A module that wants another arrival (the bubble agent: arrive inside a bubble) registers a
// handler: it is called once, at the first safe turn (Action nerve, no demo, alive), with the slot to arrive beside; true =
// it took the arrival over (our STATE is visible again from that turn; no warp), false = the default warp.
namespace MpDropInHooks {
    typedef bool (*ArrivalFn)(int targetSlot);
    void setArrivalHandler(ArrivalFn fn);   // null = default
}

namespace MpLocalCapture {
    // Fills a pose of the local player. False when there is no player to capture.
    bool capture(MpPoseF* pOut, uint32_t turn);
    void resetScene();
}

namespace MpPuppetDrawerFn {
    void create();
}

namespace MpNameTags {
    void create();
    void update();
    bool warm(bool on);               // MpWarm: one plate shows the greeting hand / the name off screen (on), then goes (off); false: none
    int shown();                      // MP3 demo: plates on screen now (the demo's log)
}
