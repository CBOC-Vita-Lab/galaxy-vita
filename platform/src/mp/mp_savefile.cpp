// Galaxy 3.0 multiplayer: host-side save file logic (owner: save). See mp_savefile.h. Pure: images and files only.
#include "mp_savefile.h"

#include "mp_savecheck.h"
#include "port/mp_test_env.h"   // MP_TEST_GETENV (SMG_MP_TEST_SAVE_FAIL: desktop only)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "port/mp_proto.h"
#include "port/port.h"  // port_thread_class (weak: nothing outside the Vita)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <time.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace {

constexpr uint32_t kHeaderBytes = 0x10;
constexpr uint32_t kEntryBytes = 0x10;

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
uint32_t roundUp32(uint32_t v) { return (v + 31u) & ~31u; }

const char* entryName(const uint8_t* image, uint32_t i) { return (const char*)(image + kHeaderBytes + i * kEntryBytes); }
uint32_t entryOffset(const uint8_t* image, uint32_t i) { return rd32(image + kHeaderBytes + i * kEntryBytes + MP_SAVE_NAME_BYTES); }

bool slotName(char (&out)[16], const char* kind, int slot) {
    if (slot < 1 || slot > MP_SAVE_SLOTS) return false;
    snprintf(out, sizeof out, "%s%d", kind, slot);
    return true;
}

const char* const kKinds[3] = {"mario", "luigi", "config"};

std::atomic<int> sCrashAfter{0};  // test hook (also read by the background writer)
std::atomic<int> sCutWrite{-1};  // test hook: a "power cut" after this many bytes of the temporary were written (-1 = off)
std::atomic<int> sWritesInFlight{0};  // savefix: mp_save_write_in_flight (system quit waits while > 0)
struct InFlight {
    InFlight() { sWritesInFlight++; }
    ~InFlight() { sWritesInFlight--; }
};
bool crashHere(int step) { return sCrashAfter != 0 && step >= sCrashAfter; }

bool writeAll(int fd, const uint8_t* data, uint32_t len) {
    uint32_t at = 0;
    while (at < len) {
        ssize_t n = write(fd, data + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += (uint32_t)n;
    }
    return true;
}

bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// 1: there; 0: absent (ENOENT); -1: could not tell (any other stat error: the card may be failing: the caller changes nothing)
int existsState(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

// 1: reads and validates; 0: absent, too big, or read fine but not valid; -1: could not be read (an I/O error: says nothing about the content)
int fileState(const char* path, uint8_t* buf, MpSaveValidator valid) {
    uint32_t len = 0;
    const int rc = mp_save_read_file(path, buf, MP_SAVE_MAX_FILE, &len);
    if (rc == -ENOENT || rc == -EFBIG) return 0;
    if (rc != 0) return -1;
    return valid(buf, len) ? 1 : 0;
}

}  // namespace

const char* mp_save_check_name(int check) {
    switch (check) {
    case MP_SAVE_OK: return "ok";
    case MP_SAVE_ERR_SHORT: return "short";
    case MP_SAVE_ERR_VERSION: return "version";
    case MP_SAVE_ERR_COUNT: return "count";
    case MP_SAVE_ERR_SIZE: return "size";
    case MP_SAVE_ERR_LENGTH: return "length";
    case MP_SAVE_ERR_CHECKSUM: return "checksum";
    case MP_SAVE_ERR_TABLE: return "table";
    case MP_SAVE_ERR_MISSING: return "missing block";
    default: return "?";
    }
}

uint32_t mp_save_checksum(const uint8_t* image, uint32_t fileSize) {
    uint16_t sum = 0, inv = 0;
    if (fileSize < 4) return 0;
    const uint32_t words = (fileSize - 4) / 2;
    const uint8_t* p = image + 4;
    for (uint32_t i = 0; i < words; i++, p += 2) {
        const uint16_t w = (uint16_t)(p[0] | (p[1] << 8));  // the port is little-endian: the game reads u16 natively
        sum = (uint16_t)(sum + w);
        inv = (uint16_t)(inv + (uint16_t)~w);
    }
    return ((uint32_t)sum << 16) | inv;
}

int mp_save_validate(const uint8_t* image, uint32_t fileLen, bool requireExactLength, bool checkChecksum) {
    if (!image || fileLen < kHeaderBytes) return MP_SAVE_ERR_SHORT;
    const uint32_t version = rd32(image + 4), count = rd32(image + 8), size = rd32(image + 12);
    if (version != MP_SAVE_VERSION) return MP_SAVE_ERR_VERSION;
    if (count == 0 || count >= MP_SAVE_MAX_ENTRIES) return MP_SAVE_ERR_COUNT;
    const uint32_t tableEnd = kHeaderBytes + count * kEntryBytes;
    if (size >= MP_SAVE_MAX_FILE || size < tableEnd) return MP_SAVE_ERR_SIZE;
    if (fileLen < size) return MP_SAVE_ERR_SHORT;
    if (requireExactLength && fileLen != roundUp32(size)) return MP_SAVE_ERR_LENGTH;
    uint32_t prev = tableEnd;
    for (uint32_t i = 0; i < count; i++) {
        const char* name = entryName(image, i);
        if (memchr(name, 0, MP_SAVE_NAME_BYTES) == nullptr || name[0] == 0) return MP_SAVE_ERR_TABLE;
        const uint32_t off = entryOffset(image, i);
        if (off < prev || off > size) return MP_SAVE_ERR_TABLE;  // ascending, inside the file (sizes are offset deltas)
        prev = off;
    }
    for (int s = 1; s <= MP_SAVE_SLOTS; s++) {
        for (const char* kind : kKinds) {
            char n[16];
            slotName(n, kind, s);
            uint32_t off, sz;
            if (!mp_save_find(image, n, &off, &sz)) return MP_SAVE_ERR_MISSING;
        }
    }
    if (checkChecksum && rd32(image) != mp_save_checksum(image, size)) return MP_SAVE_ERR_CHECKSUM;
    return MP_SAVE_OK;
}

uint32_t mp_save_file_size(const uint8_t* image) { return rd32(image + 12); }
uint32_t mp_save_disk_length(const uint8_t* image) { return roundUp32(rd32(image + 12)); }

bool mp_save_find(const uint8_t* image, const char* name, uint32_t* offset, uint32_t* size) {
    const uint32_t count = rd32(image + 8), fileSize = rd32(image + 12);
    bool found = false;
    for (uint32_t i = 0; i < count && i < MP_SAVE_MAX_ENTRIES; i++) {
        if (strncmp(entryName(image, i), name, MP_SAVE_NAME_BYTES) != 0) continue;
        const uint32_t off = entryOffset(image, i);
        const uint32_t end = i + 1 == count ? fileSize : entryOffset(image, i + 1);
        if (end < off || end > fileSize) return false;  // only for unvalidated images
        *offset = off;
        *size = end - off;
        found = true;
    }
    return found;
}

bool mp_save_copy_slot(const uint8_t* src, int srcSlot, uint8_t* dst, int dstSlot) {
    uint32_t so[3], ss[3], dO[3], ds[3];
    for (int k = 0; k < 3; k++) {
        char sn[16], dn[16];
        if (!slotName(sn, kKinds[k], srcSlot) || !slotName(dn, kKinds[k], dstSlot)) return false;
        if (!mp_save_find(src, sn, &so[k], &ss[k]) || !mp_save_find(dst, dn, &dO[k], &ds[k])) return false;
    }
    for (int k = 0; k < 3; k++) {  // all found: now copy (no partial copy on a lookup failure)
        const uint32_t n = ss[k] < ds[k] ? ss[k] : ds[k];
        memmove(dst + dO[k], src + so[k], n);
        if (ds[k] > n) memset(dst + dO[k] + n, 0, ds[k] - n);
    }
    return true;
}

bool mp_save_finalize(uint8_t* image, uint32_t dstCap) {
    const uint32_t size = rd32(image + 12);
    if (size < 4 || size >= MP_SAVE_MAX_FILE || roundUp32(size) > dstCap) return false;
    memset(image + size, 0, roundUp32(size) - size);
    wr32(image, mp_save_checksum(image, size));
    return true;
}

// ---- snapshot ----------------------------------------------------------------------------------------------------------------
uint32_t mp_save_crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static const uint32_t kSnapCaps[3] = {MP_SAVE_GAME_BLOCK, MP_SAVE_GAME_BLOCK, MP_SAVE_CONFIG_BLOCK};

uint32_t mp_save_snapshot_pack(const uint8_t* mario, uint32_t marioLen, const uint8_t* luigi, uint32_t luigiLen, const uint8_t* config,
                               uint32_t configLen, int slot, uint8_t flags, uint8_t* out, uint32_t cap) {
    const uint8_t* blocks[3] = {mario, luigi, config};
    const uint32_t lens[3] = {marioLen, luigiLen, configLen};
    uint32_t total = MP_SAVE_SNAP_HEADER;
    for (int k = 0; k < 3; k++) {
        if (!blocks[k] || lens[k] == 0 || lens[k] > kSnapCaps[k]) return 0;
        total += lens[k];
    }
    if (slot < 1 || slot > MP_SAVE_SLOTS || !out || total > cap || total > MP_SNAPSHOT_MAX) return 0;
    MpWriter w(out, cap);
    w.u32(MP_SAVE_SNAP_MAGIC);
    w.u16(MP_SAVE_SNAP_VERSION);
    w.u8((uint8_t)slot);
    w.u8(flags);
    for (int k = 0; k < 3; k++) w.u16((uint16_t)lens[k]);
    w.u16(0);
    w.u32(0);  // crc, below
    for (int k = 0; k < 3; k++) w.bytes(blocks[k], lens[k]);
    if (!w.ok() || w.n != total) return 0;
    const uint32_t crc = mp_save_crc32(out + MP_SAVE_SNAP_HEADER, total - MP_SAVE_SNAP_HEADER);
    MpWriter c(out + 16, 4);
    c.u32(crc);
    return total;
}

int mp_save_check_view(const MpSaveSnapshotView* view, int level, MpSaveCheckResult* check) {
    if (!view) {
        if (check) *check = MpSaveCheckResult{MP_SC_ARGS, -1, 0};
        return MP_SC_ARGS;
    }
    return mp_savecheck_user_file(view->block, view->len, level, level >= MP_SAVECHECK_FULL ? mp_savecheck_installed() : nullptr, check);
}

bool mp_save_snapshot_parse(const uint8_t* data, uint32_t len, MpSaveSnapshotView* view) {
    return mp_save_snapshot_parse_checked(data, len, view, nullptr);
}

bool mp_save_snapshot_parse_checked(const uint8_t* data, uint32_t len, MpSaveSnapshotView* view, MpSaveCheckResult* check) {
    if (check) *check = MpSaveCheckResult{MP_SC_OK, -1, 0};
    if (!data || !view || len < MP_SAVE_SNAP_HEADER || len > MP_SNAPSHOT_MAX) return false;
    MpReader r(data, len);
    const uint32_t magic = r.u32();
    const uint16_t version = r.u16();
    const uint8_t slot = r.u8(), flags = r.u8();
    uint16_t lens[3];
    for (int k = 0; k < 3; k++) lens[k] = r.u16();
    const uint16_t reserved = r.u16();
    const uint32_t crc = r.u32();
    if (!r.ok() || magic != MP_SAVE_SNAP_MAGIC || version != MP_SAVE_SNAP_VERSION || reserved != 0) return false;
    if (slot < 1 || slot > MP_SAVE_SLOTS || (flags & ~MP_SAVE_SNAP_FLAG_MARIO) != 0) return false;
    uint32_t total = MP_SAVE_SNAP_HEADER;
    for (int k = 0; k < 3; k++) {
        if (lens[k] == 0 || lens[k] > kSnapCaps[k]) return false;
        total += lens[k];
    }
    if (total != len) return false;
    if (mp_save_crc32(data + MP_SAVE_SNAP_HEADER, len - MP_SAVE_SNAP_HEADER) != crc) return false;
    MpSaveSnapshotView v;
    const uint8_t* p = data + MP_SAVE_SNAP_HEADER;
    for (int k = 0; k < 3; k++) {
        v.block[k] = p;
        v.len[k] = lens[k];
        p += lens[k];
    }
    v.slot = slot;
    v.flags = flags;
    v.crc = crc;
    // MP3 snapval: the CRC only guards the transport; the blocks go into the game's parsers, which trust every size and count.
    if (mp_save_check_view(&v, MP_SAVECHECK_FULL, check) != MP_SC_OK) return false;
    *view = v;
    return true;
}

bool mp_save_snapshot_apply(const MpSaveSnapshotView* view, uint8_t* image, int dstSlot) {
    if (!view || !image || mp_save_check_view(view, MP_SAVECHECK_STRUCT, nullptr) != MP_SC_OK) return false;  // MP3 snapval
    uint32_t off[3], size[3];
    for (int k = 0; k < 3; k++) {
        char n[16];
        if (!slotName(n, kKinds[k], dstSlot) || !mp_save_find(image, n, &off[k], &size[k])) return false;
        if (view->len[k] > size[k]) return false;
    }
    for (int k = 0; k < 3; k++) {
        memcpy(image + off[k], view->block[k], view->len[k]);
        memset(image + off[k] + view->len[k], 0, size[k] - view->len[k]);
    }
    return true;
}

uint32_t mp_save_snapshot_pack_image(const uint8_t* image, int slot, uint8_t flags, uint8_t* out, uint32_t cap) {
    uint32_t off[3], size[3];
    for (int k = 0; k < 3; k++) {
        char n[16];
        if (!slotName(n, kKinds[k], slot) || !mp_save_find(image, n, &off[k], &size[k])) return 0;
        if (size[k] > kSnapCaps[k]) size[k] = kSnapCaps[k];
    }
    return mp_save_snapshot_pack(image + off[0], size[0], image + off[1], size[1], image + off[2], size[2], slot, flags, out, cap);
}

// ---- files -------------------------------------------------------------------------------------------------------------------
int mp_save_read_file(const char* path, uint8_t* buf, uint32_t cap, uint32_t* outLen) {
    *outLen = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    uint32_t at = 0;
    for (;;) {
        uint8_t extra;
        uint8_t* dst = at < cap ? buf + at : &extra;
        const uint32_t want = at < cap ? cap - at : 1;
        ssize_t n = read(fd, dst, want);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            const int e = errno;
            close(fd);
            return -e;
        }
        if (n == 0) break;
        if (at >= cap) {
            close(fd);
            return -EFBIG;
        }
        at += (uint32_t)n;
    }
    close(fd);
    *outLen = at;
    return 0;
}

bool mp_save_mkdirs(const char* dir) {
    char p[512];
    const size_t n = strlen(dir);
    if (n == 0 || n >= sizeof p) return false;
    memcpy(p, dir, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (p[i] == '/' || p[i] == 0) {
            const char c = p[i];
            p[i] = 0;
            if (i > 0 && p[i - 1] != ':') mkdir(p, 0770);  // "ux0:" itself is not a directory to make
            p[i] = c;
        }
    }
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

// savefix: the Vita has no directory fsync; after the renames the whole device's cache is written out (sceIoSync, SceLibKernel),
// so a power cut after a reported success cannot take the renames back. Elsewhere the file's own fsync is all there is.
#ifdef __vita__
extern "C" int sceIoSync(const char* device, unsigned int flags);
static void syncDevice(const char* path) {
    const char* colon = strchr(path, ':');
    if (!colon || colon - path > 15) return;
    char dev[20];
    memcpy(dev, path, (size_t)(colon - path) + 1);
    dev[colon - path + 1] = 0;
    sceIoSync(dev, 0);
}
#else
static void syncDevice(const char*) {}
#endif

// savefix: steps 4-6 of the atomic replace, shared with the port's NAND layer (nand.cpp routes the game's own GameData.bin writes
// here while multiplayer code is involved). tmp is complete and on the medium; `want` (len bytes) is what it must hold.
static bool commitTmp(const char* path, const char* tmp, const char* bak, const uint8_t* want, uint32_t len, uint32_t cap = MP_SAVE_MAX_FILE) {
    // Read back and compare before anything touches the old file.
    uint8_t* check = (uint8_t*)malloc(cap);
    if (!check) { unlink(tmp); return false; }
    uint32_t got = 0;
    bool ok = mp_save_read_file(tmp, check, cap, &got) == 0 && got == len && memcmp(check, want, len) == 0;
    if (!ok) { free(check); unlink(tmp); return false; }
    // 4-6: keep the old file as .mpbak (rename does not replace on the Vita), then put the new one in place.
    bool moved = false;
    if (exists(path)) {
        if (unlink(bak) != 0 && errno != ENOENT) { free(check); unlink(tmp); return false; }
        if (crashHere(4)) { free(check); return false; }
        if (rename(path, bak) != 0) { free(check); unlink(tmp); return false; }
        moved = true;
        if (crashHere(5)) { free(check); return false; }
    }
    if (rename(tmp, path) != 0) {
        free(check);
        // smallfix: when the old file is back the failed write is dropped (it never comes back later); when it could not be put back
        // (both renames refused) the complete .mptmp stays: it is the proof of an interrupted replace that boot repair needs now
        // that a missing file with only a .mpbak is no longer restored (that is a deliberate deletion).
        if (!moved || rename(bak, path) == 0) unlink(tmp);
        return false;
    }
    if (crashHere(6)) { free(check); return false; }
    syncDevice(path);
    // savefix: the file now in place is the one we wrote (a rename that reported success but did not happen, a stale cache)
    ok = mp_save_read_file(path, check, cap, &got) == 0 && got == len && memcmp(check, want, len) == 0;
    free(check);
    return ok;
}

// hooks: "[mp] save write file=<name> ok=<0|1>" once per atomic write (the game's own GameData.bin through the NAND layer, a recovery file, an
// MP save). Weak: the host tests that link this file without the port's log simply print nothing.
extern "C" void mp_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
static void logWrite(const char* path, bool ok) {
    if (!mp_log) return;
    const char* name = path ? strrchr(path, '/') : nullptr;
    name = name ? name + 1 : (path ? path : "?");
    if (strncmp(name, "online", 6) == 0) name = "account";   // (the account file's name never reaches the log: keepsafe_test Account)
    mp_log("save write file=%s ok=%d", name, ok ? 1 : 0);
}

bool mp_save_write_atomic(const char* path, const uint8_t* data, uint32_t len) { return mp_save_write_atomic_max(path, data, len, MP_SAVE_MAX_FILE); }

static bool writeAtomicImpl(const char* path, const uint8_t* data, uint32_t len, uint32_t maxLen) {
    InFlight inFlight;
    char tmp[600], bak[600];
    if (snprintf(tmp, sizeof tmp, "%s.mptmp", path) >= (int)sizeof tmp || snprintf(bak, sizeof bak, "%s.mpbak", path) >= (int)sizeof bak)
        return false;
    if (!data || len == 0 || len > maxLen || maxLen > MP_SAVE_ATOMIC_MAX) return false;
    // 1-3: the new bytes in the temporary, on the medium.
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
    if (fd < 0) return false;
    if (crashHere(1)) { close(fd); return false; }
    {   // test hook (A4): the cut falls on any byte boundary of the write; the torn temporary stays, as after a power cut
        const int cut = sCutWrite.load();
        if (cut >= 0 && (uint32_t)cut < len) { writeAll(fd, data, (uint32_t)cut); close(fd); return false; }
    }
    const uint32_t half = len / 2;
    bool ok = writeAll(fd, data, half);
    if (ok && crashHere(2)) { close(fd); return false; }
    ok = ok && writeAll(fd, data + half, len - half);
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok) { unlink(tmp); return false; }
    if (crashHere(3)) return false;
    return commitTmp(path, tmp, bak, data, len, maxLen);
}

// Test hook (scenarios-final): SMG_MP_TEST_SAVE_FAIL=<n> (desktop only, MP_TEST_GETENV): from the n-th commit of a GameData.bin on, the commit fails and its complete
// .mptmp stays beside the file, as after a failed save (no rename, the old file and its .mpbak untouched). n=2 lets the first save of a session through (it makes the
// .mpbak) and fails the later ones, so the LAST save of the session failed. mp_save_test_fail_from(n) sets it without the environment (n<=0 off); tests/mp/save_test.cpp.
static std::atomic<int> sFailFrom{-2};    // -2: environment not read yet
static std::atomic<int> sFailCount{0};
void mp_save_test_fail_from(int n) { sFailFrom = n; sFailCount = 0; }
static bool testFailThisCommit(const char* path) {
    int n = sFailFrom.load();
    if (n == -2) {
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_SAVE_FAIL");
        n = e && atoi(e) > 0 ? atoi(e) : 0;
        sFailFrom = n;
    }
    if (n <= 0 || !path) return false;
    const size_t l = strlen(path), k = strlen("GameData.bin");
    if (l < k || strcmp(path + l - k, "GameData.bin") != 0) return false;
    return ++sFailCount >= n;
}

static bool commitTmpImpl(const char* path, MpSaveValidator valid) {
    if (testFailThisCommit(path)) return false;   // (test hook above: the .mptmp stays)
    InFlight inFlight;
    char tmp[600], bak[600];
    if (!path || snprintf(tmp, sizeof tmp, "%s.mptmp", path) >= (int)sizeof tmp ||
        snprintf(bak, sizeof bak, "%s.mpbak", path) >= (int)sizeof bak)
        return false;
    uint8_t* buf = (uint8_t*)malloc(MP_SAVE_MAX_FILE);
    if (!buf) { unlink(tmp); return false; }
    uint32_t len = 0;
    bool ok = mp_save_read_file(tmp, buf, MP_SAVE_MAX_FILE, &len) == 0 && len > 0 && (!valid || valid(buf, len));
    if (!ok) unlink(tmp);  // a short / torn temporary never replaces anything
    ok = ok && commitTmp(path, tmp, bak, buf, len);
    free(buf);
    return ok;
}

bool mp_save_write_atomic_max(const char* path, const uint8_t* data, uint32_t len, uint32_t maxLen) {
    const bool ok = writeAtomicImpl(path, data, len, maxLen);
    logWrite(path, ok);
    return ok;
}

bool mp_save_commit_tmp(const char* path, MpSaveValidator valid) {
    const bool ok = commitTmpImpl(path, valid);
    logWrite(path, ok);
    return ok;
}

void mp_save_note_write(int delta) { sWritesInFlight += delta; }
bool mp_save_write_in_flight(void) { return sWritesInFlight.load() > 0 || !mp_save_async_idle(); }

bool mp_save_keep_aside(const char* path) {
    char a[620], b[620];
    // .mpbad (newest), .mpbad.1, .mpbad.2 (oldest kept): an earlier file kept aside is never overwritten by a later one
    snprintf(a, sizeof a, "%s.mpbad.2", path);
    unlink(a);
    snprintf(a, sizeof a, "%s.mpbad.1", path);
    snprintf(b, sizeof b, "%s.mpbad.2", path);
    if (exists(a)) rename(a, b);
    snprintf(a, sizeof a, "%s.mpbad", path);
    snprintf(b, sizeof b, "%s.mpbad.1", path);
    if (exists(a)) rename(a, b);
    return rename(path, a) == 0;
}

bool mp_save_is_newer(const uint8_t* data, uint32_t len) {
    if (!data || len < 16) return false;
    const uint32_t version = rd32(data + 4);
    return version > MP_SAVE_VERSION && version < 0x10000;
}

bool mp_save_is_gamedata(const uint8_t* data, uint32_t len) { return mp_save_validate(data, len, true, true) == MP_SAVE_OK; }

bool mp_save_recover(const char* path, bool* changed) { return mp_save_recover_with(path, mp_save_is_gamedata, changed); }

bool mp_save_drop_session_backup(const char* path, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    char tmp[600], bak[600];
    if (!path || snprintf(tmp, sizeof tmp, "%s.mptmp", path) >= (int)sizeof tmp || snprintf(bak, sizeof bak, "%s.mpbak", path) >= (int)sizeof bak) {
        *why = "bad path";
        return false;
    }
    if (!exists(bak)) { *why = "no .mpbak"; return false; }
    if (exists(tmp)) { *why = "a .mptmp is beside GameData.bin (an unfinished save)"; return false; }
    uint8_t* buf = (uint8_t*)malloc(MP_SAVE_MAX_FILE);
    if (!buf) { *why = "no memory"; return false; }
    uint32_t len = 0;
    const int rc = mp_save_read_file(path, buf, MP_SAVE_MAX_FILE, &len);
    const bool good = rc == 0 && mp_save_is_gamedata(buf, len);
    free(buf);
    if (!good) { *why = rc == -ENOENT ? "GameData.bin is missing" : "GameData.bin does not read or validate"; return false; }
    if (unlink(bak) != 0) { *why = "unlink failed"; return false; }
    return true;
}

bool mp_save_recover_with(const char* path, MpSaveValidator valid, bool* changed) {
    if (changed) *changed = false;
    char tmp[600], bak[600];
    if (snprintf(tmp, sizeof tmp, "%s.mptmp", path) >= (int)sizeof tmp || snprintf(bak, sizeof bak, "%s.mpbak", path) >= (int)sizeof bak)
        return false;
    uint8_t* buf = (uint8_t*)malloc(MP_SAVE_MAX_FILE);
    if (!buf) return false;
    bool result = false;
    uint32_t plen = 0;
    const int prc = mp_save_read_file(path, buf, MP_SAVE_MAX_FILE, &plen);
    if (prc != 0 && prc != -ENOENT && prc != -EFBIG) {
        // savefix: the card could not read the file just now (not "the file is bad"): change nothing; a later call decides
        free(buf);
        return false;
    }
    if (prc == 0 && valid == mp_save_is_gamedata && mp_save_is_newer(buf, plen)) {
        // savefix (keepsafe S3): a GameData.bin of a newer build is never "repaired" with an older version: left as it is
        free(buf);
        return false;
    }
    if (prc == 0 && valid(buf, plen)) {
        if (exists(tmp)) unlink(tmp);  // a write that did not finish (the old file is intact)
        result = true;
    } else {
        // smallfix: a MISSING file is restored only when a .mptmp proves an interrupted write (the replace is: write .mptmp, rename the old
        // file to .mpbak, rename .mptmp into place; a cut between the two renames leaves no file, the old one as .mpbak and the new
        // one as .mptmp; a cut anywhere else leaves the old file in place). A missing file with only a .mpbak beside it is what the
        // player leaves by deleting the save outside the game: it stays missing (a fresh start), nothing is restored, nothing is changed.
        const int tmpS = existsState(tmp);
        if (tmpS < 0) {   // savelife review: a stat error is not "absent"
            if (mp_log) mp_log("save: %s: its .mptmp could not be looked at just now (card error): nothing repaired", path);
            free(buf);
            return false;
        }
        const bool tmpThere = tmpS == 1;
        if (prc == -ENOENT && !tmpThere) {
            if (exists(bak) && mp_log) mp_log("save: %s is missing and only its .mpbak exists (no .mptmp: not an interrupted write): not restored", path);
            free(buf);
            return true;   // (nothing to repair: not an error; *changed stays false)
        }
        // savelife (seed 9364): a read ERROR on the .mptmp or the .mpbak (a transient card error: EIO) is not "torn". Taking it for torn made the
        // repair delete the .mptmp (the proof of the interrupted replace) and leave a lone .mpbak, which the next call takes for a deleted save:
        // the previous co-op file stayed unreachable and the next session started a fresh one. Unreadable: change nothing, a later call decides.
        const int tmpState = fileState(tmp, buf, valid), bakState = tmpState == 1 ? 0 : fileState(bak, buf, valid);
        const char* from = tmpState == 1 ? tmp : bakState == 1 ? bak : nullptr;
        // (an unreadable .mptmp may hold the newer version: even a valid .mpbak is not put in its place then; an unreadable .mpbak with no valid .mptmp
        // may be the only good copy)
        if (tmpState < 0 || (!from && bakState < 0)) {
            if (mp_log) mp_log("save: %s: its .mptmp / .mpbak could not be read just now (card error): left as they are, nothing repaired", path);
            free(buf);
            return false;
        }
        if (from) {
            if (exists(path)) {
                // smallfix: restoring from the .mpbak with no .mptmp: a cut between "the torn file moved aside" and "the .mpbak renamed in" would
                // leave no file and a lone .mpbak, which the next boot takes for a deleted save. An empty .mptmp first is the "repair in progress"
                // marker: the boot repair then finishes the restore (an empty file is no valid temporary, so the .mpbak is used) and removes it.
                if (from == bak && !tmpThere) {
                    // The marker is synced (file, then the directory entry / device cache) before the torn file moves: the file system gives no
                    // ordering between two renames and a create. A marker that cannot be made (a full card) is logged: the restore goes on unprotected.
                    const int mfd = open(tmp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0660);
                    if (mfd < 0) {
                        if (mp_log) mp_log("save: %s: the repair-in-progress marker (.mptmp) could not be created (errno %d): restoring from the .mpbak without it", path, errno);
                    } else {
                        const bool synced = fsync(mfd) == 0;
                        close(mfd);
                        syncDevice(path);
                        if (!synced && mp_log) mp_log("save: %s: the repair-in-progress marker could not be synced (errno %d)", path, errno);
                    }
                }
                // An invalid primary: keep it aside rather than destroy it (savefix: earlier ones kept too).
                mp_save_keep_aside(path);
            }
            result = rename(from, path) == 0;
            if (changed) *changed = result;
            // savefix: a valid temporary that could not be put in place stays (the next call tries again). smallfix: so does a torn one
            // when the restore from the .mpbak failed: it is the proof of an interrupted write; without it a lone .mpbak is never restored
            if (result) {
                if (exists(tmp)) unlink(tmp);  // torn or already used
            }
        } else {
            // A4 (torn-write loop): an invalid primary with no valid temporary and no backup is what a medium leaves when a rename survived
            // a power cut but the data did not (right name, zeros or garbage inside). Left in place it would be taken for the player's
            // file; kept aside (.mpbad, three kept, nothing is destroyed) the state is "no file": a fresh start. Recovery records
            // (valid == mp_save_is_recovery) are left alone: an invalid one may be a newer build's format, which a prune never deletes either.
            if (prc == 0 && valid != mp_save_is_recovery) mp_save_keep_aside(path);
            result = !exists(path) && !exists(bak);  // nothing there and nothing to restore: a fresh start, not an error
            if (exists(tmp)) unlink(tmp);  // torn
        }
    }
    free(buf);
    return result;
}

namespace {
void civil(int64_t t, int* y, int* mo, int* d, int* h, int* mi, int* s) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    *h = (int)(rem / 3600); *mi = (int)(rem / 60 % 60); *s = (int)(rem % 60);
    // Howard Hinnant's days_from_civil inverse.
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mo <= 2));
}

// Backups (savefix, review finding 3): GameData-SSSSSSSS-YYYYMMDD-HHMMSS.bin, S = a sequence number (one more than the largest
// present), the date for the player only. Ordered by the sequence, never by the clock: a console clock set back (date reset,
// changed by the player) used to name the new backup "oldest" and the prune deleted it at once. Names of the earlier layout
// (GameData-YYYYMMDD-HHMMSS-NN.bin, 31 chars) count as sequence 0, older than every new one, ordered among themselves by name.
bool digits(const char* p, int n) {
    for (int i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9') return false;
    return true;
}
// -1: not a backup name; else its sequence number (0 for the earlier layout).
long long backupSeqOf(const char* n) {
    const size_t len = strlen(n);
    if (strncmp(n, "GameData-", 9) != 0 || len < 4 || strcmp(n + len - 4, ".bin") != 0) return -1;
    if (len == 31 && digits(n + 9, 8) && n[17] == '-' && digits(n + 18, 6) && n[24] == '-' && digits(n + 25, 2)) return 0;
    if (len == 37 && digits(n + 9, 8) && n[17] == '-' && digits(n + 18, 8) && n[26] == '-' && digits(n + 27, 6)) return atoll(n + 9) + 0;
    return -1;
}
struct BackupName { long long seq; char name[40]; };
int backupCmp(const void* a, const void* b) {  // oldest first
    const BackupName* x = (const BackupName*)a;
    const BackupName* y = (const BackupName*)b;
    if (x->seq != y->seq) return x->seq < y->seq ? -1 : 1;
    return strcmp(x->name, y->name);
}
// Backups in dir, oldest first (at most max). Heap: called from the host-job thread only.
int listBackups(const char* dir, BackupName* out, int max) {
    int count = 0;
    if (DIR* d = opendir(dir)) {
        while (struct dirent* e = readdir(d)) {
            const long long seq = backupSeqOf(e->d_name);
            if (seq < 0 || count >= max) continue;
            out[count].seq = seq;
            memcpy(out[count].name, e->d_name, strlen(e->d_name) + 1);  // 31 or 37 chars (backupSeqOf)
            count++;
        }
        closedir(d);
    }
    qsort(out, (size_t)count, sizeof out[0], backupCmp);
    return count;
}
}  // namespace

static const int kBackupListMax = 512;

bool mp_save_backup(const char* srcPath, const char* backupDir, int64_t unixTime, int keep, char* outPath, size_t outCap) {
    if (outPath && outCap) outPath[0] = 0;
    uint8_t* buf = (uint8_t*)malloc(MP_SAVE_MAX_FILE);
    uint8_t* check = (uint8_t*)malloc(MP_SAVE_MAX_FILE);
    BackupName* list = (BackupName*)malloc(sizeof(BackupName) * kBackupListMax);
    bool ok = buf && check && list;
    uint32_t len = 0;
    ok = ok && mp_save_read_file(srcPath, buf, MP_SAVE_MAX_FILE, &len) == 0 && mp_save_validate(buf, len, true, true) == MP_SAVE_OK;
    ok = ok && mp_save_mkdirs(backupDir);
    char path[600], name[64];
    path[0] = name[0] = 0;
    if (ok) {
        int y, mo, d, h, mi, s;
        civil(unixTime, &y, &mo, &d, &h, &mi, &s);
        if (y < 0 || y > 9999) y = mo = d = h = mi = s = 0;
        mo %= 100; d %= 100; h %= 100; mi %= 100; s %= 100;  // (already in range: tells the compiler the name fits)
        const int n = listBackups(backupDir, list, kBackupListMax);
        long long seq = 1;
        for (int i = 0; i < n; i++)
            if (list[i].seq >= seq) seq = list[i].seq + 1;
        ok = false;
        for (int tries = 0; tries < 100 && seq <= 99999999; tries++, seq++) {
            snprintf(name, sizeof name, "GameData-%08d-%04d%02d%02d-%02d%02d%02d.bin", (int)seq, y, mo, d, h, mi, s);
            snprintf(path, sizeof path, "%s/%s", backupDir, name);
            if (!exists(path)) { ok = true; break; }
        }
    }
    ok = ok && mp_save_write_atomic(path, buf, len);
    // Keep the newest `keep` by sequence; never the file just written (it is the newest by construction; checked anyway).
    if (ok && keep > 0) {
        const int count = listBackups(backupDir, list, kBackupListMax);
        // savesus (scenario 4): with keep >= 2 the OLDEST backup is pinned and the newest keep-1 are kept (still `keep` files): the copy made
        // before the first session is the player's own file; every later backup already holds what earlier sessions did to it.
        const int pin = keep >= 2 && count > 0 ? 1 : 0;
        for (int i = pin; i + (keep - pin) < count; i++) {
            if (strcmp(list[i].name, name) == 0) continue;
            char old[600];
            snprintf(old, sizeof old, "%s/%s", backupDir, list[i].name);
            unlink(old);
        }
    }
    // The promise callers rely on ("refuses without a backup"): the backup is there and holds exactly the source's bytes.
    uint32_t got = 0;
    ok = ok && mp_save_read_file(path, check, MP_SAVE_MAX_FILE, &got) == 0 && got == len && memcmp(check, buf, len) == 0;
    free(buf);
    free(check);
    free(list);
    if (!ok) return false;
    if (outPath && outCap) snprintf(outPath, outCap, "%s", path);
    return true;
}

int mp_save_backup_list(const char* backupDir, MpSaveBackupInfo* out, int max) {
    if (!backupDir || !out || max <= 0) return 0;
    BackupName* list = (BackupName*)malloc(sizeof(BackupName) * kBackupListMax);
    if (!list) return 0;
    const int n = listBackups(backupDir, list, kBackupListMax);
    int count = 0;
    for (int i = n - 1; i >= 0 && count < max; i--, count++) {  // newest first
        memcpy(out[count].name, list[i].name, sizeof out[count].name);
        out[count].seq = list[i].seq;
    }
    free(list);
    return count;
}

// ---- whole operations ---------------------------------------------------------------------------------------------------
const char* mp_save_op_name(int err) {
    switch (err) {
    case MP_SAVE_OP_OK: return "ok";
    case MP_SAVE_OP_SRC_MISSING: return "source missing";
    case MP_SAVE_OP_SRC_INVALID: return "source invalid";
    case MP_SAVE_OP_DST_MISSING: return "destination missing";
    case MP_SAVE_OP_DST_INVALID: return "destination invalid";
    case MP_SAVE_OP_SAME_FILE: return "same file";
    case MP_SAVE_OP_BACKUP: return "backup failed";
    case MP_SAVE_OP_WRITE: return "write failed";
    case MP_SAVE_OP_VERIFY: return "verify failed";
    case MP_SAVE_OP_ARGS: return "bad arguments";
    case MP_SAVE_OP_NO_MEMORY: return "no memory";
    case MP_SAVE_OP_DST_ASIDE: return "destination invalid, kept aside";
    default: return "?";
    }
}

namespace {
struct Work {
    uint8_t* a;
    uint8_t* b;
    Work() : a((uint8_t*)calloc(1, MP_SAVE_MAX_FILE)), b((uint8_t*)calloc(1, MP_SAVE_MAX_FILE)) {}
    ~Work() { free(a); free(b); }
    bool ok() const { return a && b; }
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
};

// Read + full validation. 0 ok, MP_SAVE_OP_*_MISSING / _INVALID by role.
int readValid(const char* path, uint8_t* buf, uint32_t* len, bool src) {
    const int rc = mp_save_read_file(path, buf, MP_SAVE_MAX_FILE, len);
    if (rc == -ENOENT) return src ? MP_SAVE_OP_SRC_MISSING : MP_SAVE_OP_DST_MISSING;
    if (rc != 0 || mp_save_validate(buf, *len, true, true) != MP_SAVE_OK) return src ? MP_SAVE_OP_SRC_INVALID : MP_SAVE_OP_DST_INVALID;
    return MP_SAVE_OP_OK;
}

bool sameFile(const char* a, const char* b) {
    if (strcmp(a, b) == 0) return true;
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
}  // namespace

bool mp_save_view_slot(const uint8_t* image, int slot, MpSaveSnapshotView* v) {
    if (!image || !v) return false;
    for (int k = 0; k < 3; k++) {
        char n[16];
        uint32_t off, size;
        if (!slotName(n, kKinds[k], slot) || !mp_save_find(image, n, &off, &size) || size == 0) return false;
        v->block[k] = image + off;
        v->len[k] = (uint16_t)(size > kSnapCaps[k] ? kSnapCaps[k] : size);
    }
    v->slot = (uint8_t)slot;
    v->flags = 0;
    v->crc = 0;
    return true;
}

int mp_save_put_blocks(const MpSaveSnapshotView* blocks, const char* dstPath, int dstSlot, const uint8_t* fallbackBase, uint32_t fallbackLen,
                       uint8_t* outImage) {
    if (!blocks || !dstPath || dstSlot < 1 || dstSlot > MP_SAVE_SLOTS) return MP_SAVE_OP_ARGS;
    if (mp_save_check_view(blocks, MP_SAVECHECK_STRUCT, nullptr) != MP_SC_OK) return MP_SAVE_OP_SRC_INVALID;  // MP3 snapval
    Work w;
    if (!w.ok()) return MP_SAVE_OP_NO_MEMORY;
    uint32_t len = 0;
    // gamefix: the blank base replaces the destination ONLY when there is no file. A file that exists but fails validation (or cannot
    // be read) is never silently blanked: that would drop the other five slots of a co-op file. With a fallback (the co-op file) a
    // file that was read but is invalid is moved aside as <path>.mpbad and nothing is written (the caller reports it; the next
    // attempt finds no file and starts a fresh one). An unreadable file (I/O error) is left exactly as it is. Without a fallback
    // (a single-player destination) the file is never touched at all.
    int rr = mp_save_read_file(dstPath, w.a, MP_SAVE_MAX_FILE, &len);
    if (rr == -ENOENT) {
        // savefix: no file may only mean an interrupted replace (.mptmp / .mpbak next to it): repair first, and never start a
        // fresh file over a previous version that could not be put back
        bool changed = false;
        mp_save_recover(dstPath, &changed);
        rr = mp_save_read_file(dstPath, w.a, MP_SAVE_MAX_FILE, &len);
        char side[600];
        // smallfix: a .mpbak alone is a deleted file (a fresh start); only a .mptmp that could not be put back refuses
        snprintf(side, sizeof side, "%s.mptmp", dstPath);
        if (rr == -ENOENT && existsState(side) != 0) return MP_SAVE_OP_DST_INVALID;   // (a stat error is "cannot tell": refuse too)
    }
    if (rr == -ENOENT) {
        if (!fallbackBase || fallbackLen > MP_SAVE_MAX_FILE || mp_save_validate(fallbackBase, fallbackLen, false, false) != MP_SAVE_OK) return MP_SAVE_OP_DST_MISSING;
        memset(w.a, 0, MP_SAVE_MAX_FILE);
        memcpy(w.a, fallbackBase, fallbackLen);
    } else if (rr != 0 || mp_save_validate(w.a, len, true, true) != MP_SAVE_OK) {
        const bool unreadable = rr != 0 && rr != -EFBIG;
        if (!fallbackBase || unreadable) return MP_SAVE_OP_DST_INVALID;
        if (rr == 0 && mp_save_is_newer(w.a, len)) return MP_SAVE_OP_DST_INVALID;   // savefix (keepsafe S4): a newer build's file
        // savelife review: a previous version / the next version beside the torn file: the repair decides first (it may put a good one back);
        // when it cannot decide (a side file could not be read) the file is left exactly as it is, not set aside
        bool repaired = false;
        char s1[600], s2[600];
        snprintf(s1, sizeof s1, "%s.mptmp", dstPath);
        snprintf(s2, sizeof s2, "%s.mpbak", dstPath);
        if (existsState(s1) != 0 || existsState(s2) != 0) {
            bool ch = false;
            const bool ok = mp_save_recover(dstPath, &ch);
            if (ch) {
                rr = mp_save_read_file(dstPath, w.a, MP_SAVE_MAX_FILE, &len);
                repaired = rr == 0 && mp_save_validate(w.a, len, true, true) == MP_SAVE_OK;
            }
            if (!repaired && !ok) return existsState(dstPath) == 0 ? MP_SAVE_OP_DST_ASIDE : MP_SAVE_OP_DST_INVALID;
        }
        if (!repaired) {
            if (existsState(dstPath) == 0) return MP_SAVE_OP_DST_ASIDE;   // (the repair moved it aside itself: nothing left to move)
            return mp_save_keep_aside(dstPath) ? MP_SAVE_OP_DST_ASIDE : MP_SAVE_OP_DST_INVALID;
        }
    }
    // The blocks may point into fallbackBase or anywhere else: w.a is our own copy, so applying is safe.
    if (!mp_save_snapshot_apply(blocks, w.a, dstSlot) || !mp_save_finalize(w.a, MP_SAVE_MAX_FILE)) return MP_SAVE_OP_DST_INVALID;
    const uint32_t out = mp_save_disk_length(w.a);
    if (mp_save_validate(w.a, out, true, true) != MP_SAVE_OK) return MP_SAVE_OP_VERIFY;
    if (!mp_save_write_atomic(dstPath, w.a, out)) return MP_SAVE_OP_WRITE;
    if (readValid(dstPath, w.b, &len, false) != MP_SAVE_OP_OK || len != out || memcmp(w.a, w.b, out) != 0) return MP_SAVE_OP_VERIFY;
    if (outImage) {
        memset(outImage, 0, MP_SAVE_MAX_FILE);
        memcpy(outImage, w.a, out);
    }
    return MP_SAVE_OP_OK;
}

int mp_save_put_slot(const uint8_t* srcImage, uint32_t srcLen, int srcSlot, const char* dstPath, int dstSlot, const uint8_t* fallbackBase,
                     uint32_t fallbackLen, uint8_t* outImage) {
    if (!srcImage || !dstPath || srcSlot < 1 || srcSlot > MP_SAVE_SLOTS || dstSlot < 1 || dstSlot > MP_SAVE_SLOTS) return MP_SAVE_OP_ARGS;
    if (mp_save_validate(srcImage, srcLen, false, false) != MP_SAVE_OK) return MP_SAVE_OP_SRC_INVALID;
    MpSaveSnapshotView v;
    if (!mp_save_view_slot(srcImage, srcSlot, &v)) return MP_SAVE_OP_SRC_INVALID;
    return mp_save_put_blocks(&v, dstPath, dstSlot, fallbackBase, fallbackLen, outImage);
}

bool mp_save_fill_blank_base(uint8_t* image, uint32_t cap, const MpSaveSnapshotView* blank) {
    if (!image || !blank || cap < MP_SAVE_MAX_FILE || mp_save_validate(image, cap, false, false) != MP_SAVE_OK) return false;
    for (int s = 1; s <= MP_SAVE_SLOTS; s++)
        if (!mp_save_snapshot_apply(blank, image, s)) return false;
    return true;
}

uint32_t mp_save_block_used(const uint8_t* block, uint32_t size) {
    if (!block || size < 4 || block[0] != 1) return size;
    uint32_t p = 4;
    for (uint32_t c = 0; c < block[1]; c++) {
        if (size - p < 12) return size;
        const uint32_t n = rd32(block + p + 8);
        if (n < 12 || n > size - p) return size;
        p += n;
    }
    return p;
}

bool mp_save_slot_matches(const uint8_t* image, int slot, const MpSaveSnapshotView* blocks) {
    MpSaveSnapshotView v;
    if (!blocks || !mp_save_view_slot(image, slot, &v)) return false;
    for (int k = 0; k < 3; k++) {
        const uint32_t a = mp_save_block_used(v.block[k], v.len[k]), b = mp_save_block_used(blocks->block[k], blocks->len[k]);
        if (a != b || memcmp(v.block[k], blocks->block[k], a) != 0) return false;
    }
    return true;
}

int mp_save_import_file(const char* spPath, int spSlot, const char* mpPath, int mpSlot, const uint8_t* fallbackBase, uint32_t fallbackLen,
                        uint8_t* outImage) {
    if (!spPath || !mpPath) return MP_SAVE_OP_ARGS;
    if (sameFile(spPath, mpPath)) return MP_SAVE_OP_SAME_FILE;  // the single-player file is never a destination
    Work w;
    if (!w.ok()) return MP_SAVE_OP_NO_MEMORY;
    uint32_t len = 0;
    const int rc = readValid(spPath, w.a, &len, true);  // mp_save_read_file: O_RDONLY, nothing else touches spPath
    if (rc != MP_SAVE_OP_OK) return rc;
    MpSaveSnapshotView v;  // MP3 snapval: the imported user file goes into the game's parsers in multiplayer: same check as a snapshot
    if (mp_save_view_slot(w.a, spSlot, &v) && mp_save_check_view(&v, MP_SAVECHECK_FULL, nullptr) != MP_SC_OK) return MP_SAVE_OP_SRC_INVALID;
    return mp_save_put_slot(w.a, len, spSlot, mpPath, mpSlot, fallbackBase, fallbackLen, outImage);
}

int mp_save_writeback_file(const char* mpPath, int mpSlot, const char* spPath, int spSlot, const char* backupDir, int64_t unixTime, int keep,
                           char* backupOut, size_t backupCap, uint8_t* outImage) {
    if (backupOut && backupCap) backupOut[0] = 0;
    if (!mpPath || !spPath || !backupDir) return MP_SAVE_OP_ARGS;
    if (sameFile(spPath, mpPath)) return MP_SAVE_OP_SAME_FILE;
    Work w;
    if (!w.ok()) return MP_SAVE_OP_NO_MEMORY;
    uint32_t len = 0, spLen = 0;
    int rc = readValid(mpPath, w.a, &len, true);
    if (rc != MP_SAVE_OP_OK) return rc;
    // Never replace a single-player file that is missing or does not validate (it may be valid for a newer build).
    rc = readValid(spPath, w.b, &spLen, false);
    if (rc != MP_SAVE_OP_OK) return rc;
    if (!mp_save_backup(spPath, backupDir, unixTime, keep, backupOut, backupCap)) return MP_SAVE_OP_BACKUP;
    return mp_save_put_slot(w.a, len, mpSlot, spPath, spSlot, nullptr, 0, outImage);
}

int mp_save_restore_backup(const char* backupPath, const char* spPath, const char* backupDir, int64_t unixTime, int keep, char* beforeOut,
                           size_t beforeCap) {
    if (beforeOut && beforeCap) beforeOut[0] = 0;
    if (!backupPath || !spPath || !backupDir) return MP_SAVE_OP_ARGS;
    if (sameFile(backupPath, spPath)) return MP_SAVE_OP_SAME_FILE;
    Work w;
    if (!w.ok()) return MP_SAVE_OP_NO_MEMORY;
    uint32_t len = 0, curLen = 0;
    int rc = readValid(backupPath, w.a, &len, true);   // the backup must be a whole, valid GameData.bin
    if (rc != MP_SAVE_OP_OK) return rc;
    // What is there now is kept first: a valid file as a normal backup (required), a damaged one aside as it is. A file that
    // cannot be read just now is not replaced.
    const int cr = mp_save_read_file(spPath, w.b, MP_SAVE_MAX_FILE, &curLen);
    if (cr == 0 && mp_save_validate(w.b, curLen, true, true) == MP_SAVE_OK) {
        if (curLen == len && memcmp(w.a, w.b, len) == 0) return MP_SAVE_OP_OK;   // (already that file)
        if (!mp_save_backup(spPath, backupDir, unixTime, keep, beforeOut, beforeCap)) return MP_SAVE_OP_BACKUP;
    } else if (cr == 0 || cr == -EFBIG) {
        char aside[600];
        snprintf(aside, sizeof aside, "%s/Damaged-%ld.bin", backupDir, (long)unixTime);
        if (!mp_save_mkdirs(backupDir)) return MP_SAVE_OP_BACKUP;
        if (cr == 0 && !mp_save_write_atomic(aside, w.b, curLen)) return MP_SAVE_OP_BACKUP;
        if (beforeOut && beforeCap) snprintf(beforeOut, beforeCap, "%s", aside);
    } else if (cr != -ENOENT) {
        return MP_SAVE_OP_DST_INVALID;
    }
    if (!mp_save_write_atomic(spPath, w.a, len)) return MP_SAVE_OP_WRITE;
    if (readValid(spPath, w.b, &curLen, false) != MP_SAVE_OP_OK || curLen != len || memcmp(w.a, w.b, len) != 0) return MP_SAVE_OP_VERIFY;
    return MP_SAVE_OP_OK;
}

int mp_save_slot_state_file(const char* path, bool coop, int slot, int* stars) {
    if (stars) *stars = -1;
    if (!path || slot < 1 || slot > MP_SAVE_SLOTS) return -1;
    uint8_t* buf = (uint8_t*)malloc(MP_SAVE_MAX_FILE);
    if (!buf) return -1;
    uint32_t len = 0;
    const int rc = mp_save_read_file(path, buf, MP_SAVE_MAX_FILE, &len);
    int st = -1;
    if (rc == -ENOENT) {
        // no file: new, unless an interrupted replace left its next version (.mptmp) beside it (not "no file yet"). A .mpbak alone is
        // a save deleted outside the game (smallfix): new.
        char side[620];
        snprintf(side, sizeof side, "%s.mptmp", path);
        st = exists(side) ? -1 : 0;
    } else if (rc == 0 && mp_save_validate(buf, len, true, true) == MP_SAVE_OK) {
        const int created = mp_save_slot_state(buf, slot);
        if (created < 0) st = -1;
        else if (created == 0 && !coop) st = 0;
        else {
            int m = 0, l = 0;
            if (mp_save_block_stars_ok(buf, slot, &m, &l)) {
                st = 1;
                if (stars) *stars = m + l;
            }
        }
    }
    free(buf);
    return st;
}

// ---- progress compare --------------------------------------------------------------------------------------------------------
namespace {
uint16_t gameHash16(const char* s) {  // MR::getHashCode (hash = c + hash * 31), truncated to u16 like the attribute table
    uint32_t h = 0;
    for (; *s; s++) h = (uint32_t)(signed char)*s + h * 31u;
    return (uint16_t)h;
}
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
}  // namespace

int mp_save_block_stars(const uint8_t* block, uint32_t size) {
    if (!block || size < 4) return -1;
    if (block[0] == 0 && block[1] == 0) return 0;  // never written
    if (block[0] != 1) return -1;
    const uint32_t chunks = block[1];
    uint32_t p = 4;
    for (uint32_t c = 0; c < chunks; c++) {
        if (size - p < 12) return -1;
        const uint32_t sig = rd32(block + p), chunkSize = rd32(block + p + 8);
        if (chunkSize < 12 || chunkSize > size - p) return -1;
        if (sig == 0x47414C41u) {  // 'GALA'
            const uint8_t* d = block + p + 12;
            const uint32_t n = chunkSize - 12;
            if (n < 6) return -1;
            const uint32_t galaxies = rd16(d), attrs = rd16(d + 2), recSize = rd16(d + 4);
            const uint32_t header = 4 + attrs * 4;
            if (2 + header > n) return -1;
            const uint16_t want = gameHash16("mPowerStarFlag");
            uint32_t flagOff = 0xFFFFFFFFu;
            for (uint32_t a = 0; a < attrs; a++) {
                if (rd16(d + 2 + 4 + a * 4) == want) flagOff = rd16(d + 2 + 4 + a * 4 + 2);
            }
            if (flagOff == 0xFFFFFFFFu || flagOff >= recSize || recSize == 0) return -1;
            if ((uint64_t)galaxies * recSize > n - 2 - header) return -1;
            int stars = 0;
            const uint8_t* rec = d + 2 + header;
            for (uint32_t g = 0; g < galaxies; g++) {
                uint8_t f = rec[g * recSize + flagOff];
                while (f) { stars += f & 1; f >>= 1; }
            }
            return stars;
        }
        p += chunkSize;
    }
    return 0;  // no galaxy chunk: nothing recorded
}

bool mp_save_block_stars_ok(const uint8_t* image, int slot, int* mario, int* luigi) {
    char m[16], l[16];
    uint32_t mo, ms, lo, ls;
    if (!slotName(m, "mario", slot) || !slotName(l, "luigi", slot) || !mp_save_find(image, m, &mo, &ms) || !mp_save_find(image, l, &lo, &ls))
        return false;
    const int a = mp_save_block_stars(image + mo, ms), b = mp_save_block_stars(image + lo, ls);
    if (a < 0 || b < 0) return false;
    *mario = a;
    *luigi = b;
    return true;
}

int mp_save_slot_stars(const uint8_t* image, int slot, int* mario, int* luigi) {
    char m[16], l[16];
    uint32_t mo, ms, lo, ls;
    if (!slotName(m, "mario", slot) || !slotName(l, "luigi", slot) || !mp_save_find(image, m, &mo, &ms) || !mp_save_find(image, l, &lo, &ls))
        return -1;
    int a = mp_save_block_stars(image + mo, ms), b = mp_save_block_stars(image + lo, ls);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (mario) *mario = a;
    if (luigi) *luigi = b;
    return a + b;
}

// flow (review 2 #3): whether a user file was ever created, the way the game's file select decides it: the "config<N>" block's
// CONF chunk (ConfigDataCreateChunk::mIsCreated, written 0xFF when the player made the file). mp_save_slot_stars counts an uncreated
// slot as 0 stars (right for comparisons), but a menu that offers the slot must not take it for a file.
// 1 created (and both player blocks parse), 0 never created, -1 not readable (no such slot, or a block that does not parse).
// the config block's CONF chunk: 1 created, 0 never created, -1 not readable
static int configCreated(const uint8_t* cfg, uint32_t cs) {
    if (cs < 4) return -1;
    if (cfg[0] == 0 && cfg[1] == 0) return 0;   // the config block was never written: no file
    if (cfg[0] != 1) return -1;
    int created = -1;
    for (uint32_t k = 0, p = 4; k < cfg[1]; k++) {
        if (cs - p < 12) return -1;
        const uint32_t sig = rd32(cfg + p), size = rd32(cfg + p + 8);
        if (size < 12 || size > cs - p) return -1;
        if (sig == 0x434F4E46u) {   // 'CONF'
            if (size < 13) return -1;
            created = cfg[p + 12] != 0 ? 1 : 0;
        }
        p += size;
    }
    return created;
}

int mp_save_slot_state(const uint8_t* image, int slot) {
    char m[16], l[16], c[16];
    uint32_t mo, ms, lo, ls, co, cs;
    if (!image || !slotName(m, "mario", slot) || !slotName(l, "luigi", slot) || !slotName(c, "config", slot) ||
        !mp_save_find(image, m, &mo, &ms) || !mp_save_find(image, l, &lo, &ls) || !mp_save_find(image, c, &co, &cs))
        return -1;
    const int created = configCreated(image + co, cs);
    if (created < 0) return -1;
    if (created == 0) return 0;
    if (mp_save_block_stars(image + mo, ms) < 0 || mp_save_block_stars(image + lo, ls) < 0) return -1;
    return 1;
}

// savebug: the same for a snapshot / recovery view (its config block): 1 a created file's progress, 0 a never-created (blank) slot, -1 unreadable
int mp_save_view_state(const MpSaveSnapshotView* v) {
    if (!v || !v->block[0] || !v->block[1] || !v->block[2]) return -1;
    const int created = configCreated(v->block[2], v->len[2]);
    if (created < 0) return -1;
    if (created == 0) return 0;
    if (mp_save_block_stars(v->block[0], v->len[0]) < 0 || mp_save_block_stars(v->block[1], v->len[1]) < 0) return -1;
    return 1;
}

int mp_save_compare_progress(const uint8_t* srcImage, int srcSlot, const uint8_t* dstImage, int dstSlot) {
    const int a = mp_save_slot_stars(srcImage, srcSlot, nullptr, nullptr), b = mp_save_slot_stars(dstImage, dstSlot, nullptr, nullptr);
    return (a < 0 ? 0 : a) - (b < 0 ? 0 : b);
}

// ---- session save policy -------------------------------------------------------------------------------------------------
int mp_save_policy(bool active, int localSlot, int hostSlot, unsigned hostEpoch, bool bridgeSuppressed) {
    if (!active) return MP_SAVE_POLICY_NORMAL;
    if (bridgeSuppressed) return MP_SAVE_POLICY_SUPPRESS;
    if (localSlot == 0 && hostSlot == 0 && hostEpoch == 0) return MP_SAVE_POLICY_AUTOCONFIRM;
    return MP_SAVE_POLICY_SUPPRESS;
}

// ---- recovery checkpoints -------------------------------------------------------------------------------------------------
uint32_t mp_save_recovery_pack(const MpSaveRecoveryInfo* info, const MpSaveSnapshotView* blocks, uint8_t* out, uint32_t cap) {
    if (!info || !blocks || !out || info->slot < 1 || info->slot > MP_SAVE_SLOTS || info->srcKind > MP_SAVE_SRC_SINGLE ||
        info->srcSlot > MP_SAVE_SLOTS)
        return 0;
    uint32_t total = MP_SAVE_REC_HEADER;
    for (int k = 0; k < 3; k++) {
        if (!blocks->block[k] || blocks->len[k] == 0 || blocks->len[k] > kSnapCaps[k]) return 0;
        total += blocks->len[k];
    }
    if (total > cap) return 0;
    MpWriter w(out, cap);
    w.u32(MP_SAVE_REC_MAGIC);
    w.u16(MP_SAVE_REC_VERSION);
    w.u16(MP_SAVE_REC_HEADER);
    w.u32(info->sessionId);
    w.u32(info->seq);
    w.u32((uint32_t)(uint64_t)info->unixTime);
    w.u32((uint32_t)((uint64_t)info->unixTime >> 32));
    w.u8(info->flags);
    w.u8(info->slot);
    w.u8(info->srcKind);
    w.u8(info->srcSlot);
    w.u8(info->scenario);
    w.u8(0);
    w.u16(info->stars);
    char g[32];
    memset(g, 0, sizeof g);
    strncpy(g, info->galaxy, sizeof g - 1);
    w.bytes(g, sizeof g);
    for (int k = 0; k < 3; k++) w.u16(blocks->len[k]);
    w.u16(0);
    w.u32(0);
    for (int k = 0; k < 3; k++) w.bytes(blocks->block[k], blocks->len[k]);
    if (!w.ok() || w.n != total) return 0;
    MpWriter c(out + MP_SAVE_REC_HEADER - 4, 4);
    c.u32(mp_save_crc32(out + MP_SAVE_REC_HEADER, total - MP_SAVE_REC_HEADER));
    return total;
}

bool mp_save_recovery_parse(const uint8_t* data, uint32_t len, MpSaveRecoveryInfo* info, MpSaveSnapshotView* blocks) {
    if (!data || len < MP_SAVE_REC_HEADER || len > MP_SAVE_REC_MAX) return false;
    MpReader r(data, len);
    MpSaveRecoveryInfo i;
    memset(&i, 0, sizeof i);
    const uint32_t magic = r.u32();
    const uint16_t version = r.u16(), header = r.u16();
    i.sessionId = r.u32();
    i.seq = r.u32();
    const uint32_t lo = r.u32(), hi = r.u32();
    i.unixTime = (int64_t)(((uint64_t)hi << 32) | lo);
    i.flags = r.u8();
    i.slot = r.u8();
    i.srcKind = r.u8();
    i.srcSlot = r.u8();
    i.scenario = r.u8();
    const uint8_t res1 = r.u8();
    i.stars = r.u16();
    r.bytes(i.galaxy, 32);
    i.galaxy[32] = 0;
    uint16_t lens[3];
    for (int k = 0; k < 3; k++) lens[k] = r.u16();
    const uint16_t res2 = r.u16();
    const uint32_t crc = r.u32();
    if (!r.ok() || magic != MP_SAVE_REC_MAGIC || version != MP_SAVE_REC_VERSION || header != MP_SAVE_REC_HEADER || res1 || res2) return false;
    if (i.slot < 1 || i.slot > MP_SAVE_SLOTS || i.srcKind > MP_SAVE_SRC_SINGLE || i.srcSlot > MP_SAVE_SLOTS ||
        (i.srcKind != MP_SAVE_SRC_NONE && i.srcSlot == 0) || (i.flags & ~(MP_SAVE_REC_FLAG_ORIGINAL_HOST | MP_SAVE_REC_FLAG_MARIO)))
        return false;
    for (char* c = i.galaxy; *c; c++)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) return false;  // stage names are ASCII
    uint32_t total = MP_SAVE_REC_HEADER;
    for (int k = 0; k < 3; k++) {
        if (lens[k] == 0 || lens[k] > kSnapCaps[k]) return false;
        total += lens[k];
    }
    if (total != len || mp_save_crc32(data + MP_SAVE_REC_HEADER, len - MP_SAVE_REC_HEADER) != crc) return false;
    MpSaveSnapshotView v;
    const uint8_t* p = data + MP_SAVE_REC_HEADER;
    for (int k = 0; k < 3; k++) {
        v.block[k] = p;
        v.len[k] = lens[k];
        p += lens[k];
    }
    v.slot = i.slot;
    v.flags = (i.flags & MP_SAVE_REC_FLAG_MARIO) ? MP_SAVE_SNAP_FLAG_MARIO : 0;
    v.crc = crc;
    if (mp_save_check_view(&v, MP_SAVECHECK_FULL, nullptr) != MP_SC_OK) return false;  // MP3 snapval: same check as a snapshot
    if (blocks) *blocks = v;
    if (info) *info = i;
    return true;
}

bool mp_save_is_recovery(const uint8_t* data, uint32_t len) { return mp_save_recovery_parse(data, len, nullptr, nullptr); }

void mp_save_recovery_path(const char* dir, int index, char* out, size_t cap) { snprintf(out, cap, "%s/session-%08d.bin", dir, index); }

namespace {
// "session-NNNNNNNN.bin" with an optional ".mptmp" / ".mpbak": the index, else -1.
int recoveryIndexOf(const char* n) {
    if (strncmp(n, "session-", 8) != 0) return -1;
    int v = 0;
    for (int i = 8; i < 16; i++) {
        if (n[i] < '0' || n[i] > '9') return -1;
        v = v * 10 + (n[i] - '0');
    }
    const char* rest = n + 16;
    if (strcmp(rest, ".bin") == 0 || strcmp(rest, ".bin.mptmp") == 0 || strcmp(rest, ".bin.mpbak") == 0) return v;
    return -1;
}

int listIndices(const char* dir, int* out, int max) {
    int count = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    while (struct dirent* e = readdir(d)) {
        const int v = recoveryIndexOf(e->d_name);
        if (v < 0) continue;
        bool dup = false;
        for (int i = 0; i < count; i++) dup |= out[i] == v;
        if (!dup && count < max) out[count++] = v;
    }
    closedir(d);
    qsort(out, (size_t)count, sizeof(int), [](const void* a, const void* b) { return *(const int*)a - *(const int*)b; });
    return count;
}

void removeSession(const char* dir, int index) {
    char p[600], q[620];
    mp_save_recovery_path(dir, index, p, sizeof p);
    unlink(p);
    snprintf(q, sizeof q, "%s.mptmp", p);
    unlink(q);
    snprintf(q, sizeof q, "%s.mpbak", p);
    unlink(q);
}

bool pruneDir(const char* dir, int keep, bool aside = false) {
    // savesus S8: aside = the live recovery folder: a checkpoint is a session nobody has answered about (a crash), so the keep-N bound moves the
    // surplus oldest into discarded/ (where a declined one goes, itself bounded at 10 by the deleting prune) and never unlinks it.
    // savefix (keepsafe S1): only recovery files of this build's format are counted and pruned; a file it does not understand (a
    // newer format, a damaged one) is left where it is, never deleted by a prune
    int idx[256], ours[256], n2 = 0;
    const int n = listIndices(dir, idx, 256);
    uint8_t* buf = (uint8_t*)malloc(MP_SAVE_REC_MAX + 16);
    if (!buf) return false;
    for (int i = 0; i < n; i++) {
        char p[600];
        mp_save_recovery_path(dir, idx[i], p, sizeof p);
        uint32_t len = 0;
        if (mp_save_read_file(p, buf, MP_SAVE_REC_MAX + 16, &len) == 0 && mp_save_is_recovery(buf, len)) ours[n2++] = idx[i];
    }
    free(buf);
    for (int i = 0; i + keep < n2; i++) {
        if (!aside) removeSession(dir, ours[i]);
        else mp_save_recovery_discard(dir, ours[i], 10);   // (could not be set aside: it stays; over its bound by one until the next prune)
    }
    return true;
}
}  // namespace

int mp_save_recovery_list(const char* dir, int* indices, int max) { return listIndices(dir, indices, max); }

int mp_save_recovery_next_index(const char* dir) {
    int idx[256], best = 0;
    int n = listIndices(dir, idx, 256);
    if (n > 0) best = idx[n - 1];
    char sub[600];
    snprintf(sub, sizeof sub, "%s/discarded", dir);
    n = listIndices(sub, idx, 256);
    if (n > 0 && idx[n - 1] > best) best = idx[n - 1];
    // keepfix (keepsafe review 2 #3): never a number in use, never a wrap under live files (the keep-N prune deletes the lowest numbers
    // first, so a wrapped "newest" checkpoint would be the first to go and could overwrite an earlier session's). At the end of the range
    // no new checkpoint is made (-1: mp_save_recovery_write refuses it); only a crafted file can get there.
    return best >= 99999999 ? -1 : best + 1;
}

bool mp_save_recovery_write(const char* dir, int index, const uint8_t* data, uint32_t len, int keep) {
    if (index < 0 || index > 99999999 || !mp_save_is_recovery(data, len) || !mp_save_mkdirs(dir)) return false;
    char p[600];
    mp_save_recovery_path(dir, index, p, sizeof p);
    if (!mp_save_write_atomic(p, data, len)) return false;
    if (keep > 0) pruneDir(dir, keep, true);   // (savesus S8: surplus is set aside in discarded/, not deleted)
    return true;
}

bool mp_save_recovery_read(const char* dir, int index, uint8_t* buf, uint32_t cap, uint32_t* len) {
    char p[600];
    mp_save_recovery_path(dir, index, p, sizeof p);
    bool changed = false;
    mp_save_recover_with(p, mp_save_is_recovery, &changed);
    *len = 0;
    return mp_save_read_file(p, buf, cap, len) == 0 && mp_save_is_recovery(buf, *len);
}

bool mp_save_recovery_discard(const char* dir, int index, int keepDiscarded) {
    char p[600], sub[600], q[620];
    mp_save_recovery_path(dir, index, p, sizeof p);
    snprintf(sub, sizeof sub, "%s/discarded", dir);
    if (!mp_save_mkdirs(sub)) return false;
    mp_save_recovery_path(sub, index, q, sizeof q);
    bool changed = false;
    mp_save_recover_with(p, mp_save_is_recovery, &changed);
    unlink(q);
    const bool moved = !exists(p) || rename(p, q) == 0;
    removeSession(dir, index);  // leftovers (.mptmp/.mpbak)
    if (keepDiscarded > 0) pruneDir(sub, keepDiscarded);
    return moved;
}

bool mp_save_recovery_delete(const char* dir, int index) {
    removeSession(dir, index);
    char p[600];
    mp_save_recovery_path(dir, index, p, sizeof p);
    return !exists(p);
}

// ---- background writer ---------------------------------------------------------------------------------------------------
namespace {
struct AsyncState {
    std::mutex m;
    std::condition_variable cv, done;
    bool started = false, pending = false, busy = false, haveResult = false;
    char dir[512];
    int index = 0, keep = 0;
    uint32_t len = 0;
    uint8_t data[MP_SAVE_REC_MAX];
    uint8_t work[MP_SAVE_REC_MAX];
    MpSaveAsyncResult result{};
};
// Never destroyed (the worker thread waits on it forever; destroying a waited-on condition variable at exit hangs), and placed
// in static storage so no heap is involved whichever thread gets here first.
AsyncState& async() {
    alignas(AsyncState) static unsigned char storage[sizeof(AsyncState)];
    static AsyncState* s = new (storage) AsyncState();
    return *s;
}
int64_t nowMicros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
std::atomic<bool> sHangWriter{false};   // test hook (mp_save_test_hang_writer)
int sAsyncTimeoutMs = 0;                // test hook (mp_save_test_async_timeout_ms)
void asyncWorker() {
    port_thread_class(4);  // background work (PS Vita; nothing elsewhere)
    AsyncState& s = async();
    char dir[512];
    for (;;) {
        int index, keep;
        uint32_t len;
        {
            std::unique_lock<std::mutex> lk(s.m);
            s.cv.wait(lk, [&] { return s.pending; });
            s.pending = false;
            s.busy = true;
            memcpy(s.work, s.data, s.len);
            memcpy(dir, s.dir, sizeof dir);
            index = s.index;
            keep = s.keep;
            len = s.len;
        }
        while (sHangWriter.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int64_t t0 = nowMicros();
        const bool ok = mp_save_recovery_write(dir, index, s.work, len, keep);
        const int64_t dt = nowMicros() - t0;
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.result = MpSaveAsyncResult{ok, index, len, (uint32_t)(dt < 0 ? 0 : dt)};
            s.haveResult = true;
            s.busy = false;
        }
        s.done.notify_all();
    }
}
}  // namespace

bool mp_save_async_submit(const char* dir, int index, const uint8_t* data, uint32_t len, int keep) {
    if (!dir || !data || len == 0 || len > MP_SAVE_REC_MAX || strlen(dir) >= sizeof(AsyncState::dir)) return false;
    AsyncState& s = async();
    {
        std::lock_guard<std::mutex> lk(s.m);
        memcpy(s.data, data, len);
        s.len = len;
        s.index = index;
        s.keep = keep;
        snprintf(s.dir, sizeof s.dir, "%s", dir);
        s.pending = true;
        if (!s.started) {
            s.started = true;
            std::thread(asyncWorker).detach();
        }
    }
    s.cv.notify_one();
    return true;
}

bool mp_save_async_idle(void) {
    AsyncState& s = async();
    std::lock_guard<std::mutex> lk(s.m);
    return !s.pending && !s.busy;
}

bool mp_save_async_wait_ms(int ms) {
    AsyncState& s = async();
    std::unique_lock<std::mutex> lk(s.m);
    return s.done.wait_for(lk, std::chrono::milliseconds(ms < 0 ? 0 : ms), [&] { return !s.pending && !s.busy; });
}

bool mp_save_async_wait(void) { return mp_save_async_wait_ms(sAsyncTimeoutMs > 0 ? sAsyncTimeoutMs : MP_SAVE_ASYNC_WAIT_MS); }

void mp_save_test_hang_writer(int on) { sHangWriter = on != 0; }
void mp_save_test_async_timeout_ms(int ms) { sAsyncTimeoutMs = ms; }

bool mp_save_async_take_result(MpSaveAsyncResult* out) {
    AsyncState& s = async();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.haveResult) return false;
    s.haveResult = false;
    if (out) *out = s.result;
    return true;
}

void mp_save_test_crash_after(int step) { sCrashAfter = step; }
void mp_save_test_cut_write(int bytes) { sCutWrite = bytes; }
