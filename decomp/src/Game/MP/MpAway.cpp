// Galaxy 3.0 multiplayer: the local player's away state (touches). See MpAwayLogic.hpp for the rules.
#include <math.h>

#include "Game/MP/MpAway.hpp"
#include "Game/MP/MpAwayLogic.hpp"
#include "Game/MP/MpFlow.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"

namespace {
MpAwayLogic::State sState;

bool anyInput() {
    if (MR::testCorePadTriggerAnyWithoutHome(0)) return true;
    if (MR::testCorePadButtonA(0) || MR::testCorePadButtonB(0) || MR::testSubPadButtonZ(0) || MR::testSubPadButtonC(0)) return true;
    return fabsf(MR::getSubPadStickX(0)) > 0.25f || fabsf(MR::getSubPadStickY(0)) > 0.25f;
}
}  // namespace

namespace MpAway {

void reset() { sState.reset(); }

void brb() {
    sState.brb();
    mp_log("away on (phrase)");
}

bool local() { return sState.away; }

void tick() {
    if (!mp_active()) {
        if (sState.away) mp_log("away off (session over)");
        sState.reset();
        return;
    }
    const bool was = sState.away, wasPhrase = sState.byPhrase;
    const bool playing = MpGameInternal::sceneState() == MP_SCENE_ACTION && MR::isExistMario() && !MR::isPlayerDead() && !MR::isDemoActive();
    sState.step(anyInput(), playing, MpUi::isMenuOpen());
    if (sState.away != was) mp_log("away %s (%s)", sState.away ? "on" : "off", sState.away ? "idle" : (wasPhrase ? "moved after phrase" : "moved"));
}

}  // namespace MpAway
