// savelife harness (savefix): the fake game classes, the fake net bridge, the port stubs and the fault layer (link with
// -Wl,--wrap=... , see tests/mp/savelife_build.sh). See fakegame.hpp and world.hpp.
#include "fakegame.hpp"
#include "world.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "port/mp_bridge.h"
#include "../../../platform/src/mp/mp_savefile.h"

World gW;

// ---- the image (the game's layout) --------------------------------------------------------------------------------------
namespace {
const char* const kKinds[3] = {"mario", "luigi", "config"};
const u32 kSizes[3] = {0xF80, 0xF80, 0x60};
u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
void wr32(u8* p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
// (kind, data, size) of a named block of an image whose table is filled; kind 0 game, 1 config, 2 sysconf; false when absent
bool findBlock(u8* image, const char* name, u8** data, u32* size, int* kind) {
    uint32_t off, sz;
    if (!mp_save_find(image, name, &off, &sz)) return false;
    *data = image + off;
    *size = sz;
    *kind = !strncmp(name, "config", 6) ? 1 : !strncmp(name, "sysconf", 7) ? 2 : 0;
    return true;
}
}  // namespace

UserFile::UserFile() : mGameDataHolder(&gW.holder) {
    setSlot(1, true);
    resetAllData();
}
void UserFile::setSlot(int slot, bool mario) {
    snprintf(mGameName, sizeof mGameName, "%s%d", mario ? "mario" : "luigi", slot);
    snprintf(mConfigName, sizeof mConfigName, "config%d", slot);
    mIsPlayerMario = mario;
}
void UserFile::makeGameDataBinary(u8* buf, u32 size) const { memcpy(buf, game, size < sizeof game ? size : sizeof game); }
void UserFile::makeConfigDataBinary(u8* buf, u32 size) const { memcpy(buf, config, size < sizeof config ? size : sizeof config); }
void UserFile::resetAllData() {
    memcpy(game, gW.blankGame, sizeof game);
    memcpy(config, gW.blankConfig, sizeof config);
}
void UserFile::loadFromGameDataBinary(const char*, const u8* buf, u32 size) { memcpy(game, buf, size < sizeof game ? size : sizeof game); }
void UserFile::loadFromConfigDataBinary(const char*, const u8* buf, u32 size) { memcpy(config, buf, size < sizeof config ? size : sizeof config); }

void SysConfigFile::makeDataBinary(u8* buf, u32 size) const { memcpy(buf, gW.sysconf, size < sizeof gW.sysconf ? size : sizeof gW.sysconf); }

void SaveDataHandler::resetSaveData(u8* image) {
    // header + table as the game builds it; every block zeroed
    memset(image, 0, 0x10);
    wr32(image + 4, 2);
    wr32(image + 8, 19);
    u32 off = 0x140;
    int e = 0;
    for (int slot = 1; slot <= 6; slot++)
        for (int k = 0; k < 3; k++, e++) {
            u8* ent = image + 0x10 + e * 0x10;
            memset(ent, 0, 12);
            snprintf((char*)ent, 12, "%s%d", kKinds[k], slot);
            wr32(ent + 12, off);
            memset(image + off, 0, kSizes[k]);
            off += kSizes[k];
        }
    u8* ent = image + 0x10 + e * 0x10;
    memset(ent, 0, 12);
    snprintf((char*)ent, 12, "sysconf");
    wr32(ent + 12, off);
    memset(image + off, 0, 0x80);
    wr32(image + 12, off + 0x80);
    gW.noteImageReset();
}
void SaveDataHandler::initializeAllFileInSaveData(u8* image, const SysConfigFile* sys, const UserFile* file) {
    const u32 count = rd32(image + 8);
    for (u32 i = 0; i < count; i++) {
        const char* name = (const char*)(image + 0x10 + i * 0x10);
        u8* data;
        u32 size;
        int kind;
        if (!findBlock(image, name, &data, &size, &kind)) continue;
        if (kind == 0) file->makeGameDataBinary(data, size);
        else if (kind == 1) file->makeConfigDataBinary(data, size);
        else sys->makeDataBinary(data, size);
    }
}
void SaveDataHandler::copySaveDataEachFile(u8* dst, const u8* src) {
    const u32 count = rd32(src + 8);
    bool skipped = false;
    for (u32 i = 0; i < count && i < 24; i++) {
        const char* name = (const char*)(src + 0x10 + i * 0x10);
        u8 *s, *d;
        u32 ss, ds;
        int k1, k2;
        if (!findBlock(const_cast< u8* >(src), name, &s, &ss, &k1) || !findBlock(dst, name, &d, &ds, &k2)) continue;
        if (gW.faults.copySkip > 0 && i == 3) {   // fault: the game's copy leaves a block out (the self-check must notice)
            gW.faults.copySkip--;
            skipped = skipped || memcmp(d, s, ds < ss ? ds : ss) != 0;   // (half-built only if the block left out differs)
            continue;
        }
        memcpy(d, s, ds < ss ? ds : ss);
        if (ds > ss) memset(d + ss, 0, ds - ss);
    }
    if (skipped) snprintf(gW.imageRoot, sizeof gW.imageRoot, "<half-built>");
    else gW.noteImageCopied();
}
void SaveDataHandler::storeUserFile(const UserFile* file) {
    u8* data;
    u32 size;
    int kind;
    if (findBlock(_18, file->getGameDataName(), &data, &size, &kind)) file->makeGameDataBinary(data, size);
    if (findBlock(_18, file->getConfigDataName(), &data, &size, &kind)) file->makeConfigDataBinary(data, size);
}

bool SaveDataHandleSequence::isActive() const { return gW.busyFrames > 0; }
void SaveDataHandleSequence::restoreUserFile(UserFile* file, int slot, bool mario) {
    gW.restores++;
    file->setSlot(slot, mario);
    u8* data;
    u32 size;
    int kind;
    if (findBlock(mSaveDataHandler->_18, file->getGameDataName(), &data, &size, &kind)) file->loadFromGameDataBinary("", data, size);
    if (findBlock(mSaveDataHandler->_18, file->getConfigDataName(), &data, &size, &kind)) file->loadFromConfigDataBinary("", data, size);
}

template <> GameSystem* SingletonHolder< GameSystem >::get() { return &gW.system; }

namespace MR {
    ScenarioDataIter makeBeginScenarioDataIter() { return ScenarioDataIter(); }
    u32 getHashCode(const char* s) {
        u32 h = 0;
        for (; *s; s++) h = (u32)(signed char)*s + h * 31u;
        return h;
    }
    const char* getCurrentStageName() { return "HarnessGalaxy"; }
    s32 getCurrentScenarioNo() { return 1; }
}  // namespace MR

// ---- MpGame / MpUi (only what MpSaveGame.cpp calls) ----------------------------------------------------------------------
namespace MpGame { uint32_t turn() { return gW.turn; } }
namespace MpUi { void toast(uint16_t textId, int, uint32_t) { gW.toasts++; gW.lastToast = textId; } }

// ---- the net bridge ------------------------------------------------------------------------------------------------------
extern "C" {
bool mp_active(void) { return gW.net.active; }
bool mp_is_host(void) { return gW.net.active && gW.net.localSlot == gW.net.hostSlot; }
int mp_local_slot(void) { return gW.net.active ? gW.net.localSlot : -1; }
int mp_host_slot(void) { return gW.net.hostSlot; }
uint16_t mp_host_epoch(void) { return gW.net.epoch; }
bool mp_save_suppressed(void) { return gW.net.active && gW.net.localSlot != 0; }
void mp_snapshot_offer(const void*, uint32_t len) { gW.offers++; (void)len; }
uint32_t mp_snapshot_ready(void) { return gW.net.active && gW.net.snapLen ? gW.net.snapLen : 0; }
uint32_t mp_snapshot_get(void* out, uint32_t cap) {
    if (!gW.net.snapLen || gW.net.snapLen > cap) return 0;
    memcpy(out, gW.net.snap, gW.net.snapLen);
    return gW.net.snapLen;
}
void mp_snapshot_consumed(void) { gW.net.snapLen = 0; }
void mp_ui_status(MpUiStatus* out) {
    memset(out, 0, sizeof *out);
    out->phase = (MpUiPhase)gW.net.phase;
}
void mp_ui_leave(void) {
    gW.leaveCalls++;
    gW.net.active = false;
    gW.net.phase = MP_UI_IDLE;
}
void mp_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[400];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    gW.log(line);
}
}  // extern "C"

// ---- port stubs ----------------------------------------------------------------------------------------------------------
extern "C" {
void* port_host_alloc(size_t size, int align) {
    if (gW.faults.hostAllocAfter > 0) {
        gW.faults.hostAllocAfter--;
    } else if (gW.faults.hostAllocFail > 0) {
        gW.faults.hostAllocFail--;
        return nullptr;
    }
    void* p = nullptr;
    if (posix_memalign(&p, align < (int)sizeof(void*) ? sizeof(void*) : (size_t)align, size) != 0) return nullptr;
    return p;
}
void port_host_free(void* p) { free(p); }
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
void port_host_job_wait(const char*, void (*fn)(void*), void* arg) { fn(arg); }
void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[400];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    gW.log(line);
}
void port_gpu_memory_written(void*, uint32_t) {}
void port_thread_class(int) {}
}

// ---- mp_core_sys (what mp_config.cpp needs) ------------------------------------------------------------------------------
#include "../../../platform/src/mp/mp_core_sys.h"
bool mp_sys_random_try(uint32_t* out) { *out = (uint32_t)gW.rng(); return true; }
bool mp_sys_random_fill(void* out, size_t n) { for (size_t i = 0; i < n; i += 4) { const uint32_t v = (uint32_t)gW.rng(); memcpy((uint8_t*)out + i, &v, n - i < 4 ? n - i : 4); } return true; }   // (one 32-bit draw per 4 bytes, as the old stub)
bool mp_sys_mkdirs(const char* path) { return mp_save_mkdirs(path); }
MpMutex::MpMutex() {}
MpMutex::~MpMutex() {}
void MpMutex::lock() {}
bool MpMutex::tryLock() { return true; }
void MpMutex::unlock() {}

// ---- the fault layer (-Wl,--wrap) ----------------------------------------------------------------------------------------
// Every mutating file-system call counts as one "write step"; a kill at step k ends the process at once (a power cut: nothing
// after it reaches the card), before or after the call. Other faults: read errors on a path, a full card, short writes,
// failing renames, failing allocations.
extern "C" {
int __real_open(const char* path, int flags, ...);
ssize_t __real_read(int fd, void* buf, size_t n);
ssize_t __real_write(int fd, const void* buf, size_t n);
int __real_fsync(int fd);
int __real_rename(const char* a, const char* b);
int __real_unlink(const char* p);
int __real_remove(const char* p);
FILE* __real_fopen(const char* p, const char* m);
int __real_fclose(FILE* f);
int __real_open64(const char* path, int flags, ...);
int __real_mkdir(const char* p, mode_t m);
int __real_rmdir(const char* p);
size_t __real_fwrite(const void* b, size_t s, size_t n, FILE* f);
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
}

static void writeStep(const char* what, const char* path, bool after) {
    Faults& f = gW.faults;
    if (!after) f.ops++;
    if (f.killAt > 0 && f.ops == f.killAt && f.killAfter == after) {
        gW.log("KILL");
        fprintf(stderr, "%s", "");
        gW.onKill(what, path);
        _exit(kKillExit);
    }
}

static bool matches(const char* path, const char* m) { return m[0] && path && strstr(path, m) != nullptr; }

// fds whose path is under fault (read errors): Faults::eioFd / eioCount (cleared with the faults)
#define sEioFd gW.faults.eioFd
#define sEioCount gW.faults.eioCount
static bool eioFd(int fd) {
    for (int i = 0; i < sEioCount; i++)
        if (sEioFd[i] == fd) return true;
    return false;
}

typedef int (*OpenFn)(const char*, int, ...);
static int openImpl(OpenFn realOpen, const char* path, int flags, mode_t mode) {
    const bool mut = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
    if (mut) writeStep("open", path, false);
    if (!mut && gW.faults.readEio > 0 && matches(path, gW.faults.readEioMatch)) {
        // the open works, the read fails (a card that answers but cannot read the blocks)
        const int fd = realOpen(path, flags, mode);
        if (fd >= 0) {
            gW.faults.readEio--;
            if (sEioCount < 64) sEioFd[sEioCount++] = fd;
        }
        return fd;
    }
    const int fd = realOpen(path, flags, mode);
    if (fd >= 0) {
        for (int i = 0; i < sEioCount; i++)
            if (sEioFd[i] == fd) sEioFd[i] = sEioFd[--sEioCount];
    }
    if (mut) writeStep("open", path, true);
    return fd;
}
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openImpl(__real_open, path, flags, mode);
}
extern "C" int __wrap_open64(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openImpl(__real_open64, path, flags, mode);
}
extern "C" int __wrap_mkdir(const char* p, mode_t m) {
    struct stat st;
    const bool mut = stat(p, &st) != 0;   // (mkdir of a folder that exists changes nothing)
    if (mut) writeStep("mkdir", p, false);
    const int r = __real_mkdir(p, m);
    if (mut) writeStep("mkdir", p, true);
    return r;
}
extern "C" int __wrap_rmdir(const char* p) {
    struct stat st;
    const bool mut = stat(p, &st) == 0;
    if (mut) writeStep("rmdir", p, false);
    const int r = __real_rmdir(p);
    if (mut) writeStep("rmdir", p, true);
    return r;
}
// streams: the harness's own output (stdout / stderr) is not the game's file system
extern "C" size_t __wrap_fwrite(const void* b, size_t sz, size_t n, FILE* f) {
    const bool mut = f != stdout && f != stderr;
    if (mut) writeStep("fwrite", nullptr, false);
    const size_t r = __real_fwrite(b, sz, n, f);
    if (mut) writeStep("fwrite", nullptr, true);
    return r;
}
extern "C" int __wrap_fprintf(FILE* f, const char* fmt, ...) {
    const bool mut = f != stdout && f != stderr;
    if (mut) writeStep("fprintf", nullptr, false);
    va_list ap;
    va_start(ap, fmt);
    const int r = vfprintf(f, fmt, ap);
    va_end(ap);
    if (mut) writeStep("fprintf", nullptr, true);
    return r;
}
extern "C" ssize_t __wrap_read(int fd, void* buf, size_t n) {
    if (eioFd(fd)) {
        errno = EIO;
        return -1;
    }
    return __real_read(fd, buf, n);
}
extern "C" ssize_t __wrap_write(int fd, const void* buf, size_t n) {
    writeStep("write", nullptr, false);
    Faults& f = gW.faults;
    if (f.tear && f.killAt == f.ops && f.killAfter && n > 1) {
        // the power goes in the middle of this write: half of it reaches the card
        if (__real_write(fd, buf, n / 2) < 0) {}
        gW.log("KILL (torn write)");
        gW.onKill("write (torn)", nullptr);
        _exit(kKillExit);
    }
    if (f.cardFree >= 0) {
        if (f.cardFree == 0) {
            errno = ENOSPC;
            return -1;
        }
        if ((long)n > f.cardFree) n = (size_t)f.cardFree;
    }
    if (f.shortWrite > 0 && n > 1) {
        f.shortWrite--;
        n /= 2;
    }
    ssize_t r;
    if (f.writeCorrupt > 0 && n > 16) {
        f.writeCorrupt--;
        static uint8_t tmp[0x10000];
        const size_t m = n < sizeof tmp ? n : sizeof tmp;
        memcpy(tmp, buf, m);
        tmp[m / 2] ^= 0x5A;
        r = __real_write(fd, tmp, m);
    } else {
        r = __real_write(fd, buf, n);
    }
    if (r > 0 && f.cardFree >= 0) f.cardFree -= r;
    writeStep("write", nullptr, true);
    return r;
}
extern "C" int __real_stat(const char* path, struct stat* st);
extern "C" int __wrap_stat(const char* path, struct stat* st) {
    if (gW.faults.statErr > 0 && matches(path, gW.faults.statErrMatch)) {
        gW.faults.statErr--;
        errno = gW.faults.statErrno;
        return -1;
    }
    return __real_stat(path, st);
}
extern "C" int __wrap_fsync(int fd) {
    gW.faults.fsyncs++;
    writeStep("fsync", nullptr, false);
    const int r = __real_fsync(fd);
    writeStep("fsync", nullptr, true);
    return r;
}
extern "C" int __wrap_rename(const char* a, const char* b) {
    writeStep("rename", a, false);
    if (gW.faults.renameFail > 0) {
        gW.faults.renameFail--;
        errno = EIO;
        return -1;
    }
    if (gW.faults.renameLie > 0) {
        gW.faults.renameLie--;
        return 0;
    }
    if (gW.faults.renameNoReplace) {
        // the Vita's file system as the port assumes it: a rename onto an existing file fails
        struct stat st;
        if (stat(b, &st) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    const int r = __real_rename(a, b);
    writeStep("rename", a, true);
    return r;
}
extern "C" int __wrap_unlink(const char* p) {
    writeStep("unlink", p, false);
    const int r = __real_unlink(p);
    writeStep("unlink", p, true);
    return r;
}
extern "C" int __wrap_remove(const char* p) {
    writeStep("remove", p, false);
    const int r = __real_remove(p);
    writeStep("remove", p, true);
    return r;
}
extern "C" FILE* __wrap_fopen(const char* p, const char* m) {
    const bool mut = strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+');
    if (!mut && gW.faults.fopenFail > 0 && matches(p, gW.faults.fopenFailMatch)) {
        gW.faults.fopenFail--;
        errno = EIO;
        return nullptr;
    }
    if (mut) writeStep("fopen", p, false);
    if (mut && gW.faults.cardFree == 0) {
        errno = ENOSPC;
        return nullptr;
    }
    FILE* f = __real_fopen(p, m);
    if (mut) writeStep("fopen", p, true);
    return f;
}
extern "C" int __wrap_fclose(FILE* f) {
    writeStep("fclose", nullptr, false);
    const int r = __real_fclose(f);
    writeStep("fclose", nullptr, true);
    return r;
}
extern "C" void* __wrap_malloc(size_t n) {
    if (gW.faults.mallocFail > 0 && n >= 0x8000) {   // the 64 KiB work buffers only
        gW.faults.mallocFail--;
        return nullptr;
    }
    return __real_malloc(n);
}
extern "C" void* __wrap_calloc(size_t a, size_t b) {
    if (gW.faults.mallocFail > 0 && a * b >= 0x8000) {
        gW.faults.mallocFail--;
        return nullptr;
    }
    return __real_calloc(a, b);
}
