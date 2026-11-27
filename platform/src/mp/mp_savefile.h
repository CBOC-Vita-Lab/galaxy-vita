// Galaxy 3.0 multiplayer: host-side save file logic (owner: save). Pure: no game headers, no game state. Works on byte images of
// GameData.bin and on files. Used by decomp/src/Game/MP/MpSaveGame.cpp and by tests/mp/save_*.
//
// GameData.bin (little-endian in the port, as the game writes it):
//   0x00 checksum u32 = MR::calcCheckSum over bytes [4, fileSize) as 16-bit words (sum << 16 | sum of ~w)
//   0x04 version u32 (2)   0x08 entry count u32 (19, < 24)   0x0C file size u32 (< 0x10000; the file on disk is OSRoundUp32B(size))
//   0x10 entries: name char[12] (NUL padded), offset u32. An entry's size runs to the next entry's offset (the last one: to file size).
//   Entries mario1, luigi1, config1, ... mario6, luigi6, config6, sysconf; mario/luigi 0xF80, config 0x60, sysconf 0x80; data at 0x140.
#ifndef PORT_MP_SAVEFILE_H
#define PORT_MP_SAVEFILE_H

#include <stddef.h>
#include <stdint.h>

#define MP_SAVE_VERSION 2
#define MP_SAVE_MAX_ENTRIES 24      // the game: count < 24
#define MP_SAVE_MAX_FILE 0x10000    // the game: size < 0x10000; also the game's read buffer
#define MP_SAVE_SLOTS 6             // user files 1..6
#define MP_SAVE_GAME_BLOCK 0xF80    // mario / luigi
#define MP_SAVE_CONFIG_BLOCK 0x60
#define MP_SAVE_NAME_BYTES 12

enum MpSaveCheck {
    MP_SAVE_OK = 0,
    MP_SAVE_ERR_SHORT,      // shorter than the header / table / declared size
    MP_SAVE_ERR_VERSION,
    MP_SAVE_ERR_COUNT,
    MP_SAVE_ERR_SIZE,       // declared size out of range
    MP_SAVE_ERR_LENGTH,     // file length != OSRoundUp32B(size)
    MP_SAVE_ERR_CHECKSUM,
    MP_SAVE_ERR_TABLE,      // an offset out of bounds / not ascending, a name without NUL
    MP_SAVE_ERR_MISSING,    // a block that must exist (mario/luigi/config of a slot) is not in the table
};
const char* mp_save_check_name(int check);

// MR::calcCheckSum over [4, fileSize) of an image (fileSize >= 4).
uint32_t mp_save_checksum(const uint8_t* image, uint32_t fileSize);

// Validate an image. fileLen = bytes available (the file length on disk, or the buffer size of an in-memory image when
// requireExactLength is false). checkChecksum false: for the game's in-memory image (_18), whose checksum field is stale.
int mp_save_validate(const uint8_t* image, uint32_t fileLen, bool requireExactLength, bool checkChecksum);
// Size of a validated image's declared file (header) and its on-disk length (rounded to 32).
uint32_t mp_save_file_size(const uint8_t* image);
uint32_t mp_save_disk_length(const uint8_t* image);

// Find a block by name in a validated image (last match wins, like SaveDataFileAccessor::makeUserFileInfo).
bool mp_save_find(const uint8_t* image, const char* name, uint32_t* offset, uint32_t* size);

// Copy the user-file blocks marioS/luigiS/configS of src into marioD/luigiD/configD of dst (slots 1..6). Both images must be
// validated (structure). Each block copies min(src, dst) bytes and zero-fills the rest of the destination block. Everything else in
// dst (other slots, sysconf, table) is untouched. Does not recompute the checksum (call mp_save_finalize).
bool mp_save_copy_slot(const uint8_t* src, int srcSlot, uint8_t* dst, int dstSlot);

// Zero the padding up to the 32-byte disk length and recompute the checksum. dstCap = buffer size (>= disk length).
bool mp_save_finalize(uint8_t* image, uint32_t dstCap);

// ---- progress snapshot (host user-file blocks; the payload of mp_snapshot_offer / mp_snapshot_get) ---------------------------
// Layout (little-endian, MpWriter): magic u32 'MPSV', version u16 (1), slot u8 (the host's slot, information only), flags u8
// (bit0: the host plays Mario), marioLen u16, luigiLen u16, configLen u16, reserved u16 (0), crc32 u32 over everything after it,
// then the mario, luigi and config bytes. Total <= MP_SNAPSHOT_MAX.
#define MP_SAVE_SNAP_MAGIC 0x5653504Du  // "MPSV"
#define MP_SAVE_SNAP_VERSION 1
#define MP_SAVE_SNAP_HEADER 20
#define MP_SAVE_SNAP_FLAG_MARIO 0x01
struct MpSaveSnapshotView {
    uint8_t slot, flags;
    const uint8_t* block[3];  // mario, luigi, config (pointers into the validated payload)
    uint16_t len[3];
    uint32_t crc;
};
uint32_t mp_save_crc32(const void* data, size_t len);
// Pack three blocks. Returns the total length (0 on failure: too big, bad args).
uint32_t mp_save_snapshot_pack(const uint8_t* mario, uint32_t marioLen, const uint8_t* luigi, uint32_t luigiLen,
                               const uint8_t* config, uint32_t configLen, int slot, uint8_t flags, uint8_t* out, uint32_t cap);
// Validate hostile bytes; on success fills view (pointers into data). Any error -> false (drop the whole snapshot).
// MP3 snapval: besides the header, lengths and CRC, the three blocks must pass mp_savecheck at MP_SAVECHECK_FULL with the
// installed limits (mp_savecheck_install): the game's save parsers do no bounds checking of their own.
bool mp_save_snapshot_parse(const uint8_t* data, uint32_t len, MpSaveSnapshotView* view);
// Same; *check (may be null) says why the blocks were refused (err MP_SC_OK when they passed or were never reached).
struct MpSaveCheckResult;
bool mp_save_snapshot_parse_checked(const uint8_t* data, uint32_t len, MpSaveSnapshotView* view, MpSaveCheckResult* check);
// The blocks of a view through mp_savecheck (level MP_SAVECHECK_STRUCT / _FULL; FULL uses the installed limits). MP_SC_OK = 0.
int mp_save_check_view(const MpSaveSnapshotView* view, int level, MpSaveCheckResult* check);
// Write a parsed snapshot's blocks into slot dstSlot (1..6) of a (structurally valid) image; the rest of each block is zeroed.
// MP3 snapval: refuses (writes nothing) unless the blocks pass mp_savecheck at MP_SAVECHECK_STRUCT.
bool mp_save_snapshot_apply(const MpSaveSnapshotView* view, uint8_t* image, int dstSlot);
// Pack slot S of an image (what the host would send; used by tests and as a fallback when no live user file is available).
uint32_t mp_save_snapshot_pack_image(const uint8_t* image, int slot, uint8_t flags, uint8_t* out, uint32_t cap);

// ---- files -----------------------------------------------------------------------------------------------------------------
// Read a whole file (<= cap bytes). Returns 0 ok, -ENOENT missing, -EFBIG larger than cap, other -errno.
int mp_save_read_file(const char* path, uint8_t* buf, uint32_t cap, uint32_t* outLen);
// mkdir -p of a directory path ("ux0:data/smg/mp/saves/..." or "/x/y").
bool mp_save_mkdirs(const char* dir);
// Atomic replace (as atomic as the file system allows): write path.mptmp, fsync, close, read it back and compare; then
// remove path.mpbak, rename path -> path.mpbak (the Vita's rename does not replace), rename path.mptmp -> path. A crash at any step
// leaves a valid old or new file, or a state mp_save_recover() repairs. The .mpbak (previous version) is kept.
bool mp_save_write_atomic(const char* path, const uint8_t* data, uint32_t len);
// savefix (keepsafe S5): the same steps for a file up to maxLen bytes (maxLen <= MP_SAVE_ATOMIC_MAX; the read-back buffer is maxLen).
#define MP_SAVE_ATOMIC_MAX (1u << 20)
bool mp_save_write_atomic_max(const char* path, const uint8_t* data, uint32_t len, uint32_t maxLen);
// savefix: steps 4-6 for a temporary someone else wrote (the port's NAND layer, nand.cpp): path.mptmp must be complete and, when
// valid is given, pass it; then the same keep-.mpbak / rename / read-back as mp_save_write_atomic. A temporary that fails is
// removed and nothing else is touched. False on any failure (the old file is in place, or recoverable by mp_save_recover).
typedef bool (*MpSaveValidator)(const uint8_t* data, uint32_t len);
bool mp_save_commit_tmp(const char* path, MpSaveValidator valid);
// savefix (for the system quit path): a save-file write is in progress right now (mp_save_write_atomic, mp_save_commit_tmp,
// the recovery writer, or a NAND write the port counts with mp_save_note_write(+1 / -1)). Any thread; cheap.
#ifdef __cplusplus
extern "C" {
#endif
bool mp_save_write_in_flight(void);
void mp_save_note_write(int delta);
#ifdef __cplusplus
}
#endif
// Repair after a crash during mp_save_write_atomic: when path is missing or invalid (per mp_save_validate, exact length, checksum),
// take a valid path.mptmp, else a valid path.mpbak. A stale .mptmp next to a valid file is removed. Returns true when path is valid
// afterwards (or nothing was there at all and nothing could be done: *changed tells whether anything was renamed).
// savefix: a primary that exists but cannot be read (an I/O error) is never replaced: false, nothing changed.
bool mp_save_recover(const char* path, bool* changed);
// smallfix: remove path.mpbak (a safe-write copy a session left) ONLY when path reads and validates as a GameData.bin AND no path.mptmp is beside
// it; otherwise it may be the only good copy and stays. Returns true when it was removed; *why (static text) says why not (may be null).
bool mp_save_drop_session_backup(const char* path, const char** why);
bool mp_save_recover_with(const char* path, MpSaveValidator valid, bool* changed);  // same, any file kind
bool mp_save_is_gamedata(const uint8_t* data, uint32_t len);
// savefix: keep a damaged file aside as path.mpbad (earlier ones move to .mpbad.1, .mpbad.2; the oldest of three goes).
bool mp_save_keep_aside(const char* path);
// savefix (keepsafe S3/S4): a GameData.bin whose version is newer than this build's (never repaired, replaced or kept aside here).
bool mp_save_is_newer(const uint8_t* data, uint32_t len);                       // mp_save_validate(exact, checksum) == OK
// Copy srcPath to backupDir/GameData-SSSSSSSS-YYYYMMDD-HHMMSS.bin through mp_save_write_atomic (S: a sequence number, one more
// than the largest present; the date (UTC from unixTime) is for the player only), then delete the oldest backups beyond `keep`
// by sequence (earlier-layout names GameData-YYYYMMDD-HHMMSS-NN.bin count as sequence 0), never the one just written. True only
// when the new backup exists afterwards and reads back byte-identical to the source. outPath gets the backup's path. The
// source must validate.
bool mp_save_backup(const char* srcPath, const char* backupDir, int64_t unixTime, int keep, char* outPath, size_t outCap);
// The backups in backupDir, newest first (by sequence). Returns the count (<= max).
struct MpSaveBackupInfo { char name[40]; long long seq; };
int mp_save_backup_list(const char* backupDir, MpSaveBackupInfo* out, int max);

// ---- whole operations (the game side passes paths and its in-memory image; tests drive the same code) -----------------------
enum MpSaveOpError {
    MP_SAVE_OP_OK = 0,
    MP_SAVE_OP_SRC_MISSING = 100, MP_SAVE_OP_SRC_INVALID, MP_SAVE_OP_DST_MISSING, MP_SAVE_OP_DST_INVALID, MP_SAVE_OP_SAME_FILE,
    MP_SAVE_OP_BACKUP, MP_SAVE_OP_WRITE, MP_SAVE_OP_VERIFY, MP_SAVE_OP_ARGS, MP_SAVE_OP_NO_MEMORY,
    MP_SAVE_OP_DST_ASIDE,   // gamefix: the destination exists but is not a valid file: kept aside as <path>.mpbad, nothing written
};
const char* mp_save_op_name(int err);
// Put user file srcSlot of srcImage (a structurally valid image: a file or the game's in-memory image) into slot dstSlot of the
// GameData.bin at dstPath, atomically. The destination base is the file at dstPath when it validates; otherwise fallbackBase (an
// in-memory image of the same root, may be null: then a missing/invalid destination is an error). Everything outside the slot is
// kept. On success outImage (MP_SAVE_MAX_FILE bytes, may be null) holds the image that was written.
// Blocks of slot `slot` of an image as a view (pointers into image).
bool mp_save_view_slot(const uint8_t* image, int slot, MpSaveSnapshotView* v);
// Same as mp_save_put_slot with the three blocks given directly (a snapshot, a recovery file, a live user file).
// MP3 snapval: blocks that fail mp_savecheck at MP_SAVECHECK_STRUCT are refused (MP_SAVE_OP_SRC_INVALID), nothing is written.
int mp_save_put_blocks(const MpSaveSnapshotView* blocks, const char* dstPath, int dstSlot, const uint8_t* fallbackBase,
                       uint32_t fallbackLen, uint8_t* outImage);
// Turn an in-memory image (cap >= MP_SAVE_MAX_FILE, structurally valid) into a fresh one: every slot gets the blank blocks (what
// a never-created user file serialises to). Table and sysconf stay. Used as the base when a co-op GameData.bin does not exist yet.
bool mp_save_fill_blank_base(uint8_t* image, uint32_t cap, const MpSaveSnapshotView* blank);
// Bytes of a block the game's chunk list actually uses (4 + chunk sizes); the rest of a block in the game's image can hold stale
// bytes. Unparseable: size.
uint32_t mp_save_block_used(const uint8_t* block, uint32_t size);
// The slot's blocks equal the given ones (used bytes only).
bool mp_save_slot_matches(const uint8_t* image, int slot, const MpSaveSnapshotView* blocks);
int mp_save_put_slot(const uint8_t* srcImage, uint32_t srcLen, int srcSlot, const char* dstPath, int dstSlot,
                     const uint8_t* fallbackBase, uint32_t fallbackLen, uint8_t* outImage);
// Import: single-player file spSlot -> co-op file mpSlot. The single-player file is only ever opened read-only (O_RDONLY) and
// must be a different file from mpPath (same device and inode is refused). MP3 snapval: the slot's blocks must pass
// mp_savecheck at MP_SAVECHECK_FULL (installed limits), else MP_SAVE_OP_SRC_INVALID.
int mp_save_import_file(const char* spPath, int spSlot, const char* mpPath, int mpSlot, const uint8_t* fallbackBase,
                        uint32_t fallbackLen, uint8_t* outImage);
// Write-back (only after an explicit Yes): co-op file mpSlot -> single-player file spSlot. Backs up the whole single-player file
// to backupDir first (refuses without a backup, and refuses when the single-player file is missing or invalid), writes atomically
// and reads the result back through mp_save_validate. backupOut gets the backup's path.
int mp_save_writeback_file(const char* mpPath, int mpSlot, const char* spPath, int spSlot, const char* backupDir, int64_t unixTime,
                           int keep, char* backupOut, size_t backupCap, uint8_t* outImage);
// savefix: restore backup file backupPath over the single-player GameData.bin spPath (an explicit player choice). The backup must
// validate. A valid current file is backed up first (mp_save_backup; refuses without it, beforeOut gets its path); a damaged one
// is kept aside as backupDir/Damaged-<time>.bin; a file that cannot be read now refuses (MP_SAVE_OP_DST_INVALID). Then an atomic
// replace and a read-back. MP_SAVE_OP_OK also when the file already is that backup.
int mp_save_restore_backup(const char* backupPath, const char* spPath, const char* backupDir, int64_t unixTime, int keep, char* beforeOut,
                           size_t beforeCap);

// ---- progress compare (prompt before a co-op slot with more stars is overwritten) -------------------------------------------
// Power stars recorded in a mario/luigi block (the 'GALA' chunk: popcount of every galaxy's mPowerStarFlag). -1 if the block
// cannot be parsed (an empty or never-created block parses as 0).
int mp_save_block_stars(const uint8_t* block, uint32_t size);
// Stars of user file `slot` in an image: mario + luigi (each -1 -> 0). -1 when the slot is not in the image.
int mp_save_slot_stars(const uint8_t* image, int slot, int* mario, int* luigi);
int mp_save_view_state(const MpSaveSnapshotView* v);   // savebug: 1 created file's progress, 0 never-created (blank) slot, -1 unreadable
// savefix: both player blocks of slot parse; their stars. False: missing or unparseable.
bool mp_save_block_stars_ok(const uint8_t* image, int slot, int* mario, int* luigi);
// savefix (m-files): the state of user file `slot` of the GameData.bin at path, reading it once.
//   -1 unreadable: the file is there but cannot be read / validated, the slot does not parse, no memory, or no file but an
//      interrupted replace (.mpbak / .mptmp) beside it;  0 new: no GameData.bin, or (single-player only, coop false) never created;
//   1 used: the slot parses; *stars = Power Stars (mario + luigi). *stars = -1 unless 1.
int mp_save_slot_state_file(const char* path, bool coop, int slot, int* stars);
// flow (review 2 #3): 1 the user file was created (config<N>'s CONF chunk, as the game's file select), 0 never created, -1 no such slot
// or a block that does not parse.
int mp_save_slot_state(const uint8_t* image, int slot);
// < 0 when src has fewer stars than dst (the ui should ask before overwriting dst), 0 equal, > 0 more.
int mp_save_compare_progress(const uint8_t* srcImage, int srcSlot, const uint8_t* dstImage, int dstSlot);

// ---- session save policy (DESIGN.md sections 1 and 8) -----------------------------------------------------------------------
// Only the ORIGINAL host (slot 0, before any host migration) saves during a session, with the prompt auto-confirmed. Every
// other console, a console that became host by migration, and everyone after a migration (epoch > 0) keeps saves suppressed.
enum MpSavePolicy { MP_SAVE_POLICY_NORMAL = 0, MP_SAVE_POLICY_AUTOCONFIRM = 1, MP_SAVE_POLICY_SUPPRESS = 2 };
int mp_save_policy(bool active, int localSlot, int hostSlot, unsigned hostEpoch, bool bridgeSuppressed);

// ---- recovery checkpoints (<mp root>/../recovery/session-NNNNNNNN.bin; never a save file) ---------------------------------
// One file per session per console, rewritten atomically at each checkpoint; the newest MP_SAVE_RECOVERY_KEEP sessions are kept.
// Layout (little-endian): magic u32 'MPRC', version u16 (1), header bytes u16 (76), session id u32, seq u32, unix time s64,
// flags u8, session slot u8, source kind u8, source slot u8, scenario u8, reserved u8, stars u16, galaxy char[32] (NUL padded),
// marioLen u16, luigiLen u16, configLen u16, reserved u16, crc32 u32 over everything after the header, then the three blocks.
#define MP_SAVE_REC_MAGIC 0x4352504Du  // "MPRC"
#define MP_SAVE_REC_VERSION 1
#define MP_SAVE_REC_HEADER 76
#define MP_SAVE_REC_MAX (MP_SAVE_REC_HEADER + 2 * MP_SAVE_GAME_BLOCK + MP_SAVE_CONFIG_BLOCK)
#define MP_SAVE_RECOVERY_KEEP 3
#define MP_SAVE_REC_FLAG_ORIGINAL_HOST 0x01
#define MP_SAVE_REC_FLAG_MARIO 0x02
enum MpSaveSource { MP_SAVE_SRC_NONE = 0, MP_SAVE_SRC_COOP = 1, MP_SAVE_SRC_SINGLE = 2 };
struct MpSaveRecoveryInfo {
    uint32_t sessionId, seq;
    int64_t unixTime;
    uint8_t flags, slot, srcKind, srcSlot, scenario;
    uint16_t stars;
    char galaxy[33];
};
uint32_t mp_save_recovery_pack(const MpSaveRecoveryInfo* info, const MpSaveSnapshotView* blocks, uint8_t* out, uint32_t cap);
// MP3 snapval: the blocks must pass mp_savecheck at MP_SAVECHECK_FULL (installed limits), like a snapshot.
bool mp_save_recovery_parse(const uint8_t* data, uint32_t len, MpSaveRecoveryInfo* info, MpSaveSnapshotView* blocks);
bool mp_save_is_recovery(const uint8_t* data, uint32_t len);
void mp_save_recovery_path(const char* dir, int index, char* out, size_t cap);
// Session indices present in dir (a session-N.bin or its .mptmp/.mpbak), ascending. Returns the count (<= max).
int mp_save_recovery_list(const char* dir, int* indices, int max);
int mp_save_recovery_next_index(const char* dir);  // max present (also in dir/discarded) + 1
// Atomic write of session `index`, then delete all but the newest `keep` sessions (with their .mptmp/.mpbak).
bool mp_save_recovery_write(const char* dir, int index, const uint8_t* data, uint32_t len, int keep);
// Repair (mp_save_recover_with) and read session `index`; true when it parses.
bool mp_save_recovery_read(const char* dir, int index, uint8_t* buf, uint32_t cap, uint32_t* len);
// Move session `index` to dir/discarded (the newest keepDiscarded kept there), or delete it outright.
bool mp_save_recovery_discard(const char* dir, int index, int keepDiscarded);
bool mp_save_recovery_delete(const char* dir, int index);

// ---- background writer (recovery checkpoints never wait on the SD card on the game thread) --------------------------------
// submit copies the bytes (latest wins if a write is still pending) and wakes one worker thread (created on first use: call it
// inside a PortHostAllocScope on a game thread). Results come back through take_result (call from the game thread to log them).
struct MpSaveAsyncResult { bool ok; int index; uint32_t bytes; uint32_t micros; };
bool mp_save_async_submit(const char* dir, int index, const uint8_t* data, uint32_t len, int keep);
bool mp_save_async_idle(void);
// fix2: bounded. wait_ms: true when the writer is idle, false after ms milliseconds (a hung card write must not block the game thread).
// mp_save_async_wait = wait_ms(MP_SAVE_ASYNC_WAIT_MS); callers treat false as "the file can't be used right now".
#define MP_SAVE_ASYNC_WAIT_MS 15000
bool mp_save_async_wait_ms(int ms);
bool mp_save_async_wait(void);
void mp_save_test_hang_writer(int on);   // test hook: the background writer stops before its write while on
void mp_save_test_async_timeout_ms(int ms);   // test hook: the timeout of mp_save_async_wait (0 = default)
bool mp_save_async_take_result(MpSaveAsyncResult* out);

// Test hook: make mp_save_write_atomic stop ("crash") right after step n (1 tmp created, 2 half the bytes written, 3 tmp complete
// and synced, 4 old .mpbak removed, 5 primary renamed to .mpbak, 6 tmp renamed to primary). 0 = off.
void mp_save_test_crash_after(int step);
// Test hook: the next atomic writes stop after `bytes` bytes of the temporary (a power cut inside write(); the torn temporary stays). -1 = off.
void mp_save_test_cut_write(int bytes);
void mp_save_test_fail_from(int n);   // scenarios-final: from the n-th GameData.bin commit on, mp_save_commit_tmp fails and leaves the .mptmp (n<=0: off)

#endif  // PORT_MP_SAVEFILE_H
