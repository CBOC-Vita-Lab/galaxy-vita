// GXV_NAND_SAFE (strk26; platform/src/nand/nand.cpp): a save that a crash leaves half written is put back from its backup. Host test: the real nand.cpp
// with the host-job hook stubbed to run inline; a forked child writes half a record and dies without closing (a process death or power cut: the
// bytes written so far are in the file, the close and its sync never happen); the parent (a new process state: nothing of the child's open files is
// in its memory) then reads the save back through NANDOpen and must see the record from before.
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include "revolution/nand.h"

extern "C" {
void port_nand_set_root(const char* root);
void port_gpu_memory_written(void*, uint32_t) {}
void port_nand_set_atomic_writes(bool on);
void port_host_job_wait(const char*, void (*fn)(void*), void* arg) { fn(arg); }
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
void port_log(const char*, ...) {}   // (merge c99: nand.cpp now also holds the multiplayer atomic writer, which logs)
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static const char kSave[] = "save/Galaxy.bin";
static std::string Slurp(const std::string& p) {
    std::string out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "<none>";
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return out;
}
static bool Exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static std::string Rec(char c, size_t n) { return std::string(n, c); }
// NANDWriteRecord: what the game does: create (exists is fine), open for writing (no truncation), write, close.
static int WriteRecord(const std::string& rec, bool crashHalfway) {
    NANDCreate(kSave, 0, 0);
    NANDFileInfo info;
    if (NANDOpen(kSave, &info, NAND_ACCESS_WRITE) != NAND_RESULT_OK) return -1;
    if (crashHalfway) {
        NANDWrite(&info, rec.data(), static_cast<u32>(rec.size() / 2));
        _exit(0);  // (no NANDClose: no sync, no mark removed)
    }
    NANDWrite(&info, rec.data(), static_cast<u32>(rec.size()));
    return NANDClose(&info);
}
static std::string ReadSave() {
    NANDFileInfo info;
    if (NANDOpen(kSave, &info, NAND_ACCESS_READ) != NAND_RESULT_OK) return "<open failed>";
    char buf[8192];
    s32 n = NANDRead(&info, buf, sizeof buf);
    NANDClose(&info);
    return n < 0 ? "<read failed>" : std::string(buf, static_cast<size_t>(n));
}
static void ForkWrite(const std::string& rec) {
    pid_t pid = fork();
    if (pid == 0) { WriteRecord(rec, true); _exit(1); }
    int st = 0;
    waitpid(pid, &st, 0);
}

static int OffCase() {
    char tmpl[] = "/tmp/nandoffXXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return 2;
    setenv("GXV_NAND_SAFE", "0", 1);
    port_nand_set_root(dir);
    const std::string live = std::string(dir) + "/title/00010000/524d4750/data/" + kSave;
    const std::string A = Rec('A', 3000), B = Rec('B', 3000);
    WriteRecord(A, false);
    WriteRecord(B, false);
    CHECK(Slurp(live) == B && !Exists(live + ".bak") && !Exists(live + ".wip"));
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--off")) return OffCase();
    char tmpl[] = "/tmp/nandsafeXXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return 2;
    setenv("GXV_NAND_SAFE", "1", 1);
    port_nand_set_root(dir);
    const std::string home = std::string(dir) + "/title/00010000/524d4750/data/";
    const std::string live = home + kSave;
    const std::string A = Rec('A', 3000), B = Rec('B', 3000), C = Rec('C', 3000);

    // 1. an ordinary save, then another: the live file is the newer one, the backup the older, no mark left
    CHECK(WriteRecord(A, false) == NAND_RESULT_OK);
    CHECK(Slurp(live) == A);
    CHECK(!Exists(live + ".bak"));  // (first write: the file was empty, nothing to keep)
    CHECK(WriteRecord(B, false) == NAND_RESULT_OK);
    CHECK(Slurp(live) == B && Slurp(live + ".bak") == A && !Exists(live + ".wip"));
    CHECK(ReadSave() == B);

    // 2. a write that dies halfway: the file is a mix; the next open (new process state) puts B back
    ForkWrite(C);
    const std::string mixed = Slurp(live);
    CHECK(mixed == C.substr(0, 1500) + B.substr(1500));  // (the damage the cut did)
    CHECK(Exists(live + ".wip") && Slurp(live + ".bak") == B);
    CHECK(ReadSave() == B);
    CHECK(Slurp(live) == B && !Exists(live + ".wip"));

    // 3. a second cut before anything read it: the backup is not overwritten by the mixed file
    ForkWrite(C);
    ForkWrite(A);  // (writes over the mix of the first; the .wip of the first is still there: the backup must stay B)
    CHECK(Slurp(live + ".bak") == B);
    CHECK(ReadSave() == B);

    // 4. a missing or empty save with a backup is put back
    unlink(live.c_str());
    CHECK(ReadSave() == B);
    {
        int fd = open(live.c_str(), O_WRONLY | O_TRUNC);
        close(fd);
    }
    CHECK(ReadSave() == B);

    // 5. a deleted save does not come back
    CHECK(NANDDelete(kSave) == NAND_RESULT_OK);
    CHECK(!Exists(live) && !Exists(live + ".bak") && !Exists(live + ".wip"));
    {
        NANDFileInfo info;
        CHECK(NANDOpen(kSave, &info, NAND_ACCESS_READ) != NAND_RESULT_OK);
    }

    // 5b. a save-root change between NANDOpen and NANDClose: the write is committed under the root of the open (the file it was made in),
    // and its .wip mark is removed there; the root of the close must not decide where the write went
    {
        char t1[] = "/tmp/nandrootAXXXXXX";
        char t2[] = "/tmp/nandrootBXXXXXX";
        char* dirA = mkdtemp(t1);
        char* dirB = mkdtemp(t2);
        if (!dirA || !dirB) return 2;
        const std::string liveA = std::string(dirA) + "/title/00010000/524d4750/data/" + kSave;
        const std::string D = Rec('D', 3000), E = Rec('E', 3000), G = Rec('G', 3000);
        port_nand_set_root(dirA);
        CHECK(WriteRecord(D, false) == NAND_RESULT_OK);
        CHECK(WriteRecord(E, false) == NAND_RESULT_OK && Slurp(liveA) == E && Slurp(liveA + ".bak") == D);
        NANDCreate(kSave, 0, 0);
        NANDFileInfo info;
        CHECK(NANDOpen(kSave, &info, NAND_ACCESS_WRITE) == NAND_RESULT_OK);
        CHECK(NANDWrite(&info, G.data(), static_cast<u32>(G.size())) == static_cast<s32>(G.size()));
        port_nand_set_root(dirB);  // (the root changes while the save is open)
        CHECK(NANDClose(&info) == NAND_RESULT_OK);
        port_nand_set_root(dirA);
        CHECK(Slurp(liveA) == G && !Exists(liveA + ".wip"));
        CHECK(ReadSave() == G);
    }

    // 5c. (merge c99) ONE save design per path. The atomic writer is OFF by default now (single player), so it is switched on here: GameData.bin then
    // goes through the multiplayer writer (temporary + rename) and GXV_NAND_SAFE (on) must leave no .bak / .wip beside it. The image is the test
    // save (the atomic commit validates it as a GameData.bin); without it the two cases are skipped, not passed.
    port_nand_set_root(dir);   // (5b left the root at its directory A)
    const std::string gd = "save/GameData.bin";
    const std::string gl = home + gd;
    std::string img = Slurp("/data/saves0/saves/title/00010000/524d4750/data/GameData.bin");
    if (img == "<none>") {
        printf("nand_safe: SKIP 5c/5d (no test save at /data/saves0/saves)\n");
    } else {
        port_nand_set_atomic_writes(true);
        NANDCreate(gd.c_str(), 0, 0);
        NANDFileInfo info;
        CHECK(NANDOpen(gd.c_str(), &info, NAND_ACCESS_WRITE) == NAND_RESULT_OK);
        CHECK(NANDWrite(&info, img.data(), static_cast<u32>(img.size())) == static_cast<s32>(img.size()));
        CHECK(NANDClose(&info) == NAND_RESULT_OK);
        CHECK(Slurp(gl) == img);                                       // (it really went through the atomic writer: committed, valid)
        CHECK(!Exists(gl + ".wip") && !Exists(gl + ".bak"));          // (nothing of the other design)
        port_nand_set_atomic_writes(false);

        // 5d. (RISK A) a single-player save whose close failed left .wip + an OLD .bak; a later multiplayer session writes GameData.bin
        // atomically; once the session is over (atomic off again) the next open must see the session's progress, not the stale backup.
        {
            const std::string stale = Rec('S', 3000);
            FILE* f = fopen(gl.c_str(), "wb"); fwrite("torn", 1, 4, f); fclose(f);               // (what the failed in-place write left)
            f = fopen((gl + ".bak").c_str(), "wb"); fwrite(stale.data(), 1, stale.size(), f); fclose(f);
            f = fopen((gl + ".wip").c_str(), "wb"); fclose(f);
            port_nand_set_atomic_writes(true);                                                    // (the session)
            NANDFileInfo w;
            CHECK(NANDOpen(gd.c_str(), &w, NAND_ACCESS_WRITE) == NAND_RESULT_OK);
            CHECK(NANDWrite(&w, img.data(), static_cast<u32>(img.size())) == static_cast<s32>(img.size()));
            CHECK(NANDClose(&w) == NAND_RESULT_OK);
            port_nand_set_atomic_writes(false);                                                   // (the session ends)
            NANDFileInfo r;
            CHECK(NANDOpen(gd.c_str(), &r, NAND_ACCESS_READ) == NAND_RESULT_OK);
            std::string got(img.size(), 0);
            CHECK(NANDRead(&r, &got[0], static_cast<u32>(got.size())) == static_cast<s32>(got.size()));
            NANDClose(&r);
            CHECK(got == img);                                                                    // (session progress kept)
            CHECK(Slurp(gl) == img && !Exists(gl + ".wip") && !Exists(gl + ".bak"));
        }
    }

    // 6. the switch off: nothing but the game's own writes
    setenv("GXV_NAND_SAFE", "0", 1);
    printf("(GXV_NAND_SAFE is read once: the off case is run in a child)\n");
    pid_t pid = fork();
    if (pid == 0) {
        // (a new static is not possible after the first call in this process; the child re-executes the check through the environment of exec)
        execl("/proc/self/exe", "nand_safe_test", "--off", static_cast<char*>(nullptr));
        _exit(3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("nand_safe: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
