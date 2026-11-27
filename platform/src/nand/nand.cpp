// NAND flash emulation: save data lives in files under the app's storage.
#include <stdint.h>
extern "C" void port_gpu_memory_written(void* addr, uint32_t size);
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/nand.h"
#include "../mp/mp_savefile.h"  // MP3: boot-time repair after an interrupted write-back

// Slow host file-system calls run on a helper thread while this guest thread sleeps (platform/src/dvd/dvd.cpp): an fsync or a file create
// on the SD card can take hundreds of ms, and a guest thread that blocks in the host holds the CPU token and starves the audio interrupts.
extern "C" void port_host_job_wait(const char* what, void (*fn)(void*), void* arg);
template <class F>
static void hostJob(const char* what, F& f) {
    port_host_job_wait(what, [](void* p) { (*static_cast<F*>(p))(); }, &f);
}

// ---- GXV_NAND_SAFE (strk26; after the Strikers port's np_safe_file.h): a save that a power cut or a crash leaves half written
// The game writes its save in place: NANDOpen (no truncation), NANDWrite of the whole record, NANDClose (fsync). A cut in the middle leaves the old
// and the new record mixed in one file; the game's own checksum then rejects it, and nothing is left to go back to. So, for a file opened for writing:
//   1. P.bak is made first: a complete copy of P as it is (when P is not empty and no earlier interrupted write is waiting), written and synced;
//   2. P.wip is created (empty): "a write is under way";
//   3. the game writes P as before; NANDClose syncs it and removes P.wip.
// Any open of P (reading or writing) that finds P.wip with no write of this process open on P knows the last write never finished: P is put back from P.bak
// (copied in place and synced, then P.wip is removed; a cut during the copy leaves P.wip and P.bak, so the next open does it again). A P that is missing
// or empty with a P.bak beside it is put back the same way. Nothing is renamed over an existing file (newlib's rename on the Vita is remove + rename, not
// atomic), nothing the game sees changes, and the files beside P are never listed by the game (it opens its save by name). NANDDelete takes P.bak and
// P.wip with P: a deleted save does not come back.
//   GXV_NAND_SAFE=1 (default 0 until proven on the console) / 0: the writes as before.  "nand-safe:" basic-log line when a save is put back or a backup made.
static bool nandSafeOn() {
    static const bool on = [] { const char* e = getenv("GXV_NAND_SAFE"); return e && *e && atoi(e) != 0; }();
    return on;
}
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
static std::mutex sSafeMutex;
static std::set<std::string> sSafeOpen;  // host paths open for writing now (this process)
// fd -> the host path it was opened under: the save root can change while a save is open, so NANDClose must not rebuild the path from the
// root of the moment (that lost the mark and left a .wip behind, which the next open would take as an interrupted write).
static std::map<int, std::string> sSafeOpenHp;
static unsigned sSafeBackups = 0, sSafeRestores = 0, sSafeFailures = 0;
static bool safeExists(const std::string& p, off_t* size = nullptr) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (size) *size = st.st_size;
    return true;
}
// Copies src over dst (dst made or truncated), synced. False: dst is not a complete copy.
static bool safeCopy(const std::string& src, const std::string& dst) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (out < 0) { close(in); return false; }
    bool ok = true;
    char buf[8192];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, static_cast<size_t>(n - done));
            if (w <= 0) { ok = false; break; }
            done += w;
        }
        if (!ok) break;
    }
    if (ok) fsync(out);
    if (close(out) != 0) ok = false;
    close(in);
    return ok;
}
static void safeLog(const char* what, const std::string& path) {
    if (port_log_summary) port_log_summary("nand-safe: %s %s (backups %u, restores %u, failures %u)", what, path.c_str(), sSafeBackups, sSafeRestores, sSafeFailures);
}
// Before any open of hp: put the file back when the last write of it never finished (or it has gone missing / empty and a backup exists).
static void safeRecover(const std::string& hp) {
    const std::string bak = hp + ".bak", wip = hp + ".wip";
    {
        std::lock_guard<std::mutex> l(sSafeMutex);
        if (sSafeOpen.count(hp)) return;  // (a write of this process is under way: the .wip is ours)
    }
    off_t size = 0;
    const bool have = safeExists(hp, &size);
    const bool interrupted = safeExists(wip);
    off_t bakSize = 0;
    const bool haveBak = safeExists(bak, &bakSize) && bakSize > 0;
    if (!haveBak) {
        if (interrupted && have) unlink(wip.c_str());  // (nothing to go back to: what is there is all there is)
        return;
    }
    if (!(interrupted || !have || size == 0)) return;
    if (safeCopy(bak, hp)) {
        unlink(wip.c_str());
        ++sSafeRestores;
        safeLog(interrupted ? "an interrupted write: the save is back from its backup:" : "a missing or empty save is back from its backup:", hp);
    } else {
        ++sSafeFailures;
        safeLog("the backup could not be copied back:", hp);
    }
}
// Before an open for writing: the backup of what is there, and the "write under way" mark.
static void safePrepareWrite(const std::string& hp) {
    const std::string bak = hp + ".bak", wip = hp + ".wip";
    off_t size = 0;
    if (safeExists(hp, &size) && size > 0 && !safeExists(wip)) {  // (an unrecovered .wip is only here when safeRecover found no usable backup: keep the file as it is)
        if (safeCopy(hp, bak)) {
            ++sSafeBackups;
            if (sSafeBackups <= 3) safeLog("backup made:", hp);
        } else {
            ++sSafeFailures;
            safeLog("the backup could not be made:", hp);
            unlink(bak.c_str());  // (a torn backup is worse than none)
            return;                 // (no backup: no mark; the write goes ahead as it always did)
        }
    }
    if (safeExists(bak)) {
        int fd = open(wip.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
    PortHostAllocScope scope;  // (heap36: a host job runs inline on the game thread when interrupts are off (dvd.cpp port_host_job_wait): the set's node is host memory)
    std::lock_guard<std::mutex> l(sSafeMutex);
    sSafeOpen.insert(hp);
}
static void safeFinishWrite(const std::string& hp, bool synced) {
    {
        std::lock_guard<std::mutex> l(sSafeMutex);
        if (!sSafeOpen.erase(hp)) return;
    }
    if (synced) unlink((hp + ".wip").c_str());
}

static std::string sNandRoot;
static std::string sNandBootRoot;  // MP3: the root port_boot set: the single-player saves
static const char kHomeDir[] = "/title/00010000/524d4750/data";

// savelife review: a stat answer other than "there" or "not there" (ENOENT, or ENOTDIR for a folder that is not there yet) says nothing about the file.
static bool statUnknown(int rc, int err) { return rc != 0 && err != ENOENT && err != ENOTDIR; }

// savelife review (boot-freeze way out): GameData.bin is missing and a .mptmp (an interrupted replace) is beside it. mp_save_recover completes it. When it
// cannot (a card read error on the .mptmp), the first such boot only refuses (the game gets a read error, nothing is created or truncated) and leaves an
// empty GameData.bin.mptmp.failed. A LATER boot that still cannot repair and finds that file renames the unreadable .mptmp to GameData.bin.mptmp.unread
// (.unread.1, .2 ... when taken: nothing is ever deleted, and nothing reads it), creates the empty "repair in progress" marker and repairs again: that
// restores a valid .mpbak. If the file is STILL missing and a .mpbak is still there (unreadable or torn too), the .mpbak is renamed the same way to
// GameData.bin.mpbak.unread[.N] (never the rotating .mpbak.old), the marker and .failed go, and the game starts fresh. Every step is safe to be cut by a
// power loss: the next boot finds .failed (also with no .mptmp) and goes on. A successful repair removes .failed. Everything is logged; the set-aside
// and fresh-start events also go to the summary log. Only *.unread* names are ever created for the player's data; nothing deletes them.
static int fileIs(const std::string& p, off_t* size = nullptr) {   // 1 there, 0 not there, -1 cannot tell
    struct stat st;
    const int rc = stat(p.c_str(), &st), e = errno;
    if (rc == 0) {
        if (size) *size = st.st_size;
        return 1;
    }
    return statUnknown(rc, e) ? -1 : 0;
}
static std::string freeAsideName(const std::string& from) {
    for (int n = 0; n < 1000; n++) {
        const std::string to = from + ".unread" + (n ? "." + std::to_string(n) : "");
        if (fileIs(to) == 0) return to;
    }
    return std::string();
}
static void bootSummary(const char* what, const char* name) {
    if (port_log_summary) port_log_summary("nand: %s %s", what, name);
}
static void bootRepairMissing(const std::string& primary) {
    const std::string tmp = primary + ".mptmp", failed = primary + ".mptmp.failed", bak = primary + ".mpbak";
    off_t tsz = 0;
    bool changed = false;
    if (fileIs(tmp) == 1) {
        const bool ok = mp_save_recover(primary.c_str(), &changed);
        port_log("nand: GameData.bin was missing and a .mptmp (an interrupted save) was beside it; restored: %s", ok && changed ? "yes" : "no");
    }
    int binS = fileIs(primary), tmpS = fileIs(tmp, &tsz), failS = fileIs(failed);
    if (binS != 0) {   // repaired (or cannot tell)
        if (binS == 1 && failS == 1 && unlink(failed.c_str()) == 0) port_log("nand: the repair worked: %s removed", failed.c_str());
        return;
    }
    if (tmpS < 0 || failS < 0) return;   // cannot tell: change nothing
    if (tmpS == 0 && fileIs(bak) != 1) {   // the .mptmp was torn and removed, or only a stale .failed was left: nothing to restore
        if (failS == 1) unlink(failed.c_str());
        return;
    }
    if (failS == 0) {   // boot 1: refuse, make the mark
        const int fd = open(failed.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0660);
        if (fd >= 0) {
            close(fd);
            port_log("nand: GameData.bin is missing and its .mptmp / .mpbak could not be read or put in place (boot 1): refusing; %s made, a second boot will set them aside", failed.c_str());
        } else {
            port_log("nand: boot 1: %s could not be made (errno %d): refusing without the mark (no second-boot way out until it can)", failed.c_str(), errno);
        }
        return;
    }
    // a later boot that still cannot repair
    if (tmpS == 1 && tsz > 0) {
        mp_save_recover(primary.c_str(), &changed);   // one more try just before setting anything aside
        binS = fileIs(primary);
        tmpS = fileIs(tmp, &tsz);
        if (binS == 1) {
            if (unlink(failed.c_str()) == 0) port_log("nand: boot 2: the repair worked this time: %s removed", failed.c_str());
            return;
        }
        if (tmpS == 1 && tsz > 0) {
            const std::string to = freeAsideName(tmp);
            if (to.empty() || rename(tmp.c_str(), to.c_str()) != 0) {
                port_log("nand: boot 2: could not set the unreadable .mptmp aside (errno %d): it stays, GameData.bin stays refused", errno);
                return;
            }
            port_log("nand: boot 2: the .mptmp still cannot be read: kept aside as %s (never deleted); trying the .mpbak", to.c_str());
            bootSummary("the unreadable save file was kept aside as", to.c_str());
            tmpS = 0;
        }
    }
    if (tmpS == 0) {   // (also the state after a power cut between the rename and the marker)
        const int mfd = open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0660);
        if (mfd < 0) {
            port_log("nand: boot 2: the repair marker could not be made (errno %d): %s kept, nothing repaired", errno, failed.c_str());
            return;
        }
        fsync(mfd);
        close(mfd);
    }
    bool ch2 = false;
    const bool ok2 = mp_save_recover(primary.c_str(), &ch2);
    port_log("nand: boot 2: repair with the repair marker: ok: %s, restored from the .mpbak: %s", ok2 ? "yes" : "no", ch2 ? "yes" : "no");
    if (fileIs(primary) == 1) {
        if (unlink(failed.c_str()) != 0) port_log("nand: could not remove %s (errno %d)", failed.c_str(), errno);
        bootSummary("an earlier save was restored (an unreadable copy, if any, is kept as .unread)", primary.c_str());
        return;
    }
    // still no file: the .mpbak (if any) could not be used either: kept aside, never deleted, and the game starts fresh
    if (fileIs(bak) == 1) {
        const std::string to = freeAsideName(bak);
        if (to.empty() || rename(bak.c_str(), to.c_str()) != 0) {
            port_log("nand: boot 2: could not set the unreadable .mpbak aside (errno %d): the marker and %s stay", errno, failed.c_str());
            return;
        }
        port_log("nand: boot 2: the .mpbak could not be used either: kept aside as %s (never deleted); the game starts with a fresh save", to.c_str());
        bootSummary("no readable save: the old copy was kept aside as", to.c_str());
    } else {
        port_log("nand: boot 2: no usable save was left: the game starts with a fresh save");
        bootSummary("no readable save: fresh start", primary.c_str());
    }
    off_t msz = 0;
    if (fileIs(tmp, &msz) == 1 && msz == 0) unlink(tmp.c_str());   // (our own empty marker)
    unlink(failed.c_str());
}

// MP3: safe root setter. Multiplayer switches the root at run time (decomp/src/Game/MP/MpSaveGame.cpp, title/file select only, no
// save sequence running): the string is always (re)allocated in the host heap, also when called from a game thread (a game-heap
// allocation would dangle after a scene change). The first call (port_boot) records the boot root and repairs the single-player
// GameData.bin after an interrupted write (mp_save_recover): a MISSING file is completed from its .mptmp (a lone .mpbak is a deleted save: left as
// it is and logged); an existing file with a .mptmp or .mpbak beside it is checked: a valid one loses only the stale .mptmp, a torn one is replaced
// from a valid .mptmp, else a valid .mpbak (and kept aside as .mpbad). One stat per boot otherwise, nothing else changes.
extern "C" void port_nand_set_root(const char* root) {
    PortHostAllocScope scope;
    sNandRoot = root ? root : "";
    if (sNandBootRoot.empty()) {
        sNandBootRoot = sNandRoot;
        const std::string primary = sNandRoot + kHomeDir + "/GameData.bin";
        struct stat st;
        if (stat(primary.c_str(), &st) == 0) {
            // smallfix: GameData.bin is there. A .mptmp beside it is a write cut before the old file moved (steps 1-4); a .mpbak is the previous
            // version. mp_save_recover decides on content: a valid GameData.bin removes the stale .mptmp (so a later deletion cannot bring it
            // back); a torn one is replaced from a valid .mptmp, else a valid .mpbak, and kept aside as .mpbad. Nothing else is touched.
            if (stat((primary + ".mptmp").c_str(), &st) == 0 || stat((primary + ".mpbak").c_str(), &st) == 0) {
                bool changed = false;
                const bool ok = mp_save_recover(primary.c_str(), &changed);
                port_log("nand: GameData.bin exists and a .mptmp / .mpbak is beside it; checked: %s, restored from it: %s", ok ? "ok" : "not ok", changed ? "yes" : "no");
            }
            unlink((primary + ".mptmp.failed").c_str());   // (a stale refusal mark of an earlier boot: the file is there now)
        } else {
            if (stat((primary + ".mptmp").c_str(), &st) == 0 || stat((primary + ".mptmp.failed").c_str(), &st) == 0) {
                // smallfix: a .mptmp beside a missing file proves a replace that was cut between its two renames: complete it
                bootRepairMissing(primary);
            } else if (stat((primary + ".mpbak").c_str(), &st) == 0) {
                // smallfix: only a .mpbak: no sign of an interrupted write; the player deleted the save outside the game. Nothing is restored.
                port_log("nand: GameData.bin is missing and only GameData.bin.mpbak exists (no .mptmp): not restored, left as it is");
            }
        }
    }
}

// MP3: the current and the boot save roots (copies; NUL-terminated). Return the length, or -1 when cap is too small.
static int copyRoot(const std::string& r, char* out, int cap) {
    if (!out || cap <= 0 || r.size() + 1 > (size_t)cap) return -1;
    memcpy(out, r.c_str(), r.size() + 1);
    return (int)r.size();
}
extern "C" int port_nand_get_root(char* out, int cap) { return copyRoot(sNandRoot, out, cap); }
extern "C" int port_nand_get_boot_root(char* out, int cap) { return copyRoot(sNandBootRoot, out, cap); }

static std::string hostPath(const char* path) {
    PortHostAllocScope scope;
    std::string p = path ? path : "";
    if (p.empty() || p[0] != '/') {
        p = std::string(kHomeDir) + "/" + p;
    }
    return sNandRoot + p;
}

static void makeDirs(const std::string& path) {
    PortHostAllocScope scope;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            std::string d = path.substr(0, i);
            mkdir(d.c_str(), 0770);
        }
    }
}

// MP3 savefix: while multiplayer code is involved (the multiplayer save root always; the single-player root during a "Play on my
// file" session; decomp/src/Game/MP/MpSaveGame.cpp decides, port_nand_set_atomic_writes) the game's own write of GameData.bin goes
// through the atomic writer: NANDCreate does not create the file, NANDOpen for writing opens GameData.bin.mptmp (truncated),
// NANDWrite fills it, NANDClose syncs it and hands it to mp_save_commit_tmp (validated as a GameData.bin, read back, old file kept
// as .mpbak, renamed in, read back). A power cut at any point leaves the old file or the new one, or a state the boot / root
// switch repair (mp_save_recover) completes. Single player with Safe saves off (the default; SMG_ATOMIC_SAVES=0 forces it) and no multiplayer session: exactly the original in-place write.
static std::atomic<bool> sAtomicWrites{false};
static const unsigned char kAtomicMark = 0xA7;   // NANDFileInfo::mark of a write that goes to the temporary
extern "C" void port_nand_set_atomic_writes(bool on) { sAtomicWrites.store(on); }
extern "C" bool port_nand_atomic_writes(void) { return sAtomicWrites.load(); }
// A1 (savebug): the atomic writer for EVERY GameData.bin write, single player included, is an OPTION, OFF by default (owner decision, 13 Nov):
// a power cut, a pulled card or a full card in the middle of the game's own in-place write can leave a torn GameData.bin (22 of 300 random
// cut sequences in the savelife harness; 0 of 300 with the atomic writer), but the writer costs 4.5 to 6 ms a save on the dev machine against
// 1 to 2 ms (an fsync, three reads of 48,640 bytes) and puts a second 48 KiB copy (.mpbak) beside the player's GameData.bin.
// Safe saves is ALWAYS ON since 3.0 (the owner's decision; Settings has no row for it). Testers: env SMG_ATOMIC_SAVES=0 forces it off,
// =1 on (ux0:data/smg/env.txt on the Vita). An env value, when there is one, wins over port_nand_set_all_atomic().
// While a multiplayer session is active the atomic writer is on regardless (sAtomicWrites, MpSaveGame.cpp). Read once, at the first write.
static std::atomic<int> sAllAtomic{1};   // the set value (default ON since 3.0: Safe saves is always on; env SMG_ATOMIC_SAVES=0 still forces it off)
extern "C" void port_nand_set_all_atomic(bool on) { sAllAtomic.store(on ? 1 : 0); }
static bool allAtomic() {
    static const int env = [] {   // -1: no env value; 0 / 1: the override
        const char* e = getenv("SMG_ATOMIC_SAVES");
        return (e && e[0] == '0') ? 0 : (e && e[0] == '1') ? 1 : -1;
    }();
    return env >= 0 ? env == 1 : sAllAtomic.load() == 1;
}
extern "C" bool port_nand_all_atomic(void) { return allAtomic(); }
// setfix31b: what the native Settings' "Save and Start Again" watches while a save is in flight: finished NANDWrite / NANDClose calls (they move while
// a save goes on) and the ones that failed.
static std::atomic<unsigned> sNandProgress{0}, sNandFailures{0};
static s32 noteNandProgress(s32 r) { sNandProgress.fetch_add(1, std::memory_order_relaxed); if (r < 0) sNandFailures.fetch_add(1, std::memory_order_relaxed); return r; }
extern "C" unsigned port_nand_progress(void) { return sNandProgress.load(std::memory_order_relaxed); }
extern "C" unsigned port_nand_failures(void) { return sNandFailures.load(std::memory_order_relaxed); }   // smallfix: the player's "Safe saves" choice (or the env override), not the multiplayer flag
// savesus S6: two facts decided at one call must still hold at the next, whatever the flag or the root did in between.
//  (1) NANDCreate that skipped creating GameData.bin (atomic on) is followed by an Open that finds the flag off: that Open creates the file
//      (only after such a skip, only for a missing file) instead of failing the player's first save.
//  (2) an atomic write opened under one root is committed under THAT root (the host path is kept per open file, not rebuilt at Close from
//      the root of the moment). The save sequence is the only writer; four slots are far more than it ever has open.
static std::atomic<bool> sCreateSkipped{false};
namespace {
std::mutex sAtomicMu;
struct AtomicOpen { int fd = -1; std::string hp; } sAtomicOpen[4];
}  // namespace
static void rememberAtomicOpen(int fd, const std::string& hp) {
    PortHostAllocScope scope;   // (the strings live in the host heap, as sNandRoot)
    std::lock_guard<std::mutex> l(sAtomicMu);
    for (AtomicOpen& a : sAtomicOpen) if (a.fd < 0) { a.fd = fd; a.hp = hp; return; }
}
static bool atomicOpenFor(const std::string& hp) {   // an atomic write of this file is open right now
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> l(sAtomicMu);
    for (const AtomicOpen& a : sAtomicOpen) if (a.fd >= 0 && a.hp == hp) return true;
    return false;
}
static bool takeAtomicOpen(int fd, std::string* hp) {
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> l(sAtomicMu);
    for (AtomicOpen& a : sAtomicOpen) if (a.fd == fd) { *hp = a.hp; a.fd = -1; a.hp.clear(); return true; }
    return false;
}
static bool atomicTarget(const char* path) {
    if (!(sAtomicWrites.load() || allAtomic()) || path == nullptr) return false;
    const char* base = strrchr(path, '/');
    return strcmp(base ? base + 1 : path, "GameData.bin") == 0;
}

static s32 errnoToNand(int e) {
    switch (e) {
    case ENOENT:
        return NAND_RESULT_NOEXISTS;
    case EEXIST:
        return NAND_RESULT_EXISTS;
    case EACCES:
    case EPERM:
        return NAND_RESULT_ACCESS;
    case ENOSPC:
        return NAND_RESULT_MAXBLOCKS;
    case ENOTEMPTY:
        return NAND_RESULT_NOTEMPTY;
    default:
        return NAND_RESULT_UNKNOWN;
    }
}

extern "C" {

s32 NANDInit(void) { return NAND_RESULT_OK; }

s32 NANDGetHomeDir(char path[NAND_MAX_PATH]) {
    strcpy(path, kHomeDir);
    return NAND_RESULT_OK;
}

// smallfix ("a deleted save stays deleted"): a GameData.bin is about to be created where none exists. A lone .mpbak beside that missing file (the
// player deleted only GameData.bin outside the game) is the old, deleted save: it is moved aside as GameData.bin.mpbak.old (.old.1, .old.2, ...
// when taken), never deleted and never restored by the game, so a power cut that tears the new file cannot bring the old save back at boot.
// Not when a .mptmp is there (that is an interrupted save, the boot repair's business) ... except an EMPTY .mptmp when `atomicWrite` is set: that is
// the "repair in progress" marker of a restore that failed (no file, marker, .mpbak); the atomic save that follows would rotate that .mpbak away
// at its next commit, so it is kept aside the same way. Runs on every atomic write-open (one stat when the file exists) and on the host job thread.
static void asideLoneBak(const std::string& hp, bool atomicWrite = false) {
    const char* base = strrchr(hp.c_str(), '/');
    if (strcmp(base ? base + 1 : hp.c_str(), "GameData.bin") != 0) return;
    struct stat st;
    if (stat(hp.c_str(), &st) == 0) return;
    if (stat((hp + ".mptmp").c_str(), &st) == 0 && !(atomicWrite && st.st_size == 0)) return;
    if (stat((hp + ".mpbak").c_str(), &st) != 0) return;
    for (int n = 0; n < 100; n++) {
        const std::string to = hp + ".mpbak.old" + (n ? "." + std::to_string(n) : "");
        if (stat(to.c_str(), &st) == 0) continue;
        if (rename((hp + ".mpbak").c_str(), to.c_str()) == 0) port_log("nand: GameData.bin is being created and only a .mpbak was beside it (a deleted save, or a failed restore): kept aside as %s", to.c_str());
        else port_log("nand: could not move %s.mpbak aside as %s (errno %d): it stays", hp.c_str(), to.c_str(), errno);
        return;
    }
    port_log("nand: could not move %s.mpbak aside: .old names 0-99 are all taken", hp.c_str());
}

// savelife review: GameData.bin is MISSING and a non-empty .mptmp (the newest save: a replace cut between its two renames) is beside it. The repair
// is tried here; if it cannot put the file back (a card read error on the .mptmp), nothing may create or truncate anything there: a new
// GameData.bin would make the next repair delete the .mptmp, and a write-open would truncate it. A stat error on either file is "cannot tell":
// also refused. True = refuse (the game shows its save error). Never while an atomic write of this file is open (its .mptmp is being written).
static bool blockedByInterruptedReplace(const std::string& hp) {
    const char* base = strrchr(hp.c_str(), '/');
    if (strcmp(base ? base + 1 : hp.c_str(), "GameData.bin") != 0) return false;
    if (atomicOpenFor(hp)) return false;
    struct stat st;
    int rc = stat(hp.c_str(), &st), e = errno;
    if (rc == 0) return false;
    if (statUnknown(rc, e)) {
        port_log("nand: GameData.bin could not be looked at (errno %d): refusing to create or write it", e);
        return true;
    }
    rc = stat((hp + ".mptmp").c_str(), &st);
    e = errno;
    if (rc != 0) {
        if (statUnknown(rc, e)) port_log("nand: GameData.bin.mptmp could not be looked at (errno %d): refusing to create or write GameData.bin", e);
        return statUnknown(rc, e);
    }
    if (st.st_size == 0) return false;
    bool changed = false;
    mp_save_recover(hp.c_str(), &changed);
    if (stat(hp.c_str(), &st) == 0) return false;
    rc = stat((hp + ".mptmp").c_str(), &st);
    e = errno;
    if (rc != 0 && !statUnknown(rc, e)) return false;   // (a torn .mptmp the repair removed)
    port_log("nand: GameData.bin is missing and its .mptmp (the newest save) could not be put in place: refusing to create or write it");
    return true;
}

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
    (void)perm;
    (void)attr;
    if (atomicTarget(path)) {   // MP3 savefix: the temporary is created by NANDOpen; no empty GameData.bin
        bool blocked = false;
        const std::string ahp = hostPath(path);
        auto check = [&] { blocked = blockedByInterruptedReplace(ahp); };
        hostJob("NANDCreateCheck", check);
        if (blocked) return errnoToNand(EIO);
        sCreateSkipped.store(true);
        return NAND_RESULT_OK;
    }
    std::string hp = hostPath(path);
    s32 result = NAND_RESULT_OK;
    auto job = [&] {
        if (blockedByInterruptedReplace(hp)) {
            result = errnoToNand(EIO);
            return;
        }
        makeDirs(hp);
        asideLoneBak(hp);
        int fd = open(hp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0660);
        if (fd < 0) {
            result = errnoToNand(errno);
            return;
        }
        close(fd);
    };
    hostJob("NANDCreate", job);
    return result;
}

s32 NANDPrivateCreate(const char* path, u8 perm, u8 attr) { return NANDCreate(path, perm, attr); }

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode) {
    std::string hp = hostPath(path);
    int flags = O_CLOEXEC;
    switch (mode) {
    case NAND_ACCESS_READ:
        flags |= O_RDONLY;
        break;
    case NAND_ACCESS_WRITE:
        flags |= O_WRONLY;
        break;
    default:
        flags |= O_RDWR;
        break;
    }
    int fd = -1;
    int err = 0;
    const bool target = atomicTarget(path);   // (decided ONCE: the flag can move while this open runs; the atomic and safe choices below must agree)
    const bool atomic = mode != NAND_ACCESS_READ && target;   // MP3 savefix
    // MERGE (c99): one save design per path. GameData.bin with the atomic writer on (the default) is MP3's temporary + rename and nothing
    // else: GXV_NAND_SAFE's .bak / .wip never touch it (its recover step on a file that a rename replaces would act on the wrong file).
    // GXV_NAND_SAFE (default off) protects every other file, and GameData.bin whenever the atomic writer is off (its default outside a multiplayer session). The atomic writer WINS while it is active; the two never act on one write.
    const bool safe = nandSafeOn() && !target;
    const bool recoverWip = nandSafeOn();   // (RISK A: the .wip / .bak put-back also runs for a path the atomic writer owns, see below)
    const bool writing = mode != NAND_ACCESS_READ;
    auto job = [&] {
        // RISK A (c99 review): a single-player save whose fsync/close failed leaves .wip + an old .bak. If a multiplayer session then writes this file
        // atomically, nothing would ever remove the pair and the next plain open (safe on, atomic off) would copy the stale .bak over the session's
        // progress. So the put-back of an interrupted write runs for every open of a path under GXV_NAND_SAFE, also when the atomic writer owns it
        // (it restores the file the failed write tore, BEFORE the atomic writer makes its own backup of it), and a successful atomic commit
        // removes the pair (NANDClose). Only .wip / .bak are touched; no new file is made here.
        if (recoverWip && !safe) safeRecover(hp);
        if (writing && blockedByInterruptedReplace(hp)) {
            fd = -1;
            errno = EIO;
        } else if (atomic) {
            makeDirs(hp);
            asideLoneBak(hp, true);
            fd = open((hp + ".mptmp").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        } else {
            if (safe) {
                safeRecover(hp);
                if (writing && safeExists(hp)) safePrepareWrite(hp);
            }
            fd = open(hp.c_str(), flags);
            if (fd < 0 && errno == ENOENT && mode == NAND_ACCESS_READ && strcmp(strrchr(path, '/') ? strrchr(path, '/') + 1 : path, "GameData.bin") == 0) {
                // No GameData.bin and a .mptmp beside it. (1) The empty "repair in progress" marker of a failed restore (+ the .mpbak): the restore is
                // retried ONCE per run, so the save comes back in this session. (2) A NON-EMPTY .mptmp (an interrupted replace the boot repair could not
                // finish, e.g. a card read error): the repair is retried at every such read-open; still unrepaired = a read error (EIO), never "no file".
                // Only at a READ-open: a write-open must not restore (its commit would rotate the old save away; asideLoneBak keeps it instead), and
                // while the .mptmp cannot be repaired a write-open or NANDCreate is refused (blockedByInterruptedReplace).
                static bool sRetried = false;
                const int openErrno = errno;   // (the stat calls below must not change the answer "no file")
                struct stat mst, bst;
                // savelife review: also a NON-EMPTY .mptmp (the newest save of a replace cut between its renames, e.g. when the boot repair could not read
                // it): the repair is tried at every such read-open; if it still cannot restore it the answer is a read error, never "no file" (the game
                // would create a new save over the newest one).
                const int tmpRc = stat((hp + ".mptmp").c_str(), &mst), tmpErr = errno;
                const bool haveTmp = tmpRc == 0;
                const bool nonEmptyTmp = haveTmp && mst.st_size > 0;
                const bool marker = haveTmp && mst.st_size == 0 && stat((hp + ".mpbak").c_str(), &bst) == 0;
                if (statUnknown(tmpRc, tmpErr)) {
                    errno = EIO;   // the .mptmp cannot be looked at: not "no file" (the game would create a new save)
                } else if (!atomicOpenFor(hp) && ((!sRetried && marker) || nonEmptyTmp)) {
                    if (marker) sRetried = true;
                    bool changed = false;
                    const bool ok = mp_save_recover(hp.c_str(), &changed);
                    port_log("nand: GameData.bin missing with a .mptmp (marker or interrupted save) at a read-open: restore retried, ok: %s, restored: %s", ok ? "yes" : "no", changed ? "yes" : "no");
                    fd = open(hp.c_str(), flags);
                    if (fd >= 0) unlink((hp + ".mptmp.failed").c_str());   // (the repair worked after all: the boot's refusal mark goes)
                    const int again = stat((hp + ".mptmp").c_str(), &mst), againErr = errno;
                    if (fd < 0 && nonEmptyTmp && (again == 0 || statUnknown(again, againErr))) errno = EIO;   // still unrepaired: a read error, not "no file"
                    else if (fd < 0) errno = openErrno;
                } else {
                    errno = openErrno;
                }
            }
            if (fd < 0 && errno == ENOENT && mode != NAND_ACCESS_READ && sCreateSkipped.exchange(false)) {   // savesus S6 (1)
                asideLoneBak(hp);
                fd = open(hp.c_str(), (flags & ~O_ACCMODE) | O_CREAT | O_WRONLY, 0660);
            }
        }
        err = errno;
        if (fd < 0 && safe && writing) safeFinishWrite(hp, false);
    };
    hostJob("NANDOpen", job);
    if (fd < 0) {
        return errnoToNand(err);
    }
    if (safe && writing) {
        PortHostAllocScope scope;
        std::lock_guard<std::mutex> l(sSafeMutex);
        sSafeOpenHp[fd] = hp;
    }
    memset(info, 0, sizeof(*info));
    info->fileDescriptor = fd;
    info->accType = mode;
    strncpy(info->origPath, path, NAND_MAX_PATH - 1);
    if (mode != NAND_ACCESS_READ) mp_save_note_write(+1);   // MP3 savefix: mp_save_write_in_flight (system quit waits)
    if (atomic) {
        info->mark = kAtomicMark;
        sCreateSkipped.store(false);
        rememberAtomicOpen(fd, hp);
    }
    return NAND_RESULT_OK;
}

s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 mode) { return NANDOpen(path, info, mode); }

s32 NANDClose(NANDFileInfo* info) {
    s32 result = NAND_RESULT_OK;
    if (info->fileDescriptor >= 0) {
        const int fd = info->fileDescriptor;
        const bool writable = info->accType != NAND_ACCESS_READ;
        const bool atomic = writable && info->mark == kAtomicMark;   // MP3 savefix
        std::string hp;
        if (atomic && !takeAtomicOpen(fd, &hp)) hp = hostPath(info->origPath);   // (savesus S6 (2): the path of the Open)
        info->fileDescriptor = -1;
        info->mark = 0;
        // (perf: GXV_NAND_SAFE's open-path map; MP3's atomic opens are tracked by takeAtomicOpen above and never enter it)
        const bool safe = !atomic && nandSafeOn() && writable;
        if (safe) {
            {
                PortHostAllocScope scope;
                std::lock_guard<std::mutex> l(sSafeMutex);
                auto it = sSafeOpenHp.find(fd);
                if (it != sSafeOpenHp.end()) {   // (the path of the open, not of the root now)
                    hp = it->second;
                    sSafeOpenHp.erase(it);
                }
            }
            if (hp.empty()) hp = hostPath(info->origPath);
        }
        auto job = [&] {
            if (atomic) {
                const bool synced = fsync(fd) == 0;
                const bool closed = close(fd) == 0;
                if (!synced || !closed) {
                    unlink((hp + ".mptmp").c_str());
                    result = NAND_RESULT_UNKNOWN;
                } else if (!mp_save_commit_tmp(hp.c_str(), mp_save_is_gamedata)) {
                    result = NAND_RESULT_UNKNOWN;   // the old GameData.bin stays (the game shows its save error)
                } else {
                    unlink((hp + ".wip").c_str());   // RISK A: the committed file supersedes any GXV_NAND_SAFE pair left by an earlier failed in-place write
                    unlink((hp + ".bak").c_str());
                }
                return;
            }
            bool synced = true;
            if (writable) {
                synced = fsync(fd) == 0;
            }
            if (close(fd) != 0) synced = false;
            if (safe) safeFinishWrite(hp, synced);
        };
        hostJob("NANDClose", job);
        if (writable) mp_save_note_write(-1);
    }
    return noteNandProgress(result);
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
    ssize_t n = -1;
    int err = 0;
    auto job = [&] {
        n = read(info->fileDescriptor, buf, length);
        err = errno;
    };
    hostJob("NANDRead", job);
    if (n > 0) port_gpu_memory_written(buf, (uint32_t)n);
    return n < 0 ? errnoToNand(err) : (s32)n;
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
    ssize_t n = -1;
    int err = 0;
    auto job = [&] {
        n = write(info->fileDescriptor, buf, length);
        err = errno;
    };
    hostJob("NANDWrite", job);
    return noteNandProgress(n < 0 ? errnoToNand(err) : (s32)n);
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
    off_t r = lseek(info->fileDescriptor, offset, whence);
    return r < 0 ? errnoToNand(errno) : (s32)r;
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    struct stat st;
    if (fstat(info->fileDescriptor, &st) != 0) {
        return errnoToNand(errno);
    }
    *length = (u32)st.st_size;
    return NAND_RESULT_OK;
}

static int removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (d) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        return rmdir(path.c_str());
    }
    return unlink(path.c_str());
}

s32 NANDDelete(const char* path) {
    PortHostAllocScope scope;
    std::string hp = hostPath(path);
    s32 result = NAND_RESULT_OK;
    auto job = [&] {
        // A7: a deliberately deleted save must not come back: the boot repair restores a file that is missing while its .mptmp (an unfinished
        // write of the atomic writer) is there. smallfix: the side files go FIRST, the .mptmp before the .mpbak, then the file itself: a power cut
        // in between leaves the file still there (the delete did not happen), never a missing file with a .mptmp beside it. NOT .mpbad: that is a
        // damaged file kept aside for the player, and is never deleted by this.
        unlink((hp + ".wip").c_str());   // (GXV_NAND_SAFE: a deleted save takes its backup and mark with it)
        unlink((hp + ".bak").c_str());
        unlink((hp + ".mptmp").c_str());
        unlink((hp + ".mpbak").c_str());
        if (removeTree(hp) != 0) {
            result = errnoToNand(errno);
        }
    };
    hostJob("NANDDelete", job);
    return result;
}

s32 NANDMove(const char* path, const char* destDir) {
    PortHostAllocScope scope;
    std::string src = hostPath(path);
    std::string name = src.substr(src.rfind('/') + 1);
    std::string dst = hostPath(destDir) + "/" + name;
    s32 result = NAND_RESULT_OK;
    auto job = [&] {
        makeDirs(dst);
        if (rename(src.c_str(), dst.c_str()) != 0) {
            result = errnoToNand(errno);
        }
    };
    hostJob("NANDMove", job);
    return result;
}

s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer) {
    (void)fsBlock;
    (void)inode;
    *answer = 0;  // enough space and inodes
    return NAND_RESULT_OK;
}

void NANDInitBanner(NANDBanner* banner, u32 flag, const u16* title, const u16* comment) {
    memset(banner, 0, sizeof(*banner));
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flag;
    for (int i = 0; title && i < NAND_BANNER_COMMENT_SIZE && title[i]; i++) {
        banner->comment[0][i] = title[i];
    }
    for (int i = 0; comment && i < NAND_BANNER_COMMENT_SIZE && comment[i]; i++) {
        banner->comment[1][i] = comment[i];
    }
}

}  // extern "C"
