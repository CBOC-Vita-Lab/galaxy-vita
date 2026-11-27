// midload26 (GXV_NO_MIDHOLD, default 1): which scene loads may be held on a closed screen for the stage warm-up (StageWarmHold), and for how long. Pure, standard
// library only (the backend and tests/midhold_model.cpp share it).
//
// Owner's rule (12 Nov): no loading screen in the middle of a mission, ever; the original game never shows one (zones stream in during launch-star flights). The port's
// hold (GXV_STAGEWARM_HOLD_MS, 5 s, 12 s while the stage's own programs are not linked) only ever lengthens a screen the game has closed itself (it is polled from
// GameSystemSceneController::isReadyToStartScene, i.e. after a real scene build), but a death restart, a boss or cut-scene stage change, or the exit after a star are
// also scene builds behind a closed wipe, and the original keeps that short. So the hold is allowed only where the game also has a "start": a stage load started from the
// star select / galaxy map, or a load when no stage has been played yet (title, file select) or the stage left was only passed through. Any other load is not held: its
// queue is worked off in small slices behind play (GXV_MIDHOLD_SLICE_MS) and a "midhold: suppressed" line says so.
#pragma once
#include <cstdint>

namespace vgx::midhold {

struct Facts {
    int mode;               // GXV_NO_MIDHOLD: 0 = old behaviour (log only), 1 = the rule above
    bool starSelect;        // the star select / galaxy map was drawn since this load began
    uint32_t playedKey;     // the stage played before this load (0 = none yet)
    double playedMs;        // live (screen open, no menu) play time of that stage, ms
    uint32_t targetKey;     // the stage being loaded
    double minPlayMs;       // GXV_MIDHOLD_MIN_PLAY_S * 1000: shorter than this is "passed through"
};
enum Why : uint8_t {
    kOff = 0,        // switch 0
    kStarSelect,     // allowed: started from the star select / galaxy map
    kNoPlay,         // allowed: no stage played before this load
    kPassedThrough,  // allowed: the stage left was played for less than minPlayMs
    kRestart,        // suppressed: the stage being played is loaded again (death, retry)
    kInMission       // suppressed: another stage inside a mission, no star select (cut-scene, boss, zone, star-get exit)
};
inline bool Allowed(Why w) { return w != kRestart && w != kInMission; }
inline const char* WhyText(Why w) {
    switch (w) {
        case kOff: return "switch off (GXV_NO_MIDHOLD=0)";
        case kStarSelect: return "start from the star select / galaxy map";
        case kNoPlay: return "no stage played before this load";
        case kPassedThrough: return "the stage left was only passed through";
        case kRestart: return "restart of the stage being played (death / retry)";
        case kInMission: return "scene change inside a mission without a star select (cut-scene / boss / star-get exit)";
    }
    return "?";
}
inline Why Judge(const Facts& f) {
    if (f.mode <= 0) return kOff;
    if (f.starSelect) return kStarSelect;
    if (f.playedKey == 0) return kNoPlay;
    if (f.playedMs < f.minPlayMs) return kPassedThrough;
    return f.targetKey == f.playedKey ? kRestart : kInMission;
}
// The longest a hold may last whatever the stage warm-up's own caps say (0 = no hard cap). `base` and `own` are the caps in force (GXV_STAGEWARM_HOLD_MS and, while
// the stage's own programs are not linked, GXV_STAGEWARM_HOLD_OWN_MS); the pack tour (prewarm mode 2) keeps its own and is passed as exempt.
inline double HardCap(double cap, double hardMs, bool exempt) {
    return (hardMs > 0.0 && !exempt && cap > hardMs) ? hardMs : cap;
}

} // namespace vgx::midhold
