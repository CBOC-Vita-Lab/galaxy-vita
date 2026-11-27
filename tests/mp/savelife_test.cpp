// savelife (savefix): the save-safety harness. Compiles the REAL decomp/src/Game/MP/MpSaveGame.cpp (+ MpSaveGate.hpp, the game's
// save hook MpSaveHook.hpp), platform/src/mp/mp_savefile.cpp, mp_savecheck.cpp, mp_config.cpp and platform/src/nand/nand.cpp
// against a small fake of the game classes (tests/mp/savelife/fakegame.*), a fake net bridge, a fake clock and a fault layer
// (read errors, short writes, a full card, failing renames, failing allocations, a busy save sequence, a game block copy that
// leaves a block out, and a power cut at any mutating file-system call). The game's own pieces it cannot compile (the save
// nerves, the file select, the Multiplayer menu's entry paths) are modelled here with the same calls in the same order; the
// save hook itself is the production header.
//
// Each lifecycle runs in a forked child (fresh statics = a fresh process); a power cut ends the child (_exit) and the next
// child boots on the same files: "kill and relaunch". The expectations live in shared memory.
//
// Invariants, checked after every step and after every relaunch:
//  (i)   the single-player GameData.bin changes only in a slot the player chose to write (single-player play of that slot, a
//        file select action on an up-to-date file select, "Play on my file" N, save / recovery to "my file" N, restore of a
//        backup), only that slot's blocks, only with this console's own progress for that slot, and for the multiplayer
//        writes only while a backup of what was there before the choice exists. It is never left invalid.
//  (ii)  a write of the game's image goes only to the root the image was loaded from (and never with a half-built image).
//  (iii) the file select's cached files come from the root its writes go to.
//  (iv)  a co-op slot changes only when it is the one being played / chosen.
//  (v)   mp.cfg never loses a key or a value the player set (in mp.cfg, its .bak or a kept-aside copy).
//
//   tests/mp/savelife_build.sh               build + scripted lifecycles + 20000 random sequences
//   savelife_test --scripted | --random N [--seed S] [--steps K] [--verbose] [--one SEED]
#include "savelife/world.hpp"

#include "Game/MP/MpSaveGame.hpp"
#include "Game/MP/MpSaveGate.hpp"
#include "Game/MP/MpSaveHook.hpp"
#include "port/mp_bridge.h"
#include "../../platform/src/mp/mp_config.h"
#include "../../platform/src/mp/mp_savefile.h"
#include "../../platform/src/mp/mp_savecheck.h"
// (the NAND layer's interface, as revolution/nand.h declares it; that header is not included here: its intrinsics header clashes
// with glibc under g++ in a second translation unit)
extern "C" {
#define NAND_RESULT_OK 0
#define NAND_RESULT_EXISTS (-6)
enum { NAND_ACCESS_NONE_, NAND_ACCESS_READ, NAND_ACCESS_WRITE, NAND_ACCESS_RW_ };
typedef struct NANDFileInfo {
    s32 fileDescriptor;
    s32 origFd;
    char origPath[64];
    char tmpPath[64];
    u8 accType;
    u8 stage;
    u8 mark;
} NANDFileInfo;
s32 NANDCreate(const char* path, u8 perm, u8 attr);
s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode);
s32 NANDClose(NANDFileInfo* info);
s32 NANDRead(NANDFileInfo* info, void* buf, u32 length);
s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length);
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#ifndef SAVELIFE_API
#define SAVELIFE_API 2   // 1: the save layer as reviewed (mp-perf); 2: savefix (file select guard, restore, atomic sp writes)
#endif

extern "C" int port_nand_get_root(char* out, int cap);
extern "C" bool port_nand_atomic_writes(void);
extern "C" void port_nand_set_root(const char* root);

// ---- the fake clock ------------------------------------------------------------------------------------------------------
static int64_t* sClock;   // in shared memory
extern "C" time_t __real_time(time_t*);
extern "C" time_t __wrap_time(time_t* t) {
    const time_t v = (time_t)*sClock;
    if (t) *t = v;
    return v;
}

// ---- progress blocks -----------------------------------------------------------------------------------------------------
// A user file's progress is the test save's file 2 (50 Power Stars) with a subset of its stars cleared: every mask is a different,
// valid block. Each mask carries a tag: whose progress it is.
enum Tag : uint8_t { T_NONE = 0, T_SP, T_MINE, T_COOP, T_FOREIGN, T_NOCHOICE, T_BLANK };
static const char* tagName(int t) {
    static const char* n[] = {"none", "sp", "mine", "coop", "foreign", "nochoice", "blank"};
    return t >= 0 && t <= 6 ? n[t] : "?";
}

static const int kMaxProgs = 4096;
struct Prog { uint64_t mask; uint8_t tag, slot; };

enum Phase : uint8_t { P_TITLE = 0, P_SPPLAY, P_SESSION, P_ENDED };
enum Choice : uint8_t { C_COOP = 1, C_COPY, C_MINE, C_QUICK, C_NONE, C_GUEST };
static const char* choiceName(int c) {
    static const char* n[] = {"-", "coop", "copy", "mine", "quick", "none", "guest"};
    return c >= 0 && c <= 6 ? n[c] : "?";
}

// alt: the value of a save that was started and not seen to finish (a power cut, or a refused save). A cut after the final rename has
// already made the new value the file's; a cut before it leaves the old one. Either is the player's file; one of the two must remain.
struct CfgKey { char key[24]; char val[48]; char alt[48]; char alt2[48]; bool carried; };   // carried: the pending save was in an earlier life (the value in memory is gone)

struct Model {
    // templates (from the test save)
    uint8_t base[0x10000];
    uint32_t baseLen;
    uint8_t blankGame[0xF80], blankConfig[0x60], sysconf[0x80], baseMario[0xF80];
    int starByte[64], starBit[64], nStars;
    // sequence
    uint64_t seed, rng;
    int step, nsteps, lifetimes;
    int64_t clock;
    bool booted, verbose, scripted;
    int phase;
    // expectations on disk
    uint8_t sp[0x10000];
    uint32_t spLen;
    uint8_t coop[0x10000];
    uint32_t coopLen;
    bool coopExists;
    // what may change
    int permitSp;                 // a multiplayer write to single-player file N is chosen (backup required)
    uint8_t spAtPermit[0x10000];  // what was in the file when it was chosen (a backup of it must exist)
    uint32_t spAtPermitLen;
    uint8_t mineAt[7][0x10000];   // per file: the single-player file when "Play on my file N" was chosen (its session's progress
    uint32_t mineAtLen[7];        // may reach file N later, through any save of that image, as long as this backup exists)
    int spPlaySlot;               // plain single-player play of file N
    int fsSlot;                   // a file select action on file N (an up-to-date file select)
    int restoreSlotMask;          // a restore of a backup (whole file may change)
    // what the player legitimately put into the image's single-player file N (single-player play, an up-to-date file select's
    // action): a save that failed leaves it in the image and the next save of any kind writes it; that is allowed
    uint64_t allowSp[7][32];
    int allowSpN[7];
    int allowBlank;
    int permitCoopMask;           // co-op slots that may change (bit s)
    int deferCoopMask;            // co-op slots whose chosen write a power cut interrupted (the repair may complete it later)
    // the file select's cache
    char cacheRoot[256];
    bool cacheStale;              // (model) a root switch happened after the cache was taken
    int fsPending, fsPendingSince, fsPendingTry;
    unsigned cacheGen;
    bool fsHoldInput;
    // session
    int choice, coopSlot, spFile;
    bool menuNew, hostileFs;
    bool mine, migrated, foreign;
    int frame;
    // lineage
    Prog progs[kMaxProgs];
    int nprogs;
    // mp.cfg
    CfgKey cfg[32];
    int ncfg;
    int cfgLoadFaults;
    // results
    int violations;
    char why[4096];
    // stats
    long stat[64];
};
static Model* M;
static std::string gDir, gSpRoot, gMpRoot, gBackups, gCfgDir, gSpPath, gMpPath;

enum Stat { S_STEPS, S_KILLS, S_SP_WRITES, S_COOP_WRITES, S_SUPPRESSED, S_SESSIONS, S_LEAVE_SP, S_LEAVE_COOP, S_RECOVERY, S_RACE,
            S_MIGRATION, S_SNAPSHOT, S_FS_ACTION, S_FS_BLOCKED, S_PENDING, S_RESTORE, S_CFG_SAVE, S_FAULTS, S_LAUNCH_REFUSED,
            S_FS_REBUILD, S_IMAGE_WRITES, S_COUNT };
static const char* kStatNames[] = {"steps", "kills", "sp_writes", "coop_writes", "suppressed_saves", "sessions", "leave_sp", "leave_coop",
                                   "recovery_keep", "race_saves", "migrations", "snapshots", "fs_actions", "fs_blocked", "fs_pending",
                                   "restores", "cfg_saves", "faults", "launch_refused", "fs_rebuild", "image_writes"};

static uint64_t rnd() {
    M->rng ^= M->rng << 13;
    M->rng ^= M->rng >> 7;
    M->rng ^= M->rng << 17;
    return M->rng;
}
static int rn(int n) { return n <= 1 ? 0 : (int)(rnd() % (uint64_t)n); }
static bool chance(int pct) { return rn(100) < pct; }

static void violation(const char* fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    M->violations++;
    const size_t n = strlen(M->why);
    snprintf(M->why + n, sizeof M->why - n, "VIOLATION seed=%llu step=%d life=%d: %s\n", (unsigned long long)M->seed, M->step,
             M->lifetimes, line);
    fprintf(stderr, "VIOLATION seed=%llu step=%d life=%d: %s\n", (unsigned long long)M->seed, M->step, M->lifetimes, line);
    gW.dumpLog(stderr);
}
static void note(const char* fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    gW.log(line);
    if (M->verbose) fprintf(stderr, "[%d] %s\n", M->step, line);
}

// ---- blocks --------------------------------------------------------------------------------------------------------------
static void encode(uint64_t mask, uint8_t* out) {
    memcpy(out, M->baseMario, 0xF80);
    for (int i = 0; i < M->nStars; i++)
        if (mask & (1ull << i)) out[M->starByte[i]] &= (uint8_t)~(1u << M->starBit[i]);
}
static int newProg(int tag, int slot) {
    if (M->nprogs >= kMaxProgs) M->nprogs = 1;   // (old entries reused: lineage of very old states is not needed)
    Prog& p = M->progs[M->nprogs];
    for (;;) {
        p.mask = rnd() & ((1ull << M->nStars) - 1);
        bool dup = false;
        for (int i = 1; i < M->nprogs && !dup; i++) dup = M->progs[i].mask == p.mask;
        if (!dup) break;
    }
    p.tag = (uint8_t)tag;
    p.slot = (uint8_t)slot;
    return M->nprogs++;
}
// (tag, slot) of a mario block: T_BLANK, a known progress, or T_NONE (unknown bytes)
static Prog identify(const uint8_t* block, uint32_t len) {
    Prog r{0, T_NONE, 0};
    if (len >= 0xF80 && mp_save_block_used(block, len) == mp_save_block_used(M->blankGame, 0xF80) &&
        memcmp(block, M->blankGame, mp_save_block_used(M->blankGame, 0xF80)) == 0) {
        r.tag = T_BLANK;
        return r;
    }
    uint64_t mask = 0;
    for (int i = 0; i < M->nStars; i++)
        if (!(block[M->starByte[i]] & (1u << M->starBit[i]))) mask |= 1ull << i;
    uint8_t enc[0xF80];
    encode(mask, enc);
    const uint32_t used = mp_save_block_used(enc, 0xF80);
    if (len < used || memcmp(enc, block, used) != 0) return r;
    for (int i = 1; i < M->nprogs; i++)
        if (M->progs[i].mask == mask) return M->progs[i];
    return r;
}

// ---- files ---------------------------------------------------------------------------------------------------------------
static bool readAll(const std::string& p, std::vector<uint8_t>& out) {
    out.assign(MP_SAVE_MAX_FILE, 0);
    uint32_t n = 0;
    if (mp_save_read_file(p.c_str(), out.data(), MP_SAVE_MAX_FILE, &n) != 0) {
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
}
static void writeRaw(const std::string& p, const uint8_t* d, size_t n) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    if (fwrite(d, 1, n, f) != n) {}
    fclose(f);
}
static bool fileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static bool slotBlocks(const uint8_t* image, int slot, const uint8_t** b, uint32_t* len) {
    MpSaveSnapshotView v;
    if (!mp_save_view_slot(image, slot, &v)) return false;
    for (int k = 0; k < 3; k++) {
        b[k] = v.block[k];
        len[k] = v.len[k];
    }
    return true;
}
static bool slotEqual(const uint8_t* a, const uint8_t* b, int slot) {
    const uint8_t *ba[3], *bb[3];
    uint32_t la[3], lb[3];
    if (!slotBlocks(a, slot, ba, la) || !slotBlocks(b, slot, bb, lb)) return false;
    for (int k = 0; k < 3; k++)
        if (la[k] != lb[k] || memcmp(ba[k], bb[k], la[k]) != 0) return false;
    return true;
}
static bool slotBlank(const uint8_t* a, int slot) {
    const uint8_t* b[3];
    uint32_t l[3];
    if (!slotBlocks(a, slot, b, l)) return false;
    return identify(b[0], l[0]).tag == T_BLANK;
}
// a backup in the backups folder holds exactly these bytes
static bool backupHolds(const uint8_t* data, uint32_t len) {
    DIR* d = opendir(gBackups.c_str());
    if (!d) return false;
    bool found = false;
    std::vector<uint8_t> b;
    while (struct dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "GameData-", 9) != 0) continue;
        if (readAll(gBackups + "/" + e->d_name, b) && b.size() == len && memcmp(b.data(), data, len) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

// ---- the invariants ------------------------------------------------------------------------------------------------------
static void allowSpContent(int slot) {
    uint32_t off, size;
    char n[16];
    snprintf(n, sizeof n, "mario%d", slot);
    if (slot < 1 || slot > 6 || !mp_save_find(gW.image, n, &off, &size)) return;
    const Prog p = identify(gW.image + off, size);
    if (p.tag == T_BLANK) M->allowBlank |= 1 << slot;
    else if (p.tag != T_NONE) M->allowSp[slot][M->allowSpN[slot]++ % 32] = p.mask;
}
static bool spContentAllowed(int slot, const Prog& p) {
    if (p.tag == T_BLANK) return (M->allowBlank >> slot) & 1;
    if (p.tag == T_NONE) return false;
    for (int i = 0; i < M->allowSpN[slot] && i < 32; i++)
        if (M->allowSp[slot][i] == p.mask) return true;
    return false;
}
static void checkSp() {
    std::vector<uint8_t> cur;
    bool have = readAll(gSpPath, cur);
    if (!have) {
        // a replace that failed twice (the card refused both renames) leaves no GameData.bin but the previous one as .mpbak: the
        // next boot / root switch puts it back (nand.cpp, mp_save_recover); it must hold exactly what was there
        std::vector<uint8_t> bak;
        if (readAll(gSpPath + ".mpbak", bak) && bak.size() == M->spLen && memcmp(bak.data(), M->sp, M->spLen) == 0) return;
    }
    if (!have || mp_save_validate(cur.data(), (uint32_t)cur.size(), true, true) != MP_SAVE_OK) {
        violation("(i) single-player GameData.bin %s", have ? "invalid (torn / overwritten)" : "missing");
        return;
    }
    if (cur.size() == M->spLen && memcmp(cur.data(), M->sp, M->spLen) == 0) return;
    // which slots changed; everything outside the slots (table, sysconf) must not
    int changed = 0;
    for (int s = 1; s <= 6; s++)
        if (!slotEqual(cur.data(), M->sp, s)) changed |= 1 << s;
    {
        uint32_t o1, s1, o2, s2;
        if (mp_save_find(cur.data(), "sysconf", &o1, &s1) && mp_save_find(M->sp, "sysconf", &o2, &s2) &&
            (s1 != s2 || memcmp(cur.data() + o1, M->sp + o2, s1) != 0))
            violation("(i) single-player sysconf changed");
    }
    for (int s = 1; s <= 6; s++) {
        if (!(changed & (1 << s))) continue;
        const uint8_t* b[3];
        uint32_t l[3];
        slotBlocks(cur.data(), s, b, l);
        const Prog p = identify(b[0], l[0]);
        if (M->restoreSlotMask & (1 << s)) continue;   // a restore: checked by its own step
        if (s == M->spPlaySlot && p.tag == T_SP && p.slot == s) continue;
        if (spContentAllowed(s, p)) continue;
        if (p.tag == T_MINE && p.slot == s && M->mineAtLen[s]) {
            if (!backupHolds(M->mineAt[s], M->mineAtLen[s])) violation("(i) single-player file %d: session progress without the session's backup", s);
            continue;
        }
        if (s == M->fsSlot && (p.tag == T_BLANK || p.tag == T_SP || p.tag == T_MINE)) continue;   // (create, delete, copy of a file)
        if (s == M->permitSp && ((p.tag == T_MINE && p.slot == s) || (p.tag == T_SP && p.slot == s))) {
            if (!backupHolds(M->spAtPermit, M->spAtPermitLen)) violation("(i) single-player file %d written without a backup of what was there", s);
            continue;
        }
        violation("(i) single-player file %d changed to %s%d progress (permit sp=%d play=%d fs=%d)", s, tagName(p.tag), p.slot,
                  M->permitSp, M->spPlaySlot, M->fsSlot);
    }
    M->stat[S_SP_WRITES]++;
    memcpy(M->sp, cur.data(), cur.size());
    M->spLen = (uint32_t)cur.size();
}

static void checkCoop() {
    std::vector<uint8_t> cur;
    if (!readAll(gMpPath, cur) || mp_save_validate(cur.data(), (uint32_t)cur.size(), true, true) != MP_SAVE_OK) return;   // (repaired on entry)
    if (M->coopExists && cur.size() == M->coopLen && memcmp(cur.data(), M->coop, M->coopLen) == 0) return;
    for (int s = 1; s <= 6; s++) {
        const bool same = M->coopExists ? slotEqual(cur.data(), M->coop, s) : slotBlank(cur.data(), s);
        if (same || ((M->permitCoopMask | M->deferCoopMask) & (1 << s))) continue;
        const uint8_t* b[3];
        uint32_t l[3];
        slotBlocks(cur.data(), s, b, l);
        const Prog p = identify(b[0], l[0]);
        // the original host's own play on co-op file s from a session where it chose file s: its progress belongs there (a
        // save of it that did not happen during that session may happen later with the same image)
        if (p.tag == T_COOP && p.slot == s) continue;
        violation("(iv) co-op file %d changed to %s%d progress (permitted mask %x)", s, tagName(p.tag), p.slot, M->permitCoopMask);
    }
    M->stat[S_COOP_WRITES]++;
    memcpy(M->coop, cur.data(), cur.size());
    M->coopLen = (uint32_t)cur.size();
    M->coopExists = true;
}

// mp.cfg: every key = value the player has (initial file, successful saves) is in mp.cfg, mp.cfg.bak or a kept-aside copy
static bool cfgFileHas(const std::string& path, const CfgKey& k) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        std::string key(line, eq - line), val(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' ')) val.pop_back();
        // a look this build does not have (the newer build's 9) is read as the default look 0; a save that sets the look the file already
        // READS as keeps the file's own text (keepsafe K3: the newer build's value survives), which is the same value for this build
        auto norm = [&](const std::string& v) {
            if (key == "look" && key == k.key) {
                const int n = atoi(v.c_str());
                if (n >= MP_LOOK_COUNT) return std::string("0");
            }
            return v;
        };
        found = key == k.key && (norm(val) == norm(k.val) || (k.alt[0] && norm(val) == norm(k.alt)) || (k.alt2[0] && norm(val) == norm(k.alt2)));
    }
    fclose(f);
    return found;
}
static void checkCfg() {
    std::vector<std::string> files = {gCfgDir + "/mp.cfg", gCfgDir + "/mp.cfg.bak"};
    if (DIR* d = opendir(gCfgDir.c_str())) {
        while (struct dirent* e = readdir(d))
            if (!strncmp(e->d_name, "mp.cfg.", 7) && strcmp(e->d_name, "mp.cfg.bak") && strcmp(e->d_name, "mp.cfg.tmp"))
                files.push_back(gCfgDir + "/" + e->d_name);
        closedir(d);
    }
    for (int i = 0; i < M->ncfg; i++) {
        bool found = false;
        for (auto& f : files) found = found || cfgFileHas(f, M->cfg[i]);
        if (!found) {
            for (auto& f : files) {   // (what the files hold, for the report)
                std::vector<uint8_t> b;
                std::string t;
                if (readAll(f, b)) t.assign((const char*)b.data(), b.size() > 400 ? 400 : b.size());
                for (auto& ch : t) if (ch == '\n') ch = ';';
                note("cfg file %s: %s", f.c_str() + gCfgDir.size(), b.empty() ? "(absent or empty)" : t.c_str());
            }
            for (int k = 0; k < M->ncfg; k++) note("model cfg %s = %s (alt %s)", M->cfg[k].key, M->cfg[k].val, M->cfg[k].alt);
            violation("(v) mp.cfg lost %s = %s (or %s)", M->cfg[i].key, M->cfg[i].val, M->cfg[i].alt);
        }
    }
}

static void checkAll() {
    checkSp();
    checkCoop();
    checkCfg();
}

// ---- the game's pieces (modelled with the same calls) --------------------------------------------------------------------
static std::string nandRoot() {
    char r[256];
    return port_nand_get_root(r, sizeof r) >= 0 ? std::string(r) : std::string();
}

void World::noteImageReset() {
    char r[256];
    snprintf(imageRoot, sizeof imageRoot, "%s", port_nand_get_root(r, sizeof r) >= 0 ? r : "?");
    imageCopied = false;
    imageLoads++;
}
void World::noteImageCopied() {
    char r[256];
    snprintf(imageRoot, sizeof imageRoot, "%s", port_nand_get_root(r, sizeof r) >= 0 ? r : "?");
    imageCopied = true;
}

// A1: the safe single-player save (nand.cpp, SMG_ATOMIC_SAVES; savebug) is an option, OFF in the product by default (Settings: Safe saves). SAVELIFE_SP_ATOMIC picks the model:
//   unset or 0: the product default (Safe saves off): the old in-place write (SMG_ATOMIC_SAVES=0), cuts never inside it (the original game's own risk, not
//               modelled); multiplayer sessions still write atomically
//   1:          the Settings toggle turned ON (every GameData.bin write atomic; the model with the most to prove) and a power cut / full card / failing
//               rename may land INSIDE the single-player write too (savelife_build.sh runs this case as well)
//   2:          the old in-place write, cuts DO land inside it (what the original game risks: a report of damaged files is expected)
static int spMode() {
    static const int v = getenv("SAVELIFE_SP_ATOMIC") ? getenv("SAVELIFE_SP_ATOMIC")[0] - '0' : 0;
    return v;
}
static bool spAtomic() { return spMode() == 1 || spMode() == 2; }

// SaveDataHandler::requestSaveSaveData + NANDManagerThread::executeWriteSequence (the game's in-place write through nand.cpp)
static bool gameWriteImage(const char* why) {
    const std::string root = nandRoot();
    if (root != gW.imageRoot) violation("(ii) %s writes the image to %s; it was loaded from %s", why, root.c_str(), gW.imageRoot);
    M->stat[S_IMAGE_WRITES]++;
    static uint8_t buf[0x10000];
    memcpy(buf, gW.image, sizeof buf);
    if (!mp_save_finalize(buf, sizeof buf)) return false;
    const u32 len = mp_save_disk_length(buf);
    note("game save (%s) -> %s%s", why, root.c_str(), port_nand_atomic_writes() ? " (atomic)" : "");
    if (!port_nand_atomic_writes() && !spAtomic()) {
        // plain single player: the game's own in-place write, unchanged (a cut or a full card there is the original game's risk,
        // not modelled); the harness checks that this never happens on the multiplayer root
        if (root == gMpRoot) violation("(atomic) the game's write to the multiplayer root is not atomic");
        gW.faults.clear();
    }
    s32 r = NANDCreate("GameData.bin", 60, 0);
    if (r != NAND_RESULT_OK && r != NAND_RESULT_EXISTS) return false;
    NANDFileInfo info;
    if (NANDOpen("GameData.bin", &info, NAND_ACCESS_WRITE) != NAND_RESULT_OK) return false;
    r = NANDWrite(&info, buf, len);
    const s32 c = NANDClose(&info);   // (savefix: with atomic writes the temporary is committed here)
    return r == (s32)len && c == NAND_RESULT_OK;
}
// exeSave: the live user file into the image, then the save hook (production header), then the write
static void gameAutosave(const char* why) {
    gW.handler.storeUserFile(gW.user);
    if (M->phase == P_SPPLAY) allowSpContent(M->spPlaySlot);
    if (MpSaveHook::suppressed()) {
        M->stat[S_SUPPRESSED]++;
        note("save suppressed (%s)", why);
        return;
    }
    gameWriteImage(why);
}

// the game's boot: the boot root, mp.cfg, GameData.bin into the image (SaveDataHandleSequence's load)
static void gameBoot() {
    port_nand_set_root(gSpRoot.c_str());
    mp_config_set_dir(gCfgDir.c_str());
    mp_config_load();
    static uint8_t buf[0x10000];
    memset(buf, 0, sizeof buf);
    NANDFileInfo info;
    bool valid = false;
    if (NANDOpen("GameData.bin", &info, NAND_ACCESS_READ) == NAND_RESULT_OK) {
        const s32 n = NANDRead(&info, buf, sizeof buf);
        NANDClose(&info);
        valid = n > 0 && mp_save_validate(buf, (uint32_t)n, true, true) == MP_SAVE_OK;
    }
    gW.user->resetAllData();
    SaveDataHandler::resetSaveData(gW.image);
    SaveDataHandler::initializeAllFileInSaveData(gW.image, &gW.sys, gW.user);
    if (valid) SaveDataHandler::copySaveDataEachFile(gW.image, buf);
    note("boot (sp file %s)", valid ? "loaded" : "missing/invalid");
    if (DIR* d = opendir((gMpRoot + "/title/00010000/524d4750/data").c_str())) {   // (the co-op folder as the boot finds it)
        std::string files;
        while (struct dirent* e = readdir(d))
            if (e->d_name[0] != '.') files += std::string(" ") + e->d_name;
        closedir(d);
        note("boot: co-op folder:%s", files.c_str());
    }
}

// ---- the file select (FileSelector::init / control, MpMenuSequence::onFileSelect / fileSelectTick, as built) --------------
static void fsCache() {
    snprintf(M->cacheRoot, sizeof M->cacheRoot, "%s", gW.imageRoot);
    M->cacheStale = false;
#if SAVELIFE_API >= 2
    M->cacheGen = MpSaveGame::fileSelectOpened();
#endif
}
static void fileSelectInit() {
    M->phase = P_TITLE;
    M->spPlaySlot = 0;
    M->fsSlot = 0;
    M->fsHoldInput = false;
    // MpMenuSequence::onFileSelect (menu code: modelled as built)
    M->fsPending = 0;
    if (!mp_active()) {
        MpSaveGame::abandonHostChoice();
        if (MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession()) {
            if (!MpSaveGame::leaveSessionRoot()) {
                M->fsPending = 1;
                M->fsPendingSince = M->fsPendingTry = M->frame;
                M->stat[S_PENDING]++;
                note("file select: root switch pending");
            }
        }
    }
    fsCache();   // initUserFileArray: the six files from the image as it is now
#if SAVELIFE_API >= 2
    M->fsHoldInput = MpSaveGame::fileSelectGuard(M->cacheGen) != MpSaveGame::FS_USABLE;   // (FileSelector::control, first frame)
#endif
}
static void frameTick() {
    M->frame++;
    gW.turn++;
    if (gW.busyFrames > 0) gW.busyFrames--;
}
static void fileSelectFrame() {
    frameTick();
    if (M->fsPending && !mp_active() && M->frame - M->fsPendingTry >= 30) {
        M->fsPendingTry = M->frame;
        if (!(MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession()) || MpSaveGame::leaveSessionRoot()) {
            M->fsPending = 0;
            note("file select: root switched after %d frames", M->frame - M->fsPendingSince);
        }
    }
    if (strcmp(M->cacheRoot, gW.imageRoot) != 0) M->cacheStale = true;
#if SAVELIFE_API >= 2
    // FileSelector::control: the save layer's guard (input held while unusable; a rebuild when the image changed)
    const int g = MpSaveGame::fileSelectGuard(M->cacheGen);
    M->fsHoldInput = g != MpSaveGame::FS_USABLE;
    if (g == MpSaveGame::FS_REBUILD) {
        M->stat[S_FS_REBUILD]++;
        note("file select: rebuilt (scene change)");
        fsCache();   // MR::requestChangeSceneTitle(): a new file select caches the files again
        M->fsHoldInput = MpSaveGame::fileSelectGuard(M->cacheGen) != MpSaveGame::FS_USABLE;
    }
#endif
    MpSaveGame::tick();
#if SAVELIFE_API >= 2
    // (iii) the guard itself: a file select whose files are not the image's (or not on the root writes go to) is never usable
    if (!M->fsHoldInput && !mp_active() && (strcmp(M->cacheRoot, gW.imageRoot) != 0 || strcmp(gW.imageRoot, nandRoot().c_str()) != 0))
        violation("(iii) the file select accepts input with files from %s while the image is %s / the root %s", M->cacheRoot,
                  gW.imageRoot, nandRoot().c_str());
#endif
}
// may the (faithful) player use the file select now: as built, any time no warning window is up (fileSelectTick's warning
// after 5 s); with the guard, only when the save layer says so
static bool fsInputOpen() {
#if SAVELIFE_API >= 2
    return !M->fsHoldInput;
#else
    return !(M->fsPending && M->frame - M->fsPendingSince >= 150);
#endif
}

// a file select action on file N (create on a file shown new, delete / copy onto one shown in use): the save sequence's
// startCreateUserFile / startDeleteUserFile / storeCopyUserFile + SaveAllWithoutKeyWait (the hook decides)
static void fileSelectAction(int kind, int slot, int src) {
    const bool fresh = !M->cacheStale && strcmp(M->cacheRoot, nandRoot().c_str()) == 0 && strcmp(M->cacheRoot, gW.imageRoot) == 0;
    M->stat[S_FS_ACTION]++;
    M->fsSlot = fresh && strcmp(M->cacheRoot, gSpRoot.c_str()) == 0 ? slot : 0;
    if (fresh && strcmp(M->cacheRoot, gMpRoot.c_str()) == 0) M->permitCoopMask |= 1 << slot;
    note("file select %s file %d (cache %s, %s)", kind == 0 ? "create" : kind == 1 ? "delete" : "copy", slot, M->cacheRoot,
         fresh ? "fresh" : "STALE");
    if (kind == 2) {
        mp_save_copy_slot(gW.image, src, gW.image, slot);
    } else {
        gW.seq.restoreUserFile(gW.user, slot, true);
        gW.user->resetAllData();
        // initializeUserFileMemory(slot)
        uint32_t off, size;
        char n[16];
        snprintf(n, sizeof n, "mario%d", slot);
        if (mp_save_find(gW.image, n, &off, &size)) gW.user->makeGameDataBinary(gW.image + off, size);
        snprintf(n, sizeof n, "luigi%d", slot);
        if (mp_save_find(gW.image, n, &off, &size)) gW.user->makeGameDataBinary(gW.image + off, size);
        snprintf(n, sizeof n, "config%d", slot);
        if (mp_save_find(gW.image, n, &off, &size)) gW.user->makeConfigDataBinary(gW.image + off, size);
    }
    if (M->fsSlot) allowSpContent(slot);
    if (MpSaveHook::suppressed()) {
        M->stat[S_FS_BLOCKED]++;
        note("file select save suppressed");
        return;
    }
    if (!fresh) {
        violation("(iii) the file select (cache from %s) writes to %s", M->cacheRoot, nandRoot().c_str());
    }
    gameWriteImage("file select");
}

// ---- play ----------------------------------------------------------------------------------------------------------------
static void playProgress(int tag, int slot) {
    const int id = newProg(tag, slot);
    encode(M->progs[id].mask, gW.user->game);
}
static int playTag(int* slot);
static void playFull() {   // (scripts) all 50 stars: more progress than any file the harness starts with
    int slot = 0;
    const int tag = playTag(&slot);
    const int id = newProg(tag, slot);
    M->progs[id].mask = 0;
    encode(0, gW.user->game);
}
static void playNow() {
    int slot = 0;
    const int tag = playTag(&slot);
    playProgress(tag, slot);
}
static int playTag(int* slot) {
    if (M->phase == P_SPPLAY) {
        *slot = M->spPlaySlot;
        return T_SP;
    }
    const bool original = M->choice != C_GUEST && !M->migrated && !M->foreign;
    if (!original) {
        *slot = 0;
        return T_FOREIGN;
    }
    if (M->mine) {
        *slot = M->spFile;
        return T_MINE;
    }
    if (M->choice == C_NONE) {
        *slot = 0;
        return T_NOCHOICE;
    }
    *slot = M->coopSlot;
    return T_COOP;
}

// ---- session -------------------------------------------------------------------------------------------------------------
static void foreignSnapshot() {
    uint8_t mario[0xF80];
    const int id = newProg(T_FOREIGN, 0);
    encode(M->progs[id].mask, mario);
    const uint32_t len = mp_save_snapshot_pack(mario, 0xF80, M->blankGame, 0xF80, M->blankConfig, 0x60, 1, MP_SAVE_SNAP_FLAG_MARIO,
                                               gW.net.snap, sizeof gW.net.snap);
    gW.net.snapLen = len;
    M->stat[S_SNAPSHOT]++;
}
static void sessionFrame() {
    frameTick();
    MpSaveGame::tick();
    // (a migration counts for the progress from the first game frame that runs with it: before that no frame ran, nothing changed)
    if (gW.net.active && gW.net.epoch != 0) M->migrated = true;
    if (gW.net.active && chance(30)) {
        if (MpSaveGame::applySnapshotIfReady()) {
            M->foreign = true;
            note("snapshot applied");
        }
    }
    if (!gW.net.active && gW.net.phase == MP_UI_IDLE && M->phase == P_SESSION) {
        M->phase = P_ENDED;
        note("session over (left / refused)");
    }
}

// MpMenuSequence::prepareHostSave as built (and as the lobby's Start / the launch call it)
static bool prepareHostSave() {
    bool ok = false;
    switch (M->choice) {
    case C_COOP:
    case C_QUICK: ok = MpSaveGame::enterSessionRoot(); break;
    case C_COPY:
        M->permitCoopMask |= 1 << M->coopSlot;
        ok = MpSaveGame::importSinglePlayerFile(M->spFile, M->coopSlot) && MpSaveGame::isSessionRoot();
        break;
    case C_MINE:
        ok = (!MpSaveGame::isSessionRoot() && !MpSaveGame::heldAfterSession()) || MpSaveGame::leaveSessionRoot();
        // (the model: from here the player has chosen "Play on my file N": a backup of the file as it is now must exist)
        if (ok) {
            M->permitSp = M->spFile;
            memcpy(M->spAtPermit, M->sp, M->spLen);
            M->spAtPermitLen = M->spLen;
            memcpy(M->mineAt[M->spFile], M->sp, M->spLen);
            M->mineAtLen[M->spFile] = M->spLen;
        }
        ok = ok && MpSaveGame::backupSinglePlayerFile();
        if (ok) MpSaveGame::beginSinglePlayerSession(M->spFile);
        break;
    default: break;
    }
    if (!ok) {
        MpSaveGame::abandonHostChoice();
        if (M->choice == C_MINE) M->permitSp = 0;
    }
    if (ok && (M->choice == C_COOP || M->choice == C_QUICK || M->choice == C_COPY)) M->permitCoopMask |= 1 << M->coopSlot;
    if (ok && M->choice == C_MINE) M->mine = true;
    return ok;
}

// the menu offers "Play on my file N" only for a file that exists (MpMenuFiles.cpp: spStars(file) < 0 sounds the buzzer: never created or
// unreadable). A never-created file is blank in the image; the harness must not start a session on one (seed 7065 did).
static bool spFileExists(int slot) {
    const uint8_t* b[3];
    uint32_t l[3];
    slotBlocks(M->sp, slot, b, l);
    return identify(b[0], l[0]).tag != T_BLANK;
}
static bool logHas(const char* what) {   // (scripts: a line of the last 64 log lines)
    const int from = gW.logN > 64 ? gW.logN - 64 : 0;
    for (int i = from; i < gW.logN; i++)
        if (strstr(gW.logs[i % 64], what)) return true;
    return false;
}
static int gForceSpFile = 0;   // (scripts: the file the player picks)
static bool gAllowBlankSp = getenv("SAVELIFE_OLD_MODEL") != nullptr;   // SAVELIFE_OLD_MODEL=1: random runs may start sessions on never-created files (the save layer must hold anyway)   // (script M3 only: the old model that started a session on a never-created file)
static void startSession(int choice) {
    M->coopSlot = 1 + rn(6);
    M->spFile = 1 + rn(6);
    if (gForceSpFile) M->spFile = gForceSpFile;
    if (choice == C_MINE && !gAllowBlankSp) {
        int f = M->spFile, tries = 0;
        while (tries < 6 && !spFileExists(f)) {
            f = f % 6 + 1;
            tries++;
        }
        if (tries >= 6) {
            note("no single-player file exists: the menu offers no \"my file\"; co-op file instead");
            choice = C_COOP;
        } else {
            M->spFile = f;
        }
    }
    M->choice = choice;
    M->mine = M->migrated = M->foreign = false;
    M->spPlaySlot = 0;
    M->fsSlot = 0;
    M->permitCoopMask = 0;
    M->permitSp = 0;
    M->stat[S_SESSIONS]++;
    note("session: %s coop=%d sp=%d menu=%s", choiceName(choice), M->coopSlot, M->spFile, M->menuNew ? "new" : "built");
    const bool host = choice != C_GUEST;
    // lobby (room open): the host's Start runs prepareHostSave for a menu choice (not for Quick Match / straight in)
    gW.net = Net();
    gW.net.active = true;
    gW.net.localSlot = host ? 0 : 1;
    gW.net.hostSlot = 0;
    gW.net.phase = MP_UI_LOBBY;
    for (int i = 0; i < 3; i++) sessionFrame();
    if (host && (choice == C_COOP || choice == C_COPY || choice == C_MINE) && !prepareHostSave()) {
        note("lobby: save preparation failed (no start)");
        mp_ui_leave();
        MpSaveGame::abandonHostChoice();
        M->phase = P_ENDED;
        return;
    }
    gW.net.phase = MP_UI_PLAYING;
    // MpMenuSequence::watchSession: the launch
    int file = M->mine ? M->spFile : M->coopSlot;
    bool ready = true;
    if (!host) {
        ready = MpSaveGame::enterSessionRoot();
        file = MpSaveGame::sessionSlot();
        if (!M->menuNew) ready = true;   // as built: the result was ignored
        if (rn(2)) foreignSnapshot();
    } else if (M->menuNew) {
#if SAVELIFE_API >= 2
        if (!MpSaveGame::launchReady()) ready = prepareHostSave();
#else
        if (choice == C_QUICK || choice == C_NONE) ready = prepareHostSave();
#endif
    }
    if (!ready) {
        note("launch refused by the menu");
        M->stat[S_LAUNCH_REFUSED]++;
        mp_ui_leave();
        M->phase = P_ENDED;
        return;
    }
#if SAVELIFE_API >= 2
    MpSaveGame::fileSelectClosed();   // FileSelector::exeDemo
#endif
    gW.seq.restoreUserFile(gW.user, file, true);
    M->phase = P_SESSION;
    note("launch file %d (root %s)", file, nandRoot().c_str());
}

static void migrate(bool silentRejoin) {
    M->stat[S_MIGRATION]++;
    if (silentRejoin) {
        gW.net.active = false;
        gW.net.phase = MP_UI_RECONNECTING;
        for (int i = 0, n = 1 + rn(4); i < n; i++) sessionFrame();
    }
    gW.net.epoch++;
    if (gW.net.localSlot == gW.net.hostSlot) gW.net.hostSlot = gW.net.localSlot == 0 ? 1 : 0;
    else if (chance(50)) gW.net.hostSlot = gW.net.localSlot;   // a guest becomes host
    gW.net.active = true;
    gW.net.phase = MP_UI_PLAYING;
    note("migration epoch=%u host=%d%s", (unsigned)gW.net.epoch, gW.net.hostSlot, silentRejoin ? " (silent rejoin)" : "");
    if (gW.net.localSlot != gW.net.hostSlot && chance(70)) foreignSnapshot();
}

// session end: the leave question (offerSaveOnLeave / saveOnLeave), as the menu asks it
static void leaveDecision() {
    MpSaveGame::LeaveOffer o;
    if (!MpSaveGame::offerSaveOnLeave(&o) || !o.offer) return;
    const int r = rn(10);
    if (r < 4) {
        const int slot = 1 + rn(6);
        M->permitCoopMask |= 1 << slot;
        if (MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_SAVE_COOP, slot)) M->stat[S_LEAVE_COOP]++;
    } else if (r < 7 && o.single) {
        M->permitSp = o.singleSlot;
        memcpy(M->spAtPermit, M->sp, M->spLen);
        M->spAtPermitLen = M->spLen;
        if (MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_SAVE_SINGLE, 0)) M->stat[S_LEAVE_SP]++;
    } else if (r < 9) {
        MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_DONT_SAVE, 0);
    }
}

static void recoveryStep() {
    MpSaveGame::RecoveryInfo info;
    if (!MpSaveGame::recoveryPending(&info)) return;
    const int r = rn(10);
    if (r < 4) {
        const int slot = 1 + rn(6);
        M->permitCoopMask |= 1 << slot;
        if (MpSaveGame::recoveryKeep(slot, MpSaveGame::LEAVE_SAVE_COOP)) M->stat[S_RECOVERY]++;
    } else if (r < 7 && info.single) {
        M->permitSp = info.sourceSlot;
        memcpy(M->spAtPermit, M->sp, M->spLen);
        M->spAtPermitLen = M->spLen;
        // savebug: the menu asks "this file has more stars" first when it applies; the player answers Yes about half the time
        const bool asked = MpSaveGate::singleOverwriteWarning((int)info.stars, info.spStars, info.spNewer);
        const bool yes = asked && chance(50);
        if (MpSaveGame::recoveryKeep(0, MpSaveGame::LEAVE_SAVE_SINGLE, yes)) M->stat[S_RECOVERY]++;
        // (a refused or failed keep changes nothing: checkAll in the step; the menu shows the buzzer and the player may try again. The layer
        // looks at the file again at the keep, so a file that became readable or newer since the offer is refused unasked)
    } else {
        MpSaveGame::recoveryDiscard();
    }
}

// ---- faults --------------------------------------------------------------------------------------------------------------
static void armFaults(bool allowKill) {
    gW.faults.clear();
    gW.faults.renameNoReplace = true;   // the Vita (sceIoRename) as the port treats it
    if (!chance(35)) return;
    M->stat[S_FAULTS]++;
    switch (rn(9)) {
    case 0: gW.busyFrames = 10 + rn(80); break;
    case 1:
        gW.faults.readEio = 1 + rn(2);
        snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "%s", chance(50) ? "/mp/saves/" : "/saves/title");
        break;
    case 2: gW.faults.hostAllocFail = 1 + rn(3); break;
    case 3: gW.faults.mallocFail = 1 + rn(3); break;
    case 4: gW.faults.renameFail = 1 + rn(2); break;
    case 5: gW.faults.cardFree = rn(3) * 20000; break;
    case 6: gW.faults.shortWrite = 1 + rn(2); break;
    case 7: gW.faults.copySkip = 1; break;
    case 8:
        if (allowKill) {
            gW.faults.killAt = 1 + rn(14);
            gW.faults.killAfter = chance(50);
            gW.faults.tear = chance(50);
        }
        break;
    }
}

static void killHook(const char* what, const char* path) {
    M->stat[S_KILLS]++;
    M->deferCoopMask |= M->permitCoopMask;
    note("power cut at %s %s", what, path ? path : "");
}

// ---- mp.cfg --------------------------------------------------------------------------------------------------------------
// a save of key = val is about to start: until it is seen to finish, the file may hold the old value or this one
static void cfgPending(const char* key, const char* val) {
    for (int i = 0; i < M->ncfg; i++)
        if (!strcmp(M->cfg[i].key, key)) {
            if (M->cfg[i].carried && M->cfg[i].alt[0]) snprintf(M->cfg[i].alt2, sizeof M->cfg[i].alt2, "%s", M->cfg[i].alt);   // (still possible)
            M->cfg[i].carried = false;
            snprintf(M->cfg[i].alt, sizeof M->cfg[i].alt, "%s", val);
            return;
        }
}
// a save that finished writes EVERY value the game holds in memory, including one whose own save failed or was cut earlier (the player's
// look stays in memory; the next save, for another setting, takes it along): what was only "pending" is now the file's value
static void cfgSettle() {
    for (int i = 0; i < M->ncfg; i++)
        if (M->cfg[i].alt[0] && !M->cfg[i].carried) {
            snprintf(M->cfg[i].val, sizeof M->cfg[i].val, "%s", M->cfg[i].alt);
            M->cfg[i].alt[0] = M->cfg[i].alt2[0] = 0;
        }
}
static void cfgExpect(const char* key, const char* val) {
    if (M->verbose || getenv("SAVELIFE_CFGTRACE")) note("cfgExpect %s = %s", key, val);
    for (int i = 0; i < M->ncfg; i++)
        if (!strcmp(M->cfg[i].key, key)) {
            snprintf(M->cfg[i].val, sizeof M->cfg[i].val, "%s", val);
            M->cfg[i].alt[0] = M->cfg[i].alt2[0] = 0;   // (a finished save settles it)
            M->cfg[i].carried = false;
            return;
        }
    if (M->ncfg < 32) {
        snprintf(M->cfg[M->ncfg].key, sizeof M->cfg[0].key, "%s", key);
        snprintf(M->cfg[M->ncfg].val, sizeof M->cfg[0].val, "%s", val);
        M->cfg[M->ncfg].alt[0] = M->cfg[M->ncfg].alt2[0] = 0;
        M->cfg[M->ncfg].carried = false;
        M->ncfg++;
    }
}
static void cfgStep() {
    MpConfig c = *mp_config();
    const int r = rn(4);
    if (r == 0) {
        char name[16];
        snprintf(name, sizeof name, "P%04d", rn(10000));
        snprintf(c.name, sizeof c.name, "%s", name);
        mp_config_set(&c);
        cfgPending("name", name);
        if (mp_config_save()) {
            M->stat[S_CFG_SAVE]++;
            cfgSettle();
            cfgExpect("name", name);
        }
    } else if (r == 1) {
        const uint8_t was = c.look;
        c.look = (uint8_t)rn(2);
        mp_config_set(&c);
        char v[8];
        snprintf(v, sizeof v, "%d", c.look);
        if (c.look != was) cfgPending("look", v);
        if (mp_config_save()) {
            M->stat[S_CFG_SAVE]++;
            cfgSettle();
            if (c.look != was) cfgExpect("look", v);   // (the same value as shown: nothing changed, the file's own text stays)
        }
    } else if (r == 2) {
        uint8_t key[16];
        mp_config_console_key(key);
    } else {
        if (mp_config_save()) {
            M->stat[S_CFG_SAVE]++;
            cfgSettle();
        }
    }
}

// ---- one step ------------------------------------------------------------------------------------------------------------
static void clearPermits() {
    M->permitSp = 0;
    M->permitCoopMask = 0;
    M->spPlaySlot = 0;
    M->fsSlot = 0;
    M->restoreSlotMask = 0;
    M->mine = false;
}
// the boot after a power cut: the files are checked with the permits of the step the cut interrupted, then they end
static void boot() {
    gW.init();
    gameBoot();
    M->booted = true;
    checkAll();
    clearPermits();
    M->phase = P_TITLE;
    fileSelectInit();
}

static void stepTitle() {
    const int r = rn(100);
    if (r < 25) {
        for (int i = 0, n = 1 + rn(45); i < n; i++) fileSelectFrame();
    } else if (r < 38) {
        if (!fsInputOpen() && !M->hostileFs) return;
        // the player picks what the file select shows
        const int slot = 1 + rn(6);
        if (chance(50)) fileSelectAction(chance(50) ? 0 : 1, slot, 0);
        else fileSelectAction(2, slot, 1 + rn(6));
    } else if (r < 50) {
        if (!fsInputOpen()) return;
        // single-player play of a file (FileSelector::exeDemo: the file from the image)
        const int slot = 1 + rn(6);
        M->spPlaySlot = slot;
#if SAVELIFE_API >= 2
        MpSaveGame::fileSelectClosed();   // FileSelector::exeDemo
#endif
        gW.seq.restoreUserFile(gW.user, slot, true);
        M->phase = P_SPPLAY;
        note("single-player play file %d (root %s)", slot, nandRoot().c_str());
    } else if (r < 85) {
        static const int choices[] = {C_COOP, C_COPY, C_MINE, C_QUICK, C_NONE, C_GUEST, C_GUEST, C_MINE};
        M->menuNew = chance(60);
        startSession(choices[rn(8)]);
    } else if (r < 92) {
        recoveryStep();
    } else if (r < 96) {
        cfgStep();
#if SAVELIFE_API >= 2
    } else if (r < 99) {
        MpSaveGame::BackupList list;
        if (MpSaveGame::listBackups(&list) && list.count > 0 && fsInputOpen()) {
            M->restoreSlotMask = 0x7E;   // a restore may change any file: checked here
            std::vector<uint8_t> want;
            const std::string path = gBackups + "/" + list.name[rn(list.count)];
            readAll(path, want);
            const bool ok = MpSaveGame::restoreBackup(path.c_str());
            gW.faults.clear();   // (the checks below read the card without the step's injected faults)
            std::vector<uint8_t> cur;
            readAll(gSpPath, cur);
            if (ok && cur != want) violation("restore reported success but the file is not the backup");
            if (ok && !backupHolds(M->sp, M->spLen)) violation("restore without a backup of the file it replaced");
            if (!ok && (cur.size() != M->spLen || memcmp(cur.data(), M->sp, M->spLen) != 0) && cur != want)
                violation("a failed restore left a third state");
            if (ok) M->stat[S_RESTORE]++;
            checkSp();
            M->restoreSlotMask = 0;
            fileSelectInit();   // (the menu rebuilds the file select after a restore)
        }
#endif
    } else {
        // plain kill between steps (the player turns the console off)
        M->stat[S_KILLS]++;
        note("power off");
        _exit(kKillExit);
    }
}

static void stepSpPlay() {
    const int r = rn(10);
    if (r < 4) {
        playNow();
    } else if (r < 8) {
        if (!spAtomic()) gW.faults.killAt = 0;   // (a power cut during the game's own single-player write is the original game's risk)
        gameAutosave("single-player");
    } else {
        fileSelectInit();
    }
}

static void stepSession() {
    const int r = rn(100);
    if (r < 25) {
        for (int i = 0, n = 1 + rn(20); i < n; i++) sessionFrame();
    } else if (r < 40) {
        playNow();
    } else if (r < 55) {
        gameAutosave("session");
    } else if (r < 63) {
        // the reviewer's race: the net thread ends the session after this frame's tick; the save nerve runs first
        playNow();
        gW.net.active = false;
        gW.net.phase = chance(50) ? MP_UI_IDLE : MP_UI_FAILED;
        M->stat[S_RACE]++;
        gameAutosave("race");
        M->phase = P_ENDED;
    } else if (r < 70) {
        migrate(chance(60));
    } else if (r < 76) {
        if (gW.net.localSlot != gW.net.hostSlot) foreignSnapshot();
    } else if (r < 84) {
        // the session ends (link drop / host gone / the player leaves)
        mp_ui_leave();
        for (int i = 0, n = rn(3); i < n; i++) sessionFrame();
        M->phase = P_ENDED;
    } else {
        for (int i = 0; i < 60; i++) sessionFrame();
    }
}

static void stepEnded() {
    const int r = rn(10);
    if (r < 2) {
        for (int i = 0, n = 1 + rn(10); i < n; i++) sessionFrame();
        if (chance(30)) gameAutosave("after session");
    } else if (r < 7) {
        leaveDecision();
        fileSelectInit();
    } else {
        fileSelectInit();   // undecided: the recovery file stays
    }
}

static void runStep() {
    M->step++;
    M->stat[S_STEPS]++;
    M->clock += chance(5) ? -(int64_t)rn(400000000) : rn(4000);   // the console clock may jump back (date reset / changed)
    if (M->clock < 946684800) M->clock = 946684800;
    armFaults(M->phase != P_SPPLAY);
    switch (M->phase) {
    case P_TITLE: stepTitle(); break;
    case P_SPPLAY: stepSpPlay(); break;
    case P_SESSION: stepSession(); break;
    default: stepEnded(); break;
    }
    gW.faults.clear();
    checkAll();
    // permits end with what they were for: at the title nothing is chosen any more; single-player play keeps its file
    if (M->phase == P_TITLE) clearPermits();
    if (M->phase == P_SESSION || M->phase == P_ENDED) {
        if (!M->mine) M->permitSp = 0;
        M->fsSlot = 0;
    }
}

// one process lifetime: boot (also after a power cut), then steps until the sequence ends
// a script may name a second part that runs in the relaunch after its power cut (right after the boot's checks)
static void (*gFollow)() = nullptr;

static void lifetime() {
    gW.killHook = killHook;
    gW.rngState = M->rng | 1;
    gW.user = new UserFile();
    gW.seq.mCurrentUserFile = gW.user;
    for (int i = 0; i < M->ncfg; i++) M->cfg[i].carried = M->cfg[i].alt[0] != 0;   // (a restart: what was pending has either happened or not)
    boot();
    if (gFollow) {
        void (*f)() = gFollow;
        gFollow = nullptr;
        f();
        checkAll();
    }
    while (M->step < M->nsteps && M->violations == 0) runStep();
    _exit(0);
}

// ---- setup ---------------------------------------------------------------------------------------------------------------
static bool loadTemplates(const char* savePath) {
    std::vector<uint8_t> b;
    if (!readAll(savePath, b) || mp_save_validate(b.data(), (uint32_t)b.size(), true, true) != MP_SAVE_OK) return false;
    memcpy(M->base, b.data(), b.size());
    M->baseLen = (uint32_t)b.size();
    uint32_t off, size;
    mp_save_find(M->base, "mario1", &off, &size);
    memcpy(M->blankGame, M->base + off, 0xF80);
    mp_save_find(M->base, "config1", &off, &size);
    memcpy(M->blankConfig, M->base + off, 0x60);
    mp_save_find(M->base, "sysconf", &off, &size);
    memcpy(M->sysconf, M->base + off, 0x80);
    mp_save_find(M->base, "mario2", &off, &size);
    memcpy(M->baseMario, M->base + off, 0xF80);
    // the star flags of file 2: every set bit whose clearing changes the star count by one
    const int stars = mp_save_block_stars(M->baseMario, 0xF80);
    M->nStars = 0;
    for (uint32_t i = 0; i < 0xF80 && M->nStars < 50; i++)
        for (int bit = 0; bit < 8 && M->nStars < 50; bit++) {
            if (!(M->baseMario[i] & (1u << bit))) continue;
            M->baseMario[i] &= (uint8_t)~(1u << bit);
            const int s = mp_save_block_stars(M->baseMario, 0xF80);
            MpSaveSnapshotView v;
            v.block[0] = M->baseMario;
            v.block[1] = M->blankGame;
            v.block[2] = M->blankConfig;
            v.len[0] = v.len[1] = 0xF80;
            v.len[2] = 0x60;
            const bool ok = s == stars - 1 && mp_save_check_view(&v, MP_SAVECHECK_FULL, nullptr) == 0;
            M->baseMario[i] |= (uint8_t)(1u << bit);
            if (ok) {
                M->starByte[M->nStars] = (int)i;
                M->starBit[M->nStars] = bit;
                M->nStars++;
            }
        }
    return M->nStars >= 40;
}

static void rmrf(const std::string& d) {
    std::string c = "rm -rf '" + d + "'";
    if (system(c.c_str()) != 0) {}
}

// a console as the player has it: a single-player file with progress in some files, maybe a co-op file, an mp.cfg
static void setupSequence(uint64_t seed, int steps) {
    const long keepStats[S_COUNT] = {};
    (void)keepStats;
    M->seed = seed;
    M->rng = seed * 0x9E3779B97F4A7C15ull + 1;
    M->step = 0;
    M->nsteps = steps;
    M->lifetimes = 0;
    M->booted = false;
    M->violations = 0;
    M->why[0] = 0;
    M->nprogs = 1;
    M->ncfg = 0;
    M->frame = 0;
    M->hostileFs = chance(25);   // a player who presses through a held file select (models a FileSelector without the guard)
    M->clock = 1762600000 + rn(1000000);
    M->permitSp = M->spPlaySlot = M->fsSlot = M->permitCoopMask = M->restoreSlotMask = 0;
    memset(M->mineAtLen, 0, sizeof M->mineAtLen);
    M->deferCoopMask = 0;
    memset(M->allowSpN, 0, sizeof M->allowSpN);
    M->allowBlank = 0;
    rmrf(gDir);
    mp_save_mkdirs((gSpRoot + "/title/00010000/524d4750/data").c_str());
    mp_save_mkdirs(gCfgDir.c_str());
    // single-player file: files 1..6 with this console's own progress (some never created)
    static uint8_t img[0x10000];
    memset(img, 0, sizeof img);
    memcpy(img, M->base, M->baseLen);
    for (int s = 1; s <= 6; s++) {
        uint32_t off, size;
        char n[16];
        snprintf(n, sizeof n, "mario%d", s);
        mp_save_find(img, n, &off, &size);
        if (chance(70)) encode(M->progs[newProg(T_SP, s)].mask, img + off);
        else memcpy(img + off, M->blankGame, 0xF80);
    }
    mp_save_finalize(img, sizeof img);
    writeRaw(gSpPath, img, M->baseLen);
    memcpy(M->sp, img, M->baseLen);
    M->spLen = M->baseLen;
    // a co-op file from earlier sessions (or none yet)
    M->coopExists = chance(60);
    if (M->coopExists) {
        mp_save_mkdirs((gMpRoot + "/title/00010000/524d4750/data").c_str());
        for (int s = 1; s <= 6; s++) {
            uint32_t off, size;
            char n[16];
            snprintf(n, sizeof n, "mario%d", s);
            mp_save_find(img, n, &off, &size);
            if (chance(60)) encode(M->progs[newProg(T_COOP, s)].mask, img + off);
            else memcpy(img + off, M->blankGame, 0xF80);
        }
        mp_save_finalize(img, sizeof img);
        writeRaw(gMpPath, img, M->baseLen);
        memcpy(M->coop, img, M->baseLen);
        M->coopLen = M->baseLen;
    }
    // mp.cfg: the player's values, a value and a key from a newer build
    char cfg[1024];
    char name[16];
    snprintf(name, sizeof name, "Alex%d", rn(1000));
    const int look = rn(10) < 8 ? rn(2) : 9;   // 9: a look a newer build has
    snprintf(cfg, sizeof cfg,
             "# Super Mario Galaxy Vita: multiplayer settings (written by the game)\nversion = 1\nname = %s\nlook = %d\nconnection = 2\n"
             "max_players = 3\nrivals = 2\nconsole_key = 0123456789abcdef0123456789abcdef\nfuture_option = %d\n",
             name, look, rn(50));
    writeRaw(gCfgDir + "/mp.cfg", (const uint8_t*)cfg, strlen(cfg));
    char* save = nullptr;
    for (char* line = strtok_r(cfg, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        std::string k(line, eq - line);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        cfgExpect(k.c_str(), eq + 2);
    }
}

// one sequence: lifetimes until it ends; true when no invariant broke
static void (*gScriptBoot)() = nullptr;     // (a script that needs its own boot: files prepared and faults armed before the boot repair)
static void (*gScriptFollow)() = nullptr;   // (set by main for the script about to run)
static bool runSequence(uint64_t seed, int steps, void (*scripted)()) {
    setupSequence(seed, steps);
    for (int life = 0; life < 400; life++) {
        M->lifetimes = life;
        fflush(stderr);
        const pid_t pid = fork();
        if (pid == 0) {
            if (scripted) {
                gW.killHook = killHook;
                gW.rngState = M->rng | 1;
                gW.user = new UserFile();
                gW.seq.mCurrentUserFile = gW.user;
                if (gScriptBoot) gScriptBoot();
                else boot();
                scripted();
                _exit(0);
            }
            lifetime();
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == kKillExit) {
            if (scripted) {
                scripted = nullptr;   // after a power cut in a script: the relaunch boots and checks
                gFollow = gScriptFollow;
                M->nsteps = M->step;
            }
            continue;
        }
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return M->violations == 0;
        fprintf(stderr, "CRASH seed=%llu step=%d status=%d\n", (unsigned long long)seed, M->step, st);
        M->violations++;
        return false;
    }
    return M->violations == 0;
}

// ---- scripted lifecycles (one per finding) -------------------------------------------------------------------------------
static void title(int frames) { for (int i = 0; i < frames; i++) fileSelectFrame(); }
static void session(int frames) { for (int i = 0; i < frames; i++) sessionFrame(); }

// finding 1: back at the file select from a co-op session with the save sequence busy: the switch to the single-player root
// waits; it succeeds before the warning; the player creates a file the (stale) file select shows as new
static void scriptF1() {
    M->menuNew = true;
    startSession(C_GUEST);
    session(5);
    mp_ui_leave();
    session(2);
    M->phase = P_ENDED;
    gW.busyFrames = 40;
    fileSelectInit();
    title(70);
    // the file select's view: a file that is new in its cache
    for (int s = 1; s <= 6; s++) {
        if (fsInputOpen() && !slotBlank(M->sp, s)) {
            fileSelectAction(0, s, 0);
            break;
        }
    }
    title(5);
    checkAll();
}
// finding 2a: a host straight into a running room (no save choice) on the single-player root; the session ends between the
// frame's tick and the next save nerve
static void scriptF2a() {
    M->menuNew = false;
    startSession(C_NONE);
    session(5);
    playNow();
    gW.net.active = false;
    gW.net.phase = MP_UI_IDLE;
    gameAutosave("race");
    checkAll();
}
// finding 2b: a guest on the multiplayer root, the same race: the host's progress over the co-op file
static void scriptF2b() {
    M->menuNew = true;
    if (!M->coopExists) return;
    startSession(C_GUEST);
    foreignSnapshot();
    session(10);
    gW.net.active = false;
    gW.net.phase = MP_UI_IDLE;
    gameAutosave("race");
    checkAll();
}
// finding 3: "Play on my file" with the console clock behind the newest backups: the session's backup must survive
static void scriptF3() {
    M->menuNew = true;
    for (int i = 0; i < 10; i++) MpSaveGame::backupSinglePlayerFile();   // earlier backups at today's date
    // the player plays single-player afterwards: the file is no longer what those backups hold
    const int s = 1 + rn(6);
    M->spPlaySlot = s;
    gW.seq.restoreUserFile(gW.user, s, true);
    M->phase = P_SPPLAY;
    playProgress(T_SP, s);
    gameAutosave("single-player");
    checkAll();
    fileSelectInit();
    M->clock -= 200000000;   // the clock is set back
    startSession(C_MINE);
    session(5);
    playNow();
    gameAutosave("session");
    checkAll();
}
// finding 5: "Play on my file" N; link drop, a migration, a silent rejoin, the new host's snapshot; then "save to your file N"
static void scriptF5() {
    M->menuNew = true;
    startSession(C_MINE);
    session(5);
    playNow();
    gameAutosave("session");
    checkAll();
    migrate(true);
    foreignSnapshot();
    session(40);
    while (gW.net.snapLen && MpSaveGame::applySnapshotIfReady() == false && gW.net.active) session(1);
    M->foreign = true;
    mp_ui_leave();
    session(2);
    MpSaveGame::LeaveOffer o;
    if (MpSaveGame::offerSaveOnLeave(&o) && o.single) {
        M->permitSp = o.singleSlot;   // the player says Yes to "save to your single-player file N"
        memcpy(M->spAtPermit, M->sp, M->spLen);
        M->spAtPermitLen = M->spLen;
        MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_SAVE_SINGLE, 0);
    }
    checkAll();
}
// finding 6: a host chooses its co-op file; the co-op GameData.bin cannot be read for a moment; it plays and autosaves
static void scriptF6() {
    M->menuNew = true;
    if (!M->coopExists) return;
    gW.faults.readEio = 2;   // (the repair check's read and the load's read)
    snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "/mp/saves/");
    startSession(C_COOP);
    gW.faults.clear();
    session(5);
    playNow();
    gameAutosave("session");
    checkAll();
}
// finding 7: a switch to the multiplayer root fails its self-check and the switch back fails too (no memory); the image is
// half-built; the player goes on (a single-player file from the title) and the game saves
static void scriptF7() {
    M->menuNew = true;
    if (!M->coopExists) return;
    // loadRootIntoImage(mp) fails its self-check (the game's copy left a block out); switchRoot's restore needs a new Scratch: the
    // switch's own two buffers are fine, the restore's fail
    gW.faults.copySkip = 1;
    gW.faults.hostAllocAfter = 2;
    gW.faults.hostAllocFail = 2;
    const bool ok = MpSaveGame::enterSessionRoot();
    gW.faults.clear();
    note("switch with a failed restore: %s", ok ? "ok" : "failed");
    const int s = 1 + rn(6);
    M->spPlaySlot = s;
    gW.seq.restoreUserFile(gW.user, s, true);
    M->phase = P_SPPLAY;
    playProgress(T_SP, s);
    gameAutosave("single-player");
    checkAll();
}
// finding 8: mp.cfg cannot be read at boot (both copies), the game makes its console key and saves, the player changes the name
static void scriptF8() {
    gW.faults.fopenFail = 2;
    snprintf(gW.faults.fopenFailMatch, sizeof gW.faults.fopenFailMatch, "mp.cfg");
    mp_config_load();
    gW.faults.clear();
    uint8_t key[16];
    mp_config_console_key(key);
    MpConfig c = *mp_config();
    snprintf(c.name, sizeof c.name, "Renamed");
    mp_config_set(&c);
    if (mp_config_save()) cfgExpect("name", "Renamed");
    mp_config_save();
    checkAll();
}
// seed 4436: the player renames himself; the power goes right AFTER the final rename of mp.cfg.tmp (the new name is the file, the old one
// is in mp.cfg.bak); after the restart the next save moves the new file to .bak. The old name is then in no file, and that is right:
// the player had replaced it. The model must accept the old OR the new value of a save that was cut, and still demand one of them.
static void setName(const char* n) {
    MpConfig c = *mp_config();
    snprintf(c.name, sizeof c.name, "%s", n);
    mp_config_set(&c);
}
static void scriptCfgCut() {
    mp_config_load();
    setName("DryRun");
    const long before = gW.faults.ops;
    if (!mp_config_save()) violation("cfgcut: the dry-run save failed");
    cfgExpect("name", "DryRun");
    const long perSave = gW.faults.ops - before;
    if (gW.faults.fsyncs < 1) violation("cfgcut: mp.cfg.tmp was renamed into place without an fsync");   // (coordinator: settings writers sync)
    setName("NewName");
    cfgPending("name", "NewName");
    gW.faults.killAt = gW.faults.ops + perSave;   // the last write step of a save is rename(mp.cfg.tmp, mp.cfg)
    gW.faults.killAfter = true;
    mp_config_save();
    violation("cfgcut: the power cut did not happen");
}
static void followCfgCut() {
    char p[300];
    snprintf(p, sizeof p, "%s/mp.cfg", gCfgDir.c_str());
    if (!cfgFileHas(p, CfgKey{"name", "NewName", ""})) violation("cfgcut: the cut after the rename should have left the new name in mp.cfg");
    mp_config_load();
    if (!mp_config_save()) violation("cfgcut: the save after the restart failed");
    checkAll();
    if (cfgFileHas(gCfgDir + "/mp.cfg", CfgKey{"name", "DryRun", ""}) || cfgFileHas(gCfgDir + "/mp.cfg.bak", CfgKey{"name", "DryRun", ""}))
        note("cfgcut: the old name is still on disk (fine)");
}
// seed 7065: "Play on my file" on a file that was never created. The menu cannot do that (the buzzer), so the harness must not either:
// asked for file 1 after the player deleted it, the model picks an existing file (or the co-op file when none exists).
static void scriptMineBlank() {
    M->menuNew = true;
    fileSelectInit();
    fileSelectAction(1, 1, 0);   // delete file 1 (a blank file now)
    if (spFileExists(1)) violation("mineblank: file 1 should be blank");
    gForceSpFile = 1;
    startSession(C_MINE);
    gForceSpFile = 0;
    if (M->choice == C_MINE && M->spFile == 1) violation("mineblank: a session on a never-created file was started");
    checkAll();
}
// the same chain on a file that exists (what a player can really do): an unplayed "Play on my file N" session leaves a recovery file
// that holds the file as it was; a second session on N plays on and is saved into N; the player says "don't keep" for it.
// savebug: the older recovery file (fewer stars) is then
//   notNewer=false (the file was saved after it): not offered at all (moved to recovery/discarded), file N untouched   [M2]
//   notNewer=true  (clock ahead, so the file does not look newer): offered; a keep without the player's answer is REFUSED and changes
//                  nothing; with the answer (the "more stars" question) it is applied, after a backup holding the newer file   [M4]
static void mineOlderRecovery(bool notNewer) {
    M->menuNew = false;
    if (notNewer) M->clock = (int64_t)__real_time(nullptr) + 5000000;
    int f = 1;
    while (f <= 6 && !spFileExists(f)) f++;
    if (f > 6) return;
    gForceSpFile = f;
    startSession(C_MINE);
    session(3);
    mp_ui_leave();
    session(2);
    M->phase = P_ENDED;
    fileSelectInit();   // undecided: the recovery file stays
    startSession(C_MINE);
    gForceSpFile = 0;
    session(3);
    playFull();
    gameAutosave("session");
    mp_ui_leave();
    session(2);
    M->phase = P_ENDED;
    MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_DONT_SAVE, 0);
    fileSelectInit();
    MpSaveGame::RecoveryInfo info;
    const bool pending = MpSaveGame::recoveryPending(&info);
    if (!notNewer) {
        if (pending && info.single) violation("mineolder: an older single-player recovery file with fewer stars was offered over a newer file");
        if (!logHas("save recovery superseded")) violation("mineolder: the older recovery file was not set aside (the script did not reach its point)");
        checkAll();
        return;
    }
    if (!pending || !info.single) {
        violation("mineolder: no single-player recovery was pending (the script did not reach its point)");
        return;
    }
    if (!(info.stars < (unsigned)info.spStars)) violation("mineolder: the script needs a recovery file with fewer stars (%u vs %d)", info.stars, info.spStars);
    // no answer from the player: refused, nothing changes
    if (MpSaveGame::recoveryKeep(0, MpSaveGame::LEAVE_SAVE_SINGLE)) violation("mineolder: an unconfirmed keep overwrote a file with more stars");
    if (!logHas("keep index=") || !logHas("refused")) violation("mineolder: the unconfirmed keep was not refused by the rule");
    checkAll();
    // the player answers Yes to "this file has more stars"
    M->permitSp = info.sourceSlot;
    memcpy(M->spAtPermit, M->sp, M->spLen);
    M->spAtPermitLen = M->spLen;
    if (!MpSaveGame::recoveryKeep(0, MpSaveGame::LEAVE_SAVE_SINGLE, true)) violation("mineolder: the confirmed keep failed");
    checkAll();
}
static void scriptMineOlderRecovery() { mineOlderRecovery(false); }
static void scriptMineOlderAsked() { mineOlderRecovery(true); }
// seed 7065 as the old model ran it (a session on a file that was never created; no menu offers that, but the save layer must hold anyway):
// session A on blank file 1 ends undecided (a blank recovery file stays); session B on file 1 plays and saves; the player discards.
// The blank recovery file must not be offered, and even a direct keep must not put a blank slot over file 1.
static void scriptMineBlankRecovery() {
    M->menuNew = true;
    fileSelectInit();
    fileSelectAction(1, 1, 0);   // delete file 1 (a blank file now)
    gAllowBlankSp = true;
    gForceSpFile = 1;
    startSession(C_MINE);
    session(3);
    mp_ui_leave();
    session(2);
    M->phase = P_ENDED;
    fileSelectInit();   // undecided: the (blank) recovery file stays
    startSession(C_MINE);
    gAllowBlankSp = false;
    gForceSpFile = 0;
    session(3);
    playFull();
    gameAutosave("session");
    mp_ui_leave();
    session(2);
    M->phase = P_ENDED;
    MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_DONT_SAVE, 0);
    fileSelectInit();
    MpSaveGame::RecoveryInfo info;
    const bool pend = MpSaveGame::recoveryPending(&info);
    if (!logHas("save recovery superseded")) violation("mineblank2: the blank recovery file was not set aside (the script did not reach its point)");
    if (pend && info.single) {
        violation("mineblank2: a blank (or older) single-player recovery file was offered (stars %u, file %d)", info.stars, info.spStars);
        M->permitSp = info.sourceSlot;
        memcpy(M->spAtPermit, M->sp, M->spLen);
        M->spAtPermitLen = M->spLen;
        if (MpSaveGame::recoveryKeep(0, MpSaveGame::LEAVE_SAVE_SINGLE, true)) violation("mineblank2: a blank recovery file was kept over a file");
    }
    checkAll();
}
// "Play on my file" saves: a power cut in the middle of the session's autosave
static void scriptTorn() {
    M->menuNew = true;
    startSession(C_MINE);
    session(5);
    playNow();
    gW.faults.killAt = gW.faults.ops + 3;   // NANDCreate / open / write: the cut lands inside the write
    gW.faults.killAfter = true;
    gW.faults.tear = true;
    gameAutosave("session");
    gW.faults.clear();
    checkAll();
}

// F1 with a player who acts although the file select holds input: the save layer's backstop must refuse the write
static void scriptF1h() {
    M->hostileFs = true;
    scriptF1();
    for (int s = 1; s <= 6; s++) fileSelectAction(s % 2, s, 0);
    checkAll();
}
// the launch check: a guest that joined outside the menu (its switch to the multiplayer root failing all along) and a host with no
// save choice are NOT taken out of the room (revised: a legitimate drop-in / autopilot guest was ejected); their saves are held
static void scriptLaunchGuard() {
    M->menuNew = false;
    gW.busyFrames = 100000;   // the save sequence stays busy: every root switch refuses
    startSession(C_GUEST);
    const int before = gW.leaveCalls;
    session(200);
    int slot;
    (void)slot;
    playNow();
    gameAutosave("session");
    gW.busyFrames = 0;
    if (gW.leaveCalls != before) violation("launch check: a guest was taken out of the room");
    mp_ui_leave();
    session(2);
    M->phase = P_ENDED;
    gameAutosave("after session");
    fileSelectInit();
    M->choice = C_NONE;
    const int before2 = gW.leaveCalls;
    startSession(C_NONE);   // a host straight into a running room, no choice (menu as built)
    session(800);
    playNow();
    gameAutosave("session");
    if (gW.leaveCalls != before2) violation("launch check: a host without a save choice was taken out of the room");
    checkAll();
}
// R1: the repair of a co-op GameData.bin that cannot be read just now (a previous version beside it) changes nothing
static void scriptR1() {
    if (!M->coopExists) return;
    std::vector<uint8_t> cur;
    readAll(gMpPath, cur);
    writeRaw(gMpPath + ".mpbak", M->base, M->baseLen);   // an older version
    gW.faults.readEio = 1;
    snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "/mp/saves/");
    bool changed = false;
    mp_save_recover(gMpPath.c_str(), &changed);
    gW.faults.clear();
    std::vector<uint8_t> after;
    readAll(gMpPath, after);
    if (changed || after != cur) violation("R1 a read error made the repair replace the co-op file with its older version");
    checkAll();
}
// R2 / R3: no GameData.bin but its previous version beside it (a replace that could not be completed): a save must not start a
// fresh file over it, a root switch must not load a fresh image
static void scriptR2() {
    if (!M->coopExists) return;
    rename(gMpPath.c_str(), (gMpPath + ".mpbak").c_str());
    writeRaw(gMpPath + ".mptmp", M->coop, M->coopLen);   // smallfix: the cut between the two renames also leaves the new file as .mptmp (the proof)
    MpSaveSnapshotView v;
    mp_save_view_slot(M->coop, 2, &v);
    static uint8_t base[0x10000], out[0x10000];
    memcpy(base, M->coop, M->coopLen);
    gW.faults.renameFail = 4;   // the repair cannot put the previous version back
    const int rc = mp_save_put_blocks(&v, gMpPath.c_str(), 2, base, sizeof base, out);
    const bool entered = MpSaveGame::enterSessionRoot();
    gW.faults.clear();
    std::vector<uint8_t> now;
    if (rc == MP_SAVE_OP_OK && readAll(gMpPath, now)) violation("R2 a co-op save started a fresh file over the previous version beside it");
    if (entered && strcmp(gW.imageRoot, gMpRoot.c_str()) == 0 && !gW.imageCopied)
        violation("R3 the switch to the multiplayer root used a fresh image although the previous version was beside it");
    bool changed = false;
    mp_save_recover(gMpPath.c_str(), &changed);   // (the next repair puts it back)
    checkAll();
}
// R4 (savelife, seed 9364): a power cut between the two renames of a co-op replace (no GameData.bin, the new file as .mptmp, the old as .mpbak),
// then card read errors on the .mptmp and the .mpbak during the repair (the switch to the multiplayer root). The repair must change NOTHING (the
// .mptmp is the proof of the interrupted replace; deleting it left a lone .mpbak, taken for a deleted save: the co-op file was gone), and a later
// repair, once the card reads again, must bring the new version back. Second case: only the .mptmp is unreadable while the older .mpbak is fine:
// the older version must not be put in its place.
static void scriptR4() {
    if (!M->coopExists) {   // (the seed gave no co-op file: make one, the model knows it)
        mp_save_mkdirs((gMpRoot + "/title/00010000/524d4750/data").c_str());
        writeRaw(gMpPath, M->base, M->baseLen);
        memcpy(M->coop, M->base, M->baseLen);
        M->coopLen = M->baseLen;
        M->coopExists = true;
    }
    for (int variant = 0; variant < 2; variant++) {
        rename(gMpPath.c_str(), (gMpPath + ".mpbak").c_str());
        writeRaw(gMpPath + ".mptmp", M->coop, M->coopLen);   // the new file (what the cut replace was writing)
        gW.faults.readEio = variant == 0 ? 2 : 1;
        snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "/mp/saves/");
        if (variant == 0) MpSaveGame::enterSessionRoot();
        else {
            bool changed = false;
            mp_save_recover(gMpPath.c_str(), &changed);
            if (changed) violation("R4 an unreadable .mptmp: the repair put the older .mpbak in its place");
        }
        gW.faults.clear();
        struct stat st;
        if (stat((gMpPath + ".mptmp").c_str(), &st) != 0) violation("R4 variant %d: the repair deleted the .mptmp (the proof of the interrupted replace) after a read error", variant);
        if (stat((gMpPath + ".mpbak").c_str(), &st) != 0) violation("R4 variant %d: the repair deleted the .mpbak after a read error", variant);
        bool changed = false;
        mp_save_recover(gMpPath.c_str(), &changed);   // the card reads again
        std::vector<uint8_t> now;
        if (!readAll(gMpPath, now) || now.size() != M->coopLen || memcmp(now.data(), M->coop, M->coopLen) != 0)
            violation("R4 variant %d: after the read errors the co-op file did not come back from the .mptmp", variant);
        unlink((gMpPath + ".mpbak").c_str());
    }
    checkAll();
}
// R5 (savelife review): a torn co-op GameData.bin, a good previous version beside it, and a read error on that previous version (.mpbak, no
// .mptmp) or on the .mptmp: the repair changes nothing and the switch to the multiplayer root must REFUSE. It used to set the torn file aside and start
// a fresh one; the first save then truncated the unreadable .mptmp or moved the good .mpbak away.
static void scriptR5() {
    for (int variant = 0; variant < 3; variant++) {   // 0: .mpbak unreadable, 1: .mptmp unreadable, 2: a stat error on the .mptmp (cannot tell whether it is there)
        mp_save_mkdirs((gMpRoot + "/title/00010000/524d4750/data").c_str());
        unlink((gMpPath + ".mptmp").c_str());
        unlink((gMpPath + ".mpbak").c_str());
        writeRaw(gMpPath, M->base, 1500);                          // torn
        writeRaw(gMpPath + ".mpbak", M->base, M->baseLen);         // the good previous version
        if (variant == 1) writeRaw(gMpPath + ".mptmp", M->base, M->baseLen);
        if (variant == 2) {
            gW.faults.statErr = 50;
            snprintf(gW.faults.statErrMatch, sizeof gW.faults.statErrMatch, ".mptmp");
        } else {
            gW.faults.readEio = 1;
            snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "%s", variant == 0 ? ".mpbak" : ".mptmp");
        }
        const bool entered = MpSaveGame::enterSessionRoot();
        gW.faults.clear();
        if (entered) violation("R5 variant %d: the switch to the co-op root went on over a torn file whose previous version could not be read", variant);
        struct stat st;
        if (stat((gMpPath + ".mpbak").c_str(), &st) != 0 || st.st_size != (off_t)M->baseLen) violation("R5 variant %d: the good .mpbak was moved or changed", variant);
        if (variant == 1 && (stat((gMpPath + ".mptmp").c_str(), &st) != 0 || st.st_size != (off_t)M->baseLen)) violation("R5 variant %d: the .mptmp was changed", variant);
        if (fileExists(gMpPath + ".mpbad")) violation("R5 variant %d: the torn file was set aside and a fresh start began", variant);
        if (!fileExists(gMpPath) || !(stat(gMpPath.c_str(), &st) == 0 && st.st_size == 1500)) violation("R5 variant %d: the torn file itself was touched", variant);
        MpSaveGame::abandonHostChoice();
    }
    checkAll();
}
// R6 (savelife review): the single-player root after a cut between the two renames (no GameData.bin, the new file as .mptmp, the old as .mpbak) and a
// read error at the BOOT repair. The game must not be told "no file" and create a new save: the read-open retries the repair, and a create / write-open
// is refused while a non-empty .mptmp is beside the missing file and cannot be repaired.
static void bootR6() {
    rename(gSpPath.c_str(), (gSpPath + ".mpbak").c_str());
    writeRaw(gSpPath + ".mpbak", M->base, M->baseLen);         // the older version
    writeRaw(gSpPath + ".mptmp", M->sp, M->spLen);             // the newest save
    gW.init();
    gW.faults.readEio = 1;
    snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "/saves/title");
    gameBoot();
    gW.faults.clear();
    M->booted = true;
    M->phase = P_TITLE;
    fileSelectInit();
}
static void scriptR6() {
    std::vector<uint8_t> now;
    if (!readAll(gSpPath, now) || now.size() != M->spLen || memcmp(now.data(), M->sp, M->spLen) != 0)
        violation("R6 boot repair read error: the single-player file was not restored from the .mptmp (the game started without it)");
    // second state: missing again, unreadable .mptmp at the write side
    unlink(gSpPath.c_str());
    unlink((gSpPath + ".mpbak").c_str());
    writeRaw(gSpPath + ".mptmp", M->sp, M->spLen);
    gW.faults.readEio = 1;
    snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "/saves/title");
    const s32 rc = NANDCreate("GameData.bin", 0, 0);
    gW.faults.clear();
    struct stat st;
    if (rc == NAND_RESULT_OK) violation("R6 NANDCreate made a new GameData.bin beside an unrepaired .mptmp");
    if (stat((gSpPath + ".mptmp").c_str(), &st) != 0 || st.st_size != (off_t)M->spLen) violation("R6 the .mptmp (the newest save) was changed");
    gW.faults.readEio = 1;
    snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, "/saves/title");
    NANDFileInfo info;
    const s32 orc = NANDOpen("GameData.bin", &info, NAND_ACCESS_WRITE);
    if (orc == NAND_RESULT_OK) NANDClose(&info);
    gW.faults.clear();
    if (orc == NAND_RESULT_OK) violation("R6 a write-open of GameData.bin was accepted beside an unrepaired .mptmp");
    if (orc == NAND_RESULT_OK && stat((gSpPath + ".mptmp").c_str(), &st) == 0 && st.st_size != (off_t)M->spLen) violation("R6 a write-open truncated the .mptmp (the newest save)");
    if (!fileExists(gSpPath)) {
        bool changed = false;
        mp_save_recover(gSpPath.c_str(), &changed);
    }
    // third state: the .mptmp cannot be looked at (a stat error that is not ENOENT): create, write-open and read-open all refuse, nothing changes
    unlink(gSpPath.c_str());
    unlink((gSpPath + ".mpbak").c_str());
    writeRaw(gSpPath + ".mptmp", M->sp, M->spLen);   // (the state again: the step above may have restored it)
    for (int k = 0; k < 3; k++) {
        gW.faults.statErr = 50;
        snprintf(gW.faults.statErrMatch, sizeof gW.faults.statErrMatch, ".mptmp");
        s32 r = -1;
        NANDFileInfo fi;
        if (k == 0) r = NANDCreate("GameData.bin", 0, 0);
        else if (k == 1) { r = NANDOpen("GameData.bin", &fi, NAND_ACCESS_WRITE); if (r == NAND_RESULT_OK) NANDClose(&fi); }
        else { r = NANDOpen("GameData.bin", &fi, NAND_ACCESS_READ); if (r == NAND_RESULT_OK) NANDClose(&fi); }
        gW.faults.clear();
        if (r == NAND_RESULT_OK) violation("R6 stat error on the .mptmp: call %d was accepted (create / write-open / read-open must refuse)", k);
        if (stat((gSpPath + ".mptmp").c_str(), &st) != 0 || st.st_size != (off_t)M->spLen) violation("R6 stat error on the .mptmp: it was changed (call %d)", k);
        if (fileExists(gSpPath)) violation("R6 stat error on the .mptmp: a GameData.bin appeared (call %d)", k);
    }
    now.clear();
    if (!fileExists(gSpPath)) {
        bool changed = false;
        mp_save_recover(gSpPath.c_str(), &changed);
    }
    if (!readAll(gSpPath, now) || now.size() != M->spLen || memcmp(now.data(), M->sp, M->spLen) != 0) violation("R6 the newest single-player save is gone");
    checkAll();
}
// R7 (savelife review): the read error at the boot repair does not go away (persistent). The game must be told a read error (never "no file"), nothing may
// be created, and every file stays as it was; GameData.bin.mptmp.failed marks the refused boot. Nothing is set aside on a first boot.
static void bootR7() {
    rename(gSpPath.c_str(), (gSpPath + ".mpbak").c_str());
    writeRaw(gSpPath + ".mpbak", M->base, M->baseLen);
    writeRaw(gSpPath + ".mptmp", M->sp, M->spLen);
    gW.init();
    gW.faults.readEio = 1000;
    snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, ".mptmp");
    gameBoot();   // (the faults stay armed: the script below runs with the error still there)
    M->booted = true;
    M->phase = P_TITLE;
    fileSelectInit();
}
static void scriptR7() {
    struct stat st;
    NANDFileInfo fi;
    const s32 r = NANDOpen("GameData.bin", &fi, NAND_ACCESS_READ);
    if (r == NAND_RESULT_OK) { NANDClose(&fi); violation("R7 a read-open succeeded or answered 'no file' while the .mptmp stays unreadable"); }
    else if (r == -12) violation("R7 the read-open answered 'no file' (the game would create a new save over the newest one)");
    if (NANDCreate("GameData.bin", 0, 0) == NAND_RESULT_OK) violation("R7 NANDCreate accepted");
    if (fileExists(gSpPath)) violation("R7 a GameData.bin appeared");
    if (stat((gSpPath + ".mptmp").c_str(), &st) != 0 || st.st_size != (off_t)M->spLen) violation("R7 the .mptmp changed");
    if (stat((gSpPath + ".mpbak").c_str(), &st) != 0 || st.st_size != (off_t)M->baseLen) violation("R7 the .mpbak changed");
    if (fileExists(gSpPath + ".mptmp.unread")) violation("R7 the .mptmp was set aside on the first boot");
    if (!fileExists(gSpPath + ".mptmp.failed")) violation("R7 no .mptmp.failed was left by the refused boot");
    gW.faults.clear();
    bool changed = false;
    mp_save_recover(gSpPath.c_str(), &changed);   // (the card reads again)
    std::vector<uint8_t> now;
    if (!readAll(gSpPath, now) || now.size() != M->spLen || memcmp(now.data(), M->sp, M->spLen) != 0) violation("R7 the newest save did not come back once the card read again");
    checkAll();
}
// R8 / R9 (savelife review, the way out): R8: no .mptmp, a .mptmp.failed of the earlier boot and a valid .mpbak (a power cut after the .mptmp was renamed
// away): the boot restores the .mpbak and removes the mark. R9: boot 2 as such: .failed there, the .mptmp unreadable (two read errors: the first repair
// and the retry), a valid .mpbak: the .mptmp is kept aside (.unread), the .mpbak restored, nothing deleted.
static int gR89 = 0;
static void bootR89() {
    rename(gSpPath.c_str(), (gSpPath + ".mpbak").c_str());          // the newest save (M->sp) as the .mpbak
    writeRaw(gSpPath + ".mptmp.failed", M->base, 0);
    if (gR89 == 9) writeRaw(gSpPath + ".mptmp", M->base, M->baseLen);   // (unreadable below)
    gW.init();
    if (gR89 == 9) {
        gW.faults.readEio = 2;
        snprintf(gW.faults.readEioMatch, sizeof gW.faults.readEioMatch, ".mptmp");
    }
    gameBoot();
    gW.faults.clear();
    M->booted = true;
    M->phase = P_TITLE;
    fileSelectInit();
}
static void bootR8() { gR89 = 8; bootR89(); }
static void bootR9() { gR89 = 9; bootR89(); }
static void scriptR89() {
    std::vector<uint8_t> now;
    if (!readAll(gSpPath, now) || now.size() != M->spLen || memcmp(now.data(), M->sp, M->spLen) != 0) violation("R%d: the .mpbak was not restored at the boot", gR89);
    if (fileExists(gSpPath + ".mptmp.failed") || fileExists(gSpPath + ".mptmp")) violation("R%d: the .mptmp.failed / marker is still there", gR89);
    if (gR89 == 9 && !fileExists(gSpPath + ".mptmp.unread")) violation("R9: the unreadable .mptmp was not kept aside");
    checkAll();
}
// C2: mp.cfg missing, mp.cfg.bak holds the player's settings (a cut between the two renames); a save is cut after its first steps
static void scriptC2() {
    rename((gCfgDir + "/mp.cfg").c_str(), (gCfgDir + "/mp.cfg.bak").c_str());
    mp_config_load();
    gW.faults.killAt = gW.faults.ops + 3;   // fopen(tmp) / fclose / the next step (remove(bak) as built)
    gW.faults.killAfter = true;
    mp_config_save();
    gW.faults.clear();
    checkAll();
}
// RS: restore a backup: the file it replaces is backed up first
static void scriptRS() {
    MpSaveGame::backupSinglePlayerFile();
    const int s = 1 + rn(6);
    M->spPlaySlot = s;
    gW.seq.restoreUserFile(gW.user, s, true);
    M->phase = P_SPPLAY;
    playProgress(T_SP, s);
    gameAutosave("single-player");
    checkAll();
    fileSelectInit();
    MpSaveGame::BackupList list;
    if (!MpSaveGame::listBackups(&list) || list.count == 0) {
        violation("RS no backup listed");
        return;
    }
    const std::string path = gBackups + "/" + list.name[0];
    std::vector<uint8_t> want, cur;
    readAll(path, want);
    M->restoreSlotMask = 0x7E;
    const bool ok = MpSaveGame::restoreBackup(path.c_str());
    readAll(gSpPath, cur);
    if (!ok || cur != want) violation("RS restore did not put the backup back");
    if (!backupHolds(M->sp, M->spLen)) violation("RS the file the restore replaced has no backup");
    checkSp();
    M->restoreSlotMask = 0;
}
// W1: a write that reaches the card damaged (reported fine): the atomic writer must refuse before the old file is touched
static void scriptW1() {
    std::vector<uint8_t> before;
    readAll(gSpPath, before);
    static uint8_t img[0x10000];
    memcpy(img, M->sp, M->spLen);
    img[0x200] ^= 1;
    mp_save_finalize(img, sizeof img);
    gW.faults.writeCorrupt = 1;
    const bool ok = mp_save_write_atomic((gDir + "/w1.bin").c_str(), img, M->spLen);
    gW.faults.clear();
    std::vector<uint8_t> got;
    readAll(gDir + "/w1.bin", got);
    if (ok) violation("W1 the atomic writer reported success for a damaged write");
    if (!got.empty()) violation("W1 a damaged temporary replaced the file");
    // W2: a rename that reports success and does nothing: a co-op save must not report success
    if (M->coopExists) {
        MpSaveSnapshotView v;
        mp_save_view_slot(M->sp, 2, &v);
        static uint8_t out[0x10000];
        gW.faults.renameLie = 2;
        const int rc = mp_save_put_blocks(&v, gMpPath.c_str(), 3, nullptr, 0, out);
        gW.faults.clear();
        std::vector<uint8_t> now;
        readAll(gMpPath, now);
        if (rc == MP_SAVE_OP_OK && (now.size() != M->coopLen || memcmp(now.data(), out, now.size()) != 0))
            violation("W2 a co-op write reported success but the file is not what was written");
    }
}

// K: files of a newer build (keepsafe S3/S4/K4): a co-op GameData.bin with a newer version and an mp.cfg with version 2 are
// never repaired, replaced, kept aside or written
static void scriptNewer() {
    if (!M->coopExists) return;
    static uint8_t img[0x10000];
    memcpy(img, M->coop, M->coopLen);
    img[4] = 3;   // version 3
    mp_save_finalize(img, sizeof img);
    writeRaw(gMpPath, img, M->coopLen);
    writeRaw(gMpPath + ".mpbak", M->coop, M->coopLen);   // an older version beside it
    const char* cfg = "version = 2\nname = Future\nlook = 1\nnew_thing = 7\n";
    writeRaw(gCfgDir + "/mp.cfg", (const uint8_t*)cfg, strlen(cfg));
    mp_config_load();
    MpConfig c = *mp_config();
    snprintf(c.name, sizeof c.name, "Changed");
    mp_config_set(&c);
    const bool saved = mp_config_save();
    uint8_t key[16];
    mp_config_console_key(key);
    const bool entered = MpSaveGame::enterSessionRoot();
    MpSaveSnapshotView v;
    mp_save_view_slot(M->coop, 1, &v);
    static uint8_t out[0x10000];
    const int rc = mp_save_put_blocks(&v, gMpPath.c_str(), 1, M->coop, M->coopLen, out);
    std::vector<uint8_t> now, cfgNow;
    readAll(gMpPath, now);
    readAll(gCfgDir + "/mp.cfg", cfgNow);
    if (entered) violation("K the multiplayer root was entered with a newer build's co-op file (repaired or replaced)");
    if (rc == MP_SAVE_OP_OK || rc == MP_SAVE_OP_DST_ASIDE) violation("K a co-op save replaced / set aside a newer build's file (rc %d)", rc);
    if (now.size() != M->coopLen || memcmp(now.data(), img, M->coopLen) != 0) violation("K the newer co-op file changed");
    if (saved || cfgNow.size() != strlen(cfg) || memcmp(cfgNow.data(), cfg, strlen(cfg)) != 0) violation("K a newer mp.cfg was written");
    // (back to this console's own files for the checks)
    writeRaw(gMpPath, M->coop, M->coopLen);
    unlink((gMpPath + ".mpbak").c_str());
    M->ncfg = 0;
}

struct Script { const char* name; void (*fn)(); void (*follow)(); void (*boot)(); };
static const Script kScripts[] = {
    {"F1 stale file select after a delayed root switch", scriptF1, nullptr},
    {"F2a host without a save choice, session ends before tick", scriptF2a, nullptr},
    {"F2b guest on the co-op root, session ends before tick", scriptF2b, nullptr},
    {"F3 Play on my file with the clock behind the backups", scriptF3, nullptr},
    {"F5 Play on my file, migration, silent rejoin, save to my file", scriptF5, nullptr},
    {"F6 co-op read error at the host's switch", scriptF6, nullptr},
    {"F7 failed switch and failed restore, then a save", scriptF7, nullptr},
    {"F8 mp.cfg unreadable at boot", scriptF8, nullptr},
    {"Torn Play on my file autosave (power cut)", scriptTorn, nullptr},
    {"F1h stale file select, player presses through the hold", scriptF1h, nullptr},
    {"LG unsafe consoles stay in the room, their saves held", scriptLaunchGuard, nullptr},
    {"R1 repair with a read error keeps the co-op file", scriptR1, nullptr},
    {"R2/R3 no file but its previous version beside it", scriptR2, nullptr},
    {"C3 mp.cfg cut after its last rename, next save (seed 4436)", scriptCfgCut, followCfgCut},
    {"M1 no session on a never-created file (seed 7065)", scriptMineBlank, nullptr},
    {"M2 older recovery over a newer file is not offered (savebug)", scriptMineOlderRecovery, nullptr},
    {"M4 older recovery over a file with more stars needs the player's answer (savebug)", scriptMineOlderAsked, nullptr},
    {"M3 blank recovery file of a never-created file (seed 7065, savebug)", scriptMineBlankRecovery, nullptr},
    {"C2 mp.cfg only in .bak, save cut after its first steps", scriptC2, nullptr},
    {"RS restore a backup", scriptRS, nullptr},
    {"W1/W2 damaged write, lying rename", scriptW1, nullptr},
    {"K files of a newer build stay untouched", scriptNewer, nullptr},
    {"R4 cut between the renames, then read errors at the repair (seed 9364)", scriptR4, nullptr, nullptr},
    {"R5 torn co-op file, read error on its previous version: refuse, no fresh start", scriptR5, nullptr, nullptr},
    {"R6 boot repair read error on the single-player root", scriptR6, nullptr, bootR6},
    {"R7 persistent read error at the boot repair: EIO, nothing changed", scriptR7, nullptr, bootR7},
    {"R8 .failed, no .mptmp, valid .mpbak: restored at the boot", scriptR89, nullptr, bootR8},
    {"R9 boot 2: unreadable .mptmp set aside, .mpbak restored", scriptR89, nullptr, bootR9},
};

// savesus S2: a guest in a session receives the SAME valid snapshot N times (a flaky or hostile host: snapshotOffer on every host save,
// SNAPSHOT_DONE ok=false resends): how many times is the guest's live user file rewritten, and how many disk writes does it make?
static long gS2Applied;
static void scriptS2() {
    M->menuNew = true;
    startSession(C_GUEST);
    session(5);
    const long img0 = M->stat[S_IMAGE_WRITES], sp0 = M->stat[S_SP_WRITES], co0 = M->stat[S_COOP_WRITES];
    const int r0 = gW.restores;
    foreignSnapshot();                     // one valid image ...
    uint8_t keep[sizeof gW.net.snap];
    memcpy(keep, gW.net.snap, sizeof keep);
    const uint32_t keepLen = gW.net.snapLen;
    for (int i = 0; i < 10; i++) {         // ... delivered 10 times (the same bytes, the same crc)
        gW.net.snapLen = keepLen;
        memcpy(gW.net.snap, keep, sizeof keep);
        for (int k = 0; k < 3; k++)
            if (MpSaveGame::applySnapshotIfReady()) gS2Applied++;
    }
    M->stat[63] = gW.restores - r0;                                   // (shared with the parent: the script runs in a forked child)
    M->stat[62] = M->stat[S_IMAGE_WRITES] - img0 + M->stat[S_SP_WRITES] - sp0 + M->stat[S_COOP_WRITES] - co0;
    M->stat[61] = gS2Applied;
}

// fix2 C: apply, reload the image from the card (keepsafe reload), re-offer the SAME snapshot (same crc): it must be applied again
// (before: the identical-snapshot skip still remembered it and the reloaded image never got the session's progress back)
extern "C" void mp_savegame_keepsafe_reload(void);
static long gC2First, gC2Again;
static void scriptS2Reload() {
    M->menuNew = true;
    startSession(C_GUEST);
    session(5);
    foreignSnapshot();
    uint8_t keep[sizeof gW.net.snap];
    memcpy(keep, gW.net.snap, sizeof keep);
    const uint32_t keepLen = gW.net.snapLen;
    for (int k = 0; k < 3; k++)
        if (MpSaveGame::applySnapshotIfReady()) gC2First++;
    mp_savegame_keepsafe_reload();                     // the image is reloaded from the card
    gW.net.snapLen = keepLen;                          // the host offers the same bytes again
    memcpy(gW.net.snap, keep, sizeof keep);
    for (int k = 0; k < 3; k++)
        if (MpSaveGame::applySnapshotIfReady()) gC2Again++;
    M->stat[61] = gC2First;
    M->stat[62] = gC2Again;
}

int main(int argc, char** argv) {
    setenv("SMG_ATOMIC_SAVES", spMode() == 1 ? "1" : "0", 1);
    const char* savePath = "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    int random = 0, steps = 60;
    uint64_t seed = 1;
    bool scripted = false, verbose = false;
    long one = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--random") && i + 1 < argc) random = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--one") && i + 1 < argc) one = atol(argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--scripted")) scripted = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
    }
    unsetenv("SMG_MP_NAME");
    unsetenv("SMG_MP_LOOK");
    unsetenv("SMG_MP_DOWNED");
    M = (Model*)mmap(nullptr, sizeof(Model), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (M == MAP_FAILED) return 2;
    memset(M, 0, sizeof *M);
    sClock = &M->clock;
    M->verbose = verbose;
    if (!loadTemplates(savePath)) {
        fprintf(stderr, "savelife: no usable test save at %s\n", savePath);
        return 2;
    }
    char tmpl[] = "/tmp/savelife.XXXXXX";
    const char* base = mkdtemp(tmpl);
    if (!base) return 2;
    gDir = std::string(base) + "/c";
    gSpRoot = gDir + "/saves";
    gMpRoot = gDir + "/mp/saves";
    gBackups = gDir + "/mp/backups";
    gCfgDir = gDir + "/mp";
    gSpPath = gSpRoot + "/title/00010000/524d4750/data/GameData.bin";
    gMpPath = gMpRoot + "/title/00010000/524d4750/data/GameData.bin";
    memcpy(gW.blankGame, M->blankGame, sizeof gW.blankGame);
    memcpy(gW.blankConfig, M->blankConfig, sizeof gW.blankConfig);
    memcpy(gW.sysconf, M->sysconf, sizeof gW.sysconf);
    int failures = 0, runs = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--s2")) continue;
        // (the script runs in a forked child: its counters come back through the shared model)
        runSequence(seed + 7777, 1, scriptS2);
        printf("S2: the same valid snapshot delivered 10 times to a guest mid-session: applied %ld times, restoreUserFile (the live user file) %ld times, "
               "disk writes %ld\n", M->stat[61], M->stat[63], M->stat[62]);
        const bool once = M->stat[63] <= 1;
        if (!once) fprintf(stderr, "FAIL S2: an identical snapshot rewrites the live user file every time\n");
        printf("savelife --s2: %s\n", once ? "ok" : "FAIL");
        rmrf(base);
        return once ? 0 : 1;   // glitchfix: S2 fixed in applySnapshotIfReady (formerly an XFAIL)
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--s2reload")) continue;
        runSequence(seed + 8888, 1, scriptS2Reload);
        printf("C: snapshot applied %ld times before the reload, %ld times after it (same crc offered again)\n", M->stat[61], M->stat[62]);
        const bool ok = M->stat[61] == 1 && M->stat[62] == 1;
        if (!ok) fprintf(stderr, "FAIL C: after a reload the identical snapshot must be applied again\n");
        printf("savelife --s2reload: %s\n", ok ? "ok" : "FAIL");
        rmrf(base);
        return ok ? 0 : 1;
    }
    if (scripted) {
        int i = 0;
        for (const Script& s : kScripts) {
            gScriptFollow = s.follow;
            gScriptBoot = s.boot;
            const bool ok = runSequence(seed + 1000 + i++, 1, s.fn);
            gScriptFollow = nullptr;
            gScriptBoot = nullptr;
            printf("%-64s %s\n", s.name, ok ? "ok" : "VIOLATED");
            if (!ok) printf("%s", M->why);
            failures += !ok;
            runs++;
        }
    }
    if (one >= 0) {
        M->verbose = true;
        const bool ok = runSequence((uint64_t)one, steps, nullptr);
        printf("sequence %ld: %s\n%s", one, ok ? "ok" : "VIOLATED", M->why);
        failures += !ok;
        runs++;
    }
    for (int i = 0; i < random; i++) {
        const uint64_t s = seed + (uint64_t)i;
        const bool ok = runSequence(s, steps, nullptr);
        runs++;
        if (!ok) {
            failures++;
            if (failures <= 5) printf("random sequence seed=%llu VIOLATED (rerun: --one %llu --steps %d)\n%s", (unsigned long long)s,
                                      (unsigned long long)s, steps, M->why);
        }
        if ((i + 1) % 2000 == 0) {
            printf("  %d sequences, %d failed\n", i + 1, failures);
            fflush(stdout);
        }
    }
    printf("stats:");
    for (int i = 0; i < S_COUNT; i++) printf(" %s=%ld", kStatNames[i], M->stat[i]);
    printf("\nsavelife: %d runs, %d failed\n", runs, failures);
    rmrf(base);
    return failures ? 1 : 0;
}
