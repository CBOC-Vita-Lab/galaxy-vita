#pragma once
// crash24 (10 Nov): the pure half of the PETARI_POWERUP test hook (MarioActorMorph.cpp), host-testable (tests/powerup_hook.cpp).
//
// The hook forces player modes through MarioActor::setPlayerMode "as an item pickup does". A pickup is more than that call: the item can only
// be touched when MarioActor::tryGetItem lets it (MarioActorOffensiveMsg.cpp: Mario alive, isEnableNerveChange, no transformation running
// (_3D8 == 0), mMorphResetTimer == 0), the item's own message handler asks MR::canChangePlayerItemStatus first (relfix24: a form whose assets
// are missing leaves the item), and tryGetItem stores the item's sensor in _4A4 afterwards (the first-transformation camera reads it).
// portPowerupBlocked() lists what the hook can honour of that, so the hook offers a form only when a real pickup could happen.

// PETARI_POWERUP list: the index-th (0-based) comma separated entry as a mode number. False when the list has no such entry (used up) or the
// entry is not a number (an entry such as "x" or an empty one ends the list, as the old code's atoi() == 0 treated it as "cure": now it is
// refused, so a typo cannot silently become mode 0).
inline bool portPowerupEntry(const char* list, unsigned index, unsigned* mode) {
    if (!list) return false;
    const char* e = list;
    for (unsigned i = 0; i < index; i++) {
        while (*e && *e != ',') e++;
        if (!*e) return false;
        e++;
    }
    while (*e == ' ') e++;
    if (*e < '0' || *e > '9') return false;
    unsigned v = 0;
    while (*e >= '0' && *e <= '9') {
        v = v * 10 + (unsigned)(*e - '0');
        if (v > 1000) return false;
        e++;
    }
    *mode = v;
    return true;
}

// Is turn `turn` (1-based turns of play since the actor was made) one the schedule fires on? First at `at`, then every `every` turns.
inline bool portPowerupDue(unsigned turn, unsigned at, unsigned every) {
    if (every == 0) return false;
    return turn >= at && (turn - at) % every == 0;
}

// PETARI_POWERUP_SCENES=n: the list applies to the first n player actors (stages) of the run only; 0 = every one (the default, and what the
// hook always did: each new actor restarts the list). `scene` is 1 for the first actor made.
inline bool portPowerupSceneAllowed(unsigned maxScenes, unsigned scene) {
    return maxScenes == 0 || scene <= maxScenes;
}

struct PortPowerupState {
    unsigned currentMode;   // mPlayerMode
    unsigned health;        // mHealth
    unsigned transformTimer;  // _3D8: > 0 while a transformation (or its end) is running
    unsigned morphResetTimer; // Mario::mMorphResetTimer
    bool nerveChangeEnabled;  // MarioActor::isEnableNerveChange()
    bool formAccepted;        // MarioActor::canEnterPlayerMode(mode)
};

enum PortPowerupVerdict { PORT_POWERUP_OFFER = 0, PORT_POWERUP_WAIT = 1, PORT_POWERUP_DROP = 2 };

// Offer `mode` now (call setPlayerMode), wait (the same entry again next turn: a pickup would be possible a moment later) or drop the entry
// (a pickup would leave the item in the world for good: the form's assets are missing). `why` gets the reason when not OFFER.
// Mode 0 (cure) is always offered: it is what damage does, at any time.
inline PortPowerupVerdict portPowerupBlocked(unsigned mode, const PortPowerupState& s, const char** why) {
    if (mode == 0) return PORT_POWERUP_OFFER;
    if (!s.formAccepted) { *why = "the form's assets are missing (canEnterPlayerMode)"; return PORT_POWERUP_DROP; }
    if (s.health == 0) { *why = "the player is dead"; return PORT_POWERUP_WAIT; }
    if (s.transformTimer != 0) { *why = "a transformation is running (_3D8)"; return PORT_POWERUP_WAIT; }
    if (s.morphResetTimer != 0) { *why = "the morph reset timer runs"; return PORT_POWERUP_WAIT; }
    if (!s.nerveChangeEnabled) { *why = "the player cannot change state now (isEnableNerveChange)"; return PORT_POWERUP_WAIT; }
    return PORT_POWERUP_OFFER;
}
