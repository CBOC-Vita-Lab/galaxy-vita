// The stale-settings guard (Galaxy 3.0). A player's player-settings.cfg outlives versions. The first time a NEWER build starts on an older file this runs once, before the file
// is loaded: the file is stamped, one backup copy is made next to it, the player's existing .bak is moved aside (not removed), the few lines whose meaning changed are written
// out explicitly, and the log gets one line.
//
//   - Stamp: a comment line `# player-settings written by Galaxy 3.0.0` among the first lines. The parser skips comments and a save keeps them. No stamp = a file of 2.2.1 or
//     earlier. A stamp at or past this build's major.minor with no migration step owed is never touched (a newer file under an older build is left for its own build).
//   - Steps: each step in kSteps has a `since` version (full compare). A step is owed when since > the stamped version (every step, for an unstamped file) and since <= this build.
//     The file is Current only when no step is owed and its stamp is at or past this build's major.minor. So a step with a patch-level `since` (3.0.1) runs on a 3.0.0 file.
//   - Backup: `player-settings.before-3.0.cfg` beside the file (major.minor of this build), written complete and verified BEFORE the file is changed. It is made only when the
//     file comes from an older release line (no stamp, or an older major.minor). "Present" means a regular, non-empty file that reads back: then it is kept as it is (it is the
//     older copy) and never overwritten. If the name exists as anything else (a directory, an empty or unreadable file) or the copy cannot be made, the outcome is BackupFailed and
//     the settings file is left exactly as it is.
//   - The player's own .bak: ordinary saves rotate it as always (the store removes the previous .bak when the file becomes the new .bak). The guard's own first-boot rewrite
//     (the one that crosses to a newer release line) does NOT remove it: an existing .bak is renamed to the first free of `player-settings.before-3.0-bak.cfg`, `...-bak-2.cfg`
//     ... `...-bak-9.cfg`. If the .bak is byte for byte equal to a copy that already exists (the before-3.0 copy or one of those) nothing is moved and the ordinary rotation goes on.
//     If no name is free (or the rename fails) the .bak stays where it is and the file is not rewritten (OldBackupBlocked). If the rewrite itself then fails the .bak is put back
//     when its name is still free, else the log line says where it is. A later rewrite inside the same release line (a patch step) uses the ordinary rotation: its .bak is the file
//     the guard itself wrote earlier.
//     Nothing else is removed: the only deletions are this code's own temporary files and the store's usual .bak rotation described above.
//   - No settings file but a .bak (a cut between the store's two renames): the loader would read that .bak, so it is migrated the same way, "from the .bak": same backup-first rules, the
//     new file is written, the .bak stays where it is (it is the source). A leftover `player-settings.cfg.tmp` (a cut save, newer than the .bak) is the source instead when it parses
//     fully; either way it is renamed to the first free `player-settings.before-3.0-tmp[-N].cfg` (N up to 9) before the guard writes, so it is never truncated or deleted (if no name is
//     free the file is not written: OldBackupBlocked).
//     A .tmp source with a current stamp (the guard's own cut between the store's two renames: the loader ignores .tmp, so the file is missing and the .bak would be read every boot) is
//     never "Current": it is set aside and the file is written from its text; the player's .bak (the last committed file) is moved to the before-3.0-bak names like any other, but only
//     AFTER the write succeeded (until then the .bak is the loader's fallback, so a power cut at any point leaves a readable settings file or the .bak, never neither). A cut after the
//     set-aside leaves the .bak in place, the newer values in player-settings.before-3.0-tmp.cfg, and the next boot migrates that .bak; a cut mid-write leaves a partial .tmp, which is
//     set aside the same way; a cut after the write but before the late move leaves the new file beside the .bak (Current). A late move that fails is logged and the outcome is Migrated.
//     A current .tmp that owes nothing is renamed straight into place (no copy, no window). If the write of a copy fails, the set-aside copy is renamed back to .tmp when that name is free,
//     else the log line names it. A .bak byte for byte equal to the before-3.0 copy is not copied again by the late move.
//   - Meaning changed (explicit, version-gated, see kSteps in settings_guard.cpp): "fps_limit=0 without perf_mode" was the Quality default of 2.2.x; the parser reads it as Quality (30), as c181 does (it is not the 60 mode);
//     the file gets the pair `fps_limit=30` `perf_mode=0` that every build reads the same way. motion_gain (a percentage) gets its motion_level.
//   - New keys (Safe saves, multiplayer rows, ...) are OPTIONAL keys: absent means this build's default, so a 2.2.1 file needs no new line and the player's choices are all kept.
//     NEVER make a new key required: a file without it would read as an incomplete snapshot (Malformed) and fall back to the .bak or to the defaults.
//   - Every other line (controls, language, unknown keys, comments, line endings) stays byte for byte. The rewritten text must read back as the SAME Config as the old file did;
//     if not, nothing is written.
//   - A file that cannot be read (damaged, stamped or not, or too large) is not touched and gets no stamp (the loader falls back to .bak as before); a byte copy of it is kept
//     once as `player-settings.unreadable-3.0.cfg` when that name is free and the file can be read (up to 1 MiB); an empty file gets no copy (nothing in it), only the log line. A file with schema > 1 (a newer format) is left completely alone.
// All file access goes through FileOps (the same as the store), so the host test runs it on memory files.
#pragma once
#include "port/player_settings.h"
#include <string>
#include <vector>

// No version default lives here: the caller passes this build's release (vita/vgx-game/settings_runtime.cpp takes GALAXY_SETTINGS_VERSION, which
// vita/vgx-game/CMakeLists.txt derives from SMG_APP_VERSION in platform/src/mp/mp_service.cpp and #errors without).

namespace port::settings {
enum class GuardOutcome : uint8_t {
    NoFile,           // no settings file: the build's own defaults, nothing to stamp
    Current,          // already stamped by this build or a newer one: nothing done
    Migrated,         // stamped, backed up (or the backup was already there), meaning-changed lines written
    Unreadable,       // damaged or too large: left as it is (a copy kept when possible)
    NewerFormat,      // schema above 1: left alone
    BackupFailed,     // the backup copy could not be made or its name is taken by something that is not a good copy: the file is left as it is
    WriteFailed,      // the new text could not be put in place (the store's rotation leaves the old file or .bak)
    Rejected,         // the migrated text would read differently from the old file: not written (a bug guard)
    IoError,          // stat or read failed: nothing changed
    BadVersion,       // this build's version string is not dotted numbers
    OldBackupBlocked, // the player's .bak could not be moved to its before-3.0 name (name taken, or the rename failed): .bak and the file are left as they are
};
struct GuardReport {
    GuardOutcome outcome = GuardOutcome::NoFile;
    bool backupMade = false;       // this run wrote the backup copy
    bool backupExisted = false;    // a backup of this name was already there and was kept
    unsigned changedLines = 0;     // lines written or replaced besides the stamp
    std::string backupPath;        // empty when none
    bool oldBackupMoved = false;   // the player's .bak was renamed to oldBackupPath before the rewrite
    std::string oldBackupPath;     // where it went; empty when none
    std::string tmpSetAside;       // a leftover player-settings.cfg.tmp (cut save) renamed to this name before the write; empty when none
    std::string line;              // one log line (no newline); empty for NoFile / Current
};
// One migration step: owed to a file stamped before `since` (or not stamped) and when `since` <= this build. It sees the old text and the Config the old text reads as, and
// returns the text. It must keep that Config (RunSettingsGuard checks). `notes` collects the names of what it changed, `changed` counts lines.
struct GuardStep { const char* since; const char* name; std::string (*run)(const std::string& text, const Config& cfg, const controls::ControlApi&, std::vector<std::string>& notes, unsigned& changed); };
// The one-time backup name for `path`: "player-settings.cfg" -> "player-settings.before-3.0.cfg".
std::string GuardBackupPath(const char* path, const char* version);
// The names the player's existing .bak can get on the first-boot rewrite: "player-settings.cfg" -> "player-settings.before-3.0-bak.cfg".
std::string GuardOldBackupPath(const char* path, const char* version, int n = 1);   // n = 1: "-bak", n >= 2: "-bak-N"
GuardReport RunSettingsGuard(const char* path, const char* version, const controls::ControlApi&, const FileOps&);
// setfix31: the stamp line ("# player-settings written by Galaxy <version>", no newline) the guard writes, for SetWriterStamp (port/player_settings.h).
std::string GuardStampLine(const char* version);
// The outcomes after which this build owns the file (or there is none): only then do the writers stamp it (SetWriterStamp).
inline bool GuardFileIsCurrent(GuardOutcome o) { return o == GuardOutcome::NoFile || o == GuardOutcome::Current || o == GuardOutcome::Migrated; }
// The same with an explicit step table (the host test uses it for a patch-level step).
GuardReport RunSettingsGuardSteps(const char* path, const char* version, const controls::ControlApi&, const FileOps&, const GuardStep* steps, size_t count);
}
