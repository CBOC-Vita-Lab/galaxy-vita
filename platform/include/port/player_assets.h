#pragma once
// luigi24 (10 Nov): which optional player assets a Mario/Luigi mode needs, as a plain function of pointers (host-testable, no game types).
//
// The game builds Mario's transformation models only when the stage loaded their archives (a stage places the Bee / Ice / Rainbow /
// Spring / Boo item objects, and each item's makeArchiveList asks for its archives): MarioActor::initBeeMario / initIceMario /
// initInvincibleMario / initHopperMario / initTeresaMario return early when an archive is not loaded and leave the member nullptr.
// Every later use of those members (calcAnim's body switch, the hand models, the wings, the Boo model, the ice steps) assumes the
// mode was only entered when they exist. MarioActor::setPlayerMode is the one entry to every mode (mPlayerMode is written nowhere
// else), so it refuses a mode whose assets are missing with portPlayerModeMissing() before any state is touched.
//
// Modes (MarioActor.hpp PlayerMode_*): 0 normal, 1 invincible (rainbow), 2 fire (no model, a colour animation of the body; needs the three fireball actors _B54),
// 3 ice, 4 bee, 5 hopper (spring), 6 teresa (boo), 7 foo (flying/red star: no model; needs Mario::mFoo), 8 unused, 9 tornado (MarioTornado is a
// stationed archive; mTornadoMario is null-checked at every use).
struct PortPlayerAssets {
    const void* body[6];  // MarioActor::mModels: 0 normal, 1 bone, 2 bee, 3 ice, 4 invincible, 5 hopper
    const void* bee;      // _9E4
    const void* beeWing;  // _9E8
    const void* beeFur;   // _9EC
    const void* ice;      // _9C0
    const void* iceHandL; // _A40
    const void* iceHandR; // _A44
    const void* iceStep;  // _B4C
    const void* rainbow;  // _9C8
    const void* rainbowHandL;  // _A50
    const void* rainbowHandR;  // _A54
    const void* spring;   // _A00
    const void* boo;      // _9A4
    const void* booAnim;  // _9B8
    const void* fireBall[3];  // _B54[0..2] (FireMarioBall, made by initFireBall for every Mario; Fire Mario throws them)  [crash24]
    const void* foo;      // Mario::mFoo (MarioFoo, the flying state: made by the Mario constructor, its _3DC timer is set by setPlayerMode)  [crash24]
};

// Name of the first asset `mode` needs and does not have, or nullptr when the mode can be entered.
inline const char* portPlayerModeMissing(unsigned mode, const PortPlayerAssets& a) {
    switch (mode) {
    case 1:
        if (!a.rainbow) return "rainbow model (_9C8)";
        if (!a.body[4]) return "rainbow body (mModels[4])";
        if (!a.rainbowHandL) return "rainbow hand L (_A50)";
        if (!a.rainbowHandR) return "rainbow hand R (_A54)";
        return nullptr;
    case 3:
        if (!a.ice) return "ice model (_9C0)";
        if (!a.body[3]) return "ice body (mModels[3])";
        if (!a.iceHandL) return "ice hand L (_A40)";
        if (!a.iceHandR) return "ice hand R (_A44)";
        if (!a.iceStep) return "ice steps (_B4C)";
        return nullptr;
    case 4:
        if (!a.bee) return "bee model (_9E4)";
        if (!a.body[2]) return "bee body (mModels[2])";
        if (!a.beeWing) return "bee wings (_9E8)";
        if (!a.beeFur) return "bee fur (_9EC)";
        return nullptr;
    case 2:
        // crash24: Fire Mario's support objects. initFireBall makes them for every actor, so this never refuses in a normal run; it keeps a
        // fire form from being entered on an actor that lacks them (shootFireBall indexes _B54 without a null test).
        if (!a.fireBall[0] || !a.fireBall[1] || !a.fireBall[2]) return "fire balls (_B54)";
        return nullptr;
    case 7:
        if (!a.foo) return "flying state (mFoo)";
        return nullptr;
    case 5:
        if (!a.spring) return "spring model (_A00)";
        if (!a.body[5]) return "spring body (mModels[5])";
        return nullptr;
    case 6:
        if (!a.boo) return "boo model (_9A4)";
        if (!a.booAnim) return "boo animation (_9B8)";
        return nullptr;
    default:
        return nullptr;
    }
}

// relfix24 (10 Nov): the item codes MarioAccess::changeItemStatus takes (what a pickup passes to MR::changePlayerItemStatus) and the player
// mode each one asks setPlayerMode for. -1: the code is not a mode change (9 tornado, 10 life-up, anything unknown). The pickup code asks
// first (MR::canChangePlayerItemStatus) so that a refused change leaves the item where it was; this table is its half that needs no game types.
inline int portPlayerItemMode(int item) {
    switch (item) {
    case 0:
    case 8: return 0;  // cure
    case 1: return 5;  // spring (hopper)
    case 2: return 4;  // bee
    case 3: return 6;  // boo (teresa)
    case 4: return 3;  // ice
    case 5: return 2;  // fire
    case 6: return 7;  // flying (red star)
    case 7: return 1;  // rainbow (invincible)
    default: return -1;
    }
}

// Would a pickup of `item` change the form, given the player's current mode and assets? The same rule MarioActor::canEnterPlayerMode applies
// to the real actor (a mode the player is already in is accepted: setPlayerMode only refreshes its timer).
inline bool portPlayerItemAccepted(int item, unsigned currentMode, const PortPlayerAssets& a) {
    const int mode = portPlayerItemMode(item);
    if (mode < 0 || (unsigned)mode == currentMode) {
        return true;
    }
    return portPlayerModeMissing((unsigned)mode, a) == nullptr;
}

// beefix25 (11 Nov): the pure parts of the late Bee initialisation (after community PR #34 by Arfatnaik0, CBOC-Vita-Lab/galaxy-vita), as plain
// functions so that tests/bee_late_init.cpp can cover them without game types.

// The Bee body archive of the player (MarioActor::initBeeMario, the retry, the stage-start line, MorphItemObjNeo::makeArchiveList). The wings
// archive "BeeMarioWing" is shared by both players.
inline const char* portBeeArchiveName(bool luigi) {
    return luigi ? "BeeLuigi" : "BeeMario";
}

// Should canEnterPlayerMode try initBeeMario again? Only when the retry is on, a try is left, NOTHING of the Bee exists yet (initBeeMario
// returns before it makes anything when an archive is missing, and makes body and wings together after the checks; a state with only one of
// them cannot be rebuilt by it and is refused as it is) and both archives are loaded now.
inline bool portBeeRetryWanted(const PortPlayerAssets& a, bool beeArchiveLoaded, bool wingArchiveLoaded, int retriesLeft, bool retryOn) {
    return retryOn && retriesLeft > 0 && !a.bee && !a.beeWing && !a.body[2] && !a.beeFur && beeArchiveLoaded && wingArchiveLoaded;
}

// The Bee eyelid textures (MarioActor::initBeeTextureSwap): four consecutive textures from "mario_eyeLid.0". `found` is the index of that name in
// the Bee model's texture table or -1; `fallback` is the normal body's index (the value the original code carried over: the community
// version used 0, which can point at an unrelated texture).
inline unsigned portBeeEyelidFirst(int found, unsigned fallback) {
    return found >= 0 ? static_cast< unsigned >(found) : fallback;
}

// May createTextureDL read the four textures first .. first+3? They must exist in the Bee model's own table (formTextureNum) and inside the
// scratch table _B64 (tableSize entries: it was allocated from the NORMAL body's texture count, not the Bee's).
inline bool portBeeEyelidFits(unsigned first, unsigned formTextureNum, unsigned tableSize) {
    return first < formTextureNum && first < tableSize && formTextureNum - first >= 4 && tableSize - first >= 4;
}
