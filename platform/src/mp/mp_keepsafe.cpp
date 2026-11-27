// Galaxy 3.0 multiplayer (keepsafe): copies of the multiplayer data and bringing items back from them. See port/mp_keepsafe.h and
// docs/MP-DATA-FORMATS.md. keepfix 3: one file per change, each by its own writer; copies are only ever NEW files.
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));

#include "port/mp_keepsafe.h"

#include "mp_config.h"
#include "mp_savecheck.h"
#include "mp_savefile.h"

#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#if !defined(__vita__)
#include <sys/statvfs.h>
#else
#include <psp2/appmgr.h>
extern "C" int sceIoSync(const char* device, unsigned int flags);
#endif

extern "C" void mp_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" void mp_service_shutdown(void) __attribute__((weak));
extern "C" const char* mp_service_online_dir(void) __attribute__((weak));
const char* mp_config_dir(void) __attribute__((weak));
const MpConfig* mp_config(void) __attribute__((weak));
void mp_config_hold(bool hold) __attribute__((weak));
bool mp_config_replace_held(const char* text, size_t len) __attribute__((weak));
extern "C" bool mp_ui_flow_replace_file(const char* name, const char* text) __attribute__((weak));

namespace {

typedef std::vector<uint8_t> Bytes;

// ---- state ---------------------------------------------------------------------------------------------------------------------
struct State {
    MpKsPaths paths;
    bool pathsSet = false;
    uint32_t dataHash = 0;
    char version[24] = {0}, build[33] = {0};
    bool mayMigrate = true;
    int notice = MP_KS_NOTICE_NONE;
    int64_t fakeTime = 0, fakeFree = -1;
    int reads = 0, stats = 0;
    uint32_t seqHint = 0;
    void (*readHook)(const char*) = nullptr;   // tests: told of every file read
    void (*hooks[8])(void) = {};
    int nHooks = 0;
    bool (*guard)(void) = nullptr;
};
State& S() {
    static State s;
    return s;
}
std::atomic<int> gActive{0};
std::atomic<bool> gOp{false}, gAcct{false};
struct Busy {   // one keepsafe call at a time (mp_keepsafe_busy), from any thread
    bool ok;
    Busy() {
        bool f = false;
        ok = gOp.compare_exchange_strong(f, true);
        gActive.fetch_add(1);
        if (port_host_alloc_scope_enter) port_host_alloc_scope_enter();
    }
    ~Busy() {
        if (port_host_alloc_scope_leave) port_host_alloc_scope_leave();
        gActive.fetch_sub(1);
        if (ok) gOp.store(false);
    }
};
struct AcctLock {   // the account files: SaveAccount may run on the network helper thread
    AcctLock() {
        bool f = false;
        while (!gAcct.compare_exchange_weak(f, true)) { f = false; usleep(500); }
    }
    ~AcctLock() { gAcct.store(false); }
};

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {   // (never a token, never an account file's name)
    if (!mp_log) return;
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    mp_log("keepsafe: %s", b);
}

const MpKsPaths& P() {
    State& s = S();
    if (!s.pathsSet) {
        mp_keepsafe_default_paths(&s.paths);
        s.pathsSet = true;
    }
    return s.paths;
}
std::string Parent(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
}
std::string CopiesDir() { return std::string(P().dataDir) + "/mp-backups"; }
std::string StampPath() { return std::string(P().dataDir) + "/mp-keepsafe.stamp"; }
std::string OwnPath() { return std::string(P().onlineDir) + "/keepsafe-own.txt"; }   // credentials this console itself held

// ---- files ---------------------------------------------------------------------------------------------------------------------
int64_t Now() { return S().fakeTime ? S().fakeTime : (int64_t)time(nullptr); }
int64_t FreeBytes() {
    if (S().fakeFree >= 0) return S().fakeFree;
#if defined(__vita__)
    uint64_t maxSize = 0, freeSize = 0;
    return sceAppMgrGetDevInfo("ux0:", &maxSize, &freeSize) < 0 ? -1 : (int64_t)freeSize;
#else
    struct statvfs v;
    return statvfs(P().dataDir, &v) != 0 ? -1 : (int64_t)v.f_bavail * (int64_t)v.f_frsize;
#endif
}
int Read(const std::string& path, Bytes* out) {   // 0 or -errno; a 4 KiB buffer first (the stamp at every start)
    S().reads++;
    if (S().readHook) S().readHook(path.c_str());
    uint32_t cap = 4096, n = 0;
    out->assign(cap, 0);
    int rc = mp_save_read_file(path.c_str(), out->data(), cap, &n);
    for (uint32_t bigger : {MP_SAVE_MAX_FILE + 16u, (uint32_t)MP_SAVE_ATOMIC_MAX}) {   // (a Team File is under 64 KiB: the 1 MiB buffer is for the rest)
        if (rc != -EFBIG) break;
        cap = bigger;
        out->assign(cap, 0);
        rc = mp_save_read_file(path.c_str(), out->data(), cap, &n);
    }
    if (rc != 0) out->clear();
    else out->resize(n);
    return rc;
}
bool Exists(const std::string& p) {
    S().stats++;
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
bool IsDir(const std::string& p) {
    S().stats++;
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool ListDir(const std::string& dir, std::vector<std::string>* names) {
    names->clear();
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (struct dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names->push_back(e->d_name);
    closedir(d);
    std::sort(names->begin(), names->end());
    return true;
}
bool SameAs(const std::string& path, const Bytes& b) {
    Bytes c;
    return Read(path, &c) == 0 && c == b;
}
void SyncDevice() {
#if defined(__vita__)
    const char* d = P().dataDir;
    const char* colon = strchr(d, ':');
    if (!colon || colon - d > 15) return;
    char dev[20];
    memcpy(dev, d, (size_t)(colon - d) + 1);
    dev[colon - d + 1] = 0;
    sceIoSync(dev, 0);
#endif
}
// keepfix 5: why the last WriteNew failed (errno of the call): "the card is full" is said only for a real ENOSPC.
int gErr = 0;
int IoResult() { return gErr == ENOSPC ? MP_KS_NO_SPACE : MP_KS_IO; }
// A NEW file (never an existing name: O_EXCL), written, fsynced, read back. On failure only a file this call created is removed.
bool WriteNew(const std::string& path, const Bytes& b) {
    gErr = 0;
    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0660);
    if (fd < 0) {
        gErr = errno;
        return false;
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < b.size()) {
        const ssize_t w = write(fd, b.data() + done, b.size() - done);
        if (w <= 0) {
            ok = false;
            gErr = w < 0 ? errno : ENOSPC;
        } else {
            done += (size_t)w;
        }
    }
    if (fsync(fd) != 0 && ok) {
        ok = false;
        gErr = errno;
    }
    if (close(fd) != 0 && ok) {
        ok = false;
        gErr = errno;
    }
    if (ok && !SameAs(path, b)) {
        ok = false;
        gErr = EIO;
    }
    if (!ok) unlink(path.c_str());   // (ours: O_EXCL created it)
    return ok;
}
const char kUnfinished[] = ".unfinished";
// Under the copies folder only (never live data): a folder this module made, and everything in it. The mark goes last and only when everything else
// is gone (keepfix 5: with "disk full" on an unlink, a folder with files left must keep its mark: never an unmarked half folder).
bool RemoveCopyTree(const std::string& dir) {
    std::vector<std::string> names;
    if (dir.compare(0, CopiesDir().size() + 1, CopiesDir() + "/") != 0 || !ListDir(dir, &names)) return false;
    bool marked = false, gone = true;
    for (const std::string& n : names) {
        const std::string p = dir + "/" + n;
        if (n == kUnfinished) marked = true;   // (the mark goes last: a cut leaves a marked folder, which the next prune finishes removing)
        else if (IsDir(p)) gone = RemoveCopyTree(p) && gone;
        else gone = unlink(p.c_str()) == 0 && gone;
    }
    if (!gone) return false;
    if (marked && unlink((dir + "/" + kUnfinished).c_str()) != 0) return false;
    return rmdir(dir.c_str()) == 0;
}

// ---- text, accounts, fingerprints ------------------------------------------------------------------------------------------------
int TextVersion(const Bytes& b) {   // "version = N" of a text file; absent: 1
    size_t s = 0;
    while (s < b.size()) {
        size_t e = s;
        while (e < b.size() && b[e] != '\n') e++;
        std::string l((const char*)b.data() + s, e - s);
        s = e + 1;
        size_t k = 0;
        while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
        if (l.compare(k, 7, "version") != 0) continue;
        k += 7;
        while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
        if (k < l.size() && l[k] == '=') return atoi(l.c_str() + k + 1);
    }
    return 1;
}
uint32_t G32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t G16(const uint8_t* p) { return p[0] | (p[1] << 8); }
void P32(Bytes* b, uint32_t v) { for (int i = 0; i < 4; i++) b->push_back((uint8_t)(v >> (8 * i))); }
void P16(Bytes* b, uint32_t v) { b->push_back((uint8_t)v); b->push_back((uint8_t)(v >> 8)); }
// online.dat (mp_service.cpp): "SMGO" | version u32 | pid u32 | token[64] | name[20] | code[12] | crc32. 112 bytes.
bool AccountOk(const Bytes& b) { return b.size() == 112 && !memcmp(b.data(), "SMGO", 4) && G32(&b[4]) == 1 && G32(&b[108]) == mp_save_crc32(b.data(), 108); }
bool AccountNewer(const Bytes& b) { return b.size() >= 8 && !memcmp(b.data(), "SMGO", 4) && G32(&b[4]) > 1; }
// FNV-1a 64 folded to 32 bits (a CRC of a file that ends with its own CRC is one constant per length: it cannot tell two accounts apart)
uint32_t Fp(const uint8_t* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32));
}
uint32_t Credential(const Bytes& b) { return b.size() >= 76 ? Fp(b.data() + 8, 68) : Fp(b.data(), b.size()); }   // player id + token
void DateOf(int64_t t, char out[16]) {
    time_t tt = (time_t)t;
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    gmtime_r(&tt, &tmv);
    snprintf(out, 16, "%04u%02u%02u", (unsigned)(tmv.tm_year + 1900) % 10000u, (unsigned)(tmv.tm_mon + 1) % 100u, (unsigned)tmv.tm_mday % 100u);
}
bool PlainChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }
bool NameOk(const std::string& n) {
    if (n.empty() || n.size() > 90 || n[0] == '.') return false;
    for (char c : n)
        if (!PlainChar(c)) return false;
    return true;
}
bool RelOk(const std::string& rel) {
    if (rel.size() < 4 || rel.size() > 200 || (rel.compare(0, 3, "mp/") != 0 && rel.compare(0, 7, "online/") != 0)) return false;
    size_t part = 0;
    for (size_t i = 0; i <= rel.size(); i++) {
        if (i == rel.size() || rel[i] == '/') {
            const std::string p = rel.substr(part, i - part);
            if (p.empty() || p == "." || p == "..") return false;
            part = i + 1;
        } else if (!PlainChar(rel[i])) {
            return false;
        }
    }
    return true;
}
std::string Abs(const std::string& rel) {
    if (rel.compare(0, 3, "mp/") == 0) return std::string(P().mpDir) + "/" + rel.substr(3);
    if (rel.compare(0, 7, "online/") == 0) return std::string(P().onlineDir) + "/" + rel.substr(7);
    return std::string();
}
std::string TeamRel() {
    const std::string tf = P().teamFile, mp = std::string(P().mpDir) + "/";
    return tf.compare(0, mp.size(), mp) == 0 ? "mp/" + tf.substr(mp.size()) : "mp/saves/title/00010000/524d4750/data/GameData.bin";
}
bool IsAccountName(const std::string& n) {   // online.dat, online_<8 hex>.dat
    if (n == "online.dat") return true;
    if (n.size() != 19 || n.compare(0, 7, "online_") != 0 || n.compare(15, 4, ".dat") != 0) return false;   // (keepfix 5: 7 + 8 hex + 4 = 19)
    for (size_t i = 7; i < 15; i++)
        if (!isxdigit((unsigned char)n[i])) return false;
    return true;
}

// ---- stamp ---------------------------------------------------------------------------------------------------------------------
// "keepsafe = 1" / version / build / seq / warned / unknown lines (kept) / crc = <crc-32 of the lines above>
struct Stamp {
    bool valid = false, newer = false, unreadable = false, present = false;
    std::string version, build, warned, extra;
    uint32_t seq = 0;
};
bool ParseStamp(const Bytes& b, Stamp* s) {
    *s = Stamp();
    s->present = true;
    const std::string t((const char*)b.data(), b.size());
    const std::string first = t.substr(0, t.find('\n'));
    if (first.compare(0, 11, "keepsafe = ") != 0) return false;
    if (atoi(first.c_str() + 11) > 1) {
        s->newer = true;
        return false;
    }
    const size_t c = t.rfind("crc = ");
    if (c == std::string::npos || (c && t[c - 1] != '\n') || strtoul(t.c_str() + c + 6, nullptr, 16) != mp_save_crc32(t.data(), c)) return false;
    size_t p = first.size() + 1;
    while (p < c) {
        const size_t nl = t.find('\n', p);
        const std::string l = t.substr(p, nl - p);
        p = nl + 1;
        if (l.compare(0, 10, "version = ") == 0) s->version = l.substr(10);
        else if (l.compare(0, 8, "build = ") == 0) s->build = l.substr(8);
        else if (l.compare(0, 6, "seq = ") == 0) s->seq = (uint32_t)strtoul(l.c_str() + 6, nullptr, 10);
        else if (l.compare(0, 9, "warned = ") == 0) s->warned = l.substr(9);
        else s->extra += l + "\n";   // (unknown lines, also an earlier design's pending/declined: kept as text, never read)
    }
    s->valid = true;
    return true;
}
void ReadStamp(Stamp* s) {
    Bytes b;
    const int rc = Read(StampPath(), &b);
    if (rc == 0 && (ParseStamp(b, s) || s->newer)) return;
    for (const char* a : {".mptmp", ".mpbak"}) {   // the atomic writer's own copies, read as they are
        Stamp t;
        if (Read(StampPath() + a, &b) == 0 && ParseStamp(b, &t)) {
            *s = t;
            return;
        }
    }
    *s = Stamp();
    s->present = rc != -ENOENT;
    s->unreadable = rc != 0 && rc != -ENOENT;   // there, but not readable now: nothing is decided
}
bool WriteStamp(const Stamp& s) {
    std::string t = "keepsafe = 1\nversion = " + s.version + "\n";
    if (!s.build.empty()) t += "build = " + s.build + "\n";
    t += Fmt("seq = %u\n", (unsigned)s.seq);
    if (!s.warned.empty()) t += "warned = " + s.warned + "\n";
    t += s.extra;
    t += Fmt("crc = %08x\n", (unsigned)mp_save_crc32(t.data(), t.size()));
    return mp_save_write_atomic(StampPath().c_str(), (const uint8_t*)t.data(), (uint32_t)t.size());
}

// ---- version copies (folders) -------------------------------------------------------------------------------------------------
// MANIFEST.ksm: "SMKM" fmt u16 (2; 1 read) u16 0 class u8 reason u8 u16 0 seq u32 created s64 version[24] (fmt 2: from[24]) count u32,
// per file (u16 name length, name, u32 size, u32 fingerprint; fmt 1: CRC-32), CRC-32 of the manifest. `from` pins a version change.
struct SnapFile {
    std::string rel;
    uint32_t size = 0, sum = 0;
};
struct Snap {
    std::string name, dir, version, from;
    int fmt = 2;
    uint32_t seq = 0, bytes = 0;
    int64_t created = 0;
    std::vector<SnapFile> files;
};
const size_t kBeforeKeep = 16;   // "before bringing back" copies always kept (the newest); older ones only while they hold something nothing else does
const size_t kBeforeCap = 64;    // keepfix 5: never more than this many: at the cap a new bring-back is refused (MP_KS_TOO_MANY), nothing unique is deleted
uint32_t SumOf(const Snap& s, const Bytes& b) { return s.fmt == 1 ? mp_save_crc32(b.data(), b.size()) : Fp(b.data(), b.size()); }
Bytes BuildManifest(const Snap& s) {
    Bytes o = {'S', 'M', 'K', 'M'};
    P16(&o, 2);
    P16(&o, 0);
    P16(&o, 0);   // class 0, reason 0
    P16(&o, 0);
    P32(&o, s.seq);
    P32(&o, (uint32_t)(uint64_t)s.created);
    P32(&o, (uint32_t)((uint64_t)s.created >> 32));
    char v[24] = {0}, f[24] = {0};
    snprintf(v, sizeof v, "%s", s.version.c_str());
    snprintf(f, sizeof f, "%s", s.from.c_str());
    o.insert(o.end(), v, v + 24);
    o.insert(o.end(), f, f + 24);
    P32(&o, (uint32_t)s.files.size());
    for (const SnapFile& x : s.files) {
        P16(&o, (uint32_t)x.rel.size());
        o.insert(o.end(), x.rel.begin(), x.rel.end());
        P32(&o, x.size);
        P32(&o, x.sum);
    }
    P32(&o, mp_save_crc32(o.data(), o.size()));
    return o;
}
bool ParseManifest(const Bytes& m, Snap* s) {
    const size_t len = m.size();
    if (len < 56 || memcmp(m.data(), "SMKM", 4) != 0) return false;
    s->fmt = (int)G16(&m[4]);
    if ((s->fmt != 1 && s->fmt != 2) || G32(&m[len - 4]) != mp_save_crc32(m.data(), len - 4)) return false;   // (a newer one: not ours)
    s->seq = G32(&m[12]);
    s->created = (int64_t)(((uint64_t)G32(&m[20]) << 32) | G32(&m[16]));
    s->version.assign((const char*)&m[24], strnlen((const char*)&m[24], 23));
    size_t p = 48;
    if (s->fmt == 2) {
        if (len < 80) return false;
        s->from.assign((const char*)&m[48], strnlen((const char*)&m[48], 23));
        p = 72;
    }
    const uint32_t n = G32(&m[p]);
    p += 4;
    if (m[8] != 0 || n > 4000) return false;   // (class 0 only: the earlier design's undo snapshots, class 1, are left alone: never read)
    for (uint32_t i = 0; i < n; i++) {
        if (len - 4 < p + 2) return false;
        const uint32_t nl = G16(&m[p]);
        p += 2;
        if (nl == 0 || nl > 200 || len - 4 < p + nl + 8) return false;
        SnapFile f;
        f.rel.assign((const char*)&m[p], nl);
        p += nl;
        f.size = G32(&m[p]);
        f.sum = G32(&m[p + 4]);
        p += 8;
        if (!RelOk(f.rel) || f.size > MP_SAVE_ATOMIC_MAX) return false;   // (keepfix 5: what Read can read; 64 KiB failed a copy of a whitelisted file above it at every start)
        s->bytes += f.size;
        s->files.push_back(f);
    }
    return p == len - 4;
}
enum SnapState { kGood, kDamaged, kUnparseable, kMarkedUnfinished };
SnapState Classify(const std::string& name, Snap* s) {
    *s = Snap();
    s->name = name;
    s->dir = CopiesDir() + "/" + name;
    Bytes m;
    const int rc = Read(s->dir + "/MANIFEST.ksm", &m);
    if (rc == -ENOENT) return Exists(s->dir + "/" + kUnfinished) ? kMarkedUnfinished : kUnparseable;   // (no mark: not known to be ours: kept)
    if (rc != 0 || !ParseManifest(m, s)) return kUnparseable;
    for (const SnapFile& f : s->files) {
        Bytes b;
        if (Read(s->dir + "/" + f.rel, &b) != 0 || b.size() != f.size || SumOf(*s, b) != f.sum) return kDamaged;
    }
    return kGood;
}
// The multiplayer data a version copy holds: mp/ without mp/backups (the single-player safety copies), without mp/recovery (the recovery
// checkpoints: never in a copy), without the writers' .tmp temporaries and the earlier design's .kstmp / .ksold leftovers (never read), and the
// account folder's own online* files. keepfix 5: only files whose NAMES this game's own writers use (KnownFile); anything else is skipped and the
// log says so (a stray file of any size must not make the copy fail at every start).
bool Skipped(const std::string& n) {
    const size_t k = n.size();
    if (k > 6 && (n.compare(k - 6, 6, ".kstmp") == 0 || n.compare(k - 6, 6, ".ksold") == 0)) return true;
    return k > 4 && n.compare(k - 4, 4, ".tmp") == 0;   // (a writer's temporary: never a file of the player's)
}
bool KnownFile(const std::string& n, bool online) {
    static const char* const sfx[] = {".prev.1", ".prev.2", ".prev", ".mpbad.1", ".mpbad.2", ".mpbad", ".mptmp", ".mpbak", ".bak"};   // (the writers' own side files)
    std::string b = n;
    for (int round = 0; round < 3; round++) {
        bool cut = false;
        for (const char* x : sfx) {
            const size_t l = strlen(x);
            if (b.size() > l && b.compare(b.size() - l, l, x) == 0) {
                b.resize(b.size() - l);
                cut = true;
                break;
            }
        }
        if (!cut) break;
    }
    if (online) return IsAccountName(b);
    return b == "mp.cfg" || b == "flow.cfg" || b == "coop.cfg" || b == "conntest.txt" || b == "report.txt" || b == "GameData.bin";
}
struct Listing {
    std::vector<std::string> rels;
    int skipped = 0;
    bool ok = true;   // false: a folder that is there could not be listed (the data set is incomplete)
};
void Walk(const std::string& abs, const std::string& rel, Listing* out, int depth) {
    std::vector<std::string> names;
    if (!ListDir(abs, &names)) {
        if (IsDir(abs)) out->ok = false;   // (a folder that is not there has no files; one that is there and cannot be listed has an unknown set)
        return;
    }
    for (const std::string& n : names) {
        const std::string r = rel + "/" + n;
        if (r == "mp/backups" || r == "mp/recovery" || Skipped(n)) continue;
        if (IsDir(abs + "/" + n)) {
            if (depth < 8) Walk(abs + "/" + n, r, out, depth + 1);
        } else if (!KnownFile(n, false)) {
            out->skipped++;
            Log("not copied (not one of the game's files): %s", r.c_str());
        } else {
            out->rels.push_back(r);
        }
    }
}
Listing DataSet() {
    Listing out;
    Walk(P().mpDir, "mp", &out, 0);
    std::vector<std::string> names;
    if (ListDir(P().onlineDir, &names)) {
        int unknown = 0;
        for (const std::string& n : names) {
            if (n.compare(0, 6, "online") != 0 || Skipped(n) || IsDir(std::string(P().onlineDir) + "/" + n)) continue;
            if (KnownFile(n, true)) out.rels.push_back("online/" + n);
            else unknown++;
        }
        out.skipped += unknown;
        if (unknown) Log("not copied (not one of the game's files): %d file(s) in the account folder", unknown);   // (names of account files are never logged)
    } else if (IsDir(P().onlineDir)) {
        out.ok = false;
    }
    std::sort(out.rels.begin(), out.rels.end());
    return out;
}
// A name this module made: <prefix>-<YYYYMMDD>-<n> (a folder; prefix = the version) or before|copy-<YYYYMMDD>-<n>.smgmp (a file), n of 3 to 9
// digits. Its number in *seq. (keepfix 5: nothing else orders copies or counts toward the next number: a stray name cannot scramble either.)
bool OwnName(const std::string& n, uint32_t* seq) {
    const bool file = n.size() > 6 && n.compare(n.size() - 6, 6, ".smgmp") == 0;
    const std::string b = file ? n.substr(0, n.size() - 6) : n;
    const size_t d2 = b.find_last_of('-');
    if (d2 == std::string::npos || d2 == 0) return false;
    const size_t d1 = d2 == 0 ? std::string::npos : b.find_last_of('-', d2 - 1);
    if (d1 == std::string::npos || d1 == 0 || d2 - d1 != 9) return false;
    for (size_t i = d1 + 1; i < d2; i++)
        if (!isdigit((unsigned char)b[i])) return false;
    const size_t digits = b.size() - d2 - 1;
    if (digits < 3 || digits > 9) return false;
    uint32_t v = 0;
    for (size_t i = d2 + 1; i < b.size(); i++) {
        if (!isdigit((unsigned char)b[i])) return false;
        v = v * 10 + (uint32_t)(b[i] - '0');
    }
    const std::string prefix = b.substr(0, d1);
    if (file ? (prefix != "before" && prefix != "copy") : !NameOk(prefix)) return false;
    if (seq) *seq = v;
    return true;
}
uint32_t SeqOf(const std::string& n) {
    uint32_t v = 0;
    return OwnName(n, &v) ? v : 0;
}
uint32_t NextSeq() {
    uint32_t best = std::min<uint32_t>(S().seqHint, 999999000u);
    std::vector<std::string> names;
    ListDir(CopiesDir(), &names);
    for (const std::string& n : names) {
        uint32_t v = 0;
        if (OwnName(n, &v)) best = std::max(best, v);
    }
    return best + 1;
}
size_t Prune(const std::string& keep);

// A NEW folder <version>-<date>-<n>: the mark, every file (new, fsynced, read back), one device sync, the manifest, a full read-back, the
// mark removed. A name that exists is never used (mkdir must make it); a failure removes only what this call made.
int MakeVersionCopy(const char* from, std::string* nameOut) {
    nameOut->clear();
    const Listing set = DataSet();
    if (!set.ok) {   // a folder that is there could not be listed: a copy of what was seen would lack files and still look complete
        Log("no copy: a folder could not be listed");
        return MP_KS_IO;
    }
    const std::vector<std::string>& rels = set.rels;
    if (rels.empty()) return MP_KS_NOTHING;
    uint64_t total = 0;
    std::vector<Bytes> data(rels.size());
    for (size_t i = 0; i < rels.size(); i++) {
        const int rc = Read(Abs(rels[i]), &data[i]);
        if (rc == -EFBIG) return MP_KS_TOO_BIG;
        if (rc != 0) return MP_KS_IO;
        total += data[i].size();
    }
    const int64_t freeB = FreeBytes();
    if (freeB >= 0 && (uint64_t)freeB < total + 64 * 1024 + rels.size() * 8192) return MP_KS_NO_SPACE;
    mp_save_mkdirs(CopiesDir().c_str());
    Snap s;
    s.created = Now();
    s.version = S().version;
    s.from = from;
    char date[16];
    DateOf(s.created, date);
    std::string v;
    for (char c : s.version) v.push_back(isalnum((unsigned char)c) || c == '.' || c == '_' ? c : '_');
    for (int tries = 0; tries < 100 && s.name.empty(); tries++) {
        s.seq = NextSeq() + (uint32_t)tries;
        const std::string n = Fmt("%s-%s-%03u", v.empty() ? "x" : v.c_str(), date, (unsigned)s.seq);
        if (mkdir((CopiesDir() + "/" + n).c_str(), 0770) == 0) s.name = n;
        else if (errno != EEXIST) return errno == ENOSPC ? MP_KS_NO_SPACE : MP_KS_IO;
    }
    if (s.name.empty()) return MP_KS_IO;
    s.dir = CopiesDir() + "/" + s.name;
    bool ok = WriteNew(s.dir + "/" + kUnfinished, Bytes());
    for (size_t i = 0; ok && i < rels.size(); i++) {
        mp_save_mkdirs(Parent(s.dir + "/" + rels[i]).c_str());
        ok = WriteNew(s.dir + "/" + rels[i], data[i]);
        SnapFile f;
        f.rel = rels[i];
        f.size = (uint32_t)data[i].size();
        f.sum = Fp(data[i].data(), data[i].size());
        s.files.push_back(f);
        s.bytes += f.size;
    }
    if (ok) {
        SyncDevice();
        ok = WriteNew(s.dir + "/MANIFEST.ksm", BuildManifest(s));
    }
    Snap check;
    ok = ok && Classify(s.name, &check) == kGood && unlink((s.dir + "/" + kUnfinished).c_str()) == 0;
    if (!ok) {
        const int why = IoResult();   // (before the cleanup)
        RemoveCopyTree(s.dir);   // (this call's own new folder)
        return why;
    }
    S().seqHint = s.seq;
    *nameOut = s.name;
    Prune(s.name);
    Log("copy %s: %d files, %u bytes", s.name.c_str(), (int)s.files.size(), (unsigned)s.bytes);
    return MP_KS_OK;
}

// ---- .smgmp copies (files) ------------------------------------------------------------------------------------------------------
// "SMGMPEXP" | u16 format (1) | u16 header (64) | u32 entries | s64 created | u32 data hash | u32 flags (0) | char[32] build | entries:
// u16 name length, u16 0, u32 length, u32 CRC-32, name, data | u32 CRC-32 of everything before.
struct Item {   // canonical names: mp/saves/GameData.bin, mp/mp.cfg, mp/flow.cfg, mp/coop.cfg, online/online*.dat
    std::string rel;
    Bytes data;
};
struct Parsed {
    int64_t created = 0;
    uint32_t dataHash = 0;
    std::vector<Item> items;
};
Bytes BuildFile(const std::vector<Item>& items) {
    Bytes b = {'S', 'M', 'G', 'M', 'P', 'E', 'X', 'P'};
    P16(&b, 1);
    P16(&b, 64);
    P32(&b, (uint32_t)items.size());
    const int64_t t = Now();
    P32(&b, (uint32_t)(uint64_t)t);
    P32(&b, (uint32_t)((uint64_t)t >> 32));
    P32(&b, S().dataHash);
    P32(&b, 0);
    char bb[32] = {0};
    snprintf(bb, sizeof bb, "%s", S().version);
    b.insert(b.end(), bb, bb + 32);
    for (const Item& it : items) {
        P16(&b, (uint32_t)it.rel.size());
        P16(&b, 0);
        P32(&b, (uint32_t)it.data.size());
        P32(&b, mp_save_crc32(it.data.data(), it.data.size()));
        b.insert(b.end(), it.rel.begin(), it.rel.end());
        b.insert(b.end(), it.data.begin(), it.data.end());
    }
    P32(&b, mp_save_crc32(b.data(), b.size()));
    return b;
}
// 0, MP_KS_BAD_FILE or MP_KS_NEWER. Structure and every CRC; unknown names are skipped (a later format's entries).
int ParseFile(const Bytes& f, Parsed* out) {
    *out = Parsed();
    if (f.size() < 68 || memcmp(f.data(), "SMGMPEXP", 8) != 0 || G16(&f[8]) == 0) return MP_KS_BAD_FILE;
    if (G16(&f[8]) > 1) return MP_KS_NEWER;
    const uint32_t hdr = G16(&f[10]);
    if (hdr < 64 || hdr > f.size() - 4 || G32(&f[f.size() - 4]) != mp_save_crc32(f.data(), f.size() - 4)) return MP_KS_BAD_FILE;
    if (G32(&f[28]) != 0) return MP_KS_NEWER;
    const uint32_t count = G32(&f[12]);
    if (count > 64) return MP_KS_BAD_FILE;
    out->created = (int64_t)(((uint64_t)G32(&f[20]) << 32) | G32(&f[16]));
    out->dataHash = G32(&f[24]);
    size_t p = hdr;
    const size_t end = f.size() - 4;
    std::set<std::string> seen;
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 12) return MP_KS_BAD_FILE;
        const uint32_t nl = G16(&f[p]), res = G16(&f[p + 2]), dl = G32(&f[p + 4]), crc = G32(&f[p + 8]);
        p += 12;
        if (res != 0 || nl == 0 || nl > 120 || end - p < nl || end - p - nl < dl) return MP_KS_BAD_FILE;
        Item it;
        it.rel.assign((const char*)&f[p], nl);
        it.data.assign(f.begin() + (long)(p + nl), f.begin() + (long)(p + nl + dl));
        p += nl + dl;
        if (mp_save_crc32(it.data.data(), it.data.size()) != crc || !RelOk(it.rel) || !seen.insert(it.rel).second) return MP_KS_BAD_FILE;
        const bool known = it.rel == "mp/saves/GameData.bin" || it.rel == "mp/mp.cfg" || it.rel == "mp/flow.cfg" || it.rel == "mp/coop.cfg" ||
                           (it.rel.compare(0, 7, "online/") == 0 && IsAccountName(it.rel.substr(7)));
        if (known && it.data.empty()) return MP_KS_BAD_FILE;   // (a copy of ours never holds an empty one)
        if (known) out->items.push_back(it);
    }
    return p == end ? 0 : MP_KS_BAD_FILE;
}
// A NEW file <prefix>-<date>-<n>.smgmp in the copies folder: O_EXCL (an existing name is never used), read back, validated.
int WriteCopyFile(const char* prefix, const std::vector<Item>& items, std::string* nameOut) {
    nameOut->clear();
    const Bytes f = BuildFile(items);
    if (f.size() > MP_SAVE_ATOMIC_MAX) return MP_KS_TOO_BIG;
    const int64_t freeB = FreeBytes();
    if (freeB >= 0 && (uint64_t)freeB < f.size() + 64 * 1024) return MP_KS_NO_SPACE;
    mp_save_mkdirs(CopiesDir().c_str());
    char date[16];
    DateOf(Now(), date);
    for (int tries = 0; tries < 100; tries++) {
        const uint32_t seq = NextSeq() + (uint32_t)tries;
        const std::string n = Fmt("%s-%s-%03u.smgmp", prefix, date, (unsigned)seq);
        if (Exists(CopiesDir() + "/" + n)) continue;
        if (!WriteNew(CopiesDir() + "/" + n, f)) {
            if (gErr == EEXIST) continue;
            return IoResult();
        }
        Parsed p;
        if (ParseFile(f, &p) != 0) return MP_KS_VERIFY;
        S().seqHint = seq;
        *nameOut = n;
        return MP_KS_OK;
    }
    return MP_KS_IO;
}
int CopyKind(const std::string& n) { return n.compare(0, 7, "before-") == 0 ? MP_KS_COPY_BEFORE : MP_KS_COPY_MADE; }
bool IsCopyFile(const std::string& n) { return n.size() > 6 && n.compare(n.size() - 6, 6, ".smgmp") == 0 && NameOk(n); }
// ---- pruning -------------------------------------------------------------------------------------------------------------------
// Version copies: the 3 newest verified ones, and the first copy of each version change (from -> to: pinned). With more than 8 pinned, a pinned
// one beyond the newest 8 goes only when a newer verified copy holds the same files (names, sizes, fingerprints): otherwise it stays, so there
// can be more than 8 (keepfix 5). "before" copies: the newest 16 always; an older one goes only when every Team File slot in it that is not
// blank is also in this console's file or in a copy that stays (and at 64 a new bring-back is refused instead). Made copies: never. Nothing that
// does not parse is ever removed; a folder marked unfinished goes once a newer verified copy exists. Order everywhere: the creation time the
// copy records, ties by the number in the name this module gave it, then the name.
// Removal of a copy folder: the mark first, then the manifest, then the files (a cut leaves a marked, unfinished folder).
void RemoveSnapshot(const Snap& s) {
    if (!Exists(s.dir + "/" + kUnfinished) && !WriteNew(s.dir + "/" + kUnfinished, Bytes())) return;
    if (unlink((s.dir + "/MANIFEST.ksm").c_str()) != 0) return;
    RemoveCopyTree(s.dir);
}
bool SameFiles(const Snap& a, const Snap& b) {
    if (a.fmt != 2 || b.fmt != 2 || a.files.size() != b.files.size()) return false;
    for (size_t i = 0; i < a.files.size(); i++)
        if (a.files[i].rel != b.files[i].rel || a.files[i].sum != b.files[i].sum || a.files[i].size != b.files[i].size) return false;
    return true;
}
// ---- reading a copy -----------------------------------------------------------------------------------------------------------
struct Copy {
    int kind = 0;
    int64_t created = 0;
    uint32_t dataHash = 0;
    std::vector<Item> items;
};
const Item* Find(const Copy& c, const char* rel);
int TeamFileOk(const Bytes& t);
int LoadCopy(const std::string& name, Copy* c) {
    *c = Copy();
    if (!NameOk(name)) return MP_KS_ARGS;
    if (IsCopyFile(name)) {
        Bytes f;
        const int rc = Read(CopiesDir() + "/" + name, &f);
        if (rc == -ENOENT) return MP_KS_NOT_FOUND;
        if (rc != 0) return MP_KS_BAD_FILE;
        Parsed p;
        const int r = ParseFile(f, &p);
        if (r) return r;
        c->kind = CopyKind(name);
        c->created = p.created;
        c->dataHash = p.dataHash;
        for (Item& i : p.items)   // (settings come back only from this console's own copies)
            if (c->kind != MP_KS_COPY_MADE || i.rel.compare(0, 7, "online/") == 0 || i.rel == "mp/saves/GameData.bin") c->items.push_back(i);
        return 0;
    }
    Snap s;
    const SnapState st = Classify(name, &s);
    if (st == kUnparseable && !IsDir(CopiesDir() + "/" + name)) return MP_KS_NOT_FOUND;
    if (st != kGood) return MP_KS_BAD_FILE;
    c->kind = MP_KS_COPY_VERSION;
    c->created = s.created;
    const std::string team = TeamRel();
    for (const SnapFile& f : s.files) {
        std::string canon;
        if (f.rel == team) canon = "mp/saves/GameData.bin";
        else if (f.rel == "mp/mp.cfg" || f.rel == "mp/flow.cfg" || f.rel == "mp/coop.cfg") canon = f.rel;
        else if (f.rel.compare(0, 7, "online/") == 0 && IsAccountName(f.rel.substr(7))) canon = f.rel;
        else continue;
        Item it;
        it.rel = canon;
        if (Read(s.dir + "/" + f.rel, &it.data) != 0 || it.data.empty()) return MP_KS_BAD_FILE;
        c->items.push_back(it);
    }
    if (!Find(*c, "mp/saves/GameData.bin")) {   // keepfix 5: a copy made right after a cut in the old version's save: no primary, the writer's own files hold it
        for (const char* suffix : {".mptmp", ".mpbak"}) {   // (the valid temporary first, else the valid backup: as the save layer's recovery would pick; read only here)
            bool done = false;
            for (const SnapFile& f : s.files) {
                if (f.rel != team + suffix) continue;
                Item it;
                it.rel = "mp/saves/GameData.bin";
                if (Read(s.dir + "/" + f.rel, &it.data) == 0 && TeamFileOk(it.data) == 0) {
                    c->items.push_back(it);
                    done = true;
                }
                break;
            }
            if (done) break;
        }
    }
    return 0;
}
const Item* Find(const Copy& c, const char* rel) {
    for (const Item& i : c.items)
        if (i.rel == rel) return &i;
    return nullptr;
}
// A Team File the game loads: the file's own checks (keepfix 5: split from the slot structure check)
int TeamFileOk(const Bytes& t) {
    const int v = mp_save_validate(t.data(), (uint32_t)t.size(), true, true);
    if (v == MP_SAVE_ERR_VERSION) return mp_save_is_newer(t.data(), (uint32_t)t.size()) ? MP_KS_NEWER : MP_KS_BAD_FILE;   // (only a newer game's file is "newer")
    return v == MP_SAVE_OK ? 0 : MP_KS_BAD_FILE;
}
// One slot's structure (the game's parsers trust every size): asked of the slot that is brought IN and of the same slot in the result, never of the
// slots that stay as they are (a damaged slot must be repairable from a good copy). The image passed file-level checks.
bool SlotStructOk(const Bytes& t, int slot) {
    MpSaveSnapshotView view;
    return mp_save_view_slot(t.data(), slot, &view) && mp_save_check_view(&view, MP_SAVECHECK_STRUCT, nullptr) == MP_SC_OK;
}

// ---- the star-set rule ---------------------------------------------------------------------------------------------------------
// The copy's file has everything this console's has: every block (mario, luigi, config) the same chunks; the galaxy records of this file
// all present (by galaxy name) with at least the same bits (Power Stars, coin records); every other chunk a bitwise superset.
bool Chunks(const uint8_t* b, uint32_t size, std::vector<std::pair<uint32_t, uint32_t> >* out) {
    out->clear();
    if (size < 4) return false;
    if (b[0] == 0 && b[1] == 0) return true;
    if (b[0] != 1) return false;
    uint32_t p = 4;
    for (uint32_t c = 0; c < b[1]; c++) {
        if (size - p < 12) return false;
        const uint32_t cs = G32(b + p + 8);
        if (cs < 12 || cs > size - p) return false;
        out->push_back(std::make_pair(p, cs));
        p += cs;
    }
    return true;
}
bool Superset(const uint8_t* in, const uint8_t* cur, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if ((in[i] & cur[i]) != cur[i]) return false;
    return true;
}
bool GalaDominates(const uint8_t* in, uint32_t inN, const uint8_t* cur, uint32_t curN) {
    if (inN < 6 || curN < 6) return false;
    const uint32_t attrs = G16(cur + 2), rec = G16(cur + 4), head = 6 + attrs * 4;
    if (G16(in + 2) != attrs || G16(in + 4) != rec || rec == 0 || head > curN || head > inN || memcmp(in + 2, cur + 2, head - 2) != 0) return false;
    const uint32_t ng = G16(cur), ni = G16(in);
    if ((uint64_t)ng * rec > curN - head || (uint64_t)ni * rec > inN - head) return false;
    uint32_t h = 0, nameOff = 0xFFFFFFFFu;
    for (const char* c = "mGalaxyName"; *c; c++) h = (uint32_t)(signed char)*c + h * 31u;
    for (uint32_t a = 0; a < attrs; a++)
        if (G16(cur + 6 + a * 4) == (uint16_t)h) nameOff = G16(cur + 8 + a * 4);
    if (nameOff == 0xFFFFFFFFu || nameOff + 2 > rec) return false;
    for (uint32_t g = 0; g < ng; g++) {
        const uint8_t* c = cur + head + g * rec;
        bool found = false;
        for (uint32_t k = 0; k < ni && !found; k++) {
            const uint8_t* i = in + head + k * rec;
            if (memcmp(i + nameOff, c + nameOff, 2) == 0) {
                found = true;
                if (!Superset(i, c, rec)) return false;
            }
        }
        if (!found) return false;
    }
    return true;
}
bool SlotDominates(const uint8_t* in, const uint8_t* cur, int slot) {
    for (const char* k : {"mario", "luigi", "config"}) {
        char n[16];
        snprintf(n, sizeof n, "%s%d", k, slot);
        uint32_t io, is, co, cs;
        std::vector<std::pair<uint32_t, uint32_t> > ci, cc;
        if (!mp_save_find(in, n, &io, &is) || !mp_save_find(cur, n, &co, &cs) || !Chunks(in + io, is, &ci) || !Chunks(cur + co, cs, &cc)) return false;
        if (cc.empty()) continue;
        if (ci.size() != cc.size() || memcmp(in + io, cur + co, 4) != 0) return false;
        for (size_t x = 0; x < cc.size(); x++) {
            const uint8_t* a = in + io + ci[x].first;
            const uint8_t* b = cur + co + cc[x].first;
            if (memcmp(a, b, 8) != 0) return false;
            if (G32(b) == 0x47414C41u) {   // 'GALA'
                if (!GalaDominates(a + 12, ci[x].second - 12, b + 12, cc[x].second - 12)) return false;
            } else if (ci[x].second != cc[x].second || !Superset(a + 12, b + 12, cc[x].second - 12)) {
                return false;
            }
        }
    }
    return true;
}
// This console's Team File, read as it is (this module never repairs, renames or removes a live file: the save layer's recovery runs at
// start). 1 there, 0 none, -result.
int LiveTeam(Bytes* t) {
    const int rc = Read(P().teamFile, t);
    if (rc == -ENOENT) return 0;
    if (rc != 0) return -MP_KS_IO;
    const int ok = TeamFileOk(*t);   // (keepfix 5: the file's checks only: a file the game loads is a file that can be copied; slot structure is asked of the slot that is brought back)
    return ok ? -ok : 1;
}

// ---- pruning (the code) -------------------------------------------------------------------------------------------------------
struct BeforeCopy {
    std::string name;
    int64_t created = 0;
    uint32_t seq = 0;
    Parsed p;
    bool gone = false;
};
bool NewerThan(int64_t ca, uint32_t sa, const std::string& na, int64_t cb, uint32_t sb, const std::string& nb) {
    if (ca != cb) return ca > cb;
    if (sa != sb) return sa > sb;
    return na > nb;
}
const Bytes* TeamData(const std::vector<Item>& items) {
    for (const Item& i : items)
        if (i.rel == "mp/saves/GameData.bin") return &i.data;
    return nullptr;
}
// Is slot `slot` of `img` in `other` (same slot)? Both passed the file checks.
bool HeldIn(const Bytes& img, int slot, const Bytes& other) {
    MpSaveSnapshotView v;
    return mp_save_view_slot(img.data(), slot, &v) && mp_save_slot_matches(other.data(), slot, &v);
}
// Returns how many "before" copies that parse remain.
size_t Prune(const std::string& keep) {
    std::vector<std::string> names;
    if (!ListDir(CopiesDir(), &names)) return 0;
    std::vector<Snap> good;
    std::vector<BeforeCopy> before;
    std::vector<std::string> unfinished, made;
    uint32_t newest = 0;
    for (const std::string& n : names) {
        if (!NameOk(n)) continue;
        if (IsCopyFile(n)) {
            Bytes b;
            BeforeCopy bc;
            if (Read(CopiesDir() + "/" + n, &b) != 0 || ParseFile(b, &bc.p) != 0) continue;
            if (CopyKind(n) == MP_KS_COPY_BEFORE) {
                bc.name = n;
                bc.created = bc.p.created;
                bc.seq = SeqOf(n);
                before.push_back(bc);
            } else {
                made.push_back(n);
            }
            continue;
        }
        if (!IsDir(CopiesDir() + "/" + n)) continue;
        Snap s;
        const SnapState st = Classify(n, &s);
        if (st == kGood) {
            good.push_back(s);
            newest = std::max(newest, s.seq);
        } else if (st == kMarkedUnfinished) {
            unfinished.push_back(n);
        }
    }
    for (const std::string& u : unfinished) {
        uint32_t us = 0;
        if (OwnName(u, &us) && us < newest && u != keep) RemoveCopyTree(CopiesDir() + "/" + u);
    }
    // version copies: newest first
    std::sort(good.begin(), good.end(), [](const Snap& a, const Snap& b) { return NewerThan(a.created, a.seq, a.name, b.created, b.seq, b.name); });
    std::set<std::string> pairs;
    std::vector<size_t> pinned;   // oldest first
    for (size_t i = good.size(); i-- > 0;)
        if (pairs.insert(good[i].from + ">" + good[i].version).second) pinned.push_back(i);
    std::set<size_t> drop;
    for (size_t k = 0; k + 8 < pinned.size(); k++) {   // the oldest pinned ones beyond 8, when a newer verified copy holds the same files
        const size_t i = pinned[k];
        for (size_t j = 0; j < i; j++)
            if (SameFiles(good[i], good[j])) {
                drop.insert(i);
                break;
            }
    }
    std::vector<std::string> keptVersions;
    for (size_t i = 0; i < good.size(); i++) {
        const bool isPinned = std::find(pinned.begin(), pinned.end(), i) != pinned.end();
        if (good[i].name == keep || (i < 3 && !drop.count(i)) || (isPinned && !drop.count(i))) {
            keptVersions.push_back(good[i].name);
            continue;
        }
        RemoveSnapshot(good[i]);
    }
    // "before" copies: newest first; the newest kBeforeKeep stay; an older one goes (oldest first) only when nothing in it is unique
    std::sort(before.begin(), before.end(), [](const BeforeCopy& a, const BeforeCopy& b) { return NewerThan(a.created, a.seq, a.name, b.created, b.seq, b.name); });
    if (before.size() > kBeforeKeep) {
        Bytes live;
        const bool haveLive = Read(P().teamFile, &live) == 0 && TeamFileOk(live) == 0;
        std::vector<Bytes> held;   // the Team Files of the version copies and made copies that stay
        for (const std::vector<std::string>* group : {&keptVersions, &made})
            for (const std::string& n : *group) {
                Copy c;
                if (LoadCopy(n, &c) == 0)
                    if (const Item* t = Find(c, "mp/saves/GameData.bin"))
                        if (TeamFileOk(t->data) == 0) held.push_back(t->data);
            }
        std::vector<int> beforeOk(before.size(), 0);   // (each "before" copy's Team File checked once)
        for (size_t j = 0; j < before.size(); j++) {
            const Bytes* t = TeamData(before[j].p.items);
            beforeOk[j] = t && TeamFileOk(*t) == 0;
        }
        for (size_t i = before.size(); i-- > kBeforeKeep;) {
            if (before[i].name == keep) continue;
            const Bytes* team = TeamData(before[i].p.items);
            bool unique = false;
            if (team) {
                if (TeamFileOk(*team) != 0) unique = true;   // (cannot tell what is in it: it stays)
                for (int slot = 1; slot <= MP_SAVE_SLOTS && !unique; slot++) {
                    if (mp_save_slot_state(team->data(), slot) == 0) continue;   // blank: nothing to lose
                    bool found = haveLive && HeldIn(*team, slot, live);
                    for (size_t h = 0; h < held.size() && !found; h++) found = HeldIn(*team, slot, held[h]);
                    for (size_t j = 0; j < before.size() && !found; j++) {
                        if (j == i || before[j].gone) continue;
                        found = beforeOk[j] && HeldIn(*team, slot, *TeamData(before[j].p.items));
                    }
                    unique = !found;
                }
            }
            if (!unique && unlink((CopiesDir() + "/" + before[i].name).c_str()) == 0) before[i].gone = true;
        }
    }
    size_t left = 0;
    for (const BeforeCopy& b : before)
        if (!b.gone) left++;
    return left;
}
// keepfix 5: true when a new "before" copy may not be made: 64 exist and none can go.
bool BeforeFull() { return Prune("") >= kBeforeCap; }

// ---- the account store -------------------------------------------------------------------------------------------------------
// The account store owns these files (online*.dat, their .prev chain, the own list): it reads them the way the save layer's single recovery
// path says (a file whose atomic write was cut is put back by mp_save_recover_with, which mp_service does at its load too).
bool AccountAny(const uint8_t* d, uint32_t n) { return n == 112 && !memcmp(d, "SMGO", 4) && G32(d + 108) == mp_save_crc32(d, 108); }   // (a newer game's too)
bool OwnListOk(const uint8_t* d, uint32_t n) {
    if (n == 0 || n % 9) return false;
    for (uint32_t i = 0; i < n; i++)
        if (i % 9 == 8 ? d[i] != '\n' : !isxdigit(d[i])) return false;
    return true;
}
int ReadRecovered(const std::string& path, Bytes* out, MpSaveValidator valid) {
    int rc = Read(path, out);
    bool changed = false;
    if (rc == -ENOENT && mp_save_recover_with(path.c_str(), valid, &changed) && changed) rc = Read(path, out);
    return rc;
}
// The own list (keepsafe-own.txt). 0: read (*listed: it is there and passes its own check; otherwise it counts as no list: raw is empty); -1: it
// could not be read (an I/O error): the caller changes nothing (keepfix 5).
int OwnList(Bytes* raw, std::set<uint32_t>* out, bool* listed) {
    out->clear();
    *listed = false;
    const int rc = ReadRecovered(OwnPath(), raw, OwnListOk);
    if (rc != 0 && rc != -ENOENT) return -1;
    if (rc == 0 && OwnListOk(raw->data(), (uint32_t)raw->size())) {
        *listed = true;
        for (size_t p = 0; p + 8 <= raw->size(); p += 9) out->insert((uint32_t)strtoul(std::string((const char*)&raw->data()[p], 8).c_str(), nullptr, 16));
    } else {
        raw->clear();
    }
    return 0;
}
bool WriteOwn(Bytes list, const std::set<uint32_t>& have, const std::vector<uint32_t>& add) {   // one atomic write; false when nothing new (nothing written) is not an error
    std::set<uint32_t> seen = have;
    bool any = false;
    for (uint32_t c : add) {
        if (!seen.insert(c).second) continue;
        const std::string line = Fmt("%08x\n", (unsigned)c);
        list.insert(list.end(), line.begin(), line.end());
        any = true;
    }
    return !any || mp_save_write_atomic(OwnPath().c_str(), list.data(), (uint32_t)list.size());
}
// Replace the account file at `path` with `in`. The credential there is kept first (.prev; older ones move to .prev.1, .prev.2) when this
// console held it (its own list; a console without a usable list: the one there is its own) and it is not kept already. Then ONE atomic write.
// keepfix 5: everything is READ first; a read that fails (anything but "does not exist") ends the call with nothing changed.
bool StoreAccount(const std::string& path, const Bytes& in, bool own) {
    AcctLock al;
    Bytes cur;
    const int rc = ReadRecovered(path, &cur, AccountAny);
    if (rc != 0 && rc != -ENOENT) return false;   // there but unreadable now: never written over
    if (rc == 0 && AccountNewer(cur)) return false;   // a newer game's account file: left alone
    Bytes listRaw;
    std::set<uint32_t> mine;
    bool listed = false;
    if (OwnList(&listRaw, &mine, &listed) != 0) return false;
    const uint32_t inC = Credential(in);
    std::vector<uint32_t> add;
    if (rc == 0 && cur == in) {
        if (!own) return true;
        add.push_back(inC);
        return WriteOwn(listRaw, mine, add);
    }
    bool rotate = false;
    Bytes prev[3];
    bool have[3] = {false, false, false};
    if (rc == 0 && !cur.empty() && Credential(cur) != inC && (!listed || mine.count(Credential(cur)))) {
        bool kept = false;
        for (int i = 0; i < 3; i++) {
            const int r = ReadRecovered(path + (i == 0 ? ".prev" : i == 1 ? ".prev.1" : ".prev.2"), &prev[i], AccountAny);
            if (r != 0 && r != -ENOENT) return false;   // (a credential that cannot be read is not "absent": nothing is rotated over it)
            have[i] = r == 0 && !prev[i].empty();
            kept = kept || (have[i] && Credential(prev[i]) == Credential(cur));
        }
        rotate = !kept;
        if (!listed) add.push_back(Credential(cur));
    }
    if (own) add.push_back(inC);
    if (rotate) {   // (each a single atomic write; a cut leaves a credential in two places, never in none)
        if (have[1] && !mp_save_write_atomic((path + ".prev.2").c_str(), prev[1].data(), (uint32_t)prev[1].size())) return false;
        if (have[0] && !mp_save_write_atomic((path + ".prev.1").c_str(), prev[0].data(), (uint32_t)prev[0].size())) return false;
        if (!mp_save_write_atomic((path + ".prev").c_str(), cur.data(), (uint32_t)cur.size())) return false;
    }
    if (!WriteOwn(listRaw, mine, add)) return false;
    return mp_save_write_atomic(path.c_str(), in.data(), (uint32_t)in.size());
}

// mp.cfg from a copy never carries another console's identity (keepfix 5): console_key (the secret) and upnp_permanent (the router mapping) are
// this console's own. The copy's text with those two lines taken from what the running settings hold (in place; appended when the copy has none).
Bytes OwnIdentityText(const Bytes& copyText) {
    const MpConfig* cfg = mp_config ? mp_config() : nullptr;
    if (!cfg) return copyText;
    std::string keyLine, upnpLine;
    bool keySet = false;
    for (int i = 0; i < 16; i++) keySet = keySet || cfg->consoleKey[i];
    if (keySet) {
        keyLine = "console_key = ";
        for (int i = 0; i < 16; i++) keyLine += Fmt("%02x", (unsigned)cfg->consoleKey[i]);
        keyLine += "\n";
    }
    if (cfg->upnpPermanent) upnpLine = Fmt("upnp_permanent = %u\n", (unsigned)cfg->upnpPermanent);
    std::string out;
    bool sawKey = false, sawUpnp = false;
    const std::string t((const char*)copyText.data(), copyText.size());
    size_t at = 0;
    while (at < t.size()) {
        size_t e = t.find('\n', at);
        if (e == std::string::npos) e = t.size();
        const std::string line = t.substr(at, e - at);
        at = e + 1;
        size_t k = 0;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) k++;
        size_t ke = k;
        while (ke < line.size() && line[ke] != ' ' && line[ke] != '\t' && line[ke] != '=') ke++;
        const std::string key = line.substr(k, ke - k);
        if (key == "console_key") {
            if (!sawKey) out += keyLine;
            sawKey = true;
        } else if (key == "upnp_permanent") {
            if (!sawUpnp) out += upnpLine;
            sawUpnp = true;
        } else {
            out += line + "\n";
        }
    }
    if (!sawKey) out += keyLine;
    if (!sawUpnp) out += upnpLine;
    return Bytes(out.begin(), out.end());
}
void RunHooks() {
    for (int i = 0; i < S().nHooks; i++) S().hooks[i]();
}
bool HaveCopyOfNow() {   // a verified version copy of exactly this data (names, sizes, fingerprints)
    Snap now;
    const Listing set = DataSet();
    if (!set.ok) return false;   // (an incomplete listing: no proof that a copy holds all of it)
    for (const std::string& r : set.rels) {
        Bytes b;
        if (Read(Abs(r), &b) != 0) return false;
        SnapFile f;
        f.rel = r;
        f.size = (uint32_t)b.size();
        f.sum = Fp(b.data(), b.size());
        now.files.push_back(f);
    }
    if (now.files.empty()) return true;
    std::vector<std::string> names;
    ListDir(CopiesDir(), &names);
    for (const std::string& n : names) {
        Snap s;
        if (NameOk(n) && !IsCopyFile(n) && Classify(n, &s) == kGood && SameFiles(s, now)) return true;
    }
    return false;
}

}  // namespace

// ================================================================================================================================
extern "C" {

const char* mp_keepsafe_result_name(int r) {
    static const char* const k[] = {"ok", "nothing", "args", "io", "no space", "too big", "bad file", "newer", "not found", "verify", "busy", "confirm", "too many"};
    return r >= 0 && r <= MP_KS_TOO_MANY ? k[r] : "?";
}

void mp_keepsafe_default_paths(MpKsPaths* o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    std::string mp, online;
    const bool cfgDir = mp_config_dir && mp_config_dir() && *mp_config_dir();
    const bool onDir = mp_service_online_dir && mp_service_online_dir() && *mp_service_online_dir();
#if defined(__vita__)
    mp = cfgDir ? mp_config_dir() : "ux0:data/smg/mp";
    online = onDir ? mp_service_online_dir() : "ux0:data/smg/online";
#else
    const char* d = getenv("PETARI_DATA_DIR");
    const char* e = getenv("SMG_ONLINE_DIR");
    const char* sd = getenv("SMG_DATA_DIR");
    mp = cfgDir ? std::string(mp_config_dir()) : std::string(d && *d ? d : ".") + "/mp";
    online = onDir ? std::string(mp_service_online_dir()) : e && *e ? std::string(e) : sd && *sd ? std::string(sd) + "/online" : "smg-data/online";
#endif
    snprintf(o->mpDir, sizeof o->mpDir, "%s", mp.c_str());
    snprintf(o->dataDir, sizeof o->dataDir, "%s", Parent(mp).c_str());
    snprintf(o->onlineDir, sizeof o->onlineDir, "%s", online.c_str());
    snprintf(o->teamFile, sizeof o->teamFile, "%s/saves/title/00010000/524d4750/data/GameData.bin", mp.c_str());
    snprintf(o->recoveryDir, sizeof o->recoveryDir, "%s/recovery", mp.c_str());
}
void mp_keepsafe_set_paths(const MpKsPaths* p) {
    S().pathsSet = p != nullptr;
    if (p) S().paths = *p;
}
void mp_keepsafe_set_data_hash(uint32_t h) { S().dataHash = h; }
int mp_keepsafe_notice_take(void) {
    const int n = S().notice;
    S().notice = MP_KS_NOTICE_NONE;
    return n;
}
bool mp_keepsafe_may_migrate(void) { return S().mayMigrate; }
bool mp_keepsafe_busy(void) { return gActive.load() > 0; }
void mp_keepsafe_add_reload_hook(void (*fn)(void)) {
    State& s = S();
    for (int i = 0; i < s.nHooks; i++)
        if (s.hooks[i] == fn) return;
    if (fn && s.nHooks < 8) s.hooks[s.nHooks++] = fn;
}
void mp_keepsafe_set_guard(bool (*fn)(void)) { S().guard = fn; }

void mp_keepsafe_boot(const char* version, const char* build) {
    Busy b;
    State& s = S();
    snprintf(s.version, sizeof s.version, "%s", version && *version ? version : "?");
    snprintf(s.build, sizeof s.build, "%s", build ? build : "");
    s.mayMigrate = true;
    s.notice = MP_KS_NOTICE_NONE;
    Stamp st;
    ReadStamp(&st);
    if (st.unreadable) {   // there but cannot be read now: nothing is decided at this start, nothing is written; tried again at the next
        s.mayMigrate = false;
        return;
    }
    // (keepfix 5: a stamp that is there and read but does not parse is no stamp: the copy is made as from "none", then a good stamp is written)
    s.seqHint = st.seq;
    std::string name;
    if (st.newer) {   // a newer game's stamp: never rewritten; a copy of this data unless one exists
        s.mayMigrate = false;
        if (!HaveCopyOfNow()) MakeVersionCopy("newer", &name);
        return;
    }
    if (st.valid && st.version == s.version && st.build == s.build) return;   // the normal start: one small read
    const int rc = MakeVersionCopy(st.valid ? st.version.c_str() : "none", &name);
    if (rc == MP_KS_OK || rc == MP_KS_NOTHING) {
        Stamp ns = st;
        ns.seq = s.seqHint;
        ns.version = s.version;
        ns.build = s.build;
        WriteStamp(ns);   // (not written: the copy is made again at the next start)
        return;
    }
    s.mayMigrate = false;   // no copy: nothing is changed, tried again at the next start; the menus say so
    s.notice = rc == MP_KS_NO_SPACE ? MP_KS_NOTICE_NO_SPACE : MP_KS_NOTICE_NO_COPY;
    Log("no copy at this update (%s)", mp_keepsafe_result_name(rc));
}

int mp_keepsafe_list_copies(MpKsCopy* out, int max) {
    Busy b;
    if (!out || max <= 0) return 0;
    std::vector<std::string> names;
    std::vector<MpKsCopy> v;
    ListDir(CopiesDir(), &names);
    for (const std::string& n : names) {
        Copy c;
        if (!NameOk(n) || LoadCopy(n, &c) != 0) continue;
        MpKsCopy e;
        memset(&e, 0, sizeof e);
        e.kind = c.kind;
        snprintf(e.name, sizeof e.name, "%s", n.c_str());
        e.created = c.created;
        for (const Item& i : c.items) {
            e.bytes += (uint32_t)i.data.size();
            e.teamFiles |= i.rel == "mp/saves/GameData.bin";
            e.account |= i.rel.compare(0, 7, "online/") == 0;
            e.settings |= i.rel.compare(0, 3, "mp/") == 0 && i.rel != "mp/saves/GameData.bin";
        }
        Snap s;
        if (c.kind == MP_KS_COPY_VERSION && Classify(n, &s) == kGood) snprintf(e.version, sizeof e.version, "%s", s.version.c_str());
        v.push_back(e);
    }
    std::sort(v.begin(), v.end(), [](const MpKsCopy& a, const MpKsCopy& c) { return a.created != c.created ? a.created > c.created : strcmp(a.name, c.name) > 0; });
    int n = 0;
    for (const MpKsCopy& e : v)
        if (n < max) out[n++] = e;
    return n;
}

int mp_keepsafe_make_copy(int flags, char* nameOut, size_t cap) {
    Busy b;
    if (nameOut && cap) nameOut[0] = 0;
    if (!b.ok) return MP_KS_BUSY;
    if (!mp_save_async_wait()) return MP_KS_BUSY;   // (a save the game is writing right now is finished first; fix2: bounded, a hung write is 'busy')
    std::vector<Item> items;
    Bytes t;
    const int lt = LiveTeam(&t);
    if (lt < 0) return -lt;   // (a Team File that cannot be read now is not copied as if there were none)
    if (lt == 1) items.push_back(Item{"mp/saves/GameData.bin", t});
    std::vector<std::string> names;
    if (!(flags & MP_KS_COPY_NO_ACCOUNT)) {
        AcctLock al;   // (keepfix 5: the account store's two renames are never seen half done)
        if (ListDir(P().onlineDir, &names))
            for (const std::string& n : names) {
                Bytes a;
                if (IsAccountName(n) && Read(std::string(P().onlineDir) + "/" + n, &a) == 0 && AccountOk(a)) items.push_back(Item{"online/" + n, a});
            }
    }
    if (items.empty()) return MP_KS_NOTHING;
    std::string name;
    const int rc = WriteCopyFile("copy", items, &name);
    if (nameOut && cap) snprintf(nameOut, cap, "%s", name.c_str());
    Log("make a copy: %s", mp_keepsafe_result_name(rc));
    return rc;
}

int mp_keepsafe_inspect(const char* name, MpKsCopyInfo* info) {
    Busy b;
    if (!name || !info) return MP_KS_ARGS;
    memset(info, 0, sizeof *info);
    Copy c;
    int rc = LoadCopy(name, &c);
    if (rc) return rc;
    info->kind = c.kind;
    info->created = c.created;
    info->dataHashDiffers = c.dataHash && S().dataHash && c.dataHash != S().dataHash;
    for (int i = 0; i < 6; i++) info->copyStars[i] = info->liveStars[i] = -1;
    if (const Item* team = Find(c, "mp/saves/GameData.bin")) {
        if ((rc = TeamFileOk(team->data)) != 0) return rc;
        Bytes live;
        const int lt = LiveTeam(&live);
        for (int s = 1; s <= 6; s++) {
            const uint8_t bit = (uint8_t)(1u << (s - 1));
            info->copyStars[s - 1] = (int16_t)mp_save_slot_stars(team->data.data(), s, nullptr, nullptr);
            if (lt == 1) info->liveStars[s - 1] = (int16_t)mp_save_slot_stars(live.data(), s, nullptr, nullptr);
            if (mp_save_slot_state(team->data.data(), s) != 1 || !SlotStructOk(team->data, s)) {   // (blank, or a slot that cannot be brought in: not offered)
                info->slotBlank |= bit;
                continue;
            }
            if (lt != 1) continue;
            MpSaveSnapshotView v;
            if (mp_save_view_slot(team->data.data(), s, &v) && mp_save_slot_matches(live.data(), s, &v)) {
                info->slotSame |= bit;
                continue;
            }
            if (mp_save_slot_state(live.data(), s) == 0) continue;
            info->slotInUse |= bit;
            if (!SlotDominates(team->data.data(), live.data(), s)) info->slotLacks |= bit;
        }
    }
    for (const Item& i : c.items) {
        if (i.rel.compare(0, 7, "online/") == 0) {
            if (AccountOk(i.data) && !SameAs(Abs(i.rel), i.data)) info->account = 1;
        } else if (c.kind != MP_KS_COPY_MADE && i.rel != "mp/saves/GameData.bin" && !SameAs(Abs(i.rel), i.data)) {
            info->settings |= i.rel == "mp/mp.cfg" ? MP_KS_SET_CONFIG : i.rel == "mp/flow.cfg" ? MP_KS_SET_FLOW : MP_KS_SET_COOP;
        }
    }
    return 0;
}

int mp_keepsafe_bring_back_slot(const char* name, int slot, int confirmed, char* beforeOut, size_t cap) {
    Busy b;
    if (beforeOut && cap) beforeOut[0] = 0;
    if (!name || slot < 1 || slot > 6) return MP_KS_ARGS;
    if (!b.ok || (S().guard && !S().guard())) return MP_KS_BUSY;
    if (!mp_save_async_wait()) return MP_KS_BUSY;   // fix2: bounded
    Copy c;
    int rc = LoadCopy(name, &c);
    if (rc) return rc;
    const Item* team = Find(c, "mp/saves/GameData.bin");
    if (!team) return MP_KS_NOTHING;
    if ((rc = TeamFileOk(team->data)) != 0) return rc;
    const int copySlot = mp_save_slot_state(team->data.data(), slot);
    if (copySlot == 0) return MP_KS_NOTHING;   // a blank file never replaces anything
    if (copySlot != 1 || !SlotStructOk(team->data, slot)) return MP_KS_BAD_FILE;   // (keepfix 5: the slot that comes in is checked, the copy's other slots are not used)
    Bytes live;
    const int lt = LiveTeam(&live);
    if (lt < 0) return -lt;   // (never written over a file that cannot be read now, or a newer game's)
    Bytes img(MP_SAVE_MAX_FILE, 0);
    if (lt == 1) {
        MpSaveSnapshotView v;
        if (mp_save_view_slot(team->data.data(), slot, &v) && mp_save_slot_matches(live.data(), slot, &v)) return MP_KS_NOTHING;
        if (mp_save_slot_state(live.data(), slot) != 0) {   // (a damaged slot is in use too: its before copy keeps it)
            if (BeforeFull()) return MP_KS_TOO_MANY;   // (asked before the question: nothing would be done with the answer)
            if (!confirmed) return MP_KS_CONFIRM;
            std::string before;   // the whole Team File as it is, in a new verified copy first: or nothing is replaced
            if ((rc = WriteCopyFile("before", std::vector<Item>{Item{"mp/saves/GameData.bin", live}}, &before)) != 0) return rc;
            if (beforeOut && cap) snprintf(beforeOut, cap, "%s", before.c_str());
            Prune(before);
        }
        memcpy(img.data(), live.data(), live.size());
    } else {   // no Team File here: a new file with ONLY this slot (keepfix 5): every other slot blank, never another file's progress
        memcpy(img.data(), team->data.data(), team->data.size());
        int blank = 0;
        for (int k = 1; k <= 6 && !blank; k++)
            if (mp_save_slot_state(team->data.data(), k) == 0) blank = k;
        for (int s = 1; s <= 6; s++) {
            if (s == slot) continue;
            if (blank) {   // (the copy's own blank slot)
                mp_save_copy_slot(team->data.data(), blank, img.data(), s);
                continue;
            }
            for (const char* k : {"mario", "luigi", "config"}) {   // (the copy has none: a never-created file is all zero blocks)
                char n[16];
                uint32_t off, size;
                snprintf(n, sizeof n, "%s%d", k, s);
                if (mp_save_find(img.data(), n, &off, &size)) memset(img.data() + off, 0, size);
            }
        }
    }
    if (!mp_save_copy_slot(team->data.data(), slot, img.data(), slot) || !mp_save_finalize(img.data(), MP_SAVE_MAX_FILE)) return MP_KS_BAD_FILE;
    img.resize(mp_save_disk_length(img.data()));
    if (TeamFileOk(img) != 0 || !SlotStructOk(img, slot)) return MP_KS_BAD_FILE;   // (the result: the file's checks and this slot; the slots that stay are as they were)
    mp_save_mkdirs(Parent(P().teamFile).c_str());
    if (!mp_save_write_atomic(P().teamFile, img.data(), (uint32_t)img.size())) return MP_KS_IO;
    RunHooks();
    Log("Team File %d brought back from %s", slot, name);
    return MP_KS_OK;
}

int mp_keepsafe_use_account(const char* name, int confirmed) {
    Busy b;
    if (!name) return MP_KS_ARGS;
    if (!b.ok || (S().guard && !S().guard())) return MP_KS_BUSY;   // (keepfix 5: not during a session, nor while a save is being written)
    mp_save_async_wait();
    Copy c;
    const int rc = LoadCopy(name, &c);
    if (rc) return rc;
    std::vector<const Item*> todo;
    for (const Item& i : c.items)
        if (i.rel.compare(0, 7, "online/") == 0 && AccountOk(i.data) && !SameAs(Abs(i.rel), i.data)) todo.push_back(&i);
    if (todo.empty()) return MP_KS_NOTHING;
    if (!confirmed) return MP_KS_CONFIRM;
    mp_save_mkdirs(P().onlineDir);
    for (const Item* i : todo)   // (each file one write through the store; the credential replaced is kept first when it was this console's)
        if (!StoreAccount(Abs(i->rel), i->data, false)) return MP_KS_IO;
    if (mp_service_shutdown) mp_service_shutdown();
    Log("account from a copy used");
    return MP_KS_OK;
}

int mp_keepsafe_bring_back_settings(const char* name, int which, char* beforeOut, size_t cap) {
    Busy b;
    if (beforeOut && cap) beforeOut[0] = 0;
    if (!name || !which) return MP_KS_ARGS;
    if (!b.ok || (S().guard && !S().guard())) return MP_KS_BUSY;
    if (!mp_config_hold || !mp_config_replace_held || !mp_ui_flow_replace_file) return MP_KS_IO;
    Copy c;
    int rc = LoadCopy(name, &c);
    if (rc) return rc;
    if (c.kind == MP_KS_COPY_MADE) return MP_KS_NOTHING;   // never another console's settings
    mp_config_hold(true);   // the settings lock BEFORE anything is read or copied (a helper save waits)
    std::vector<Item> want, old;
    const char* const rels[3] = {"mp/mp.cfg", "mp/flow.cfg", "mp/coop.cfg"};
    for (int k = 0; k < 3 && rc == 0; k++) {
        const Item* found = (which & (1 << k)) ? Find(c, rels[k]) : nullptr;
        if (!found || TextVersion(found->data) > 1 || memchr(found->data.data(), 0, found->data.size())) continue;
        Item i = *found;
        if (k == 0) i.data = OwnIdentityText(i.data);   // (keepfix 5: this console's console_key / upnp_permanent, whatever the copy's name says)
        if (SameAs(Abs(rels[k]), i.data)) continue;
        Bytes cur;
        const int r = Read(Abs(rels[k]), &cur);
        if (r == 0 && TextVersion(cur) > 1) continue;   // a newer game's file here: left alone
        if (r != 0 && r != -ENOENT) rc = MP_KS_IO;   // (never written over a file that cannot be read now)
        want.push_back(i);
        if (r == 0) old.push_back(Item{rels[k], cur});
    }
    std::string before;
    if (rc == 0 && want.empty()) rc = MP_KS_NOTHING;
    if (rc == 0 && !old.empty() && BeforeFull()) rc = MP_KS_TOO_MANY;
    if (rc == 0 && !old.empty()) rc = WriteCopyFile("before", old, &before);
    if (rc == 0 && !before.empty()) Prune(before);
    for (size_t k = 0; rc == 0 && k < want.size(); k++) {
        const std::string text((const char*)want[k].data.data(), want[k].data.size());
        const bool ok = want[k].rel == "mp/mp.cfg" ? mp_config_replace_held(text.data(), text.size()) : mp_ui_flow_replace_file(want[k].rel.c_str() + 3, text.c_str());
        if (!ok) rc = MP_KS_IO;
    }
    if (!want.empty() && rc != MP_KS_NOTHING) RunHooks();   // (mp.cfg was loaded by its writer while the lock is held)
    mp_config_hold(false);
    if (beforeOut && cap) snprintf(beforeOut, cap, "%s", before.c_str());
    Log("settings brought back from %s: %s", name, mp_keepsafe_result_name(rc));
    return rc;
}

bool mp_keepsafe_account_store(const char* path, const uint8_t* data, uint32_t len) {
    if (!path || !data || len != 112) return false;
    mp_save_mkdirs(Parent(path).c_str());
    return StoreAccount(path, Bytes(data, data + len), true);
}
bool mp_keepsafe_account_previous(const char* path, int index, uint8_t out[112]) {
    Bytes b;
    if (!path || !out || index < 0 || index > 2) return false;
    if (ReadRecovered(std::string(path) + (index == 0 ? ".prev" : index == 1 ? ".prev.1" : ".prev.2"), &b, AccountAny) != 0 || !AccountOk(b)) return false;
    memcpy(out, b.data(), 112);
    return true;
}

void mp_keepsafe_test_set_time(int64_t t) { S().fakeTime = t; }
void mp_keepsafe_test_set_free_bytes(int64_t b) { S().fakeFree = b; }
int mp_keepsafe_test_read_count(void) { return S().reads; }
void mp_keepsafe_test_set_read_hook(void (*fn)(const char* path)) { S().readHook = fn; }
int mp_keepsafe_test_stat_count(void) { return S().stats; }
void mp_keepsafe_test_reset(void) {
    State& s = S();
    const MpKsPaths p = s.paths;
    const bool set = s.pathsSet;
    const int64_t t = s.fakeTime, f = s.fakeFree;
    s = State();
    s.paths = p;
    s.pathsSet = set;
    s.fakeTime = t;
    s.fakeFree = f;
}

}  // extern "C"
