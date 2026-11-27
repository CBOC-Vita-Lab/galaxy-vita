// bootscr26: the pure decisions of "the port's own full-screen pictures (boot screen, in-play progress picture, plain bar, startup dialog) never show once play has
// started, and the boot screen ends for good". Standard library only: vgx_host_galaxy_vita.cpp, vgx_backend_gl.cpp and tests/boot_screen_model.cpp share it.
//
// Why: the sister port Strikers drew its boot screen over a match because the screen's fade-out step ran on one presentation path only; a later long present found the
// screen still in its fading mode with its thread alive and drew it again. Galaxy's screen cannot do that by construction (its thread writes the display only in kDirect,
// before the renderer exists; afterwards only the render thread swaps it, and only while Phase == kGl), but nothing STATED that, and the owner saw "the loading screen flash
// up and pause the game" in a mission (console job 1786c). So the rule is now written once, here, and every composing site asks it:
//
//   PortPicture(kind, ...)   may the port compose or swap this picture now? (one function for the boot screen, the dialog, the branded progress picture and the plain bar)
//   EndDue(...)              must the boot screen end for good now, and why? (called from every path a game picture can arrive by, from the idle loop and after each swap)
//   LatchPlay(...)           PlayStarted: set at the first live picture of a real stage after the title, never cleared (a return to boot does not happen in this program)
#pragma once
#include <cstdint>

namespace vgx::bootscreen {

// Same numbering as splash::Phase in vgx_host_galaxy_vita.cpp.
enum Phase : uint8_t { kOff = 0, kDirect, kGl, kDone, kYielded };
enum class Pic : uint8_t { Boot, Dialog, Progress, PlainBar };
constexpr unsigned kPicKinds = 4;

inline const char* PicName(Pic k) {
    switch (k) {
    case Pic::Boot: return "boot";
    case Pic::Dialog: return "dialog";
    case Pic::Progress: return "progress";
    case Pic::PlainBar: return "plain-bar";
    }
    return "?";
}
inline const char* PhaseName(int p) {
    switch (p) {
    case kOff: return "off";
    case kDirect: return "direct";
    case kGl: return "gl";
    case kDone: return "done";
    case kYielded: return "yielded";
    }
    return "?";
}

struct Verdict {
    bool ok;
    const char* reason; // why not (or "allowed")
};

// `playStarted`: the latch. `screenLive`: the game's picture is live this frame (a stage is being played, no wipe/hold/pause hides it).
// `pcsInPlay`: noprog26's switchable rule (GXV_NO_PLAY_COMPILE_STOP on and AlInPlay); it can only add a refusal, never lift the hard one.
//   Boot / Dialog : refused once play has started, and once the boot screen has ended (Off or Done).
//   Progress/Plain: refused while the picture is live after play has started (the hard rule, no switch), and by noprog26's own rule when its switch is on.
//                   A closed or quiet screen after play started (a stage load behind the game's own wipe, the pause menu) keeps its picture.
inline Verdict PortPicture(Pic kind, int phase, bool playStarted, bool screenLive, bool pcsInPlay) {
    switch (kind) {
    case Pic::Boot:
    case Pic::Dialog:
        if (playStarted) return {false, "play has started"};
        if (kind == Pic::Boot && (phase == kOff || phase == kDone)) return {false, "the boot screen has ended"};
        return {true, "allowed"};
    case Pic::Progress:
    case Pic::PlainBar:
        if (playStarted && screenLive) return {false, "play has started and the game's picture is live"};
        if (pcsInPlay) return {false, "GXV_NO_PLAY_COMPILE_STOP: shader work in play"};
        return {true, "allowed"};
    }
    return {true, "allowed"};
}

// ---- the end of the boot screen
enum class EndWhy : uint8_t { None, FadedDark, FadedGrace, Deadline, PlayStarted };
inline const char* EndName(EndWhy w) {
    switch (w) {
    case EndWhy::None: return "none";
    case EndWhy::FadedDark: return "faded out and shown dark";
    case EndWhy::FadedGrace: return "faded out, game pictures waiting";
    case EndWhy::Deadline: return "did not fade out in time";
    case EndWhy::PlayStarted: return "play started";
    }
    return "?";
}
constexpr int kDarkSwaps = 3;            // every buffer of the swap chain shown dark (as before)
constexpr double kFadeEndS = 0.5;        // the fade is over this long after the title (loading_screen thread: 0.20 s bar + 0.30 s fade)
constexpr double kGraceS = 1.0;          // faded + this long and a game picture is waiting: end, whatever the swaps counted
constexpr double kHardLimitS = 10.0;     // GXV_BOOT_FINISH_SEC=0 (never) or a larger value cannot keep the screen longer than this

struct EndInput {
    int phase = kOff;
    bool failed = false;       // a start-up failure is on the screen: it stays
    bool titleSeen = false;    // told to finish
    bool faded = false;        // the picture has gone dark
    int darkSwaps = 0;         // boot pictures swapped while faded
    double sinceTitleS = 0.0;
    double limitS = 5.0;       // GXV_BOOT_FINISH_SEC (default 5)
    bool playStarted = false;
};
inline EndWhy EndDue(const EndInput& in) {
    if (in.phase != kGl && in.phase != kYielded) return EndWhy::None;
    if (in.playStarted) return EndWhy::PlayStarted; // (even over a failure text: a game that is being played has nothing to say with the boot screen)
    if (in.failed || !in.titleSeen) return EndWhy::None;
    if (in.faded && in.darkSwaps >= kDarkSwaps) return EndWhy::FadedDark;
    if (in.faded && in.sinceTitleS >= kFadeEndS + kGraceS) return EndWhy::FadedGrace;
    const double limit = (in.limitS > 0.0 && in.limitS <= kHardLimitS) ? in.limitS : kHardLimitS;
    if (in.sinceTitleS >= limit) return EndWhy::Deadline;
    return EndWhy::None;
}

// PlayStarted: true on the call that sets it.
inline bool LatchPlay(bool& started, bool liveStagePicture) {
    if (started || !liveStagePicture) return false;
    started = true;
    return true;
}

} // namespace vgx::bootscreen
