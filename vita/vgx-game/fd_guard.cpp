// A seek on a file handle that is not open must fail, not crash.
//
// The Vita C library's _lseek_r (the call under fseek) asks __vita_fd_grab for the handle's record and reads it without checking
// for NULL. A player's start-up crashed there (data abort at address 4, in the first seconds of a start, once in a few
// launches), so the call is wrapped: a handle with no record returns -1 with EBADF, which fseek reports to its caller like any
// failed seek (the callers all handle that: the loading screen falls back to a plain background, a pack that cannot be read is
// not used). Every other seek goes on to the library unchanged.
#include <errno.h>
#include <reent.h>
#include <stdio.h>
#include <sys/types.h>
#include <mutex>

extern "C" {
void* __vita_fd_grab(int fd);
void __vita_fd_drop(void* descriptor);
void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));  // (the basic log in ux0:data/smg/log.txt)
off_t __real__lseek_r(struct _reent* reent, int fd, off_t offset, int whence);

off_t __wrap__lseek_r(struct _reent* reent, int fd, off_t offset, int whence) {
    void* descriptor = __vita_fd_grab(fd);
    if (!descriptor) {
        static int reported = 0;
        if (reported++ < 3 && port_log_summary) port_log_summary("io: a seek on file handle %d, which is not open, was refused (EBADF)", fd);
        if (reent) reent->_errno = EBADF;
        return static_cast<off_t>(-1);
    }
    __vita_fd_drop(descriptor);
    return __real__lseek_r(reent, fd, offset, whence);
}

// fopen, one at a time. This C library's __sfp (the search for a free FILE slot, inside fopen) takes no lock in the Vita build (its code has no call
// to __sfp_lock_acquire; only fclose and the failure path of fopen take it), so two threads in fopen together can be given the same slot: one
// writes the slot's descriptor (-1 until its open is done, and again if the other reserves it after) under the other, and the second thread's fseek
// then runs on file handle -1 (the "io: a seek on file handle -1" line above the "game data not found" abort). Start-up has several threads opening
// files with fopen at once (the loading screen, the shader pack, the HD pack, the log). A lock around fopen alone closes the race.
// The wrap (-Wl,--wrap=fopen in vita/vgx-game/CMakeLists.txt) applies to every undefined reference of the ONE link of the game: the runtime
// archive, the game archive, vitaGL and libstdc++ are inputs of the same ld command, so all of their fopen calls take this lock; a call made inside
// the C library's own objects does not. freopen searches for a FILE slot the same way and shares the lock (strikersrev25, from the Strikers port's
// review; the player's build calls it once, in a diagnostics switch, after the splash thread has started). fdopen, tmpfile and popen are not
// called anywhere in what the Vita build compiles (platform/src/gx/vita_xfb.cpp and vita_frame.cpp, which use fdopen, are excluded by
// tools/vgx_build/vita_runtime_gen_vgx.py), so they are not wrapped: a wrap of a symbol nothing references would only pull more of the library in.
// fclose joins the same lock (strk1118, from the Strikers port's 18 Nov note: its np_stdio_lock.cpp wraps fopen, freopen, fdopen, tmpfile AND fclose
// under one guard). Newlib's fclose gives a FILE slot back (its flags are cleared, its buffer freed) while the lock that should keep __sfp away from
// that slot is, in this build, empty: a thread in fopen can claim the slot between the two steps of the release, and then fclose frees or clears
// what the new owner has just set up (a wrong descriptor, a freed buffer: heap damage, or a read from the wrong file). The threads that open and
// close FILEs together in this game: the game thread (dvd.cpp, bootio, card reads), the card writer (card_queue.h saves), the log writer
// (os_alarm.cpp, the log file and its rotation), the shader / texture cache and HD pack readers (vgx_shader_pack.h, vgx_texcache.cpp, vgx_hdtex.cpp,
// the splash's loading_screen.h) and the net threads (vlink_online.cpp, np_https). The player's build has them too (it only discards the console).
// The lock is recursive so that a hook that opens a file from inside the release (the cold-trace build's sceIoClose observer) cannot wait for
// itself. An fclose that flushes a big buffer holds the lock for that write; opens wait, nothing else does. GXV_FCLOSE_LOCK=0 (env.txt, default 1)
// leaves fclose unlocked as before (fopen and freopen keep their lock either way).
extern "C++" {
static std::recursive_mutex& StdioOpenLock() {
    static std::recursive_mutex lock;
    return lock;
}
}
FILE* __real_fopen(const char* path, const char* mode);
FILE* __wrap_fopen(const char* path, const char* mode) {
    std::lock_guard<std::recursive_mutex> hold(StdioOpenLock());
    return __real_fopen(path, mode);
}
FILE* __real_freopen(const char* path, const char* mode, FILE* stream);
FILE* __wrap_freopen(const char* path, const char* mode, FILE* stream) {
    std::lock_guard<std::recursive_mutex> hold(StdioOpenLock());
    return __real_freopen(path, mode, stream);
}
int __real_fclose(FILE* stream);
static int sFcloseLock = 1;   // (read once, by port_fd_guard_configure, before the threads that matter exist; a plain int: a stale 1 only keeps the lock)
void port_fd_guard_configure(const char* fcloseLock) { sFcloseLock = !(fcloseLock && fcloseLock[0] == '0'); }
int __wrap_fclose(FILE* stream) {
    if (!sFcloseLock) return __real_fclose(stream);
    std::lock_guard<std::recursive_mutex> hold(StdioOpenLock());
    return __real_fclose(stream);
}
}
