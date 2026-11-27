// netfix B8b: small whole-file reads and writes with POSIX open/read/write/close. The Vita's stdio is not safe when two threads fopen at the
// same time (the multiplayer helper thread saving mp.cfg / the report / reading the account while the game's loader opens its own files);
// the POSIX calls need no lock (the save layer, mp_savefile.cpp, already uses them). Header only, no state.
#ifndef PORT_MP_POSIX_IO_H
#define PORT_MP_POSIX_IO_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

// Reads up to `cap` bytes from the start of the file (or from `from` when it is > 0) into buf. Returns the bytes read, or -1 when the file cannot
// be opened or read (errno kept). *size, when given, is the file's whole size (lseek end) or -1.
static inline long mp_io_read(const char* path, void* buf, size_t cap, long from = 0, long* size = nullptr) {
    if (size) *size = -1;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (size) {
        const off_t end = lseek(fd, 0, SEEK_END);
        *size = end < 0 ? -1 : (long)end;
    }
    if (from > 0 && lseek(fd, (off_t)from, SEEK_SET) < 0) { const int e = errno; close(fd); errno = e; return -1; }
    if (from <= 0 && size && lseek(fd, 0, SEEK_SET) < 0) { const int e = errno; close(fd); errno = e; return -1; }
    size_t at = 0;
    while (at < cap) {
        const ssize_t n = read(fd, (char*)buf + at, cap - at);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { const int e = errno; close(fd); errno = e; return -1; }
        if (n == 0) break;
        at += (size_t)n;
    }
    close(fd);
    return (long)at;
}

// Creates / truncates the file and writes all of data. True only when every byte was written and the file closed cleanly.
static inline bool mp_io_write(const char* path, const void* data, size_t len) {
    const int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
    if (fd < 0) return false;
    size_t at = 0;
    bool ok = true;
    while (at < len) {
        const ssize_t n = write(fd, (const char*)data + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        at += (size_t)n;
    }
    if (ok && fsync(fd) != 0) ok = false;   // keepfix 5: on the medium before the caller renames it into place
    return close(fd) == 0 && ok;
}

// As mp_io_write, and the data is forced to the card (fsync) before the close: for the temporary file of a write-then-rename, so a power cut
// after the rename cannot leave the new name on a file whose data never arrived (savebug: mp.cfg / flow.cfg, like the Team File writer).
static inline bool mp_io_write_sync(const char* path, const void* data, size_t len) {
    const int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
    if (fd < 0) return false;
    size_t at = 0;
    bool ok = true;
    while (at < len) {
        const ssize_t n = write(fd, (const char*)data + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        at += (size_t)n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    return close(fd) == 0 && ok;
}

// Whether the file can be opened for reading (errno kept for a failed open).
static inline bool mp_io_can_open(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    close(fd);
    return true;
}

#endif  // PORT_MP_POSIX_IO_H
