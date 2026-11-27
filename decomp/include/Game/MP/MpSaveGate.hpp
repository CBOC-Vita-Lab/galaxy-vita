// Galaxy 3.0 multiplayer: the save gate's state machine (pure logic, no game classes), used by MpSaveGame.cpp and unit-tested on
// the host (tests/mp/savegate_test.cpp).
//
// The rule it enforces: a console writes session progress to its own files only where the player chose, and never otherwise.
// Everything here decides "may a save run now" from a handful of flags; the side effects (recovery checkpoint, logging, ids) are
// the environment's (Env). Env provides:
//   bool netActive()            mp_active() right now (the net thread may flip it at any moment)
//   bool reconnecting()         the core is silently rejoining the session
//   bool localIsOriginalHost()  this console is slot 0 of a session that never migrated
//   int  policy()               mp_save_policy(...) for the current state
//   void onStart() / onResumed() / onEnd() (before the hold decision) / onEnded(held) / logNoChoice()
#pragma once
#include "../../../../platform/src/mp/mp_savefile.h"

namespace MpSaveGate {
    struct State {
        bool onMpRoot = false;           // the in-RAM image is the multiplayer root's file
        bool progressInRam = false;      // this console holds session progress that is not its own save (guest / after a migration)
        // A session ended while the in-RAM image holds progress nobody chose to write (a guest's image carries the host's progress
        // in all six files; a host without a save choice). Cleared only by a successful reload of the image from disk.
        bool heldAfterSession = false;
        bool sessionActive = false;      // mp_active() as last latched by sync
        bool sessionSeen = false;        // a session ran since its leave decision
        bool resolved = false;           // this session's leave decision is made
        bool resumable = false;          // the session went quiet but the core is rejoining it
        bool originalHost = false;
        int spSessionSlot = 0;           // the host plays on single-player file N (beginSinglePlayer)
        // savefix: the co-op root was CHOSEN (enterSessionRoot / import made for a save choice), not merely where the image is
        // (a switch back that is still pending, an undecided earlier session): only a chosen root counts as the host's choice
        bool coopChosen = false;
    };

    inline bool hostWriteChosen(const State& s) { return (s.onMpRoot && s.coopChosen) || s.spSessionSlot > 0; }
    // savefix: the co-op file was chosen (the multiplayer root entered for a save choice)
    inline void chooseCoop(State& s) { s.coopChosen = true; }

    // savefix (review finding 5): the session's progress is no longer this console's own (a host migration, another console's
    // snapshot applied): it has no single-player source any more. The leave / recovery offers then name only co-op files, and the
    // recovery checkpoints carry no source.
    inline void sourceLost(State& s) { s.spSessionSlot = 0; }

    // The host picked "Play on my single-player file N".
    inline void beginSinglePlayer(State& s, int slot) { s.spSessionSlot = slot; }
    // The choice was not used (start failed, lobby left, file select reached) and no session ran on it: forget it, so the NEXT
    // session (a Quick Match, another room) does not inherit "the host chose the single-player file".
    inline void abandonChoice(State& s) {
        if (!s.sessionSeen) {
            s.spSessionSlot = 0;
            s.coopChosen = false;
        }
    }

    // The in-RAM image was just reloaded from disk (a root switch / reload / discard): nothing stale is held any more.
    inline void imageReloaded(State& s) { s.heldAfterSession = false; s.progressInRam = false; }
    // Entering the multiplayer root: skip the load only when the image already is that root's file AND is not stale (a held image
    // is the previous session's progress: it must be reloaded before a host with a choice plays or autosaves on it).
    inline bool switchIsNoop(const State& s, bool toMp) { return s.onMpRoot == toMp && !(toMp && s.heldAfterSession); }

    // Writing session progress into a single-player file (the host's own, after a session or from a recovery file): ask first when
    // the file has more Power Stars than the progress, or was written after the progress was made (fileNewer: its modification
    // time is later than the recovery file's session). fileStars < 0: unknown / no such file.
    inline bool singleOverwriteWarning(int progressStars, int fileStars, bool fileNewer) {
        return (fileStars >= 0 && progressStars < fileStars) || fileNewer;
    }

    // Latch mp_active(). Idempotent: the answer never depends on who calls first within a frame.
    template < class Env >
    void sync(State& s, Env& env) {
        const bool active = env.netActive();
        if (!active && !s.sessionActive && s.sessionSeen && !s.resolved) s.resumable = env.reconnecting();
        if (active == s.sessionActive) return;
        s.sessionActive = active;
        if (active && s.resumable && s.sessionSeen && !s.resolved) {
            s.resumable = false;
            env.onResumed();
            return;
        }
        s.resumable = false;
        if (active) {
            // savefix: a previous session whose leave decision was never made (left undecided, its recovery file kept) must not
            // hand its "Play on my file N" to this one (a choice for THIS session is made after its start: lobby Start / launch)
            if (s.sessionSeen) {
                s.spSessionSlot = 0;
                s.coopChosen = false;
            }
            s.sessionSeen = true;
            s.resolved = false;
            s.originalHost = env.localIsOriginalHost();
            if (!s.originalHost) s.spSessionSlot = 0;   // only the original host plays on a single-player file
            env.onStart();
        } else {
            env.onEnd();   // keep what is in RAM safe right away (recovery checkpoint)
            const bool chosen = s.originalHost && hostWriteChosen(s);
            // Without a choice the session's play (stars, items) lives only in RAM and may never have met a suppressed save
            // attempt (progressInRam is set there): hold always, until the image is reloaded (the title / file select does).
            if (!chosen) s.heldAfterSession = true;
            env.onEnded(s.heldAfterSession);
        }
    }

    // May a save be blocked right now? True = the game's save must not run. Self-syncing (finding: the net thread can end the
    // session mid-frame, before the scene's tick latches it: a save nerve in the gap must see the end, not the old state).
    template < class Env >
    bool suppressed(State& s, Env& env) {
        sync(s, env);
        if (!s.sessionActive) return s.heldAfterSession;
        const int p = env.policy();
        if (!env.netActive()) {   // the session ended between the latch and the policy read
            sync(s, env);
            return s.heldAfterSession;
        }
        // (a stale image, heldAfterSession, is never written even by a host with a choice: it is reloaded first)
        if (p == MP_SAVE_POLICY_AUTOCONFIRM && hostWriteChosen(s) && !s.heldAfterSession) return false;
        if (p == MP_SAVE_POLICY_AUTOCONFIRM) env.logNoChoice();
        s.progressInRam = true;
        return true;
    }
}  // namespace MpSaveGate
