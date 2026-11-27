// Galaxy 3.0 multiplayer: first-appearance warm-up (owner: puppetprep). See MpWarm.hpp.
//
// Schedule (pictures counted from the scene's init, each one a call of MpPuppetDrawer::draw):
//   1  puppets: every pool puppet (present players and drop-in spares) draws once, solid;   bubble, name tag, pointer mark appear
//   2  puppets: once more with the see-through list (Star Race ghosts, drop-in arrival fade, stand-ins);   tag shows the greeting hand
//   3  (the layouts and the bubble are drawn)
//   4  bubble / tag / mark go away again; done: LOADED may be reported
// The scene is NOT drawn while the start barrier holds the closed screen (found with a 600 ms hold that saw 0 pictures), so the
// warm-up cannot run before LOADED: it runs in the scene's first pictures after the start, under the opening wipe (a fade from black:
// GameScene::start openWipeFade), which hides them; the barrier is not delayed. Costs the single-player scene nothing.
#include "Game/MP/MpBubble.hpp"
#include "Game/MP/MpCoop.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpWarm.hpp"
#include "port/mp_bridge.h"
#include <stdlib.h>

extern "C" int64_t port_host_time_ns(void);

namespace {

    bool sActive;          // warm-up running in this scene
    int sPictures;
    int64_t sStartNs;
    bool sBubble, sTag, sMark;   // what is currently up

    void endAll(const char* why) {
        if (sBubble) { MpBubble::warmEnd(); sBubble = false; }
        if (sTag) { MpNameTags::warm(false); sTag = false; }
        if (sMark) { MpCoop::warm(false); sMark = false; }
        const int ms = (int)((port_host_time_ns() - sStartNs) / 1000000);
        mp_log("warm done why=%s pictures=%d ms=%d", why, sPictures, ms);
        sActive = false;
    }
}  // namespace

namespace MpWarm {
    void sceneInit() {
        sActive = false;
        sBubble = sTag = sMark = false;
        if (!MpGame::sessionHeld()) {
            return;
        }
        // (coordinator) SMG_MP_WARM=0 turns the warm-up off: its gain is unmeasured on the Vita, so hardware tests can compare.
        const char* off = getenv("SMG_MP_WARM");
        if (off != nullptr && off[0] == '0') {
            mp_log("warm off (SMG_MP_WARM=0)");
            return;
        }
        sActive = true;
        sPictures = 0;
        sStartNs = port_host_time_ns();
        mp_log("warm begin");
    }

    void sceneEnd() {
        // (the scene's objects go with it: nothing to kill)
        sActive = false;
        sBubble = sTag = sMark = false;
    }

    void onDraw() {
        if (!sActive) {
            return;
        }
        if (!MpGame::sessionHeld()) {
            sActive = false;
            return;
        }
        sPictures++;
        switch (sPictures) {
        case 1:
            MpPuppetsInt::warmPuppets(0);
            sBubble = MpBubble::warmBegin();
            sTag = MpNameTags::warm(true);
            sMark = MpCoop::warm(true);
            mp_log("warm pictures=1 bubble=%d tag=%d mark=%d", sBubble ? 1 : 0, sTag ? 1 : 0, sMark ? 1 : 0);
            break;
        case 2:
            MpPuppetsInt::warmPuppets(1);
            if (sBubble) MpBubble::warmTick();
            break;
        case 3:
            if (sBubble) MpBubble::warmTick();
            break;
        case 4:
            endAll("ok");   // (sets sActive false)
            break;
        default:
            break;
        }
    }
}  // namespace MpWarm
