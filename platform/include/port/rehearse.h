// dive27 (13 Nov, GXV_REHEARSE; default 0 in code, PROPOSED 1 in both modes after one console run): REHEARSAL, the general "first use behind the loading wipe" mechanism.
// Header only, standard library only, so the game (decomp/src/Game/System/PortRehearse.cpp), the host test (tests/rehearse_model.cpp) and any other first-use path share it.
//
// WHY. A stage's conditional effect paths (the underwater filter, the wet ocean, the glow chain, a power-up's overlay, a boss intro ...) draw for the first time in play, where the
// first draw links its shader programs (tens to hundreds of ms on the console), makes its render / copy targets (60-100 ms each) and uploads its textures (2.7-5.3 ms each). The
// stage's warm-up (shader list, premade targets) only knows what automated tours drew. A REHEARSAL asks the game to exercise each such path once while the loading wipe is closed.
//
// THE REGISTRY. rehearse::add("name", fn) (or a file-scope Registrar) registers one rehearsal. fn runs on the game thread inside the draw pass of a closed-wipe picture, inside the
// renderer's warm-up scope (PORT_GX_MARK_WARM_BEGIN / _END: the draws are resolved - same keys, textures looked up and uploaded, program variants queued - but never recorded, so the
// picture is not touched), with the GX library state saved and put back by the driver. It returns
//     >= 0   done (the number of draws / captures it sent; counted in the census),
//     kSkip  this stage has nothing for it (no water, no such actor): done, counted as skipped,
//     kRetry the pre-condition is not there yet (no player, no scene object yet): asked again at the next closed picture, at most kMaxTries times, then skipped.
// An entry must not change game state (no RNG, no timers, no emitters left alive, no flag set that the game reads): it draws through the real draw path with the real members, nothing else.
// An entry that needs a render / copy target the scope's captures do not make (a pass object, a depth buffer) asks for it with port_res_request (platform/vgx, stall27's queue).
//
// The obvious candidates found while reading (NOT implemented here; each is "draw it once in a scope when the stage has the actor"):
//   power-ups / forms: decomp/src/Game/Player/MarioActorDraw.cpp (PortPrewarmFormCensus knows which forms are loaded), Game/Player/Mario*Ball*.cpp (fire / ice ball), Game/MapObj/PowerUpItem*.cpp
//   star get / clear sequence: Game/Screen/StarGetLayout / GameSceneLayoutHolder.cpp, Game/Demo/GrandStar*.cpp, Game/Effect/StarPieceFlyEffect*
//   damage / death flash: Game/Screen/MissLayout.cpp, Game/Screen/GameOverLayout.cpp, Game/Screen/ScreenFilter (WipeCircle / WipeFade: SystemWipeHolder.cpp)
//   pause menu: Game/Screen/PauseMenu.cpp, Game/Screen/PauseMenuLayout*.cpp (GXV_LAYOUTWARM covers its panes), Game/Screen/ScreenPreserver.cpp (a full-screen capture)
//   launch star: Game/MapObj/Launch Star* (LaunchStarAndWarp, WarpPod), Game/Demo/LaunchStar*, its star-trail effect and the "star flight" screen filter; the white fly-in: SystemWipeHolder.cpp
//   boss intros: Game/Boss/*Demo.cpp (DodoryuDemo, KoopaDemo, SkeletalFishBossDemo ...): their effects are in GXV_PREWARM's list, their layouts in GXV_LAYOUTWARM's, their screen filters are not.
//   other image effects: Game/Screen/BloomEffectSimple.cpp, DepthOfFieldBlur.cpp, ScreenBlurEffect.cpp (each makes its own copy targets at its first picture).
#pragma once
#include <stdint.h>
#include <string.h>

namespace port::rehearse {

constexpr unsigned kMax = 24;       // entries in the registry
constexpr unsigned kMaxTries = 90;  // closed pictures an entry may be put off for (kRetry)
constexpr int kSkip = -1;
constexpr int kRetry = -2;
using Fn = int (*)(void);

enum State : uint8_t { kTodo = 0, kDone = 1, kSkipped = 2, kGaveUp = 3 };

struct Entry {
    const char* name;
    Fn fn;
};
struct Registry {
    Entry e[kMax];
    unsigned n;
};
// (an inline variable of constant-initialised plain data: file-scope Registrars of any unit may add to it before main, in any order, with no guard barrier)
inline Registry g_reg;

// true when added (a name is kept once)
inline bool add(const char* name, Fn fn) {
    if (!name || !fn) return false;
    for (unsigned i = 0; i < g_reg.n; ++i)
        if (strcmp(g_reg.e[i].name, name) == 0) return false;
    if (g_reg.n >= kMax) return false;
    g_reg.e[g_reg.n].name = name;
    g_reg.e[g_reg.n].fn = fn;
    ++g_reg.n;
    return true;
}
struct Registrar {
    Registrar(const char* name, Fn fn) { add(name, fn); }
};

// One scene's progress. Sync(serial) starts a new one when the scene serial changed.
struct Result {
    int8_t code = 0;       // fn's last return (>= 0 done: stored as 0)
    uint8_t state = kTodo;
    uint8_t tries = 0;
    uint16_t draws = 0;    // fn's return when done
    uint32_t us = 0;       // time spent in it
};
struct Runner {
    uint32_t serial = 0;
    bool started = false;      // a closed picture has come for this scene (what the hold waits on)
    bool logged = false;
    Result r[kMax];
    unsigned frames = 0;       // closed pictures that ran at least one entry
    uint64_t usTotal = 0;
    bool valid = false;        // a scene has been synced

    void Reset(uint32_t s) {
        serial = s;
        started = false;
        logged = false;
        for (unsigned i = 0; i < kMax; ++i) r[i] = Result();
        frames = 0;
        usTotal = 0;
    }
    // true when this is a new scene (the caller may log)
    bool Sync(uint32_t s) {
        if (valid && serial == s) return false;
        Reset(s);
        valid = true;
        return true;
    }
    unsigned Pending() const {
        unsigned n = 0;
        for (unsigned i = 0; i < g_reg.n; ++i) n += r[i].state == kTodo;
        return n;
    }
    unsigned Count(State s) const {
        unsigned n = 0;
        for (unsigned i = 0; i < g_reg.n; ++i) n += r[i].state == s;
        return n;
    }
    // One closed picture: runs the entries still to do in registry order until budgetUs of `now()` is spent (at least one entry runs). open / close bracket each entry's scope.
    // Returns the number of entries that ran (finished or put off).
    template <class Clock, class Open, class Close>
    unsigned Step(Clock now, uint64_t budgetUs, Open open, Close close) {
        started = true;
        const uint64_t t0 = now();
        unsigned ran = 0;
        for (unsigned i = 0; i < g_reg.n; ++i) {
            Result& x = r[i];
            if (x.state != kTodo) continue;
            if (ran && now() - t0 >= budgetUs) break;
            const uint64_t a = now();
            open();
            const int rc = g_reg.e[i].fn();
            close();
            const uint64_t b = now();
            x.us += static_cast<uint32_t>(b - a);
            ++ran;
            if (rc >= 0) {
                x.state = kDone;
                x.draws = static_cast<uint16_t>(rc > 0xFFFF ? 0xFFFF : rc);
            } else if (rc == kSkip) {
                x.state = kSkipped;
            } else if (++x.tries >= kMaxTries) {
                x.state = kGaveUp;
            }
        }
        if (ran) {
            ++frames;
            usTotal += now() - t0;
        }
        return ran;
    }
};

}  // namespace port::rehearse
