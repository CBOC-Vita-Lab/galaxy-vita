// Galaxy 3.0 multiplayer: the power-cut model for platform/src/mp/mp_keepsafe.cpp (keepfix 3). Method of the third outside review (its `cut`
// program), adopted: every operation runs in a FORKED child that _exits right before its k-th file-changing system call (open for writing,
// write, pwrite, writev, rename, renameat, unlink, mkdir, rmdir, creat, openat, mkstemp, fsync, fdatasync, ftruncate, truncate, link, symlink; also fopen
// for writing, fwrite / fprintf / fputs / fputc / fflush / fclose of a written stream and remove, which the settings writers use). A product call
// that is NOT in this set fails the build: tests/mp/cut_audit.py lists the file-changing symbols the product objects import.
// Nothing runs after the cut: no rollback, no recovery, no read-back. Each write is also cut "torn" (half of the bytes written). Then the
// parent starts the game again as the game does (mp_keepsafe_boot, then the save layer's single recovery path for the Team File and the
// account, then the settings load), and checks:
//   1. every live file (mp/, online/) is byte-exact either its state before the operation or its state after it (a file the settings
//      writers keep as .bak is read as the loader reads it); no stray file; the copies folder keeps every verified copy it had (only
//      what the uncut operation itself removed may go, and only behind its "unfinished" mark);
//   2. what a completed change replaced is in a verified copy (or, for the account, in .prev): nothing is overwritten without a way back;
//   3. the player plays on (the game saves the Team File, mp.cfg, flow.cfg and the account through their own writers), the game starts
//      twice more (the second time as a new version): every one of those saves is still there, and the operation can be run again;
//   4. the single-player save folder is byte-identical (bytes, inode, mtime) after every single cut.
// keepfix 5 (the reviewer's third mode): every operation is also run with "disk full on this call, the process continues": the k-th changing
// call fails with ENOSPC (a write writes nothing, an open / rename / unlink / mkdir returns -1, fwrite / fclose / fopen fail) and the operation
// runs on to its end with whatever it does after a failure; then the same restart and the same checks.
// Operations: version copy, prune, make a copy, bring back a slot (in use / empty / no Team File here / the undo), use the account (own to a
// foreign one / back), the account store, settings (and their undo), and the failed-listing clash.
// Build and run: tests/mp/keepsafe_build.sh (no sanitizer: the file system calls are interposed). KS_CUT_ONLY=<operation name part>.
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <functional>

#include "keepsafe_world.h"

#include "../../platform/src/mp/mp_config.h"
#include "port/mp_ui_flow.h"

// ---- the game's side -----------------------------------------------------------------------------------------------------------
extern "C" void mp_log(const char*, ...) {}
extern "C" bool mp_core_resume(void) { return false; }
struct MpProfile;
extern "C" void mp_ui_profile(MpProfile*) {}
static int gReloads = 0, gServiceStops = 0;
static void reloadHook() { gReloads++; }
static bool mayReplace() { return true; }
extern "C" void mp_service_shutdown(void) { gServiceStops++; }

// ---- the power cut: the file-changing calls, counted -----------------------------------------------------------------------------
static int gArm = 0, gCount = 0, gTorn = 0;
static bool gFail = false, gFired = false;   // gFail: the armed call fails with ENOSPC and the process goes on
static std::set<FILE*> gWritten;   // streams opened for writing (the settings writers)
static const bool kNoLog = (setvbuf(stdout, nullptr, _IOLBF, 0), true);
extern "C" {
typedef ssize_t (*write_t)(int, const void*, size_t);
static write_t realWrite() {
    static write_t f = (write_t)dlsym(RTLD_NEXT, "write");
    return f;
}
}
// The k-th changing call: the process dies here (exit 10), or, at a write, with `torn`, after half of its bytes (12); 11 = a plain cut at a write.
// (off >= 0: a pwrite at that offset)
static bool tick(bool isWrite = false, int fd = -1, const void* b = nullptr, size_t n = 0, off_t off = -1) {   // true: this call must fail (disk full mode)
    if (!gArm) return false;
    if (++gCount != gArm) return false;
    if (gFail) {
        gFired = true;
        errno = ENOSPC;
        return true;
    }
    if (isWrite && gTorn && n >= 2) {
        if (off >= 0) {
            static ssize_t (*pw)(int, const void*, size_t, off_t) = (ssize_t(*)(int, const void*, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
            pw(fd, b, n / 2, off);
        } else {
            realWrite()(fd, b, n / 2);
        }
        _exit(12);
    }
    _exit(isWrite ? 11 : 10);
    return false;
}
static bool presentPath(const char* p) { struct stat st; return stat(p, &st) == 0; }
extern "C" {
int rename(const char* a, const char* b) {
    if (tick()) return -1;
    static int (*f)(const char*, const char*) = (int (*)(const char*, const char*))dlsym(RTLD_NEXT, "rename");
    return f(a, b);
}
int unlink(const char* a) {
    if (gArm && presentPath(a) && tick()) return -1;
    static int (*f)(const char*) = (int (*)(const char*))dlsym(RTLD_NEXT, "unlink");
    return f(a);
}
int remove(const char* a) {
    if (gArm && presentPath(a) && tick()) return -1;
    static int (*f)(const char*) = (int (*)(const char*))dlsym(RTLD_NEXT, "remove");
    return f(a);
}
int mkdir(const char* a, mode_t m) {
    if (gArm && !presentPath(a) && tick()) return -1;
    static int (*f)(const char*, mode_t) = (int (*)(const char*, mode_t))dlsym(RTLD_NEXT, "mkdir");
    return f(a, m);
}
int rmdir(const char* a) {
    if (gArm && presentPath(a) && tick()) return -1;
    static int (*f)(const char*) = (int (*)(const char*))dlsym(RTLD_NEXT, "rmdir");
    return f(a);
}
static int openImpl(const char* a, int fl, mode_t m) {
    static int (*f)(const char*, int, ...) = (int (*)(const char*, int, ...))dlsym(RTLD_NEXT, "open");
    if ((fl & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) && tick()) return -1;
    return f(a, fl, m);
}
int open(const char* a, int fl, ...) {
    mode_t m = 0;
    if (fl & O_CREAT) {
        va_list ap;
        va_start(ap, fl);
        m = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openImpl(a, fl, m);
}
int open64(const char* a, int fl, ...) {
    mode_t m = 0;
    if (fl & O_CREAT) {
        va_list ap;
        va_start(ap, fl);
        m = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openImpl(a, fl, m);
}
ssize_t write(int fd, const void* b, size_t n) {
    if (fd > 2 && gArm && tick(true, fd, b, n)) return -1;
    return realWrite()(fd, b, n);
}
FILE* fopen(const char* a, const char* mode) {
    static FILE* (*f)(const char*, const char*) = (FILE * (*)(const char*, const char*)) dlsym(RTLD_NEXT, "fopen");
    const bool w = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (w && tick()) return nullptr;
    FILE* r = f(a, mode);
    if (r && w && gArm) gWritten.insert(r);
    return r;
}
size_t fwrite(const void* b, size_t s, size_t n, FILE* f) {
    static size_t (*real)(const void*, size_t, size_t, FILE*) = (size_t(*)(const void*, size_t, size_t, FILE*))dlsym(RTLD_NEXT, "fwrite");
    if (gArm && gWritten.count(f) && tick()) return 0;
    return real(b, s, n, f);
}
int fclose(FILE* f) {
    static int (*real)(FILE*) = (int (*)(FILE*))dlsym(RTLD_NEXT, "fclose");
    if (gArm && gWritten.erase(f) && tick()) {   // (disk full on the last flush: the stream is closed, the close fails)
        real(f);
        return EOF;
    }
    return real(f);
}
// ---- the rest of the file-changing surface (rigfix): a product call that is not here would change the disk with no cut point before it ----
#define REAL(ret, name, args) static ret(*real_##name) args = (ret(*) args)dlsym(RTLD_NEXT, #name)
int fsync(int fd) {
    REAL(int, fsync, (int));
    if (gArm && tick()) return -1;   // (the data written so far may or may not be on the card: the cut lands before it is forced)
    return real_fsync(fd);
}
int fdatasync(int fd) {
    REAL(int, fdatasync, (int));
    if (gArm && tick()) return -1;
    return real_fdatasync(fd);
}
int ftruncate(int fd, off_t len) {
    REAL(int, ftruncate, (int, off_t));
    if (gArm && tick()) return -1;
    return real_ftruncate(fd, len);
}
int ftruncate64(int fd, off64_t len) {
    REAL(int, ftruncate64, (int, off64_t));
    if (gArm && tick()) return -1;
    return real_ftruncate64(fd, len);
}
int truncate(const char* a, off_t len) {
    REAL(int, truncate, (const char*, off_t));
    if (gArm && tick()) return -1;
    return real_truncate(a, len);
}
int creat(const char* a, mode_t m) {
    REAL(int, creat, (const char*, mode_t));
    if (gArm && tick()) return -1;
    return real_creat(a, m);
}
int creat64(const char* a, mode_t m) {
    REAL(int, creat64, (const char*, mode_t));
    if (gArm && tick()) return -1;
    return real_creat64(a, m);
}
static int openatImpl(const char* name, int dirfd, const char* a, int fl, mode_t m) {
    static int (*f)(int, const char*, int, ...) = nullptr;
    if (!f) f = (int (*)(int, const char*, int, ...))dlsym(RTLD_NEXT, name);
    if ((fl & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) && tick()) return -1;
    return f(dirfd, a, fl, m);
}
int openat(int dirfd, const char* a, int fl, ...) {
    mode_t m = 0;
    if (fl & O_CREAT) {
        va_list ap;
        va_start(ap, fl);
        m = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openatImpl("openat", dirfd, a, fl, m);
}
int openat64(int dirfd, const char* a, int fl, ...) {
    mode_t m = 0;
    if (fl & O_CREAT) {
        va_list ap;
        va_start(ap, fl);
        m = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openatImpl("openat64", dirfd, a, fl, m);
}
ssize_t pwrite(int fd, const void* b, size_t n, off_t off) {
    REAL(ssize_t, pwrite, (int, const void*, size_t, off_t));
    if (fd > 2 && gArm && tick(true, fd, b, n, off)) return -1;
    return real_pwrite(fd, b, n, off);
}
ssize_t pwrite64(int fd, const void* b, size_t n, off64_t off) {
    REAL(ssize_t, pwrite64, (int, const void*, size_t, off64_t));
    if (fd > 2 && gArm && tick(true, fd, b, n, (off_t)off)) return -1;
    return real_pwrite64(fd, b, n, off);
}
ssize_t writev(int fd, const struct iovec* v, int cnt) {
    REAL(ssize_t, writev, (int, const struct iovec*, int));
    if (fd > 2 && gArm && tick()) return -1;
    return real_writev(fd, v, cnt);
}
int renameat(int od, const char* a, int nd, const char* b) {
    REAL(int, renameat, (int, const char*, int, const char*));
    if (tick()) return -1;
    return real_renameat(od, a, nd, b);
}
int renameat2(int od, const char* a, int nd, const char* b, unsigned fl) {
    REAL(int, renameat2, (int, const char*, int, const char*, unsigned));
    if (tick()) return -1;
    return real_renameat2(od, a, nd, b, fl);
}
int link(const char* a, const char* b) {
    REAL(int, link, (const char*, const char*));
    if (tick()) return -1;
    return real_link(a, b);
}
int symlink(const char* a, const char* b) {
    REAL(int, symlink, (const char*, const char*));
    if (tick()) return -1;
    return real_symlink(a, b);
}
int mkstemp(char* t) {
    REAL(int, mkstemp, (char*));
    if (tick()) return -1;
    return real_mkstemp(t);
}
int mkstemp64(char* t) {
    REAL(int, mkstemp64, (char*));
    if (tick()) return -1;
    return real_mkstemp64(t);
}
int mkostemp(char* t, int fl) {
    REAL(int, mkostemp, (char*, int));
    if (tick()) return -1;
    return real_mkostemp(t, fl);
}
FILE* fopen64(const char* a, const char* mode) { return fopen(a, mode); }
// streams opened for writing: each call that feeds or flushes one is a cut point, like fwrite
int fflush(FILE* f) {
    REAL(int, fflush, (FILE*));
    if (gArm && (f ? gWritten.count(f) != 0 : !gWritten.empty()) && tick()) return EOF;
    return real_fflush(f);
}
int fputs(const char* s, FILE* f) {
    REAL(int, fputs, (const char*, FILE*));
    if (gArm && gWritten.count(f) && tick()) return EOF;
    return real_fputs(s, f);
}
int fputc(int c, FILE* f) {
    REAL(int, fputc, (int, FILE*));
    if (gArm && gWritten.count(f) && tick()) return EOF;
    return real_fputc(c, f);
}
int putc(int c, FILE* f) {
    REAL(int, putc, (int, FILE*));
    if (gArm && gWritten.count(f) && tick()) return EOF;
    return real_putc(c, f);
}
int vfprintf(FILE* f, const char* fmt, va_list ap) {
    REAL(int, vfprintf, (FILE*, const char*, va_list));
    if (gArm && gWritten.count(f) && tick()) return -1;
    return real_vfprintf(f, fmt, ap);
}
int fprintf(FILE* f, const char* fmt, ...) {
    REAL(int, vfprintf, (FILE*, const char*, va_list));
    if (gArm && gWritten.count(f) && tick()) return -1;
    va_list ap;
    va_start(ap, fmt);
    const int r = real_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
}
// opendir of a folder ending in gFailOpendir fails while it is set
#include <dirent.h>
static const char* gFailOpendir = nullptr;
extern "C" DIR* opendir(const char* p) {
    typedef DIR* (*od_t)(const char*);
    static od_t f = (od_t)dlsym(RTLD_NEXT, "opendir");
    if (gFailOpendir) {
        const size_t n = strlen(p), m = strlen(gFailOpendir);
        if (n >= m && !strcmp(p + n - m, gFailOpendir)) {
            errno = EIO;
            return nullptr;
        }
    }
    return f(p);
}

// ---- worlds ----------------------------------------------------------------------------------------------------------------------
static std::string gWorldRoot;
static int gCopyN = 0;
static void wire() {
    mp_keepsafe_test_reset();
    mp_keepsafe_add_reload_hook(reloadHook);
    mp_keepsafe_add_reload_hook(mp_ui_flow_forget);
    mp_keepsafe_set_guard(mayReplace);
}
static void dropWorld() {
    if (gWorldRoot.empty()) return;
    gSpPrint.erase(gWorldRoot + "/data/saves");
    rmrf(gWorldRoot);
    gWorldRoot.clear();
}
static void cutWorld(const char* name, const World& w = World()) {
    dropWorld();
    gCopyN = 0;
    newWorld(name);
    gWorldRoot = std::string(gP.dataDir).substr(0, std::string(gP.dataDir).size() - 5);   // (<root>/data)
    fillWorld(w);
    mp_config_set_dir(gP.mpDir);
    wire();
}
static std::string bk() { return std::string(gP.dataDir) + "/mp-backups"; }
static std::string stampPath() { return std::string(gP.dataDir) + "/mp-keepsafe.stamp"; }
static std::string accountPath() { return std::string(gP.onlineDir) + "/online.dat"; }
static std::string mpPath(const char* n) { return std::string(gP.mpDir) + "/" + n; }

static bool accountValid(const uint8_t* b, uint32_t n) { return n == 112 && !memcmp(b, "SMGO", 4) && rd32(b + 108) == mp_save_crc32(b, 108); }

// The game starting: keepsafe's start-up first (mp_port_boot), then the save layer's single recovery path for the Team File (MpSaveGame) and
// the account (mp_service), then the settings loaded (their loaders read .bak when a file is missing).
static void startGame(const char* version) {
    wire();
    mp_keepsafe_boot(version, "t");
    bool changed = false;
    mp_save_recover(gP.teamFile, &changed);
    if (!exists(accountPath())) mp_save_recover_with(accountPath().c_str(), accountValid, &changed);
    mp_config_set_dir(gP.mpDir);
    mp_config_load();
    mp_ui_flow_forget();
}

// ---- states ----------------------------------------------------------------------------------------------------------------------
static bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && !s.compare(s.size() - n, n, e); }
static bool hasPrefix(const std::string& s, const char* p) { return !s.compare(0, strlen(p), p); }
static Tree liveState() {   // mp/ and online/ ("mp/...", "online/..."), with the settings read as their loaders read them (.mpbak, else .bak, when missing)
    Tree t = dataSet();
    for (const char* n : {"mp/mp.cfg", "mp/flow.cfg", "mp/coop.cfg"})
        for (const char* suf : {".mpbak", ".bak"})   // flow.cfg / coop.cfg: mp_ui_flow's ReadKeys reads "", ".mpbak", ".bak" in that order (the save layer's atomic writer keeps the old file as .mpbak, since the menuflow / smallfix2 change); mp.cfg: .bak
            if (!t.count(n) && t.count(std::string(n) + suf)) t[n] = t[std::string(n) + suf];
    return t;
}
static Tree rawState() {   // everything outside the single-player folder: "live/..." "bk/..." and the stamp
    Tree t, bkt = treeOf(bk());
    for (auto& kv : dataSet()) t["live/" + kv.first] = kv.second;
    for (auto& kv : bkt) t["bk/" + kv.first] = kv.second;
    for (const char* s : {"", ".mpbak", ".mptmp"}) {
        Bytes b = readFile(stampPath() + s);
        if (!b.empty()) t[std::string("stamp") + s] = b;
    }
    return t;
}
static bool isTemp(const std::string& p) { return endsWith(p, ".tmp") || endsWith(p, ".mptmp") || endsWith(p, ".unfinished"); }
static bool isSecondary(const std::string& p) {   // files a writer keeps beside the primary: may be absent
    return endsWith(p, ".mpbak") || endsWith(p, ".bak") || p.find(".prev") != std::string::npos || p.find(".mpbad") != std::string::npos ||
           endsWith(p, ".unreadable") || endsWith(p, "keepsafe-own.txt");   // (the account store reads its own list through the save layer's recovery)
}
static bool contains(const Bytes& hay, const Bytes& needle) { return !needle.empty() && std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end(); }
// Is `want` held by a copy that verifies (what the lists show), as a file of a copy folder or inside a .smgmp?
static bool inVerifiedCopy(const Bytes& want) {
    MpKsCopy c[64];
    const int n = mp_keepsafe_list_copies(c, 64);
    for (int i = 0; i < n; i++) {
        const std::string p = bk() + "/" + c[i].name;
        if (isDir(p)) {
            for (auto& kv : treeOf(p))
                if (kv.second == want) return true;
        } else if (contains(readFile(p), want)) {
            return true;
        }
    }
    return false;
}
static bool inPrev(const Bytes& want) {
    for (const char* s : {".prev", ".prev.1", ".prev.2"})
        if (readFile(accountPath() + s) == want) return true;
    return false;
}

struct Scn {
    const char* name;
    std::function<void()> setup;
    std::function<int()> op;
    int expectRc;                       // the uncut operation's result (-1: any of OK / NOTHING)
    bool prune = false;                 // the operation itself removes copies (only those the uncut run removed may go)
    bool needCopy = false;              // a changed Team File / settings file must be in a verified copy (account: .prev)
    bool foreignAccount = false;        // the account this operation installs is a foreign one: never kept in .prev
    bool afterNeedsVersionCopy = false; // after the restart a verified version copy of the live data exists (the retry)
};
static int gCuts = 0, gChecked = 0, gViolations = 0;
static std::map<std::string, int> gPerOp[3];
static void violation(const char* scn, int k, int torn, const char* fmt, ...) {
    gViolations++;
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "VIOLATION %s: cut before call %d%s: %s\n", scn, k, torn == 2 ? " (disk full)" : torn ? " (torn)" : "", b);
}

// fork; the child runs `op` and dies before its k-th changing call (torn 2: that call fails with ENOSPC and the child runs on). Exit code: 10/11/12
// died there; 13 the call failed and the operation ended; 1 finished before reaching k; 2 crashed.
static int runCut(const std::function<int()>& op, int k, int torn) {
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        gCount = 0;
        gArm = k;
        gFail = torn == 2;
        gTorn = torn == 1;
        op();
        gArm = 0;
        _exit(gFired ? 13 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

static Bytes gPlayedTeam;
static std::string Parent2(const std::string& p) { return p.substr(0, p.find_last_of('/')); }
static void checkAfterCut(const Scn& s, int k, int torn, const Tree& B, const Tree& A, const Tree& rawB, const Tree& rawA) {
    // (1a) right after the cut, before anything starts: the copies folder and the stamp
    const Tree raw = rawState();
    for (auto& kv : rawB) {
        if (!hasPrefix(kv.first, "bk/")) continue;
        auto it = raw.find(kv.first);
        if (it != raw.end()) {
            if (it->second != kv.second) violation(s.name, k, torn, "a copy's file changed: %s", kv.first.c_str());
            continue;
        }
        if (!(s.prune && !rawA.count(kv.first))) {   // only what the uncut operation itself removed may go
            violation(s.name, k, torn, "a copy's file is gone: %s", kv.first.c_str());
            continue;
        }
        const size_t slash = kv.first.find('/', 3);
        if (slash == std::string::npos) continue;   // (a copy file: removed whole)
        const std::string top = kv.first.substr(0, slash);   // a copy folder: only behind its unfinished mark, or all of it gone
        bool anyLeft = false;
        for (auto& r : raw)
            if (hasPrefix(r.first, (top + "/").c_str())) anyLeft = true;
        if (anyLeft && !raw.count(top + "/.unfinished")) violation(s.name, k, torn, "a copy folder is being removed without its unfinished mark: %s", top.c_str());
    }
    const std::string stB = S_(rawB.count("stamp") ? rawB.at("stamp") : Bytes()), stA = S_(rawA.count("stamp") ? rawA.at("stamp") : Bytes());
    if (raw.count("stamp") && S_(raw.at("stamp")) != stB && S_(raw.at("stamp")) != stA) violation(s.name, k, torn, "the stamp is neither its old nor its new state");
    // (1b) the game starts
    startGame("3.0.0");
    const Tree now = liveState();
    for (auto& kv : now) {
        const std::string& p = kv.first;
        if (isTemp(p)) continue;
        const bool inB = B.count(p) > 0, inA = A.count(p) > 0;
        if (!inB && !inA) {
            violation(s.name, k, torn, "a stray file: %s", p.c_str());
            continue;
        }
        if (!((inB && kv.second == B.at(p)) || (inA && kv.second == A.at(p)))) violation(s.name, k, torn, "%s is neither before nor after", p.c_str());
    }
    for (auto& kv : B) {
        const std::string& p = kv.first;
        if (isTemp(p) || isSecondary(p) || now.count(p)) continue;
        if (A.count(p)) violation(s.name, k, torn, "%s is missing (it is there before and after)", p.c_str());
    }
    gChecked += (int)now.size();
    // (2) what a completed change replaced has a way back
    if (s.needCopy) {
        for (const char* p : {"mp/saves/title/00010000/524d4750/data/GameData.bin", "mp/mp.cfg", "mp/flow.cfg", "mp/coop.cfg"})
            if (B.count(p) && now.count(p) && now.at(p) != B.at(p) && !inVerifiedCopy(B.at(p))) violation(s.name, k, torn, "%s was replaced and its old bytes are in no verified copy", p);
        const std::string acct = "online/online.dat";
        if (B.count(acct) && now.count(acct) && now.at(acct) != B.at(acct) && !inPrev(B.at(acct)) && !inVerifiedCopy(B.at(acct)))
            violation(s.name, k, torn, "the old account was replaced and is not kept");
    }
    if (s.foreignAccount && B.count("online/online.dat") && now.count("online/online.dat")) {
        // the console's own credential is kept; nothing foreign is ever rotated into .prev
        const Bytes foreign = makeAccount(500, 'f');
        if (inPrev(foreign)) violation(s.name, k, torn, "a foreign credential was rotated into .prev");
    }
    if (s.afterNeedsVersionCopy) {
        bool held = false;
        const Bytes team = now.count("mp/saves/title/00010000/524d4750/data/GameData.bin") ? now.at("mp/saves/title/00010000/524d4750/data/GameData.bin") : Bytes();
        MpKsCopy c[64];
        const int n = mp_keepsafe_list_copies(c, 64);
        for (int i = 0; i < n; i++)
            if (c[i].kind == MP_KS_COPY_VERSION && !team.empty())
                for (auto& kv : treeOf(bk() + "/" + c[i].name))
                    if (kv.second == team) held = true;
        if (!held) violation(s.name, k, torn, "no verified version copy holds the Team File after the restart");
    }
    // (3) play on: the game saves through the save layer and the settings writers, then starts twice more
    {
        gPlayedTeam = teamSlot(2, 33, false, 9);
        mp_save_mkdirs(Parent2(gP.teamFile).c_str());
        mp_save_write_atomic(gP.teamFile, gPlayedTeam.data(), (uint32_t)gPlayedTeam.size());
        MpConfig c = *mp_config();
        snprintf(c.name, sizeof c.name, "PlayedOn");
        mp_config_set(&c);
        mp_config_save();
        MpFlowStore f;
        mp_flow_store_get(&f);
        f.lastCoop = 5;
        mp_flow_store_set(&f);
        const Bytes reg = makeAccount(900, 'q');
        mp_keepsafe_account_store(accountPath().c_str(), reg.data(), 112);
    }
    for (const char* v : {"3.0.0", "9.9.9"}) {
        startGame(v);
        const Tree t = liveState();
        const std::string team = "mp/saves/title/00010000/524d4750/data/GameData.bin";
        if (!t.count(team) || t.at(team) != gPlayedTeam) violation(s.name, k, torn, "the Team File the game saved after the restart is gone at start %s", v);
        if (!t.count("mp/mp.cfg") || S_(t.at("mp/mp.cfg")).find("name = PlayedOn") == std::string::npos) violation(s.name, k, torn, "mp.cfg saved after the restart is gone at start %s", v);
        if (!t.count("mp/flow.cfg") || S_(t.at("mp/flow.cfg")).find("last_coop = 5") == std::string::npos) violation(s.name, k, torn, "flow.cfg saved after the restart is gone at start %s", v);
        if (!t.count("online/online.dat") || t.at("online/online.dat") != makeAccount(900, 'q')) violation(s.name, k, torn, "the account saved after the restart is gone at start %s", v);
    }
    const int rc = s.op();   // the operation again: not busy for ever, not refused for ever
    if (rc == MP_KS_BUSY || rc == MP_KS_IO) violation(s.name, k, torn, "the operation fails after the restart: %s", mp_keepsafe_result_name(rc));
    if (s.foreignAccount && inPrev(makeAccount(500, 'f'))) violation(s.name, k, torn, "a foreign credential was rotated into .prev (after the restart)");
    // (4) the single-player folder
    if (checkSpPrints()) violation(s.name, k, torn, "the single-player save folder changed");
}

static void cutAll(const Scn& s) {
    const char* only = getenv("KS_CUT_ONLY");
    if (only && !strstr(s.name, only)) return;
    s.setup();
    const Tree B = liveState(), rawB = rawState();
    const int rc0 = s.op();
    const Tree A = liveState(), rawA = rawState();
    if (s.expectRc >= 0 ? rc0 != s.expectRc : (rc0 != MP_KS_OK && rc0 != MP_KS_NOTHING)) {
        violation(s.name, 0, 0, "the uncut operation returned %s", mp_keepsafe_result_name(rc0));
        return;
    }
    const int before = gViolations, c0 = gCuts, ch0 = gChecked;
    int points = 0, tornPoints = 0, fullPoints = 0;
    for (int k = 1; k < 4000; k++) {
        s.setup();
        const int r = runCut(s.op, k, 0);
        if (r == 1) break;
        if (r == 2) {
            violation(s.name, k, 0, "the child crashed");
            break;
        }
        points++;
        gCuts++;
        checkAfterCut(s, k, 0, B, A, rawB, rawA);
        if (r == 11) {   // a cut at a write: again with half of the bytes written
            s.setup();
            if (runCut(s.op, k, 1) == 12) {
                tornPoints++;
                gCuts++;
                checkAfterCut(s, k, 1, B, A, rawB, rawA);
            }
        }
        s.setup();   // the same call fails with "disk full" and the operation goes on
        const int rf = runCut(s.op, k, 2);
        if (rf == 13) {
            fullPoints++;
            gCuts++;
            checkAfterCut(s, k, 2, B, A, rawB, rawA);
        } else if (rf == 2) {
            violation(s.name, k, 2, "the child crashed after a failed call");
        }
    }
    printf("  %-22s cut points %4d  torn %3d  disk full %3d  files checked %6d  violations %d\n", s.name, points, tornPoints, fullPoints, gChecked - ch0, gViolations - before);
    (void)c0;
    fflush(stdout);
}

// ---- operations --------------------------------------------------------------------------------------------------------------------
static const std::string kTeam = "mp/saves/title/00010000/524d4750/data/GameData.bin";
static std::string gCopyName, gBeforeName, gVersionCopy;
// a made copy (file) holding `team` (and the account 500 'f' when asked), placed in this world's copies folder; the world is restored after
static std::string copyHolding(const Bytes& team, bool account) {
    const MpKsPaths keep = gP;
    const std::string keepRoot = gWorldRoot, keepDir = gP.mpDir;
    gWorldRoot.clear();   // (the scratch world is removed by hand: dropWorld() must not remove this one)
    newWorld("src");
    writeFile(gP.teamFile, team);
    if (account) writeFile(std::string(gP.onlineDir) + "/online.dat", makeAccount(500, 'f'));
    wire();
    char name[96];
    mp_keepsafe_make_copy(account ? 0 : MP_KS_COPY_NO_ACCOUNT, name, sizeof name);
    const Bytes f = readFile(std::string(gP.dataDir) + "/mp-backups/" + name);
    const std::string srcRoot = std::string(gP.dataDir).substr(0, std::string(gP.dataDir).size() - 5);
    gSpPrint.erase(std::string(gP.dataDir) + "/saves");
    rmrf(srcRoot);
    gP = keep;
    gWorldRoot = keepRoot;
    mp_keepsafe_set_paths(&gP);
    mp_config_set_dir(keepDir.c_str());
    wire();
    const std::string unique = "copy-20231114-" + std::to_string(500 + gCopyN++) + ".smgmp";   // (each scratch world numbers its copy from 1)
    writeFile(bk() + "/" + unique, f);
    return unique;
}
static const Bytes& mineTeam() { static Bytes b = teamSlot(2, 10, false); return b; }
static const Bytes& otherTeam() { static Bytes b = teamSlot(2, 10, true, 3); return b; }

static void setupSlotInUse() {
    cutWorld("slot-in-use");
    startGame("3.0.0");
    saveAtomic(gP.teamFile, mineTeam());
    gCopyName = copyHolding(otherTeam(), false);
}
static int opSlotInUse() { return mp_keepsafe_bring_back_slot(gCopyName.c_str(), 2, 1, nullptr, 0); }
static void setupSlotEmpty() {
    cutWorld("slot-empty");
    startGame("3.0.0");
    saveAtomic(gP.teamFile, mineTeam());
    gCopyName = copyHolding(teamSlot(3, 12), false);
}
static int opSlotEmpty() { return mp_keepsafe_bring_back_slot(gCopyName.c_str(), 3, 0, nullptr, 0); }
static void setupSlotNoTeam() {
    setupSlotEmpty();
    unlink(gP.teamFile);
    unlink((std::string(gP.teamFile) + ".mpbak").c_str());
}
static void setupSlotOnly() {   // no Team File here; the copy has all six files in use: only slot 3 is written, into a new file (keepfix 5)
    cutWorld("slot-only");
    startGame("3.0.0");
    int st[6] = {10, 20, 30, 40, 45, 50};
    gCopyName = copyHolding(makeTeam(st), false);
    unlink(gP.teamFile);
    unlink((std::string(gP.teamFile) + ".mpbak").c_str());
}
static void setupSlotUndo() {
    setupSlotInUse();
    char before[96];
    mp_keepsafe_bring_back_slot(gCopyName.c_str(), 2, 1, before, sizeof before);
    gBeforeName = before;
}
static int opSlotUndo() { return mp_keepsafe_bring_back_slot(gBeforeName.c_str(), 2, 1, nullptr, 0); }

static void setupAccount() {
    cutWorld("account");
    startGame("3.0.0");
    // this console's own credentials: 100 then 300 (a re-registration), through the store (the own list, .prev)
    mp_keepsafe_account_store(accountPath().c_str(), makeAccount(100, 'a').data(), 112);
    mp_keepsafe_account_store(accountPath().c_str(), makeAccount(300, 'c').data(), 112);
    saveAtomic(gP.teamFile, mineTeam());
    gCopyName = copyHolding(teamSlot(2, 5), true);
}
static int opAccount() { return mp_keepsafe_use_account(gCopyName.c_str(), 1); }
static void setupAccountBack() {   // a foreign account is in use (brought from a copy); this console's own copy brings its own back
    setupAccount();
    char own[96];
    mp_keepsafe_make_copy(0, own, sizeof own);   // (made with its own credential 300)
    mp_keepsafe_use_account(gCopyName.c_str(), 1);
    gCopyName = own;
}
static void setupAccountStore() {
    cutWorld("account-store");
    startGame("3.0.0");
    for (int pid : {100, 200, 300}) mp_keepsafe_account_store(accountPath().c_str(), makeAccount(pid, (char)('a' + pid / 100)).data(), 112);
}
static int opAccountStore() { return mp_keepsafe_account_store(accountPath().c_str(), makeAccount(400, 'd').data(), 112) ? MP_KS_OK : MP_KS_IO; }

static void setupSettings() {
    cutWorld("settings");
    startGame("3.0.0");   // the version copy holds the settings as they are
    gVersionCopy.clear();
    MpKsCopy c[8];
    const int n = mp_keepsafe_list_copies(c, 8);
    for (int i = 0; i < n; i++)
        if (c[i].kind == MP_KS_COPY_VERSION) gVersionCopy = c[i].name;
    writeFile(mpPath("mp.cfg"), B(cfgText("Changed", "00112233445566778899aabbccddeeff")));
    writeFile(mpPath("flow.cfg"), B("# menus\nreach = 0\n"));
    writeFile(mpPath("coop.cfg"), B("# who\nfile = 5\n"));
    mp_config_load();
    mp_ui_flow_forget();
}
static int opSettings() { return mp_keepsafe_bring_back_settings(gVersionCopy.c_str(), MP_KS_SET_CONFIG | MP_KS_SET_FLOW | MP_KS_SET_COOP, nullptr, 0); }
static void setupSettingsUndo() {
    setupSettings();
    char before[96];
    mp_keepsafe_bring_back_settings(gVersionCopy.c_str(), 7, before, sizeof before);
    gBeforeName = before;
}
static int opSettingsUndo() { return mp_keepsafe_bring_back_settings(gBeforeName.c_str(), 7, nullptr, 0); }

static void setupVersionCopy() { cutWorld("version-copy"); }
static int opVersionCopy() {
    mp_keepsafe_test_reset();
    mp_keepsafe_boot("3.0.0", "t");
    return MP_KS_OK;
}
static void setupPrune() {   // twelve updates of unchanged data: nine pinned copies beyond the newest three; the next update drops the oldest
    cutWorld("prune");
    for (int v = 0; v < 11; v++) {
        mp_keepsafe_test_reset();
        mp_keepsafe_boot(("3." + std::to_string(v) + ".0").c_str(), "t");
    }
}
static int opPrune() {
    mp_keepsafe_test_reset();
    mp_keepsafe_boot("3.11.0", "t");
    return MP_KS_OK;
}
static void setupBeforePrune() {   // sixteen "before" copies exist: the seventeenth drops the oldest
    setupSlotInUse();
    for (int i = 0; i < 16; i++) {
        mp_keepsafe_test_set_time(1700000000 + i + 1);
        char before[96];
        const std::string c = copyHolding(i % 2 ? mineTeam() : otherTeam(), false);
        if (mp_keepsafe_bring_back_slot(c.c_str(), 2, 1, before, sizeof before) != MP_KS_OK) fprintf(stderr, "setup: bring back %d failed\n", i);
    }
    mp_keepsafe_test_set_time(1700000100);
    gCopyName = copyHolding(teamSlot(2, 40, false, 5), false);
}
static int opBeforePrune() { return mp_keepsafe_bring_back_slot(gCopyName.c_str(), 2, 1, nullptr, 0); }
static void setupMake() {
    cutWorld("make-copy");
    startGame("3.0.0");
}
static int opMake() { char n[96]; return mp_keepsafe_make_copy(0, n, sizeof n); }

// A failed listing of the copies folder, a stale counter: a name that exists is never used, nothing is removed (review 3 #7). A folder copy and a
// copy file both sit at number 1; the counter is back at 0 (a new process on an older game's stamp) and the listing fails: the next copy of each
// kind first tries number 1.
static void clashTest() {
    const char* only = getenv("KS_CUT_ONLY");
    if (only && !strstr("clash", only)) return;
    cutWorld("clash");
    startGame("3.0.0");   // the folder copy 3.0.0-<date>-001
    char name[96];
    CHECK(mp_keepsafe_make_copy(0, name, sizeof name) == MP_KS_OK);   // copy-<date>-002.smgmp, put at number 1 by hand
    CHECK(rename((bk() + "/" + name).c_str(), (bk() + "/copy-20231114-001.smgmp").c_str()) == 0);
    const Tree before = treeOf(bk());
    CHECK(before.size() > 4);
    saveAtomic(gP.teamFile, teamSlot(2, 31));
    wire();
    gFailOpendir = "/mp-backups";
    const int rc = mp_keepsafe_make_copy(0, name, sizeof name);   // the file at number 1 exists
    writeFile(stampPath(), B("keepsafe = 2\nversion = 3.1.0\nseq = 0\n"));
    unlink((stampPath() + ".mpbak").c_str());
    wire();
    mp_keepsafe_boot("3.0.0", "t");   // a newer game's stamp: a copy again; the folder at number 1 exists
    gFailOpendir = nullptr;
    const Tree after = treeOf(bk());
    int lost = 0;
    for (auto& kv : before)
        if (!after.count(kv.first) || after.at(kv.first) != kv.second) lost++;
    CHECKM(lost == 0, "%d files of earlier copies lost or changed by copies made with a failed listing (make a copy: %s)", lost, mp_keepsafe_result_name(rc));
    CHECK(after.size() > before.size());   // (and the new copies are there: fresh names were taken)
    wire();
    CHECK(checkSpPrints() == 0);
    printf("  %-22s a failed listing, a stale counter: nothing of an earlier copy lost (%d files kept, %d now)\n", "clash", (int)before.size(), (int)after.size());
}

int main() {
    if (!loadGameData()) {
        fprintf(stderr, "keepsafe_cut_test: no valid GameData.bin (set KS_GAMEDATA): SKIPPED\n");
        return 77;
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/dev/shm/ksc_%d", (int)getpid());
    gRoot = getenv("KS_TMP") ? std::string(getenv("KS_TMP")) + "_cut" : tmp;
    rmrf(gRoot);
    mkdirs(gRoot);
    printf("keepsafe_cut_test: power cuts before every file-changing call (and torn writes), restart, play on, restart twice\n");
    const Scn scns[] = {
        {"version-copy", setupVersionCopy, opVersionCopy, -1, false, false, false, true},
        {"prune", setupPrune, opPrune, -1, true, false, false, true},
        {"make-copy", setupMake, opMake, MP_KS_OK},
        {"slot-in-use", setupSlotInUse, opSlotInUse, MP_KS_OK, false, true},
        {"slot-empty", setupSlotEmpty, opSlotEmpty, MP_KS_OK},
        {"slot-no-team-file", setupSlotNoTeam, opSlotEmpty, MP_KS_OK},
        {"slot-only", setupSlotOnly, opSlotEmpty, MP_KS_OK},
        {"slot-undo", setupSlotUndo, opSlotUndo, MP_KS_OK, false, true},
        {"before-copies-prune", setupBeforePrune, opBeforePrune, MP_KS_OK, true, true},
        {"use-account", setupAccount, opAccount, MP_KS_OK, false, true, true},
        {"use-account-back", setupAccountBack, opAccount, MP_KS_OK, false, true, true},
        {"account-store", setupAccountStore, opAccountStore, MP_KS_OK, false, true},
        {"settings", setupSettings, opSettings, MP_KS_OK, false, true},
        {"settings-undo", setupSettingsUndo, opSettingsUndo, MP_KS_OK, false, true},
    };
    for (const Scn& s : scns) cutAll(s);
    clashTest();
    dropWorld();
    rmrf(gRoot);
    printf("keepsafe_cut_test: %d cut points, %d live files checked, %d violations, %d other checks failed\n", gCuts, gChecked, gViolations, sFails);
    return gViolations || sFails ? 1 : 0;
}
