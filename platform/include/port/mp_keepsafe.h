// Galaxy 3.0 multiplayer (owner: keepsafe): copies of a player's multiplayer data, and bringing items back from them.
// Rules: docs/MP-DATA-FORMATS.md. Tests: tests/mp/keepsafe_build.sh (keepsafe_test + keepsafe_cut_test).
//
// keepfix 3 (coordinator's design after the third review): no multi-file transactions. Every change to LIVE data is ONE file replaced by
// its own writer (the save layer's mp_save_write_atomic for the Team File and the account; mp_config / mp_ui_flow's writers for the
// settings), each after its own question. A cut leaves each file old or new. Nothing here renames, removes or truncates a live file
// otherwise. Copies live in <data>/mp-backups/ and are written as NEW files only (never over an existing name):
//   <version>-<YYYYMMDD>-<n>/           the automatic copy at a version change (multiplayer data; not mp/backups): this console's own
//   before-<YYYYMMDD>-<n>.smgmp         what an item held right before it was brought back (the "undo": bring it back again)
//   copy-<YYYYMMDD>-<n>.smgmp           "Make a Copy" (Team Files and, unless left out, the account): to keep, or to move to another console
// The single-player saves (<data>/saves) and the single-player safety copies (<data>/mp/backups) are never written.
// Threading: game thread (the account store also from the network helper thread). Calls are blocking file work: menu actions only.
#ifndef PORT_MP_KEEPSAFE_H
#define PORT_MP_KEEPSAFE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum MpKsResult {
    MP_KS_OK = 0,
    MP_KS_NOTHING,       // nothing to do (no data / the item is already the same / the copy does not hold it)
    MP_KS_ARGS,
    MP_KS_IO,            // a read or write failed: the live file is as it was
    MP_KS_NO_SPACE,
    MP_KS_TOO_BIG,
    MP_KS_BAD_FILE,      // the copy, or the live file, cannot be used (damaged / not ours): nothing written
    MP_KS_NEWER,         // made by (or on this console written by) a newer game: left alone
    MP_KS_NOT_FOUND,
    MP_KS_VERIFY,        // a copy did not read back the same: not used
    MP_KS_BUSY,          // another keepsafe call runs, or the game says its data may not be replaced now (a session, a save running)
    MP_KS_CONFIRM,       // the item would replace something: ask the player, then call again with confirmed = 1
    MP_KS_TOO_MANY       // keepfix 5: 64 "before" copies exist and each holds a Team File slot nothing else holds: nothing is replaced, nothing is deleted
};
const char* mp_keepsafe_result_name(int result);

struct MpKsPaths {
    char dataDir[260];     // ux0:data/smg: the stamp and mp-backups/
    char mpDir[260];       // <dataDir>/mp
    char onlineDir[260];   // <dataDir>/online
    char teamFile[400];    // <mpDir>/saves/title/00010000/524d4750/data/GameData.bin
    char recoveryDir[300]; // <mpDir>/recovery
};
void mp_keepsafe_default_paths(struct MpKsPaths* out);
void mp_keepsafe_set_paths(const struct MpKsPaths* paths);   // tests / desktop; NULL = the defaults

// ---- start-up -------------------------------------------------------------------------------------------------------------------
// mp_port_boot calls it before any multiplayer write. Reads the stamp; when the version or build changed, makes ONE verified copy and
// writes the stamp. A stamp that cannot be READ (an I/O error), or any failure: nothing is changed and no stamp is written, so it is tried again at
// the next start. A stamp that is there but does not parse counts as no stamp (keepfix 5). Never fails the start.
void mp_keepsafe_boot(const char* version, const char* build);
void mp_keepsafe_set_data_hash(uint32_t hash);   // the StageData hash (copies record it)
// NO_COPY: the copy at this update could not be made (at every start where it fails); NO_SPACE: the same, and the card really was out of space
// (only then may the words say "full"; keepfix 5).
enum { MP_KS_NOTICE_NONE = 0, MP_KS_NOTICE_NO_COPY = 1, MP_KS_NOTICE_NO_SPACE = 2 };
int mp_keepsafe_notice_take(void);
bool mp_keepsafe_may_migrate(void);   // false when this start has no copy of the data before it (nothing may rewrite a file in a new format); see share/keep/API.md

// ---- copies ---------------------------------------------------------------------------------------------------------------------
enum { MP_KS_COPY_VERSION = 0, MP_KS_COPY_BEFORE = 1, MP_KS_COPY_MADE = 2 };
struct MpKsCopy {
    int kind;              // MP_KS_COPY_*
    char name[96];         // folder or file name in mp-backups/ (what the other calls take)
    int64_t created;       // unix seconds (UTC): the date to show
    char version[24];      // the game version that made it
    uint32_t bytes;
    uint8_t teamFiles, account, settings;   // what it holds (settings: only this console's own copies, VERSION and BEFORE)
};
// Copies that verify completely, newest first. Returns the count (<= max).
int mp_keepsafe_list_copies(struct MpKsCopy* out, int max);
// "Make a Copy": copy-<date>-<n>.smgmp in mp-backups/ (Team Files and the account; MP_KS_COPY_NO_ACCOUNT leaves the account out: a copy to
// share). A new file only, read back and validated. Never changes live data.
enum { MP_KS_COPY_NO_ACCOUNT = 1 };
int mp_keepsafe_make_copy(int flags, char* nameOut, size_t cap);

// ---- what a copy holds, against this console now --------------------------------------------------------------------------------
struct MpKsCopyInfo {
    int kind;
    int64_t created;
    int16_t copyStars[6];   // Team File 1..6 in the copy: Power Stars, -1 = not in the copy
    int16_t liveStars[6];   // this console's now (-1 = none)
    uint8_t slotBlank;      // bit s-1: the copy's file s is blank: never brought back
    uint8_t slotSame;       // bit s-1: already the same here
    uint8_t slotInUse;      // bit s-1: this console's file s is in use: bringing it back asks first (MP_KS_CONFIRM)
    uint8_t slotLacks;      // bit s-1 (of slotInUse): this console's file has progress the copy's lacks: say so in the question
    uint8_t account;        // the copy holds an online account that differs from this console's: the "Use this copy's online account" row
    uint8_t settings;       // MP_KS_SET_* the copy can bring back (this console's own copies only) that differ from now
    uint8_t dataHashDiffers;
};
int mp_keepsafe_inspect(const char* name, struct MpKsCopyInfo* info);

// ---- bringing items back (each one file, each its own call) ---------------------------------------------------------------------
// Team File `slot` (1..6) from the copy. A blank copy slot: MP_KS_NOTHING. This console's slot in use: MP_KS_CONFIRM unless confirmed.
// Before a slot in use is replaced, the whole Team File is written to before-<date>-<n>.smgmp (its name in beforeOut) and verified; if that
// copy cannot be made nothing is replaced. Then ONE atomic write of the Team File; the reload hooks run before the call returns.
// keepfix 5: a "before" copy is deleted only when every slot in it is also in this console's file or in a copy that stays (beyond 16 of them); at
// 64 that cannot be done, the call returns MP_KS_TOO_MANY and replaces nothing. No Team File here: only slot N is written, into a new blank file.
int mp_keepsafe_bring_back_slot(const char* name, int slot, int confirmed, char* beforeOut, size_t cap);
// The copy's online account(s) (never part of anything else). Without confirmed: MP_KS_CONFIRM ("This copy includes an online account.
// Use it on this console?"). Through the account store: the credential replaced is kept as .prev first when this console held it.
// keepfix 5: MP_KS_BUSY during a session (the same guard as the Team File) and while a save is being written.
int mp_keepsafe_use_account(const char* name, int confirmed);
// Settings from this console's own copies (VERSION, BEFORE): mp.cfg, flow.cfg, coop.cfg as asked (MP_KS_SET_*), each by its own writer
// under the settings lock, each after a before-copy of it; settings reloaded before the lock is released.
enum { MP_KS_SET_CONFIG = 1, MP_KS_SET_FLOW = 2, MP_KS_SET_COOP = 4 };
int mp_keepsafe_bring_back_settings(const char* name, int which, char* beforeOut, size_t cap);

// ---- the account file (online/online*.dat) --------------------------------------------------------------------------------------
// mp_service.cpp's SaveAccount: this console's own credential. The atomic writer; the credential replaced is kept (.prev, .prev.1, .prev.2,
// newest first, no duplicates) when this console held it. A newer game's file, or one that cannot be read now: refused, nothing written.
bool mp_keepsafe_account_store(const char* path, const uint8_t* data, uint32_t len);
bool mp_keepsafe_account_previous(const char* path, int index, uint8_t out[112]);

// ---- the game's side ------------------------------------------------------------------------------------------------------------
bool mp_keepsafe_busy(void);                               // a keepsafe call runs (the quit path waits on it)
void mp_keepsafe_add_reload_hook(void (*fn)(void));        // run after an item is brought back, before the call returns (mp_port_boot)
void mp_keepsafe_set_guard(bool (*mayReplace)(void));      // MpSaveGame: false while its data may not be replaced (mp_port_boot)

// ---- tests ----------------------------------------------------------------------------------------------------------------------
void mp_keepsafe_test_set_time(int64_t unixSeconds);       // 0 = the real clock
void mp_keepsafe_test_set_free_bytes(int64_t bytes);       // < 0 = the real free space
int mp_keepsafe_test_read_count(void);
void mp_keepsafe_test_set_read_hook(void (*fn)(const char* path));   // told of every file this module reads (the settings lock order test)
int mp_keepsafe_test_stat_count(void);
void mp_keepsafe_test_reset(void);                         // forget in-memory state (a new process); hooks and guard too

#ifdef __cplusplus
}
#endif

#endif  // PORT_MP_KEEPSAFE_H
