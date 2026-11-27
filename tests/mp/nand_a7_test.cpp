// A7 (nand.cpp + mp_savefile.cpp): (1) a plain boot does NOT restore a missing GameData.bin from .mpbak ALONE (smallfix: no .mptmp = no interrupted write: the player deleted
// the save outside the game; it is left as it is), and does restore with a .mptmp beside it, (2) NANDDelete takes the file's .mpbak / .mptmp with it (a deliberately deleted save must not
// come back at the next boot) and never touches .mpbad. Built and run by tests/mp/savelife_build.sh (ASan/UBSan). Reads the test save only.
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "port/mp_proto.h"
#include "mp_savefile.h"

extern "C" {
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
void port_host_job_wait(const char*, void (*fn)(void*), void* arg) { fn(arg); }
void port_log(const char*, ...) {}
void port_gpu_memory_written(void*, uint32_t) {}
uint8_t* __PortGXFifoPtr; uint8_t* __PortGXFifoLimit; int __PortGXFifoBatch;
void port_nand_set_root(const char* root);
int NANDDelete(const char* path);
void port_nand_set_atomic_writes(bool on);
int NANDCreate(const char* path, unsigned char perm, unsigned char attr);
struct CostNandInfo { int a, b; char c[64]; char d[64]; unsigned char e, f, g; };
int NANDOpen(const char* path, CostNandInfo* info, unsigned char mode);
int NANDWrite(CostNandInfo* info, const void* buf, unsigned length);
int NANDClose(CostNandInfo* info);
}

typedef std::vector<uint8_t> Bytes;
static int sFails = 0, sChecks = 0;
#define CHECK(c) do { sChecks++; if (!(c)) { sFails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool readAll(const std::string& p, Bytes& b) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    b.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return true;
}
static void writeAll(const std::string& p, const Bytes& b) { FILE* f = fopen(p.c_str(), "wb"); if (!b.empty()) fwrite(b.data(), 1, b.size(), f); fclose(f); }
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

static std::string gRoot, gData;
static void fresh() {
    std::string c = "rm -rf '" + gRoot + "' && mkdir -p '" + gData + "'";
    if (system(c.c_str()) != 0) exit(2);
}
// the case runs in its own process: port_nand_set_root repairs only at its first call
static int inChild(void (*fn)(const Bytes&), const Bytes& img) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { fn(img); _exit(sFails ? 1 : 0); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static void bootBak(const Bytes& img) {            // only .mpbak: a deliberate deletion, nothing is restored, nothing is changed
    fresh();
    writeAll(gData + "/GameData.bin.mpbak", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(!exists(gData + "/GameData.bin"));
    CHECK(readAll(gData + "/GameData.bin.mpbak", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
}
static void bootTmpOnly(const Bytes& img) {        // only a valid .mptmp (cut before the old file moved, or the first save): completed
    fresh();
    writeAll(gData + "/GameData.bin.mptmp", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
}
static void bootBakValidTmp(const Bytes& img) {    // the cut between the two renames: .mpbak = old, .mptmp = new: the new one wins
    fresh();
    Bytes old = img;
    old[0x9000] ^= 1;
    writeAll(gData + "/GameData.bin.mpbak", old);
    writeAll(gData + "/GameData.bin.mptmp", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img);
}
static void bootStaleTmp(const Bytes& img) {       // GameData.bin intact + a stale .mptmp (a cut at steps 1-4): the .mptmp goes, so a later deletion restores nothing
    fresh();
    writeAll(gData + "/GameData.bin", img);
    Bytes other = img;
    other[0x9000] ^= 1;
    writeAll(gData + "/GameData.bin.mptmp", other);
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
}
static void bootTornPrimaryGoodTmp(const Bytes& img) {   // GameData.bin torn, a complete .mptmp: the good copy is NOT deleted; the torn file is kept aside
    fresh();
    Bytes torn(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin", torn);
    writeAll(gData + "/GameData.bin.mptmp", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got, aside;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
    CHECK(readAll(gData + "/GameData.bin.mpbad", aside) && aside == torn);
}
static void bootTornPrimaryGoodBak(const Bytes& img) {   // GameData.bin torn, a valid .mpbak, no .mptmp: offered back (the .mpbak is not the only copy left to delete)
    fresh();
    Bytes torn(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin", torn);
    writeAll(gData + "/GameData.bin.mpbak", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got, aside;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img);
    CHECK(readAll(gData + "/GameData.bin.mpbad", aside) && aside == torn);
}
static void bootValidPrimaryBakKept(const Bytes& img) {  // GameData.bin intact + a .mpbak: nothing changes
    fresh();
    Bytes old = img;
    old[0x9000] ^= 1;
    writeAll(gData + "/GameData.bin", img);
    writeAll(gData + "/GameData.bin.mpbak", old);
    port_nand_set_root(gRoot.c_str());
    Bytes got, bak;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && readAll(gData + "/GameData.bin.mpbak", bak) && bak == old);
}
// the restore from a .mpbak puts an empty .mptmp first (the "repair in progress" marker); the states a cut leaves are finished by the boot repair
static void bootMarkerTornPrimary(const Bytes& img) {    // cut after the marker, before the torn file moved aside
    fresh();
    Bytes torn(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin", torn);
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    writeAll(gData + "/GameData.bin.mpbak", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
}
static void bootMarkerNoPrimary(const Bytes& img) {      // cut after the torn file moved aside, before the .mpbak renamed in
    fresh();
    Bytes torn(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin.mpbad", torn);
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    writeAll(gData + "/GameData.bin.mpbak", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got, aside;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
    CHECK(readAll(gData + "/GameData.bin.mpbad", aside) && aside == torn);
}
static void recoverLeavesNoMarker(const Bytes& img) {    // the whole restore (no cut): the marker is gone, the torn file kept aside
    fresh();
    Bytes torn(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin", torn);
    writeAll(gData + "/GameData.bin.mpbak", img);
    bool changed = false;
    CHECK(mp_save_recover((gData + "/GameData.bin").c_str(), &changed) && changed);
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp") && exists(gData + "/GameData.bin.mpbad"));
}
// a deleted save stays deleted: creating a new GameData.bin beside a lone .mpbak moves the .mpbak aside (never deleted, never restored)
static void createAsidesLoneBak(const Bytes& img) {
    fresh();
    port_nand_set_root(gRoot.c_str());
    writeAll(gData + "/GameData.bin.mpbak", img);
    CHECK(NANDCreate("GameData.bin", 0, 0) == 0);
    Bytes old;
    CHECK(exists(gData + "/GameData.bin") && !exists(gData + "/GameData.bin.mpbak") && readAll(gData + "/GameData.bin.mpbak.old", old) && old == img);
    unlink((gData + "/GameData.bin").c_str());
    Bytes img2 = img; img2[0x9000] ^= 1;
    writeAll(gData + "/GameData.bin.mpbak", img2);
    CHECK(NANDCreate("GameData.bin", 0, 0) == 0);
    CHECK(readAll(gData + "/GameData.bin.mpbak.old.1", old) && old == img2 && readAll(gData + "/GameData.bin.mpbak.old", old) && old == img);
}
static void atomicOpenAsidesLoneBak(const Bytes& img) {   // the safe writer's first save (NANDOpen of the .mptmp) does the same
    fresh();
    port_nand_set_root(gRoot.c_str());
    port_nand_set_atomic_writes(true);
    writeAll(gData + "/GameData.bin.mpbak", img);
    CostNandInfo info;
    CHECK(NANDOpen("GameData.bin", &info, 2) == 0);
    Bytes old;
    CHECK(!exists(gData + "/GameData.bin.mpbak") && readAll(gData + "/GameData.bin.mpbak.old", old) && old == img);
    NANDWrite(&info, img.data(), (unsigned)img.size());
    CHECK(NANDClose(&info) == 0);
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mpbak"));   // (no old file to keep: no .mpbak)
}
static void failedRestoreKeepsBak(const Bytes& img) {   // no file + empty marker + .mpbak (a failed restore): atomic saves never delete that .mpbak
    fresh();
    port_nand_set_root(gRoot.c_str());
    port_nand_set_atomic_writes(true);
    Bytes oldCopy = img; oldCopy.push_back(0x5a);   // (the player's old save, told apart from what the saves write)
    writeAll(gData + "/GameData.bin.mpbak", oldCopy);
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    const Bytes& img2 = img;
    for (int save = 0; save < 3; save++) {
        CostNandInfo info;
        CHECK(NANDOpen("GameData.bin", &info, 2) == 0);
        NANDWrite(&info, img2.data(), (unsigned)img2.size());
        CHECK(NANDClose(&info) == 0);
    }
    Bytes old, got;
    CHECK(readAll(gData + "/GameData.bin.mpbak.old", old) && old == oldCopy);
    CHECK(readAll(gData + "/GameData.bin", got) && got == img2 && !exists(gData + "/GameData.bin.mptmp"));
}
static void readOpenRetriesRestore(const Bytes& img) {   // no file + empty marker + valid .mpbak: a READ-open restores it (same session); a write-open does not
    fresh();
    port_nand_set_root(gRoot.c_str());
    writeAll(gData + "/GameData.bin.mpbak", img);
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    CostNandInfo info;
    CHECK(NANDOpen("GameData.bin", &info, 1) == 0);
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp"));
    NANDClose(&info);
}
static void readOpenNoRetryNonEmptyTmp(const Bytes& img) {   // savelife review: a non-empty .mptmp beside the missing file IS retried at the read-open (the boot repair may not have been able to read it): torn .mptmp + valid .mpbak = restored from the .mpbak
    fresh();
    port_nand_set_root(gRoot.c_str());
    writeAll(gData + "/GameData.bin.mpbak", img);
    Bytes half(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin.mptmp", half);
    CostNandInfo info;
    CHECK(NANDOpen("GameData.bin", &info, 1) == 0);
    Bytes t;
    CHECK(readAll(gData + "/GameData.bin", t) && t == img && !exists(gData + "/GameData.bin.mptmp"));
    NANDClose(&info);
}
static void readOpenRetriesOnce(const Bytes& img) {   // the retry happens once per run: after a failed one, a second read-open does not try again
    fresh();
    port_nand_set_root(gRoot.c_str());
    Bytes torn(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin.mpbak", torn);   // (not restorable: the first attempt fails)
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    CostNandInfo info;
    CHECK(NANDOpen("GameData.bin", &info, 1) != 0);
    writeAll(gData + "/GameData.bin.mpbak", img);    // now restorable, marker back
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    CHECK(NANDOpen("GameData.bin", &info, 1) != 0);
    CHECK(!exists(gData + "/GameData.bin") && exists(gData + "/GameData.bin.mptmp") && exists(gData + "/GameData.bin.mpbak"));
}
static void writeOpenDoesNotRestore(const Bytes& img) {   // a write-open never restores: the .mpbak is kept aside, no GameData.bin appears
    fresh();
    port_nand_set_root(gRoot.c_str());
    port_nand_set_atomic_writes(true);
    writeAll(gData + "/GameData.bin.mpbak", img);
    writeAll(gData + "/GameData.bin.mptmp", Bytes());
    CostNandInfo info;
    CHECK(NANDOpen("GameData.bin", &info, 2) == 0);
    Bytes b;
    CHECK(!exists(gData + "/GameData.bin") && readAll(gData + "/GameData.bin.mpbak.old", b) && b == img);
}
static void createKeepsInterruptedBak(const Bytes& img) {   // a .mptmp beside the missing file = an interrupted save: the .mpbak stays
    fresh();
    port_nand_set_root(gRoot.c_str());
    writeAll(gData + "/GameData.bin.mpbak", img);
    writeAll(gData + "/GameData.bin.mptmp", img);
    CHECK(NANDCreate("GameData.bin", 0, 0) == -6);   // (savelife review: the repair puts the valid .mptmp in place; the create itself then finds the file there: EXISTS)
    Bytes t;
    CHECK(exists(gData + "/GameData.bin.mpbak") && !exists(gData + "/GameData.bin.mpbak.old") && readAll(gData + "/GameData.bin", t) && t == img);
}
// savelife review, boot-freeze way out. The .mptmp is made UNREADABLE by being a directory (open works, read fails with EISDIR: a persistent read
// error). gPhase: 1 = first boot, 2 = a later boot with the error still there, 3 = a later boot after the error went away (the directory replaced by the file).
static int gPhase = 0;
static void setupFreeze(const Bytes& img) {
    fresh();
    writeAll(gData + "/GameData.bin.mpbak", img);   // the previous version
    mkdir((gData + "/GameData.bin.mptmp").c_str(), 0770);
    if (gPhase == 4) writeAll(gData + "/GameData.bin.mptmp.unread", Bytes{1, 2, 3});   // an earlier set-aside file already there
    if (gPhase == 5) { unlink((gData + "/GameData.bin.mpbak").c_str()); mkdir((gData + "/GameData.bin.mpbak").c_str(), 0770); }   // the .mpbak unreadable too
    if (gPhase == 6) unlink((gData + "/GameData.bin.mpbak").c_str());                                                              // no .mpbak at all
}
static void bootFreezeP1(const Bytes& img) {   // first boot: refuse, set nothing aside, leave the .failed mark; create / write-open / read-open refuse
    setupFreeze(img);
    port_nand_set_root(gRoot.c_str());
    struct stat st;
    CHECK(!exists(gData + "/GameData.bin") && stat((gData + "/GameData.bin.mptmp").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(exists(gData + "/GameData.bin.mptmp.failed") && (gPhase == 4 || !exists(gData + "/GameData.bin.mptmp.unread")) && (gPhase == 6 || exists(gData + "/GameData.bin.mpbak")));
    CostNandInfo info;
    const int rr = NANDOpen("GameData.bin", &info, 1);
    CHECK(rr != 0 && rr != -12);   // a read error, never "no file"
    CHECK(NANDCreate("GameData.bin", 0, 0) != 0);
    CHECK(NANDOpen("GameData.bin", &info, 2) != 0);
    CHECK(!exists(gData + "/GameData.bin") && (gPhase == 6 || exists(gData + "/GameData.bin.mpbak")));
}
static void bootFreezeP2(const Bytes& img) {   // a later boot (no fresh): the unreadable .mptmp is renamed to .unread, the .mpbak restored, .failed gone
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    struct stat st;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img);
    if (gPhase != 4) CHECK(stat((gData + "/GameData.bin.mptmp.unread").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(!exists(gData + "/GameData.bin.mptmp") && !exists(gData + "/GameData.bin.mptmp.failed"));
    // the .unread survives later operations: a save, a delete, a repeat of the repair
    if (gPhase == 4) { Bytes u; CHECK(readAll(gData + "/GameData.bin.mptmp.unread", u) && u == Bytes({1, 2, 3}) && exists(gData + "/GameData.bin.mptmp.unread.1")); }
    bool changed = false;
    mp_save_recover((gData + "/GameData.bin").c_str(), &changed);
    CHECK(NANDDelete("GameData.bin") == 0);
    CHECK(exists(gData + "/GameData.bin.mptmp.unread"));
}
static void bootFreezeP3(const Bytes& img) {   // the error went away between the boots: the newest save is restored, nothing set aside, .failed gone
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img);
    CHECK(!exists(gData + "/GameData.bin.mptmp.unread") && !exists(gData + "/GameData.bin.mptmp.failed") && !exists(gData + "/GameData.bin.mptmp"));
}
// boot 2 with the .mpbak unreadable too (gPhase 5) or missing (6): the unreadable files are kept aside by RENAME (.mptmp.unread, .mpbak.unread), nothing is
// deleted, the game starts fresh (read-open: no file), and the set-aside files survive a new save and a delete.
static void bootFreezeP2fresh(const Bytes& img) {
    (void)img;
    port_nand_set_root(gRoot.c_str());
    struct stat st;
    CHECK(stat((gData + "/GameData.bin.mptmp.unread").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    if (gPhase == 5) CHECK(stat((gData + "/GameData.bin.mpbak.unread").c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !exists(gData + "/GameData.bin.mpbak"));
    else CHECK(!exists(gData + "/GameData.bin.mpbak.unread"));
    CHECK(!exists(gData + "/GameData.bin") && !exists(gData + "/GameData.bin.mptmp") && !exists(gData + "/GameData.bin.mptmp.failed"));
    CostNandInfo info;
    CHECK(NANDOpen("GameData.bin", &info, 1) == -12);   // a fresh start, not a read error
    CHECK(NANDCreate("GameData.bin", 0, 0) == 0);
    CHECK(NANDOpen("GameData.bin", &info, 2) == 0);
    Bytes b(100, 7);
    NANDWrite(&info, b.data(), 100);
    CHECK(NANDClose(&info) == 0);
    CHECK(exists(gData + "/GameData.bin.mptmp.unread") && (gPhase != 5 || exists(gData + "/GameData.bin.mpbak.unread")));
    CHECK(NANDDelete("GameData.bin") == 0);
    CHECK(exists(gData + "/GameData.bin.mptmp.unread") && (gPhase != 5 || exists(gData + "/GameData.bin.mpbak.unread")));
}
static void bootStaleFailed(const Bytes& img) {   // a stale .mptmp.failed beside a good GameData.bin is removed at boot
    fresh();
    writeAll(gData + "/GameData.bin", img);
    writeAll(gData + "/GameData.bin.mptmp.failed", Bytes());
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img && !exists(gData + "/GameData.bin.mptmp.failed"));
}
// power cuts on the way out: the state each step leaves; the next boot finishes the job and nothing is lost
static void bootCutState(const Bytes& img) {   // gPhase: 1 no .mptmp (renamed away), 2 only the empty marker, 3 after the .mpbak was renamed away
    fresh();
    writeAll(gData + "/GameData.bin.mptmp.failed", Bytes());
    mkdir((gData + "/GameData.bin.mptmp.unread").c_str(), 0770);
    if (gPhase != 3) writeAll(gData + "/GameData.bin.mpbak", img);
    else mkdir((gData + "/GameData.bin.mpbak.unread").c_str(), 0770);
    if (gPhase >= 2) writeAll(gData + "/GameData.bin.mptmp", Bytes());
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    if (gPhase != 3) CHECK(readAll(gData + "/GameData.bin", got) && got == img);
    else CHECK(!exists(gData + "/GameData.bin"));
    CHECK(!exists(gData + "/GameData.bin.mptmp.failed") && !exists(gData + "/GameData.bin.mptmp"));
    CHECK(exists(gData + "/GameData.bin.mptmp.unread") && (gPhase != 3 || exists(gData + "/GameData.bin.mpbak.unread")));
}
// ---- (end of boot-freeze cases)
// a GameData.bin of a newer build with side files: boot touches nothing
static void bootNewerBuild(const Bytes& img) {
    fresh();
    Bytes newer = img;
    newer[4] = 3;
    mp_save_finalize(newer.data(), (uint32_t)newer.size());
    writeAll(gData + "/GameData.bin", newer);
    writeAll(gData + "/GameData.bin.mptmp", img);
    writeAll(gData + "/GameData.bin.mpbak", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got, t, b;
    CHECK(readAll(gData + "/GameData.bin", got) && got == newer && readAll(gData + "/GameData.bin.mptmp", t) && t == img &&
          readAll(gData + "/GameData.bin.mpbak", b) && b == img && !exists(gData + "/GameData.bin.mpbad"));
}
// the .mpbad rotation at boot: four boots in a row, each with a different torn file and a good .mpbak (a process per boot)
static int gRound = 0;
static Bytes tornN(const Bytes& img, int n) { Bytes t(img.begin(), img.begin() + img.size() / 2); t.back() ^= (uint8_t)n; return t; }
static void bootRotate(const Bytes& img) {
    if (gRound == 1) fresh();
    writeAll(gData + "/GameData.bin", tornN(img, gRound));
    writeAll(gData + "/GameData.bin.mpbak", img);
    port_nand_set_root(gRoot.c_str());
    Bytes got, b0, b1, b2;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img);
    CHECK(readAll(gData + "/GameData.bin.mpbad", b0) && b0 == tornN(img, gRound));
    if (gRound >= 2) CHECK(readAll(gData + "/GameData.bin.mpbad.1", b1) && b1 == tornN(img, gRound - 1));
    if (gRound >= 3) CHECK(readAll(gData + "/GameData.bin.mpbad.2", b2) && b2 == tornN(img, gRound - 2));
    if (gRound == 4) CHECK(!exists(gData + "/GameData.bin.mpbad.3"));   // the oldest (round 1) is dropped on the fourth
    unlink((gData + "/GameData.bin").c_str());
}
static void bootBakTornTmp(const Bytes& img) {     // .mpbak valid, .mptmp torn
    fresh();
    writeAll(gData + "/GameData.bin.mpbak", img);
    Bytes half(img.begin(), img.begin() + img.size() / 2);
    writeAll(gData + "/GameData.bin.mptmp", half);
    port_nand_set_root(gRoot.c_str());
    Bytes got;
    CHECK(readAll(gData + "/GameData.bin", got) && got == img);
}
static void bootNothing(const Bytes&) {            // no files at all: a fresh start, nothing invented
    fresh();
    port_nand_set_root(gRoot.c_str());
    CHECK(!exists(gData + "/GameData.bin"));
}
static void deleteCase(const Bytes& img) {
    fresh();
    port_nand_set_root(gRoot.c_str());                // (first call: boot repair finds nothing to do)
    writeAll(gData + "/GameData.bin", img);
    writeAll(gData + "/GameData.bin.mpbak", img);
    writeAll(gData + "/GameData.bin.mptmp", img);
    writeAll(gData + "/GameData.bin.mpbad", img);
    writeAll(gData + "/GameData.bin.mpbad.1", img);
    writeAll(gData + "/other.bin.mpbak", img);
    NANDDelete("GameData.bin");
    CHECK(!exists(gData + "/GameData.bin") && !exists(gData + "/GameData.bin.mpbak") && !exists(gData + "/GameData.bin.mptmp"));
    CHECK(exists(gData + "/GameData.bin.mpbad") && exists(gData + "/GameData.bin.mpbad.1"));   // kept aside for the player
    CHECK(exists(gData + "/other.bin.mpbak"));                                               // another file's backup is not ours
    // the file is already gone, only the previous version is left (a replace cut between its renames, then the player deletes the save)
    writeAll(gData + "/GameData.bin.mpbak", img);
    NANDDelete("GameData.bin");
    CHECK(!exists(gData + "/GameData.bin.mpbak"));
}
static void deleteThenReboot(const Bytes& img) {   // the boot after the delete: nothing comes back
    // (the delete ran in the previous process, see main: it leaves .mpbad only)
    port_nand_set_root(gRoot.c_str());
    CHECK(!exists(gData + "/GameData.bin"));
    (void)img;
}

// --cost N: what a GameData.bin save costs, the game's in-place write (switch off) against the atomic one (SMG_ATOMIC_SAVES=1), N saves
// each on this machine's disk: bytes read / written (/proc/self/io), system calls and wall time per save. (The console's memory card is
// slower than this disk, so the times are a floor; the byte counts are exact.)
struct Io { long long rchar, wchar, syscr, syscw; };
static Io ioNow() {
    Io r{};
    FILE* f = fopen("/proc/self/io", "r");
    char k[64]; long long v;
    while (f && fscanf(f, "%63[^:]: %lld\n", k, &v) == 2) {
        if (!strcmp(k, "rchar")) r.rchar = v; else if (!strcmp(k, "wchar")) r.wchar = v; else if (!strcmp(k, "syscr")) r.syscr = v; else if (!strcmp(k, "syscw")) r.syscw = v;
    }
    if (f) fclose(f);
    return r;
}
static int costMain(const Bytes& img, int n, bool atomic) {
    fresh();
    setenv("SMG_ATOMIC_SAVES", atomic ? "1" : "0", 1);
    port_nand_set_root(gRoot.c_str());
    writeAll(gData + "/GameData.bin", img);
    Bytes cur = img;
    const Io a = ioNow();
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) {
        cur[0x9000] ^= 1;   // (a byte inside the data; finalize repairs the checksums)
        mp_save_finalize(cur.data(), (uint32_t)cur.size());
        NANDCreate("GameData.bin", 60, 0);
        CostNandInfo info;
        if (NANDOpen("GameData.bin", &info, 2) != 0) { printf("open failed\n"); fflush(stdout); return 1; }
        NANDWrite(&info, cur.data(), (unsigned)cur.size());
        if (NANDClose(&info) != 0) { printf("close failed at %d\n", i); fflush(stdout); return 1; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const Io b = ioNow();
    const double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / n;
    printf("  %-9s per save: wrote %lld B, read %lld B, %.1f write calls, %.1f read calls, %.2f ms\n", atomic ? "atomic" : "in place",
           (b.wchar - a.wchar) / n, (b.rchar - a.rchar) / n, (double)(b.syscw - a.syscw) / n, (double)(b.syscr - a.syscr) / n, ms);
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "--cost")) {
        Bytes img;
        if (!readAll(argc > 3 ? argv[3] : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin", img)) return 2;
        char t[] = "/tmp/mp_nand_cost.XXXXXX";
        gRoot = mkdtemp(t);
        gData = gRoot + "/title/00010000/524d4750/data";
        int rc = 0;
        for (int atomic = 0; atomic <= 1; atomic++) {
            pid_t pid = fork();
            if (pid == 0) _exit(costMain(img, atoi(argv[2]), atomic));
            int st = 0; waitpid(pid, &st, 0); rc |= !WIFEXITED(st) || WEXITSTATUS(st);
        }
        std::string c = "rm -rf '" + gRoot + "'";
        if (system(c.c_str()) != 0) {}
        return rc;
    }
    // Safe saves is ON by default since 3.0 (nand.cpp); these boot-repair cases were written for the in-place write, so they pin it off.
    // (The atomic writer's own cases: savelife_test and the --cost run.)
    setenv("SMG_ATOMIC_SAVES", "0", 1);
    const char* src = argc > 1 ? argv[1] : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    Bytes img;
    if (!readAll(src, img)) { fprintf(stderr, "cannot read %s\n", src); return 2; }
    char tmpl[] = "/tmp/mp_nand_a7.XXXXXX";
    gRoot = mkdtemp(tmpl);
    gData = gRoot + "/title/00010000/524d4750/data";
    CHECK(inChild(bootBak, img) == 0);
    CHECK(inChild(bootBakTornTmp, img) == 0);
    CHECK(inChild(bootTmpOnly, img) == 0);
    CHECK(inChild(bootStaleTmp, img) == 0);
    CHECK(inChild(bootMarkerTornPrimary, img) == 0);
    CHECK(inChild(bootMarkerNoPrimary, img) == 0);
    CHECK(inChild(recoverLeavesNoMarker, img) == 0);
    CHECK(inChild(createAsidesLoneBak, img) == 0);
    CHECK(inChild(atomicOpenAsidesLoneBak, img) == 0);
    CHECK(inChild(createKeepsInterruptedBak, img) == 0);
    CHECK(inChild(failedRestoreKeepsBak, img) == 0);
    CHECK(inChild(readOpenRetriesRestore, img) == 0);
    CHECK(inChild(readOpenNoRetryNonEmptyTmp, img) == 0);
    CHECK(inChild(readOpenRetriesOnce, img) == 0);
    CHECK(inChild(writeOpenDoesNotRestore, img) == 0);
    CHECK(inChild(bootNewerBuild, img) == 0);
    for (gRound = 1; gRound <= 4; gRound++) CHECK(inChild(bootRotate, img) == 0);
    CHECK(inChild(bootTornPrimaryGoodTmp, img) == 0);
    CHECK(inChild(bootTornPrimaryGoodBak, img) == 0);
    CHECK(inChild(bootValidPrimaryBakKept, img) == 0);
    CHECK(inChild(bootBakValidTmp, img) == 0);
    CHECK(inChild(bootNothing, img) == 0);
    CHECK(inChild(deleteCase, img) == 0);
    // the boot-freeze way out: boot 1 refuses (.failed), boot 2 (error still there) sets the .mptmp aside and restores the .mpbak
    gPhase = 1; CHECK(inChild(bootFreezeP1, img) == 0);
    CHECK(inChild(bootFreezeP2, img) == 0);
    gPhase = 1; CHECK(inChild(bootFreezeP1, img) == 0);   // (again, fresh) boot 1, then the error goes away: boot 2 restores the .mptmp itself
    {
        pid_t pid = fork();
        if (pid == 0) {
            rmdir((gData + "/GameData.bin.mptmp").c_str());
            writeAll(gData + "/GameData.bin.mptmp", img);
            bootFreezeP3(img);
            _exit(sFails ? 1 : 0);
        }
        int st = 0; waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    // the name .unread is taken (an earlier set-aside): the new one goes to .unread.1 and the first stays
    gPhase = 4; CHECK(inChild(bootFreezeP1, img) == 0);
    CHECK(inChild(bootFreezeP2, img) == 0);
    gPhase = 5; CHECK(inChild(bootFreezeP1, img) == 0);
    CHECK(inChild(bootFreezeP2fresh, img) == 0);
    gPhase = 6; CHECK(inChild(bootFreezeP1, img) == 0);
    CHECK(inChild(bootFreezeP2fresh, img) == 0);
    CHECK(inChild(bootStaleFailed, img) == 0);
    for (gPhase = 1; gPhase <= 3; gPhase++) CHECK(inChild(bootCutState, img) == 0);
    // delete, then a fresh process boots on the same files
    {
        pid_t pid = fork();
        if (pid == 0) { deleteCase(img); _exit(sFails ? 1 : 0); }
        int st = 0; waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK(inChild(deleteThenReboot, img) == 0);
    }
    std::string c = "rm -rf '" + gRoot + "'";
    if (system(c.c_str()) != 0) {}
    printf("nand_a7_test: %d checks, %d failed\n", sChecks, sFails);
    return sFails ? 1 : 0;
}
