// A seek on a file handle that is not open must fail, not crash.
//
// The Vita C library's _lseek_r (the call under fseek) asks __vita_fd_grab for the handle's record and reads it without checking
// for NULL. A player's start-up crashed there (data abort at address 4, in the first seconds of a start, once in a few
// launches), so the call is wrapped: a handle with no record returns -1 with EBADF, which fseek reports to its caller like any
// failed seek (the callers all handle that: the loading screen falls back to a plain background, a pack that cannot be read is
// not used). Every other seek goes on to the library unchanged.
#include <errno.h>
#include <reent.h>
#include <sys/types.h>

extern "C" {
void* __vita_fd_grab(int fd);
void __vita_fd_drop(void* descriptor);
void port_log_summary(const char* fmt, ...) __attribute__((weak));  // (the basic log in ux0:data/smg/log.txt)
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
}
