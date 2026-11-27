// Galaxy 3.0 multiplayer: saves (owner: save). DESIGN.md section 1 "Saves". Multiplayer never writes the single-player
// GameData.bin unless the player said so: co-op files live in their own GameData.bin under the multiplayer save root
// (ux0:data/smg/mp/saves; desktop <run-dir>/mp/saves), write-back to a single-player file needs an explicit Yes and makes a backup
// (ux0:data/smg/mp/backups/GameData-*.bin, newest 10 kept).
//
// All functions run on the game thread. User-file slots are 1..6 (the game's "mario1".."mario6", FileSelector's file ids).
// The root functions and the file functions are for the title / file select only: they refuse (return false, try again next frame)
// while a save sequence runs. After enterSessionRoot / leaveSessionRoot / importSinglePlayerFile the file select's cached files are
// stale: rebuild it (MR::requestChangeSceneTitle()), or start play directly with
// GameSequenceFunction::startGameDataLoadSequence(slot, isMario) + MR::requestChangeStageInGameAfterLoadingGameData() as
// FileSelector::exeDemo does.
#pragma once

namespace MpSaveGame {
    // Switch the save root to the multiplayer root and load its GameData.bin into the game's save image (a fresh image when the
    // file does not exist yet; an invalid file is kept aside as GameData.bin.mpbad). Synchronous, with a self-check. True when on
    // the multiplayer root afterwards. Log: "save root mp".
    bool enterSessionRoot();
    // Back to the single-player root and its GameData.bin (reloaded from disk). Log: "save root sp".
    bool leaveSessionRoot();
    // Copy single-player file spSlot into co-op file mpSlot (switches to the multiplayer root first if needed). The co-op
    // GameData.bin is written atomically; the single-player file is only read. Log: "save import sp=%d mp=%d ok".
    bool importSinglePlayerFile(int spSlot, int mpSlot);
    // Only after an explicit Yes: copy co-op file mpSlot (as saved in the co-op GameData.bin) into single-player file spSlot.
    // Makes a backup of the whole single-player GameData.bin first (refuses without one), writes atomically, checks the result.
    // Log: "save writeback mp=%d sp=%d backup=%s".
    bool writeBackToSinglePlayer(int mpSlot, int spSlot);
    // Host, during a session: pack the playing user file (live progress) and hand it to the port (mp_snapshot_offer). Call at
    // join (the core may ask again) and after each star. Log: "save snapshot offer bytes=%u crc=%08x".
    bool offerSnapshot();
    // Every console but the original host, during a session: when a complete snapshot is waiting, validate it, write its blocks into the in-memory save image
    // (session slot, see setSessionSlot) and reload the user file from it. Never touches a file. Best called at a scene change.
    // Log: "save snapshot applied slot=%d".
    bool applySnapshotIfReady();
    bool isSessionRoot();
    // A host snapshot (or the original host's own progress) is in the in-memory image this session (late-join "sync valid").
    bool snapshotAppliedThisSession();

    // ---- extras (also for the ui / flow agents) -------------------------------------------------------------------------
    // Same as saveOnLeave(LEAVE_SAVE_COOP, mpSlot) (kept for the first ui draft).
    bool keepGuestProgress(int mpSlot);
    // Host "Play on single-player file N": back up the single-player GameData.bin first. Log: "save backup sp path=%s".
    bool backupSinglePlayerFile();
    // Import guard: < 0 when single-player file spSlot has fewer power stars than co-op file mpSlot (ask before overwriting),
    // 0 equal (or nothing to compare), > 0 more. Stars = mario + luigi blocks of the file.
    int compareImportProgress(int spSlot, int mpSlot);
    // Power stars (mario + luigi) of a file on disk; -1 when the file or slot cannot be read; single-player files also -1 when the user
    // file was never created (flow, review 2 #3: the game's own CONF flag, mp_save_slot_state: "New", never launched). Co-op files
    // are launched straight into play (never "created" by the file select): the menus tell an unused one by coop.cfg and its stars.
    // Reads the file: never per frame.
    int slotStars(bool coop, int slot);
    // savefix (m-files): one read of the file. SLOT_UNREADABLE: there but not readable / valid, the slot does not parse, no memory,
    // no path, or no file but an interrupted replace beside it; SLOT_NEW: no GameData.bin, or a single-player file never created;
    // SLOT_USED: the slot parses, *stars = Power Stars (co-op files: regardless of the created flag). *stars = -1 otherwise.
#define MPSAVE_HAS_SLOTSTATE 1
    enum SlotState { SLOT_UNREADABLE = -1, SLOT_NEW = 0, SLOT_USED = 1 };
    int slotState(bool coop, int slot, int* stars);
    // savefix: mp.cfg could not be read (another layout or damaged) or is from a newer game: settings changes are not saved this run.
    // = mp_config_read_only() != MP_CONFIG_WRITABLE (platform/src/mp/mp_config.h has the reason).
    bool settingsReadOnly();
    // uxfix #2: the last session ended because this console's data did not match the session's (a rejected snapshot with nothing earlier
    // to go on): the menus then say so in a plain window instead of a generic one. True once, then false.
    bool takeLeftForMismatch();
    // Session end / leaving: true on every console except the original host (DESIGN.md section 8): it holds session progress in
    // RAM; ask "Keep this progress in a co-op file?" (Yes -> keepGuestProgress(slot); No -> discardSessionProgress()).
    bool shouldOfferKeepProgress();
    // Drop the session progress in RAM: reload the co-op GameData.bin from disk (stays on the multiplayer root).
    bool discardSessionProgress();
    // Save policy for the SaveDataHandleSequence hooks (cheap; false when no session): only the ORIGINAL host (slot 0, host epoch 0)
    // saves, auto-confirmed; everyone else, a host by migration, and everyone after a migration is suppressed.
    bool savesSuppressed();
    bool savesAutoconfirm();
    // review: after a session ended with progress in RAM that nobody chose to write: the game's saves stay suppressed (also
    // without a session) until leaveSessionRoot / discardSessionProgress reload the image. False in single-player.
    bool heldAfterSession();
    // optfix: nothing pending after a session (see MpSaveGame.cpp): the entered flag may be retired.
    bool settled();
    // ---- leaving early = saving (owner request 10:43) ---------------------------------------------------------------------
    // Anyone leaving a session (guest, host by migration, original host) can keep their progress. Choices for saveOnLeave:
    enum LeaveChoice { LEAVE_SAVE_COOP = 1, LEAVE_SAVE_SINGLE = 2, LEAVE_DONT_SAVE = 3 };
    struct LeaveOffer {
        bool offer;          // ask "Save your progress?" (a session ran and its decision is not made yet)
        bool coop;           // a co-op slot can take it (always, when offer)
        bool single;         // the host came in with "Play on my single-player file N": save back to it (backup first)
        int singleSlot;      // N (0 = none)
        int sessionSlot;     // the slot the progress is played in (RAM)
        int stars;           // power stars in the progress (mario + luigi)
        bool coopUsed[6];    // co-op file slot 1..6 holds a created file (false: empty, or no co-op file yet)
        int coopStars[6];    // its stars (-1: empty / no file). stars < coopStars[i]: warn "fewer stars" before overwriting
        int singleStars;     // gamefix: stars of single-player file singleSlot on disk (-1: unknown); stars < singleStars: warn first
    };
    // Host "Play on my single-player file N": call when that choice is made (with backupSinglePlayerFile() before playing).
    void beginSinglePlayerSession(int spSlot);
    // gamefix: that choice was not used (the start failed, the lobby was closed, the file select is back) and no session ran on it:
    // forget it. Without this the NEXT session (a Quick Match) would count the host as having chosen the single-player file.
    void abandonHostChoice();
    bool offerSaveOnLeave(LeaveOffer* out);
    // gamefix: cheap (no file reads): a session ran and its leave decision (save / don't save) is not made yet.
    bool leaveDecisionPending();
    // LEAVE_SAVE_COOP + slot 1..6 / LEAVE_SAVE_SINGLE (slot ignored: file N) / LEAVE_DONT_SAVE. After success the session's
    // recovery file is deleted (saved) or moved to recovery/discarded (don't save). Logs "save leave coop mp=%d ok stars=%d",
    // "save leave sp=%d backup=%s stars=%d", "save leave discard".
    bool saveOnLeave(int choice, int slot);

    // ---- recovery saves -----------------------------------------------------------------------------------------------
    // Every console writes its in-RAM progress to <mp root>/../recovery/session-N.bin (newest 3 kept) after each star
    // (writeRecoveryCheckpoint, called by flow), every ~2 minutes of play when the progress changed (tick) and when the session
    // ends. Written on a background thread; never a save file.
    void tick();                       // flow: once per logic turn (cheap without a session)
    bool writeRecoveryCheckpoint();
    struct RecoveryInfo {
        int index;
        unsigned sessionId;
        long long unixTime;
        char galaxy[33];
        int scenario, stars, sessionSlot;
        bool originalHost, mario;
        int sourceKind, sourceSlot;  // 0 none, 1 co-op slot, 2 single-player slot
        bool single;                 // LEAVE_SAVE_SINGLE is possible (back to single-player file sourceSlot, backup first)
        int spStars;                 // gamefix: stars of single-player file sourceSlot on disk now (-1: unknown)
        bool spNewer;                // gamefix: that file was written after this recovery file's session (stale recovery): warn first
    };
    // Boot / Multiplayer menu: the newest recovery file whose progress is not already in its file ("Your last session ended
    // unexpectedly. Keep its progress?"). Files already saved are removed quietly.
    bool recoveryPending(RecoveryInfo* out);
    // into co-op slot (or back to the single-player file). savebug: back to the single-player file is refused, whatever the caller, when the
    // recovery file holds a never-created (blank) slot, and when it holds FEWER stars than file N now or file N was saved after it, unless
    // overwriteConfirmed (the player answered the "this file has more stars" question). The caller's own backup rule still holds.
    bool recoveryKeep(int slot, int choice = LEAVE_SAVE_COOP, bool overwriteConfirmed = false);
    bool recoveryDiscard();                                     // moves it to recovery/discarded

    // savefix (review finding 2): true when this console may play the session's file: the original host with a save choice that
    // succeeded (prepareHostSave), a console that joined as a guest only on the multiplayer root. The menu's launch refuses
    // without it; the save layer itself never ejects a player (its saves are held by the gate) and only logs it.
    bool launchReady();

    // ---- savefix: the file select's guard (review finding 1) --------------------------------------------------------------
    // FileSelector::init (after its files are cached from the image): returns the image generation those files come from.
    // FileSelector, when it starts a game: fileSelectClosed(). Every frame: fileSelectGuard(generation):
    //   FS_USABLE   the files shown are the save image's and writes go to the same root: input as usual
    //   FS_WAIT     the switch back to the single-player root is not done (retried here every 30 calls) or the image is not
    //               verified: hold the file select's input ("Please wait" after a while)
    //   FS_REBUILD  the image changed since the files were cached: hold input and rebuild (MR::requestChangeSceneTitle())
    // Backstop: while a file select is open outside a session and is not FS_USABLE, savesSuppressed() refuses every save.
    enum { FS_USABLE = 0, FS_WAIT = 1, FS_REBUILD = 2 };
    unsigned fileSelectOpened();
    void fileSelectClosed();
    int fileSelectGuard(unsigned cachedGeneration);

    // ---- savefix: backups (for the menu owner: "Restore a backup") ----------------------------------------------------------
    // The single-player backups (ux0:data/smg/mp/backups), newest first, file names (GameData-SSSSSSSS-YYYYMMDD-HHMMSS.bin).
    struct BackupList { int count; char name[16][40]; };
    bool listBackups(BackupList* out);
    // Put a backup (full path) back as the single-player GameData.bin: only outside a session, with no leave decision pending;
    // the backup must validate; the current file is backed up first (refuses without that backup; a damaged current file is
    // kept aside), atomic replace, read back. The save image is reloaded when on the single-player root (the file select must
    // be rebuilt: fileSelectGuard says FS_REBUILD). Log: "save restore <path> ok (the file it replaced: <backup>)".
    bool restoreBackup(const char* backupPath);
    // mergefix: a save-file write is in progress right now (any thread; = mp_save_write_in_flight()).
    bool writeInFlight();

    // The user-file slot a guest's snapshot goes into (default 1; the slot the guest then starts with).
    void setSessionSlot(int slot);
    int sessionSlot();
    // True when the last applied snapshot was the host playing Mario (start the guest's game with this character).
    bool snapshotIsMario();

    // glitchfix S1: true while this console plays on one of its single-player files ("Play on my file"): the session's progress is
    // written back to that file, so another player's claim must be verified by the console's own simulation first (MpFlow.cpp).
    bool playingOnSinglePlayerFile();
}
