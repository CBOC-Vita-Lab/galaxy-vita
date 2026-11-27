// Galaxy 3.0 multiplayer: host tests of platform/src/mp/mp_keepsafe.cpp (keepfix 3 design: copies, and bringing back one item at a time).
// Build and run: tests/mp/keepsafe_build.sh (also runs keepsafe_cut_test, the power-cut model). KS_ONLY=<name part>; KS_GAMEDATA=<a real
// GameData.bin, only ever read>. Each world is a throw-away folder; after every test the single-player save folder of every world is
// checked byte for byte, inode and mtime (nothing here may write it). The real settings code (mp_config.cpp, mp_ui_flow.cpp) is linked.
#include "keepsafe_world.h"
#include <atomic>
#include <unistd.h>

#include "../../platform/src/mp/mp_config.h"
#include "port/mp_ui_flow.h"

// ---- the game's side, faked where the test needs to see it ----------------------------------------------------------------------
static std::string gLog;
extern "C" void mp_log(const char* fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    gLog += b;
    gLog += "\n";
    if (getenv("KS_LOG")) fprintf(stderr, "log: %s\n", b);
}
extern "C" bool mp_core_resume(void) { return false; }
struct MpProfile;
extern "C" void mp_ui_profile(MpProfile*) {}
static int gReloads = 0, gServiceStops = 0;
// The settings lock, watched (the linker wraps mp_config_hold, see keepsafe_build.sh): whether it is held, when mp.cfg is read, when the hooks run.
extern "C" void __real__Z14mp_config_holdb(bool);
static bool gHeld = false;
static int gMpCfgReads = 0, gMpCfgReadsUnlocked = 0, gHooksUnlocked = 0;
extern "C" void __wrap__Z14mp_config_holdb(bool h) {
    gHeld = h;
    __real__Z14mp_config_holdb(h);
}
static void readSpy(const char* path) {
    if (!strcmp(path, (std::string(gP.mpDir) + "/mp.cfg").c_str())) {   // (this console's live file, not a copy's)
        gMpCfgReads++;
        if (!gHeld) gMpCfgReadsUnlocked++;
    }
}
static bool gMayReplace = true;
static std::string gTeamAtReload;
extern "C" bool mp_savegame_keepsafe_may_replace(void) { return gMayReplace; }
extern "C" void mp_savegame_keepsafe_reload(void) {
    gReloads++;
    if (!gHeld) gHooksUnlocked++;
    gTeamAtReload = S_(readFile(gP.teamFile));
}
extern "C" void mp_service_shutdown(void) { gServiceStops++; }
extern "C" void mp_port_keepsafe_wire(void);   // platform/src/mp/mp_boot.cpp (linked)
extern "C" void mp_core_boot(void) {}
extern "C" int64_t port_host_time_ns(void) { return 0; }   // merge1: mp_boot.cpp times the boot data read (memfix)
extern "C" void mp_core_shutdown(void) {}
extern "C" bool mp_core_goodbye(unsigned) { return true; }
extern "C" void mp_core_set_data_hash(uint32_t) {}

static void world(const char* name, const World& w = World()) {
    newWorld(name);
    fillWorld(w);
    mp_config_set_dir(gP.mpDir);
    mp_keepsafe_test_reset();
    mp_port_keepsafe_wire();
    gMayReplace = true;
}
static void boot(const char* version, const char* build = "t") {
    mp_keepsafe_test_reset();
    mp_port_keepsafe_wire();
    mp_keepsafe_boot(version, build);
}
static std::vector<MpKsCopy> copies() {
    MpKsCopy c[32];
    const int n = mp_keepsafe_list_copies(c, 32);
    return std::vector<MpKsCopy>(c, c + n);
}
static std::string backups() { return std::string(gP.dataDir) + "/mp-backups"; }
static std::string stamp() { return std::string(gP.dataDir) + "/mp-keepsafe.stamp"; }
static const std::string kTeamRel = "mp/saves/title/00010000/524d4750/data/GameData.bin";
static std::string versionCopy() {   // the newest version copy's name
    for (const MpKsCopy& c : copies())
        if (c.kind == MP_KS_COPY_VERSION) return c.name;
    return "";
}

// ================================================================================================================================
// start-up and version copies
// ================================================================================================================================
static void testBoot() {
    world("boot");
    const Tree d0 = dataSet();
    boot("3.0.0");
    std::vector<MpKsCopy> c = copies();
    CHECK(c.size() == 1 && c[0].kind == MP_KS_COPY_VERSION && c[0].teamFiles && c[0].account && c[0].settings && !strcmp(c[0].version, "3.0.0"));
    const Tree snap = treeOf(backups() + "/" + c[0].name);
    for (auto& kv : d0) {   // everything multiplayer, never the single-player safety copies in mp/backups, never the recovery checkpoints
        if (kv.first.compare(0, 11, "mp/backups/") == 0 || kv.first.compare(0, 12, "mp/recovery/") == 0) CHECK(!snap.count(kv.first));
        else CHECKM(snap.count(kv.first) && snap.at(kv.first) == kv.second, "copy lacks %s", kv.first.c_str());
    }
    CHECK(sameTree(d0, dataSet()));
    // the normal start: one small read, at most two stats, nothing written
    const Tree all0 = treeOf(gP.dataDir);
    mp_keepsafe_test_reset();
    mp_keepsafe_boot("3.0.0", "t");
    CHECKM(mp_keepsafe_test_read_count() == 1 && mp_keepsafe_test_stat_count() <= 2, "reads %d stats %d", mp_keepsafe_test_read_count(), mp_keepsafe_test_stat_count());
    CHECK(treeOf(gP.dataDir) == all0 && mp_keepsafe_may_migrate() && mp_keepsafe_notice_take() == MP_KS_NOTICE_NONE);
    // a new build under the same version is a change too
    boot("3.0.0", "t2");
    CHECK(copies().size() == 2);
    // no data at all: no copy, a stamp
    world("boot-empty", World{.team = false, .recovery = false, .account = false, .cfg = false, .flow = false, .coop = false, .conntest = false, .backups = false});
    boot("3.0.0");
    CHECK(copies().empty() && exists(stamp()));
}

static void testPruneAndPins() {
    world("pins");
    boot("3.0.0");   // 1 none>3.0.0
    int stars = 10;
    const char* seq[] = {"3.1.0", "3.0.0", "3.1.0", "3.0.0", "3.1.0", "3.0.0"};   // 2 and 3 are first of their pairs; 4..7 repeats
    for (const char* v : seq) {
        boot(v);
        saveAtomic(gP.teamFile, teamSlot(2, stars++));   // each version plays a little
    }
    std::set<std::string> gotSet;
    for (const MpKsCopy& c : copies()) gotSet.insert(std::string(c.name).substr(15));
    std::string got;
    for (const std::string& g : gotSet) got += g + " ";
    CHECKM(got == "001 002 003 005 006 007 ", "%s", got.c_str());   // the three first-of-pair copies + the newest 3
    // a copy that does not parse is never removed, and does not count; a marked unfinished one goes once a newer verified one exists
    const std::string bad = backups() + "/3.0.9-20231101-000";
    writeFile(bad + "/MANIFEST.ksm", B("not a manifest"));
    writeFile(bad + "/mp/mp.cfg", B("version = 1\n"));
    writeFile(backups() + "/3.0.8-20231101-002/.unfinished", Bytes());
    writeFile(backups() + "/3.0.8-20231101-002/mp/mp.cfg", B("x"));
    writeFile(backups() + "/3.0.7-20231101-001/mp/mp.cfg", B("no manifest, no mark"));
    for (int i = 0; i < 3; i++) boot(i % 2 ? "3.0.0" : "3.1.0");
    CHECK(exists(bad + "/mp/mp.cfg") && exists(backups() + "/3.0.7-20231101-001/mp/mp.cfg") && !exists(backups() + "/3.0.8-20231101-002"));
    // pinned copies are bounded: 12 distinct updates of unchanged data keep at most 8 pinned + the newest 3
    world("pins-bound");
    for (int v = 0; v < 12; v++) boot(("3." + std::to_string(v) + ".0").c_str());
    const size_t n = copies().size();
    CHECKM(n == 8, "%zu copies after 12 updates", n);   // (12 pins of the same data: the oldest 4 go; the newest 3 are among the 8)
    // a pinned copy whose data no newer copy holds is never removed, however many updates follow
    world("pins-keep");
    boot("3.0.0");
    const std::string first = versionCopy();
    saveAtomic(gP.teamFile, teamSlot(2, 33));   // the data changes after the first copy
    for (int v = 1; v < 14; v++) boot(("3." + std::to_string(v) + ".0").c_str());
    CHECK(isDir(backups() + "/" + first));
}

static void testCopyFailures() {
    // card full: nothing written, the player told at every such start, tried again at the next start
    world("full");
    const Tree d0 = treeOf(gP.dataDir);
    mp_keepsafe_test_set_free_bytes(1000);
    boot("3.0.0");
    CHECK(!mp_keepsafe_may_migrate() && mp_keepsafe_notice_take() == MP_KS_NOTICE_NO_SPACE && copies().empty());   // (keepfix 5: "full" only for a real out-of-space result)
    boot("3.0.0");
    CHECK(mp_keepsafe_notice_take() == MP_KS_NOTICE_NO_SPACE && copies().empty() && !exists(stamp()));   // (no stamp: nothing is decided)
    mp_keepsafe_test_set_free_bytes(-1);
    boot("3.0.0");
    CHECK(copies().size() == 1 && mp_keepsafe_may_migrate());
    // an unreadable stamp (a read error, not "missing"): nothing is decided, nothing written, at every such start
    world("stamp-unreadable");
    mkdirs(stamp());
    const Tree t0 = treeOf(gP.dataDir);
    for (const char* v : {"3.0.0", "3.0.1", "3.0.0"}) boot(v);
    CHECK(treeOf(gP.dataDir) == t0 && !mp_keepsafe_may_migrate() && isDir(stamp()));
    // a name that already exists is never used and never removed (the listing failed, the counter is behind): review 3 #7
    world("clash");
    boot("3.0.0");
    const std::string mine = versionCopy();
    const Bytes m0 = readFile(backups() + "/" + mine + "/MANIFEST.ksm");
    // the next number's folder already exists (another copy: not one this call made)
    writeFile(backups() + "/3.0.1-20231114-002/keep.txt", B("someone's"));
    writeFile(backups() + "/3.0.1-20231114-002/MANIFEST.ksm", B("not ours"));
    boot("3.0.1");
    CHECK(readFile(backups() + "/3.0.1-20231114-002/keep.txt") == B("someone's") && readFile(backups() + "/" + mine + "/MANIFEST.ksm") == m0);
    CHECK(copies().size() == 2);
    (void)d0;
}

static void testNewerStamp() {
    world("newer");
    boot("3.0.0");
    writeFile(std::string(gP.mpDir) + "/mp.cfg", B("version = 2\nname = Future\n"));
    writeFile(std::string(gP.mpDir) + "/flow.cfg", B("version = 2\nreach = 9\n"));
    writeFile(stamp(), B("keepsafe = 2\nversion = 3.1.0\n"));
    const Tree newer = dataSet();
    boot("3.0.0");
    CHECK(!mp_keepsafe_may_migrate() && readFile(stamp()) == B("keepsafe = 2\nversion = 3.1.0\n"));
    bool copied = false;
    for (const MpKsCopy& c : copies()) {
        Tree t = treeOf(backups() + "/" + c.name);
        bool all = true;
        for (auto& kv : newer) all = all && (kv.first.compare(0, 11, "mp/backups/") == 0 || kv.first.compare(0, 12, "mp/recovery/") == 0 || (t.count(kv.first) && t.at(kv.first) == kv.second));
        copied = copied || all;
    }
    CHECK(copied);
    const size_t n = copies().size();
    boot("3.0.0");
    CHECK(copies().size() == n);   // the same data: no second copy
    writeFile(std::string(gP.onlineDir) + "/online.dat", makeAccount(78, 'b'));   // (an account of the same size: different data)
    boot("3.0.0");
    CHECK(copies().size() == n + 1);
}

// ================================================================================================================================
// make a copy, inspect, damaged copies
// ================================================================================================================================
static void testMakeCopy() {
    world("make");
    boot("3.0.0");
    const Tree d0 = treeOf(gP.dataDir);
    char name[96];
    CHECK(mp_keepsafe_make_copy(0, name, sizeof name) == MP_KS_OK && !strncmp(name, "copy-20231114-", 14));
    const Bytes f = readFile(backups() + "/" + name);
    CHECK(S_(f).find("mp/saves/GameData.bin") != std::string::npos && S_(f).find("online/online.dat") != std::string::npos);
    CHECK(S_(f).find("mp/mp.cfg") == std::string::npos && S_(f).find("mp/recovery") == std::string::npos);   // never settings or checkpoints
    char share[96];
    CHECK(mp_keepsafe_make_copy(MP_KS_COPY_NO_ACCOUNT, share, sizeof share) == MP_KS_OK && strcmp(share, name) != 0);
    CHECK(S_(readFile(backups() + "/" + share)).find("online/") == std::string::npos);
    Tree after = treeOf(gP.dataDir);   // only the two new files
    after.erase("mp-backups/" + std::string(name));
    after.erase("mp-backups/" + std::string(share));
    CHECK(after == d0);
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(name, &fi) == MP_KS_OK && fi.kind == MP_KS_COPY_MADE && fi.slotSame == 0x3F - fi.slotBlank && !fi.account && !fi.settings);
    // every damage and every truncation of a copy file is refused, and bringing back from it changes nothing
    const Tree live = dataSet();
    int accepted = 0;
    for (size_t i = 0; i < f.size(); i += (i < 200 || i + 200 > f.size()) ? 1 : 97) {
        Bytes g = f;
        g[i] ^= 0x41;
        writeFile(backups() + "/copy-20231114-900.smgmp", g);
        if (mp_keepsafe_inspect("copy-20231114-900.smgmp", &fi) == MP_KS_OK) accepted++;
        mp_keepsafe_bring_back_slot("copy-20231114-900.smgmp", 2, 1, nullptr, 0);
        mp_keepsafe_use_account("copy-20231114-900.smgmp", 1);
    }
    for (size_t len = 0; len < f.size(); len += 211) {
        writeFile(backups() + "/copy-20231114-901.smgmp", Bytes(f.begin(), f.begin() + (long)len));
        if (mp_keepsafe_inspect("copy-20231114-901.smgmp", &fi) == MP_KS_OK) accepted++;
    }
    CHECKM(accepted == 0, "%d damaged copies accepted", accepted);
    Tree now = dataSet();
    CHECK(now == live);
    // crafted: an empty settings entry, a path out of the folder, a newer format
    CHECK(mp_keepsafe_inspect("../x.smgmp", &fi) == MP_KS_ARGS && mp_keepsafe_inspect("nothing.smgmp", &fi) == MP_KS_NOT_FOUND);
}

// ================================================================================================================================
// bringing back a Team File slot
// ================================================================================================================================
static std::string copyHolding(const Bytes& team, bool account = false) {   // a made copy of a world holding `team`
    const MpKsPaths keep = gP;
    const std::string mpDir = gP.mpDir;
    newWorld("src");
    writeFile(gP.teamFile, team);
    if (account) writeFile(std::string(gP.onlineDir) + "/online.dat", makeAccount(500, 'f'));
    mp_keepsafe_test_reset();
    char name[96];
    mp_keepsafe_make_copy(account ? 0 : MP_KS_COPY_NO_ACCOUNT, name, sizeof name);
    const Bytes f = readFile(std::string(gP.dataDir) + "/mp-backups/" + name);
    gP = keep;
    mp_keepsafe_set_paths(&gP);
    mp_keepsafe_test_reset();
    mp_port_keepsafe_wire();
    mp_config_set_dir(mpDir.c_str());
    static int n = 0;
    const std::string unique = "copy-20231114-" + std::to_string(500 + n++) + ".smgmp";   // (each scratch world numbers its copy from 1)
    writeFile(backups() + "/" + unique, f);
    return unique;
}
static void testBringBackSlot() {
    world("slot");
    boot("3.0.0");
    const Bytes mine = teamSlot(2, 10, false);
    saveAtomic(gP.teamFile, mine);
    // the copy: the same 10-star count, other stars (review 1 #3): in use, asked, "yours has progress the copy lacks"
    const std::string other = copyHolding(teamSlot(2, 10, true));
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(other.c_str(), &fi) == MP_KS_OK && (fi.slotInUse & 2) && (fi.slotLacks & 2) && (fi.slotBlank & 1));   // (file 1 of the copy: never created)
    gReloads = 0;
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 2, 0, nullptr, 0) == MP_KS_CONFIRM && readFile(gP.teamFile) == mine && gReloads == 0);
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 1, 1, nullptr, 0) == MP_KS_NOTHING);   // a blank copy slot never replaces anything
    char before[96];
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 2, 1, before, sizeof before) == MP_KS_OK && !strncmp(before, "before-", 7));
    CHECK(starsOf(readFile(gP.teamFile), 2) == 10 && readFile(gP.teamFile) != mine && gReloads == 1 && gTeamAtReload == S_(readFile(gP.teamFile)));
    // the undo: bring back the "before" copy
    CHECK(mp_keepsafe_bring_back_slot(before, 2, 1, nullptr, 0) == MP_KS_OK);
    {
        MpSaveSnapshotView v;
        mp_save_view_slot(mine.data(), 2, &v);
        CHECK(mp_save_slot_matches(readFile(gP.teamFile).data(), 2, &v));
    }
    {   // the copy's slot 1 is blank (never created) and this console's slot 1 is IN USE: a blank file never replaces a created one
        const Bytes keepLive = readFile(gP.teamFile);
        int st[6] = {20, 10, 0, 0, 0, 0};
        saveAtomic(gP.teamFile, makeTeam(st));
        const Tree t = dataSet();
        CHECK(mp_save_slot_state(readFile(gP.teamFile).data(), 1) == 1);
        CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 1, 1, nullptr, 0) == MP_KS_NOTHING && dataSet() == t);
        saveAtomic(gP.teamFile, keepLive);
    }
    // a superset: still asked (the slot is in use) but the question does not say "yours has more"
    const std::string more = copyHolding(teamSlot(2, 30, false));
    CHECK(mp_keepsafe_inspect(more.c_str(), &fi) == MP_KS_OK && (fi.slotInUse & 2) && !(fi.slotLacks & 2));
    // an empty slot here: written without a question, the other slots untouched
    const Bytes other3 = teamSlot(3, 12);
    const std::string c3 = copyHolding(other3);
    const Bytes t0 = readFile(gP.teamFile);
    CHECK(mp_keepsafe_bring_back_slot(c3.c_str(), 3, 0, nullptr, 0) == MP_KS_OK && starsOf(readFile(gP.teamFile), 3) == 12);
    for (int s : {1, 2, 4, 5, 6}) {
        MpSaveSnapshotView v;
        mp_save_view_slot(t0.data(), s, &v);
        CHECK(mp_save_slot_matches(readFile(gP.teamFile).data(), s, &v));
    }
    CHECK(mp_keepsafe_bring_back_slot(c3.c_str(), 3, 0, nullptr, 0) == MP_KS_NOTHING);   // already the same
    // no Team File here at all: made with that slot only
    unlink(gP.teamFile);
    unlink((std::string(gP.teamFile) + ".mpbak").c_str());
    CHECK(mp_keepsafe_bring_back_slot(c3.c_str(), 3, 0, nullptr, 0) == MP_KS_OK && starsOf(readFile(gP.teamFile), 3) == 12 &&
          mp_save_slot_state(readFile(gP.teamFile).data(), 2) == 0);
    // the game's guard says no: nothing written
    gMayReplace = false;
    const Tree g0 = dataSet();
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 2, 1, nullptr, 0) == MP_KS_BUSY && dataSet() == g0);
    gMayReplace = true;
    // a live Team File that cannot be used (damaged / a newer game's): never written over
    writeFile(gP.teamFile, Bytes(5000, 7));
    unlink((std::string(gP.teamFile) + ".mpbak").c_str());
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 2, 1, nullptr, 0) == MP_KS_BAD_FILE && readFile(gP.teamFile) == Bytes(5000, 7));
    Bytes newer = mine;
    put32(&newer, 4, 3);
    writeFile(gP.teamFile, newer);
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 2, 1, nullptr, 0) == MP_KS_NEWER && readFile(gP.teamFile) == newer);
    // the before copy cannot be made: nothing replaced
    saveAtomic(gP.teamFile, mine);
    mp_keepsafe_test_set_free_bytes(100);
    CHECK(mp_keepsafe_bring_back_slot(other.c_str(), 2, 1, nullptr, 0) == MP_KS_NO_SPACE && readFile(gP.teamFile) == mine);
    mp_keepsafe_test_set_free_bytes(-1);
}

// ================================================================================================================================
// the account
// ================================================================================================================================
static void testAccount() {
    world("acct");
    const std::string od = std::string(gP.onlineDir) + "/online.dat";
    const Bytes a = makeAccount(100, 'a'), b = makeAccount(200, 'b'), c = makeAccount(300, 'c');
    Bytes bRen = b;
    memcpy(&bRen[12 + 64], "Renamed", 7);
    put32(&bRen, 108, crc(bRen, 108));
    unlink(od.c_str());
    // SaveAccount's store: this console's own credentials, each kept when replaced, a rename keeps none, no duplicates
    CHECK(mp_keepsafe_account_store(od.c_str(), a.data(), 112) && readFile(od) == a);
    CHECK(mp_keepsafe_account_store(od.c_str(), b.data(), 112) && readFile(od + ".prev") == a);
    CHECK(mp_keepsafe_account_store(od.c_str(), bRen.data(), 112) && readFile(od + ".prev") == a && !exists(od + ".prev.1"));
    CHECK(mp_keepsafe_account_store(od.c_str(), c.data(), 112) && readFile(od + ".prev") == bRen && readFile(od + ".prev.1") == a);
    uint8_t out[112];
    CHECK(mp_keepsafe_account_previous(od.c_str(), 1, out) && Bytes(out, out + 112) == a);
    // "Use this copy's online account": asked; the own credential kept; a FOREIGN one never goes into .prev (review 3 #5)
    const std::string foreign = copyHolding(teamSlot(2, 5), true);   // holds makeAccount(500, 'f')
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(foreign.c_str(), &fi) == MP_KS_OK && fi.account);
    CHECK(mp_keepsafe_use_account(foreign.c_str(), 0) == MP_KS_CONFIRM && readFile(od) == c);
    gServiceStops = 0;
    CHECK(mp_keepsafe_use_account(foreign.c_str(), 1) == MP_KS_OK && readFile(od) == makeAccount(500, 'f') && gServiceStops == 1);
    CHECK(readFile(od + ".prev") == c && readFile(od + ".prev.1") == bRen && readFile(od + ".prev.2") == a);
    // back to this console's own credential from its own copy: the foreign one is not kept, the chain is unchanged
    const Bytes chain0 = readFile(od + ".prev"), chain1 = readFile(od + ".prev.1"), chain2 = readFile(od + ".prev.2");
    char own[96];
    writeFile(od + ".tmpc", Bytes());
    unlink((od + ".tmpc").c_str());
    {
        const Bytes f500 = readFile(od);
        writeFile(od, c);   // (this console's own c, then a copy of it)
        CHECK(mp_keepsafe_make_copy(0, own, sizeof own) == MP_KS_OK);
        writeFile(od, f500);
    }
    CHECK(mp_keepsafe_use_account(own, 1) == MP_KS_OK && readFile(od) == c);
    CHECK(readFile(od + ".prev") == chain0 && readFile(od + ".prev.1") == chain1 && readFile(od + ".prev.2") == chain2);
    // a newer game's account file, or one that cannot be read now: never written over
    writeFile(od, makeAccount(9, 'z', 2));
    CHECK(!mp_keepsafe_account_store(od.c_str(), a.data(), 112) && mp_keepsafe_use_account(foreign.c_str(), 1) == MP_KS_IO);
    CHECK(readFile(od) == makeAccount(9, 'z', 2));
    // the token never reaches the log
    const std::string tok = S_(makeAccount(500, 'f')).substr(12, 12);
    CHECK(gLog.find(tok) == std::string::npos && gLog.find("online.dat") == std::string::npos);
}

// ================================================================================================================================
// settings (this console's own copies only)
// ================================================================================================================================
static void testSettings() {
    world("settings");
    boot("3.0.0");
    const std::string mine = versionCopy();
    const Tree d0 = dataSet();
    writeFile(std::string(gP.mpDir) + "/mp.cfg", B("version = 1\nname = Changed\nupnp_permanent = 40123\nconsole_key = 00112233445566778899aabbccddeeff\n"));   // (this console's identity: the copy's mp.cfg comes back with it)
    writeFile(std::string(gP.mpDir) + "/flow.cfg", B("reach = 0\n"));
    mp_config_load();
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(mine.c_str(), &fi) == MP_KS_OK && fi.settings == (MP_KS_SET_CONFIG | MP_KS_SET_FLOW));
    char before[96];
    gReloads = 0;
    gMpCfgReads = gMpCfgReadsUnlocked = gHooksUnlocked = 0;
    mp_keepsafe_test_set_read_hook(readSpy);
    CHECK(mp_keepsafe_bring_back_settings(mine.c_str(), MP_KS_SET_CONFIG | MP_KS_SET_FLOW, before, sizeof before) == MP_KS_OK && before[0]);
    mp_keepsafe_test_set_read_hook(nullptr);
    // the settings lock is taken BEFORE mp.cfg is read or copied, and the reload hooks run under it (review 3 #10)
    CHECKM(gMpCfgReads > 0 && gMpCfgReadsUnlocked == 0 && gHooksUnlocked == 0 && !gHeld, "mp.cfg reads %d unlocked %d, hooks unlocked %d, held at the end %d", gMpCfgReads, gMpCfgReadsUnlocked, gHooksUnlocked, (int)gHeld);
    CHECK(readFile(std::string(gP.mpDir) + "/mp.cfg") == d0.at("mp/mp.cfg") && readFile(std::string(gP.mpDir) + "/flow.cfg") == d0.at("mp/flow.cfg"));
    CHECK(!strcmp(mp_config()->name, "Alex") && gReloads == 1);   // loaded (under the lock) before the call returned
    CHECK(mp_keepsafe_bring_back_settings(before, MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_OK && !strcmp(mp_config()->name, "Changed"));   // the undo
    // never from a made copy (someone else's settings: review 3 #4), even one that carries them
    const std::string made = copyHolding(teamSlot(2, 5));
    {   // crafted: a made copy with settings entries in it
        Bytes f = readFile(backups() + "/" + made);
        (void)f;
    }
    CHECK(mp_keepsafe_bring_back_settings(made.c_str(), 7, nullptr, 0) == MP_KS_NOTHING);
    {   // crafted: a copy file of the made kind that carries settings entries: never read as settings; its Team File and account are still offered
        const Bytes cfgOther = B("version = 1\nname = Stranger\nconsole_key = ffeeddccbbaa99887766554433221100\n");
        writeFile(backups() + "/copy-20231114-700.smgmp", buildCopyFile({{"mp/mp.cfg", cfgOther}, {"mp/flow.cfg", B("reach = 1\n")}, {"mp/saves/GameData.bin", teamSlot(4, 8)}}));
        MpKsCopyInfo ci;
        CHECK(mp_keepsafe_inspect("copy-20231114-700.smgmp", &ci) == MP_KS_OK && ci.settings == 0 && !(ci.slotBlank & 8));
        const Tree s0 = dataSet();
        CHECK(mp_keepsafe_bring_back_settings("copy-20231114-700.smgmp", 7, nullptr, 0) == MP_KS_NOTHING && dataSet() == s0);
    }
    // a newer game's mp.cfg here: left alone
    writeFile(std::string(gP.mpDir) + "/mp.cfg", B("version = 2\nname = Future\n"));
    mp_config_load();
    CHECK(mp_keepsafe_bring_back_settings(mine.c_str(), MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_NOTHING);
    CHECK(readFile(std::string(gP.mpDir) + "/mp.cfg") == B("version = 2\nname = Future\n"));
}

// ================================================================================================================================
// the third block, damaged copies, old-design leftovers, read-only calls, the settings writers
// ================================================================================================================================
static void testBlocksAndDamage() {
    world("blocks");
    boot("3.0.0");
    // a copy with more stars, but this console's config block (the slot's third block) has a bit the copy's lacks: "yours has progress the copy lacks"
    Bytes live = teamSlot(2, 10, false), cp = teamSlot(2, 30, false);   // (the copy has every star of this console's file, and more)
    {
        uint32_t off, size;
        CHECK(mp_save_find(live.data(), "config2", &off, &size) && size > 40);
        const uint32_t at = off + 4 + rd32(live.data() + off + 4 + 8) - 1;   // (the last byte of the block's first chunk)
        live[at] |= 0x01;   // (a bit the copy's config block does not have: set in this console's)
        cp[at] &= (uint8_t)~0x01;
        mp_save_finalize(live.data(), MP_SAVE_MAX_FILE);
        mp_save_finalize(cp.data(), MP_SAVE_MAX_FILE);
    }
    saveAtomic(gP.teamFile, live);
    const std::string c = copyHolding(cp, false);
    MpKsCopyInfo fi;
    const int ir = mp_keepsafe_inspect(c.c_str(), &fi);
    CHECKM(ir == MP_KS_OK && (fi.slotInUse & 2) && (fi.slotLacks & 2), "rc %s inUse %x lacks %x same %x", mp_keepsafe_result_name(ir), fi.slotInUse, fi.slotLacks, fi.slotSame);
    // a copy that is damaged is not listed and never counted or removed (bring back from it: refused, nothing written)
    boot("3.1.0");
    const std::string v = versionCopy();
    const Tree all0 = treeOf(gP.dataDir);
    Bytes mp = readFile(backups() + "/" + v + "/mp/mp.cfg");
    mp[mp.size() / 2] ^= 0x20;
    writeFile(backups() + "/" + v + "/mp/mp.cfg", mp);
    bool listed = false;
    for (const MpKsCopy& k : copies()) listed = listed || v == k.name;
    CHECK(!listed && mp_keepsafe_inspect(v.c_str(), &fi) == MP_KS_BAD_FILE);
    for (int i = 0; i < 12; i++) boot(("4." + std::to_string(i) + ".0").c_str());   // (prunes at every start: a copy it cannot verify is kept)
    CHECK(exists(backups() + "/" + v + "/mp/mp.cfg") && readFile(backups() + "/" + v + "/mp/mp.cfg") == mp);
    (void)all0;
}
static void testOldDesignLeftovers() {
    // a tester's card from the earlier design: its journal, stamp marks, .kstmp / .ksold files and snapshot folders are left alone and never read
    world("old");
    const std::string mpd = gP.mpDir;
    writeFile(mpd + "/saves/title/00010000/524d4750/data/GameData.bin.ksold", B("ksold-bytes"));
    writeFile(mpd + "/mp.cfg.kstmp", B("kstmp-bytes"));
    writeFile(std::string(gP.dataDir) + "/mp-keepsafe.journal", B("journal-bytes"));
    writeFile(stamp(), B("keepsafe = 1\nversion = 2.9.0\nbuild = old\nseq = 4\npending = import\ndeclined = 1234\ncrc = 00000000\n"));
    {   // (a stamp with its CRC as the earlier design wrote it)
        std::string t = "keepsafe = 1\nversion = 2.9.0\nbuild = old\nseq = 4\npending = import\ndeclined = 1234\n";
        char crcb[32];
        snprintf(crcb, sizeof crcb, "crc = %08x\n", (unsigned)mp_save_crc32(t.data(), t.size()));
        writeFile(stamp(), B(t + crcb));
    }
    const Tree before = dataSet();
    const Bytes journal = readFile(std::string(gP.dataDir) + "/mp-keepsafe.journal");
    boot("3.0.0");
    CHECK(copies().size() == 1);
    const Tree snap = treeOf(backups() + "/" + versionCopy());
    for (auto& kv : snap) CHECK(kv.first.find("ksold") == std::string::npos && kv.first.find("kstmp") == std::string::npos);
    for (auto& kv : before) CHECK(dataSet().count(kv.first) && dataSet().at(kv.first) == kv.second);   // every live file untouched
    CHECK(readFile(std::string(gP.dataDir) + "/mp-keepsafe.journal") == journal);
    CHECK(S_(readFile(stamp())).find("version = 3.0.0") != std::string::npos && copies()[0].version[0] == '3');
    // an undo snapshot folder of the earlier design (manifest class 1): never listed, never read, never removed by a later prune
    const std::string v = versionCopy();
    const std::string undo = backups() + "/undo-import-20231114-000";
    for (auto& kv : treeOf(backups() + "/" + v)) writeFile(undo + "/" + kv.first, kv.second);
    Bytes m = readFile(undo + "/MANIFEST.ksm");
    m[8] = 1;
    put32(&m, m.size() - 4, mp_save_crc32(m.data(), m.size() - 4));
    writeFile(undo + "/MANIFEST.ksm", m);
    const Tree u0 = treeOf(undo);
    for (int i = 0; i < 12; i++) boot(("5." + std::to_string(i) + ".0").c_str());
    CHECK(treeOf(undo) == u0);
    for (const MpKsCopy& c : copies()) CHECK(strcmp(c.name, "undo-import-20231114-000") != 0);
}
static void testReadOnlyCalls() {
    world("readonly");
    boot("3.0.0");
    saveAtomic(gP.teamFile, teamSlot(2, 10));
    const std::string c = copyHolding(teamSlot(2, 20), true);
    const Tree all0 = treeOf(gP.dataDir);
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(c.c_str(), &fi) == MP_KS_OK);
    CHECK(copies().size() >= 2);
    CHECK(mp_keepsafe_bring_back_slot(c.c_str(), 2, 0, nullptr, 0) == MP_KS_CONFIRM && mp_keepsafe_use_account(c.c_str(), 0) == MP_KS_CONFIRM);
    CHECK(treeOf(gP.dataDir) == all0);   // listing, inspecting and asking change nothing (not even a recovery rename)
    // the Team File's own cut write (primary missing, the new file complete as .mptmp): this module does not repair it, it only reads
    const std::string tf = gP.teamFile;
    const Bytes good = readFile(tf);
    rename(tf.c_str(), (tf + ".mptmp").c_str());
    char nm[96];
    const Tree t0 = treeOf(gP.dataDir);
    mp_keepsafe_make_copy(0, nm, sizeof nm);
    mp_keepsafe_inspect(c.c_str(), &fi);
    Tree t1 = treeOf(gP.dataDir);
    t1.erase("mp-backups/" + std::string(nm));
    CHECK(t1 == t0 && !exists(tf) && exists(tf + ".mptmp"));
    rename((tf + ".mptmp").c_str(), tf.c_str());
}
static void testSettingsWriters() {
    // flow.cfg's own writer: when the file is missing (a cut between its two renames) the .bak is the only copy and is never removed by it
    world("flowwriter");
    const std::string flow = std::string(gP.mpDir) + "/flow.cfg";
    rename(flow.c_str(), (flow + ".bak").c_str());
    const Bytes only = readFile(flow + ".bak");
    CHECK(!only.empty() && !exists(flow));
    MpFlowStore f;
    mp_flow_store_get(&f);   // (read from .bak)
    CHECK(f.reach == 2);
    f.lastCoop = 4;
    mp_flow_store_set(&f);
    CHECK(exists(flow) && readFile(flow + ".bak") == only);
}


// ================================================================================================================================
// keepfix 5 (the fourth outside review, share/reviews/keepsafe-4.md): one test per finding
// ================================================================================================================================
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>

// opendir of a folder whose path ends in gFailOpendir fails (EIO) while it is set; fsync and rename are logged by the path of the file
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
static std::vector<std::string> gIoEvents;
static bool gLogIo = false, gSlowFsync = false;   // gSlowFsync: a recovery checkpoint's fsync takes 300 ms (the async writer is still busy when a call returns without waiting)
extern "C" int fsync(int fd) {
    typedef int (*fs_t)(int);
    static fs_t f = (fs_t)dlsym(RTLD_NEXT, "fsync");
    if (gLogIo || gSlowFsync) {
        char link[64], path[400];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const ssize_t n = readlink(link, path, sizeof path - 1);
        if (n > 0 && gLogIo) gIoEvents.push_back("fsync " + std::string(path, (size_t)n));
        if (n > 0 && gSlowFsync && std::string(path, (size_t)n).find("session-") != std::string::npos) usleep(300000);
    }
    return f(fd);
}
// Deterministic window for AccountLock: when the armed account write is between its two renames (the old file moved to .mpbak, the new one not yet
// in place), Make a Copy runs on another thread and the writer waits up to 400 ms for it (with the account lock it cannot finish in that time).
static std::atomic<int> gWindowArm{0};   // 0 off, 1 armed, 2 fired
static std::atomic<bool> gWindowCopyDone{false};
static char gWindowName[96];
static void* windowCopy(void*) {
    mp_keepsafe_make_copy(0, gWindowName, sizeof gWindowName);
    gWindowCopyDone = true;
    return nullptr;
}
static pthread_t gWindowThread;
extern "C" int rename(const char* a, const char* b) {
    typedef int (*rn_t)(const char*, const char*);
    static rn_t f = (rn_t)dlsym(RTLD_NEXT, "rename");
    if (gLogIo) gIoEvents.push_back("rename " + std::string(a));
    const std::string sa = a, sb = b;
    if (gWindowArm.load() == 1 && sb.size() > 11 && !sb.compare(sb.size() - 11, 11, "/online.dat") && sa == sb + ".mptmp") {
        gWindowArm = 2;
        pthread_create(&gWindowThread, nullptr, windowCopy, nullptr);
        for (int i = 0; i < 80 && !gWindowCopyDone.load(); i++) usleep(5000);
    }
    return f(a, b);
}
static int ioIndex(const std::string& ev) {
    for (size_t i = 0; i < gIoEvents.size(); i++)
        if (gIoEvents[i] == ev) return (int)i;
    return -1;
}

static std::string onlineDir() { return gP.onlineDir; }
static bool slotStructOk(const Bytes& t, int slot) {
    MpSaveSnapshotView v;
    return mp_save_view_slot(t.data(), slot, &v) && mp_save_check_view(&v, MP_SAVECHECK_STRUCT, nullptr) == MP_SC_OK;
}
static std::string mpFile(const char* n) { return std::string(gP.mpDir) + "/" + n; }
static std::string nthCopy(const char* prefix, int n) { char b[64]; snprintf(b, sizeof b, "%s-20231114-%03d.smgmp", prefix, n); return b; }
static std::vector<MpKsCopy> allCopies() {   // (copies() stops at 32)
    MpKsCopy c[200];
    const int n = mp_keepsafe_list_copies(c, 200);
    return std::vector<MpKsCopy>(c, c + n);
}
static int countBefore() {
    int n = 0;
    for (const MpKsCopy& c : allCopies())
        if (c.kind == MP_KS_COPY_BEFORE) n++;
    return n;
}
static std::string newestBefore() {
    for (const MpKsCopy& c : copies())
        if (c.kind == MP_KS_COPY_BEFORE) return c.name;
    return "";
}

// finding 1: community-server accounts online_<8 hex>.dat (19 characters) through copy, list, inspect and use
static void testAccountNames() {
    world("acctnames");
    const Bytes comm = makeAccount(4242, 'z');
    const std::string live = onlineDir() + "/online_0badf00d.dat";
    writeFile(live, comm);
    boot("3.0.0");
    char name[96];
    CHECK(mp_keepsafe_make_copy(0, name, sizeof name) == MP_KS_OK);
    CHECK(S_(readFile(backups() + "/" + name)).find("online/online_0badf00d.dat") != std::string::npos);   // in Make a Copy
    bool seen = false;
    for (const MpKsCopy& c : copies())
        if (!strcmp(c.name, name)) seen = c.account == 1;
    CHECK(seen);   // listed as holding an account
    unlink(live.c_str());
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(name, &fi) == MP_KS_OK && fi.account == 1);   // inspect offers it
    CHECK(mp_keepsafe_use_account(name, 0) == MP_KS_CONFIRM && !exists(live));
    CHECK(mp_keepsafe_use_account(name, 1) == MP_KS_OK && readFile(live) == comm);   // use brings it back
    // the version copy too
    unlink(live.c_str());
    const std::string v = versionCopy();
    CHECK(mp_keepsafe_inspect(v.c_str(), &fi) == MP_KS_OK && fi.account == 1);
    CHECK(mp_keepsafe_use_account(v.c_str(), 1) == MP_KS_OK && readFile(live) == comm);
    // names that are not account names (18 / 20 characters, a non-hex digit, a wrong ending) are never read as accounts
    const Bytes other = makeAccount(7, 'q');
    writeFile(backups() + "/copy-20231114-800.smgmp",
              buildCopyFile({{"online/online_0badf00.dat", other}, {"online/online_0badf00dd.dat", other}, {"online/online_0badf00g.dat", other}, {"online/online_0badf00d.dax", other}}));
    CHECK(mp_keepsafe_inspect("copy-20231114-800.smgmp", &fi) == MP_KS_OK && fi.account == 0);
}

// A Team File whose slot 2 differs in USED bytes for every i (a luigi block byte; the padding after the used part is never compared)
static Bytes slotVariant(int i) {
    Bytes img = teamSlot(2, 10, false);
    uint32_t off, size;
    mp_save_find(img.data(), "luigi2", &off, &size);
    const uint32_t used = mp_save_block_used(img.data() + off, size);
    img[off + used - 1 - (uint32_t)(i / 200)] = (uint8_t)(i % 200 + 1);
    mp_save_finalize(img.data(), MP_SAVE_MAX_FILE);
    return img;
}
// finding 3: a "before" copy is the player's undo: the 17th replacement does not delete the only copy of what the first replaced
static void testBeforeCopies() {
    world("before");
    boot("3.0.0");
    const std::string tf = gP.teamFile;
    int st[6] = {0, 47, 30, 40, 45, 0};   // slot 2 holds progress that is in no version copy (the world's own file has 50 stars there)
    saveAtomic(tf, makeTeam(st));
    std::string first;
    for (int i = 1; i <= 20; i++) {
        mp_keepsafe_test_set_time(1700000000 + i * 10);
        const std::string cn = nthCopy("copy", 600 + i);
        writeFile(backups() + "/" + cn, buildCopyFile({{"mp/saves/GameData.bin", slotVariant(i)}}));
        char b[96];
        const int rcb = mp_keepsafe_bring_back_slot(cn.c_str(), 2, 1, b, sizeof b);
        CHECKM(rcb == MP_KS_OK, "round %d: %s", i, mp_keepsafe_result_name(rcb));
        if (i == 1) first = b;
    }
    // the first "before" copy is the only holder of the 47-star slot 2: it stays; the ones whose slots a kept copy or the live file holds go beyond 16
    CHECK(exists(backups() + "/" + first));
    CHECKM(countBefore() == 17, "%d before copies", countBefore());
    CHECK(mp_keepsafe_bring_back_slot(first.c_str(), 2, 1, nullptr, 0) == MP_KS_OK && starsOf(readFile(tf), 2) == 47);   // and it is still the way back
    // each "before" copy holds something unique (the made copies are removed again): the 65th bring-back is refused, nothing is deleted
    world("before-cap");
    boot("3.0.0");
    saveAtomic(gP.teamFile, teamSlot(2, 47, false));
    int refused = 0;
    std::set<std::string> kept;
    for (int i = 1; i <= 70; i++) {
        mp_keepsafe_test_set_time(1700000000 + i * 10);
        const std::string cn = nthCopy("copy", 600 + i);
        writeFile(backups() + "/" + cn, buildCopyFile({{"mp/saves/GameData.bin", slotVariant(i)}}));
        const Bytes liveBefore = readFile(gP.teamFile);
        const int rc0 = mp_keepsafe_bring_back_slot(cn.c_str(), 2, 0, nullptr, 0);   // (asked before the question: nothing would be done with the answer)
        const int rc = rc0 == MP_KS_CONFIRM ? mp_keepsafe_bring_back_slot(cn.c_str(), 2, 1, nullptr, 0) : rc0;
        unlink((backups() + "/" + cn).c_str());
        if (rc == MP_KS_TOO_MANY) {
            refused++;
            CHECK(rc0 == MP_KS_TOO_MANY && readFile(gP.teamFile) == liveBefore);
        }
        if (i == 64) for (const MpKsCopy& c : allCopies()) if (c.kind == MP_KS_COPY_BEFORE) kept.insert(c.name);
    }
    CHECKM(countBefore() == 64 && refused == 6 && kept.size() == 64, "before %d refused %d kept %zu", countBefore(), refused, kept.size());
    int still = 0;
    for (const MpKsCopy& c : allCopies())
        if (kept.count(c.name)) still++;
    CHECK(still == 64);   // not one of the 64 was deleted by a refused call
    // settings copies hold no Team File slot: beyond 16 they go (each is the player's own small file)
}

// finding 4: a Team File the game loads is copyable; a good copy repairs a damaged slot
static Bytes damagedSlotTeam(int slot) {   // the file's checks pass (the game loads it), slot `slot` fails the structure check
    static std::map<int, Bytes> cache;
    if (cache.count(slot)) return cache[slot];
    Bytes base = teamSlot(slot, 20);
    char n[32];
    snprintf(n, sizeof n, "mario%d", slot);
    uint32_t off, size;
    mp_save_find(base.data(), n, &off, &size);
    for (uint32_t pos = 0; pos < size; pos++)
        for (uint8_t val : {0xFF, 0x7F, 0x80, 0x01}) {
            Bytes img(MP_SAVE_MAX_FILE, 0);
            memcpy(img.data(), base.data(), base.size());
            img[off + pos] = val;
            mp_save_finalize(img.data(), MP_SAVE_MAX_FILE);
            img.resize(mp_save_disk_length(img.data()));
            MpSaveSnapshotView v;
            if (mp_save_validate(img.data(), (uint32_t)img.size(), true, true) == MP_SAVE_OK && mp_save_view_slot(img.data(), slot, &v) &&
                mp_save_check_view(&v, MP_SAVECHECK_STRUCT, nullptr) != MP_SC_OK)
                return cache[slot] = img;
        }
    return cache[slot] = Bytes();
}
static void testLiveStructure() {
    world("structure");
    boot("3.0.0");
    const Bytes bad2 = damagedSlotTeam(2);
    CHECK(!bad2.empty());
    const std::string good = copyHolding(teamSlot(2, 30, false));   // a good copy of slot 2
    saveAtomic(gP.teamFile, bad2);
    // Make a Copy: only the file-level checks apply to the live file (the game loads it)
    char name[96];
    CHECK(mp_keepsafe_make_copy(0, name, sizeof name) == MP_KS_OK);
    CHECK(S_(readFile(backups() + "/" + name)).find(S_(bad2)) != std::string::npos);   // its bytes, as they are
    // bring a good slot over the damaged one: it is the repair
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(good.c_str(), &fi) == MP_KS_OK && (fi.slotInUse & 2));
    char before[96];
    CHECK(mp_keepsafe_bring_back_slot(good.c_str(), 2, 0, nullptr, 0) == MP_KS_CONFIRM);
    CHECK(mp_keepsafe_bring_back_slot(good.c_str(), 2, 1, before, sizeof before) == MP_KS_OK);
    CHECK(starsOf(readFile(gP.teamFile), 2) == 30 && slotStructOk(readFile(gP.teamFile), 2));
    CHECK(readFile(backups() + "/" + before) != Bytes() && S_(readFile(backups() + "/" + before)).find(S_(bad2)) != std::string::npos);   // the damaged file is in the before copy
    // a copy whose OTHER slot is damaged: its good slot comes back; its damaged slot is refused and not offered
    const Bytes bad5 = damagedSlotTeam(5);
    CHECK(!bad5.empty());
    writeFile(backups() + "/copy-20231114-810.smgmp", buildCopyFile({{"mp/saves/GameData.bin", bad5}}));
    CHECK(mp_keepsafe_inspect("copy-20231114-810.smgmp", &fi) == MP_KS_OK && (fi.slotBlank & 16));
    CHECK(mp_keepsafe_bring_back_slot("copy-20231114-810.smgmp", 5, 1, nullptr, 0) == MP_KS_BAD_FILE);
    // the good slots of the damaged live file stay as they are (a damaged slot elsewhere is not this call's business)
    saveAtomic(gP.teamFile, bad5);
    const std::string good3 = copyHolding(teamSlot(3, 12));
    const Bytes l0 = readFile(gP.teamFile);
    CHECK(mp_keepsafe_bring_back_slot(good3.c_str(), 3, 0, nullptr, 0) == MP_KS_OK && starsOf(readFile(gP.teamFile), 3) == 12);
    MpSaveSnapshotView v;
    mp_save_view_slot(l0.data(), 5, &v);
    CHECK(mp_save_slot_matches(readFile(gP.teamFile).data(), 5, &v));
}

// finding 5: a folder that is there and cannot be listed: the copy fails, the stamp does not move on
static void testFailedListing() {
    for (const char* folder : {"/524d4750/data", "/mp", "/online"}) {
        world("listfail");
        const Tree d0 = dataSet();
        gFailOpendir = folder;
        boot("3.0.0");
        gFailOpendir = nullptr;
        CHECKM(copies().empty() && !exists(stamp()) && mp_keepsafe_notice_take() == MP_KS_NOTICE_NO_COPY && !mp_keepsafe_may_migrate(), "%s: copies %zu", folder, copies().size());
        CHECK(dataSet() == d0);
        boot("3.0.0");   // the next start: complete
        const std::vector<MpKsCopy> c = copies();
        CHECK(c.size() == 1 && c[0].teamFiles && c[0].account && c[0].settings && exists(stamp()) && mp_keepsafe_may_migrate());
    }
    // a folder that is NOT there is simply empty: no failure
    world("nofolder", World{.team = false, .recovery = false, .account = false, .cfg = false, .flow = false, .coop = false, .conntest = false, .backups = false});
    rmrf(gP.onlineDir);
    boot("3.0.0");
    CHECK(exists(stamp()) && mp_keepsafe_notice_take() == MP_KS_NOTICE_NONE);
}

// finding 6: mp.cfg that could not be read is what a restore is for; a newer game's file is still left alone
static void testConfigRestore() {
    world("cfgrestore");
    boot("3.0.0");
    const std::string v = versionCopy();
    const Bytes goodCfg = readFile(mpFile("mp.cfg"));
    writeFile(mpFile("mp.cfg"), B("this is not a settings file \x01\x02"));
    mp_config_load();
    CHECK(mp_config_read_only() == MP_CONFIG_UNREADABLE);
    MpKsCopyInfo fi;
    CHECK(mp_keepsafe_inspect(v.c_str(), &fi) == MP_KS_OK && (fi.settings & MP_KS_SET_CONFIG));
    char before[96];
    CHECK(mp_keepsafe_bring_back_settings(v.c_str(), MP_KS_SET_CONFIG, before, sizeof before) == MP_KS_OK);
    CHECK(readFile(mpFile("mp.cfg")) == goodCfg && mp_config_read_only() == MP_CONFIG_WRITABLE && !strcmp(mp_config()->name, "Alex"));
    // a newer game's file: refused (nothing changes)
    writeFile(mpFile("mp.cfg"), B("version = 2\nname = Future\n"));
    mp_config_load();
    const Tree t0 = dataSet();
    CHECK(mp_keepsafe_bring_back_settings(v.c_str(), MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_NOTHING && dataSet() == t0);
}

// finding 7: a stamp that is there but does not parse is no stamp; one that cannot be read changes nothing (CopyFailures)
static void testStampParse() {
    const char* const bad[] = {"garbage", "keepsafe = 1\nversion = 3.0.0\nseq = 4\ncrc = 00000000\n", "keepsafe = 1\nversion = 3.0.0\npending = x\npending_op = restore\nseq = 4\ncrc = 00000000\n", ""};
    for (const char* b : bad) {
        world("stampbad");
        writeFile(stamp(), B(b));
        boot("3.0.0");
        CHECKM(copies().size() == 1 && mp_keepsafe_may_migrate() && mp_keepsafe_notice_take() == MP_KS_NOTICE_NONE, "stamp '%.20s': copies %zu", b, copies().size());
        CHECK(S_(readFile(stamp())).find("version = 3.0.0") != std::string::npos);
        boot("3.0.0");   // the stamp is good now: no second copy
        CHECK(copies().size() == 1);
        boot("3.1.0");
        CHECK(copies().size() == 2);
    }
    world("stampbin");
    Bytes bin(300);
    for (size_t i = 0; i < bin.size(); i++) bin[i] = (uint8_t)(i * 7 + 3);
    writeFile(stamp(), bin);
    boot("3.0.0");
    CHECK(copies().size() == 1 && mp_keepsafe_may_migrate());
}

// finding 8: a .prev* file or the own list that cannot be read is not "absent"; an own list that fails its check is "no list"
static void testAccountReads() {
    world("acctreads");
    const std::string od = onlineDir() + "/online.dat";
    unlink(od.c_str());
    for (int pid : {100, 200, 300}) CHECK(mp_keepsafe_account_store(od.c_str(), makeAccount(pid, (char)('a' + pid / 100)).data(), 112));
    CHECK(readFile(od + ".prev") == makeAccount(200, 'c') && readFile(od + ".prev.1") == makeAccount(100, 'b'));
    {   // .prev.1 cannot be read: the store fails and changes nothing
        const Bytes p1 = readFile(od + ".prev.1");
        unlink((od + ".prev.1").c_str());
        unlink((od + ".prev.1.mpbak").c_str());
        mkdirs(od + ".prev.1");
        const Tree t0 = treeOf(gP.onlineDir);
        CHECK(!mp_keepsafe_account_store(od.c_str(), makeAccount(400, 'd').data(), 112) && treeOf(gP.onlineDir) == t0);
        rmrf(od + ".prev.1");
        writeFile(od + ".prev.1", p1);
    }
    {   // the own list cannot be read: the same
        const Bytes own = readFile(onlineDir() + "/keepsafe-own.txt");
        unlink((onlineDir() + "/keepsafe-own.txt").c_str());
        unlink((onlineDir() + "/keepsafe-own.txt.mpbak").c_str());
        mkdirs(onlineDir() + "/keepsafe-own.txt");
        const Tree t0 = treeOf(gP.onlineDir);
        CHECK(!mp_keepsafe_account_store(od.c_str(), makeAccount(400, 'd').data(), 112) && treeOf(gP.onlineDir) == t0);
        rmrf(onlineDir() + "/keepsafe-own.txt");
        writeFile(onlineDir() + "/keepsafe-own.txt", own);
    }
    CHECK(mp_keepsafe_account_store(od.c_str(), makeAccount(400, 'e').data(), 112) && readFile(od + ".prev") == makeAccount(300, 'd'));   // (and it works again)
    // an own list that fails its check (empty, garbage, a stale length) counts as no list: the account replaced is kept
    int n = 0;
    for (const Bytes& junk : {Bytes(), B("garbage!!"), B("0000006f\n00000")}) {
        world("acctlist");
        unlink(od.c_str());
        CHECK(mp_keepsafe_account_store(od.c_str(), makeAccount(100, 'a').data(), 112));
        CHECK(mp_keepsafe_account_store(od.c_str(), makeAccount(300, 'c').data(), 112));
        unlink((onlineDir() + "/keepsafe-own.txt.mpbak").c_str());
        writeFile(onlineDir() + "/keepsafe-own.txt", junk);
        CHECK(mp_keepsafe_account_store(od.c_str(), makeAccount(500, 'e').data(), 112));
        CHECKM(readFile(od + ".prev") == makeAccount(300, 'c'), "variant %d: the credential replaced is not in .prev", n);
        CHECK(readFile(od) == makeAccount(500, 'e'));
        n++;
    }
}

// finding 9: settings from a copy never carry another console's identity, whatever the copy's name says
static void testIdentity() {
    const std::string myKey = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    // a version copy made on ANOTHER console, moved here with the folder
    world("console-a", World{.keyHex = "00112233445566778899aabbccddeeff"});
    boot("3.0.0");
    const std::string vname = versionCopy();
    const Tree other = treeOf(backups() + "/" + vname);
    world("console-b", World{.keyHex = myKey.c_str()});
    for (auto& kv : other) writeFile(backups() + "/" + vname + "/" + kv.first, kv.second);
    mp_config_load();
    const std::string mineBefore = S_(readFile(mpFile("mp.cfg")));
    CHECK(mineBefore.find("console_key = " + myKey) != std::string::npos && mineBefore.find("name = Alex") != std::string::npos);
    // (the copy's name is Alex too: make this console's differ so the bring-back has something to do)
    writeFile(mpFile("mp.cfg"), B(cfgText("Local", myKey.c_str(), 5)));
    mp_config_load();
    MpKsCopyInfo fi;
    const int ir = mp_keepsafe_inspect(vname.c_str(), &fi);
    CHECKM(ir == MP_KS_OK && (fi.settings & MP_KS_SET_CONFIG), "inspect %s", mp_keepsafe_result_name(ir));
    CHECK(mp_keepsafe_bring_back_settings(vname.c_str(), MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_OK);
    std::string now = S_(readFile(mpFile("mp.cfg")));
    CHECK(now.find("name = Alex") != std::string::npos && now.find("console_key = " + myKey) != std::string::npos);   // the copy's settings, this console's identity
    CHECK(now.find("00112233445566778899aabbccddeeff") == std::string::npos && now.find("upnp_permanent = 40123") != std::string::npos);
    uint8_t key[16];
    mp_config_console_key(key);
    CHECK(key[0] == 0xaa && key[15] == 0xaa);
    // a BEFORE copy of another console with other values (a friend's file), in this console's folder
    writeFile(backups() + "/before-20990101-900.smgmp",
              buildCopyFile({{"mp/mp.cfg", B("version = 1\nname = Friend\nupnp_permanent = 55555\nconsole_key = ffeeddccbbaa99887766554433221100\n")}}));
    CHECK(mp_keepsafe_bring_back_settings("before-20990101-900.smgmp", MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_OK);
    now = S_(readFile(mpFile("mp.cfg")));
    CHECK(now.find("name = Friend") != std::string::npos && now.find("ffeeddcc") == std::string::npos && now.find("55555") == std::string::npos);
    CHECK(now.find("console_key = " + myKey) != std::string::npos && now.find("upnp_permanent = 40123") != std::string::npos);
    // a console with no key / no mapping yet: the copy's are dropped, none is invented
    writeFile(mpFile("mp.cfg"), B("version = 1\nname = Bare\n"));
    mp_config_load();
    CHECK(mp_keepsafe_bring_back_settings("before-20990101-900.smgmp", MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_OK);
    now = S_(readFile(mpFile("mp.cfg")));
    CHECK(now.find("name = Friend") != std::string::npos && now.find("ffeeddcc") == std::string::npos && now.find("upnp_permanent") == std::string::npos);
}

// finding 10: a version copy made right after a cut in the old version's Team File save takes the valid .mptmp, else the valid .mpbak
static void testCutSave() {
    for (int variant = 0; variant < 3; variant++) {
        world("cutsave");
        boot("3.0.0");
        const std::string tf = gP.teamFile;
        const Bytes pending = teamSlot(2, 33), old = teamSlot(2, 21);
        unlink(tf.c_str());
        if (variant < 2) writeFile(tf + ".mptmp", variant == 1 ? Bytes(pending.begin(), pending.begin() + (long)pending.size() / 2) : pending);   // 1: the temporary is torn; 2: none
        writeFile(tf + ".mpbak", old);
        const Tree live0 = dataSet();
        boot("3.1.0");
        const std::string v = versionCopy();
        CHECK(!v.empty() && dataSet() == live0 && !exists(tf));   // read only: nothing renamed, nothing removed
        MpKsCopyInfo fi;
        const int ir = mp_keepsafe_inspect(v.c_str(), &fi);
        const int want = variant == 0 ? 33 : 21;   // the valid .mptmp, else the valid .mpbak (the torn or missing temporary)
        CHECKM(ir == MP_KS_OK && fi.copyStars[1] == want, "variant %d: inspect %s stars %d (want %d)", variant, mp_keepsafe_result_name(ir), fi.copyStars[1], want);
        bool teamListed = false;
        for (const MpKsCopy& c : copies())
            if (v == c.name) teamListed = c.teamFiles == 1;
        CHECK(teamListed);
    }
}

// finding 11: the pinned rule as it is: beyond 8 the oldest goes only when a newer verified copy holds the same files, so there can be more than 8
static void testPinnedCount() {
    world("pins-distinct");
    for (int v = 0; v < 12; v++) {
        saveAtomic(gP.teamFile, teamSlot(2, 10 + v));   // every update finds other data
        boot(("3." + std::to_string(v) + ".0").c_str());
    }
    size_t folders = 0;
    for (const MpKsCopy& c : copies())
        if (c.kind == MP_KS_COPY_VERSION) folders++;
    CHECKM(folders == 12, "%zu version copies after 12 updates with changing data (the documented rule keeps every pinned one: more than 8)", folders);
    // (identical data: the oldest pinned beyond 8 goes: PruneAndPins pins-bound)
    world("pins-mixed");
    for (int v = 0; v < 12; v++) {
        if (v < 4) saveAtomic(gP.teamFile, teamSlot(2, 10 + v));   // the first four updates find different data; the next eight, the same
        boot(("3." + std::to_string(v) + ".0").c_str());
    }
    size_t n = 0;
    for (const MpKsCopy& c : copies())
        if (c.kind == MP_KS_COPY_VERSION) n++;
    CHECKM(n >= 9, "%zu copies: the four with other data are all still there", n);
}

// finding 12: copies are ordered by their recorded time; a stray name cannot scramble the order or the numbers
static void testOrdering() {
    world("ordering");
    boot("3.0.0");
    const std::string v = versionCopy();
    writeFile(backups() + "/copy-20990101-4294967295.smgmp", buildCopyFile({{"mp/saves/GameData.bin", teamSlot(2, 11)}}));
    writeFile(backups() + "/before-20990101-4294967295.smgmp", buildCopyFile({{"mp/mp.cfg", B("version = 1\nname = Stray\n")}}));
    writeFile(backups() + "/stray-20990101-9999999999999.smgmp", buildCopyFile({{"mp/mp.cfg", B("version = 1\nname = Stray2\n")}}));
    std::vector<std::string> made;
    for (int i = 0; i < 20; i++) {
        mp_keepsafe_test_set_time(1700000000 + (int64_t)(i + 1) * 86400);
        writeFile(mpFile("mp.cfg"), B(cfgText(("Local" + std::to_string(i)).c_str(), "00112233445566778899aabbccddeeff", i)));
        mp_config_load();
        char b[96];
        CHECK(mp_keepsafe_bring_back_settings(v.c_str(), MP_KS_SET_CONFIG, b, sizeof b) == MP_KS_OK);
        made.push_back(b);
        uint32_t num = 0;
        const std::string tail = std::string(b).substr(std::string(b).find_last_of('-') + 1);
        num = (uint32_t)atol(tail.c_str());
        CHECKM(num > 0 && num < 1000, "number %u of %s: a stray name saturated the counter", num, b);   // (the stray 4294967295 never counts)
    }
    // 21 "before" copies exist (20 of ours and the stray one, whose recorded time is the oldest): the newest 16 stay, by their recorded time
    for (size_t i = 0; i < made.size(); i++) CHECKM(exists(backups() + "/" + made[i]) == (i >= 4), "%s there=%d", made[i].c_str(), (int)exists(backups() + "/" + made[i]));
    CHECK(!exists(backups() + "/before-20990101-4294967295.smgmp"));
    CHECK(exists(backups() + "/copy-20990101-4294967295.smgmp") && exists(backups() + "/stray-20990101-9999999999999.smgmp"));   // (made copies and strangers are never deleted)
    // order is by the recorded time, not by the number in the name: a copy with the LOWEST number but the newest time stays, the oldest by time goes
    mp_keepsafe_test_set_time(1700000000 + 30 * 86400);
    const std::string lowNum = "before-20990102-001.smgmp";
    writeFile(backups() + "/" + lowNum, buildCopyFile({{"mp/mp.cfg", B("version = 1\nname = Newest\n")}}, 1700000000 + 30 * 86400));
    mp_keepsafe_test_set_time(1700000000 + 31 * 86400);
    writeFile(mpFile("mp.cfg"), B(cfgText("LocalX", "00112233445566778899aabbccddeeff", 40)));
    mp_config_load();
    char bx[96];
    CHECK(mp_keepsafe_bring_back_settings(v.c_str(), MP_KS_SET_CONFIG, bx, sizeof bx) == MP_KS_OK);
    CHECKM(exists(backups() + "/" + lowNum), "a copy with a low number but the newest time was pruned (ordered by the number in the name)");
    CHECK(!exists(backups() + "/" + made[4]) && !exists(backups() + "/" + made[5]) && exists(backups() + "/" + made[6]));
}

// finding 13: a stray file of any size in mp/ or online/ is skipped (and said so); "full" only for a real out-of-space result; no Team File here:
// only slot N; the account call guards
static void testStrayFiles() {
    world("stray");
    writeFile(mpFile("mp.cfg.unreadable"), Bytes(70 * 1024, 'x'));
    writeFile(mpFile("bigstray.bin"), Bytes(300 * 1024, 'y'));
    writeFile(onlineDir() + "/online.big", Bytes(80 * 1024, 'z'));
    writeFile(mpFile("huge.bin"), Bytes(1536 * 1024, 'h'));   // (over what one read takes: it used to fail the copy at every start)
    writeFile(onlineDir() + "/online.dat.mpbad", Bytes(10, 'w'));   // (a name the writers use: copied)
    gLog.clear();
    boot("3.0.0");
    CHECK(copies().size() == 1 && exists(stamp()) && mp_keepsafe_notice_take() == MP_KS_NOTICE_NONE && mp_keepsafe_may_migrate());
    const Tree snap = treeOf(backups() + "/" + versionCopy());
    CHECK(!snap.count("mp/mp.cfg.unreadable") && !snap.count("mp/bigstray.bin") && !snap.count("mp/huge.bin") && !snap.count("online/online.big") && snap.count("online/online.dat.mpbad") && snap.count("mp/mp.cfg"));
    CHECK(gLog.find("not copied") != std::string::npos && gLog.find("bigstray.bin") != std::string::npos && gLog.find("mp.cfg.unreadable") != std::string::npos);
    // a real out-of-space result is "full"; any other failure is not
    world("notfull");
    writeFile(std::string(gP.dataDir) + "/mp-backups", B("a file where the folder should be"));
    boot("3.0.0");
    CHECK(mp_keepsafe_notice_take() == MP_KS_NOTICE_NO_COPY && copies().empty());
}
static void testSlotOnly() {
    world("slotonly");
    boot("3.0.0");
    int st[6] = {10, 20, 30, 40, 45, 50};
    const std::string c = copyHolding(makeTeam(st));   // all six files in use
    unlink(gP.teamFile);
    unlink((std::string(gP.teamFile) + ".mpbak").c_str());
    CHECK(mp_keepsafe_bring_back_slot(c.c_str(), 3, 0, nullptr, 0) == MP_KS_OK);
    const Bytes t = readFile(gP.teamFile);
    CHECK(starsOf(t, 3) == 30);
    for (int s : {1, 2, 4, 5, 6}) CHECKM(mp_save_slot_state(t.data(), s) == 0, "slot %d has %d", s, mp_save_slot_state(t.data(), s));
    CHECK(mp_save_validate(t.data(), (uint32_t)t.size(), true, true) == MP_SAVE_OK);
}
static void testAccountGuards() {
    world("acctguard");
    const std::string od = onlineDir() + "/online.dat";
    const std::string foreign = copyHolding(teamSlot(2, 5), true);
    const Tree t0 = dataSet();
    gMayReplace = false;   // a session (or progress in RAM, a save running)
    CHECK(mp_keepsafe_use_account(foreign.c_str(), 1) == MP_KS_BUSY && dataSet() == t0);
    gMayReplace = true;
    // a recovery checkpoint is being written by the save layer's async writer: the call waits for it (it does not replace an account under a running save)
    gSlowFsync = true;
    const Bytes rec = makeRecovery(9, 20);
    CHECK(mp_save_async_submit(gP.recoveryDir, 9, rec.data(), (uint32_t)rec.size(), 3));
    CHECK(mp_keepsafe_use_account(foreign.c_str(), 1) == MP_KS_OK && readFile(od) == makeAccount(500, 'f'));
    CHECK(mp_save_async_idle());
    gSlowFsync = false;
}
struct SpamArgs { std::string od; bool stop; };
static void* storeSpam(void* a) {
    SpamArgs* s = (SpamArgs*)a;
    for (int i = 0; !s->stop && i < 4000; i++) mp_keepsafe_account_store(s->od.c_str(), makeAccount(1000 + (i % 2), 'k').data(), 112);
    return nullptr;
}
static void testMakeCopyAccountLock() {
    world("copylock");
    SpamArgs sa{onlineDir() + "/online.dat", false};
    mp_keepsafe_account_store(sa.od.c_str(), makeAccount(1000, 'k').data(), 112);
    pthread_t th;
    pthread_create(&th, nullptr, storeSpam, &sa);
    int without = 0;
    for (int i = 0; i < 300; i++) {
        char nm[96];
        if (mp_keepsafe_make_copy(0, nm, sizeof nm) != MP_KS_OK) continue;
        if (S_(readFile(backups() + "/" + nm)).find("online/online.dat") == std::string::npos) without++;
        unlink((backups() + "/" + nm).c_str());
    }
    sa.stop = true;
    pthread_join(th, nullptr);
    CHECKM(without == 0, "%d of 300 copies were made without the account while the store ran", without);
    // and deterministically: a copy that starts between the account write's two renames
    const std::string od = onlineDir() + "/online.dat";
    mp_keepsafe_account_store(od.c_str(), makeAccount(2000, 'k').data(), 112);
    gWindowCopyDone = false;
    gWindowArm = 1;
    mp_keepsafe_account_store(od.c_str(), makeAccount(2001, 'k').data(), 112);
    CHECK(gWindowArm.load() == 2);
    if (gWindowArm.load() == 2) pthread_join(gWindowThread, nullptr);
    gWindowArm = 0;
    CHECKM(S_(readFile(backups() + "/" + gWindowName)).find("online/online.dat") != std::string::npos, "a copy that started between the account write's renames has no account");
}

// finding 14: the settings writers put the temporary on the medium before the rename
static void testWritersSync() {
    world("fsync");
    mp_config_load();
    gIoEvents.clear();
    gLogIo = true;
    MpConfig c = *mp_config();
    snprintf(c.name, sizeof c.name, "Synced");
    mp_config_set(&c);
    CHECK(mp_config_save());
    MpFlowStore f;
    mp_flow_store_get(&f);
    f.lastCoop = 6;
    mp_flow_store_set(&f);
    gLogIo = false;
    const std::string cfgTmp = mpFile("mp.cfg.tmp"), flowTmp = mpFile("flow.cfg.mptmp");
    const int fc = ioIndex("fsync " + cfgTmp), rc = ioIndex("rename " + cfgTmp), ff = ioIndex("fsync " + flowTmp), rf = ioIndex("rename " + flowTmp);
    CHECKM(fc >= 0 && rc > fc, "mp.cfg: fsync at %d, rename at %d", fc, rc);
    CHECKM(ff >= 0 && rf > ff, "flow.cfg: fsync at %d, rename at %d", ff, rf);
    // and the writer keepsafe's bring-back uses for mp.cfg
    boot("3.0.0");
    writeFile(mpFile("mp.cfg"), B(cfgText("Local", "00112233445566778899aabbccddeeff", 9)));
    mp_config_load();
    gIoEvents.clear();
    gLogIo = true;
    CHECK(mp_keepsafe_bring_back_settings(versionCopy().c_str(), MP_KS_SET_CONFIG, nullptr, 0) == MP_KS_OK);
    gLogIo = false;
    const int fr = ioIndex("fsync " + cfgTmp), rr = ioIndex("rename " + cfgTmp);
    CHECKM(fr >= 0 && rr > fr, "replace_held: fsync at %d, rename at %d", fr, rr);
}

// ================================================================================================================================
static void testWiringAndQuit() {
    world("wire");
    boot("3.0.0");
    // a call made while another one runs is refused (the quit path sees busy)
    CHECK(!mp_keepsafe_busy());
}

int main() {
    if (!loadGameData()) {
        fprintf(stderr, "keepsafe_test: no valid GameData.bin (set KS_GAMEDATA): SKIPPED\n");
        return 77;
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/dev/shm/ks_%d", (int)getpid());
    gRoot = getenv("KS_TMP") ? getenv("KS_TMP") : tmp;
    rmrf(gRoot);
    mkdirs(gRoot);
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"Boot", testBoot},               {"PruneAndPins", testPruneAndPins}, {"CopyFailures", testCopyFailures}, {"NewerStamp", testNewerStamp},
        {"MakeCopy", testMakeCopy},       {"BringBackSlot", testBringBackSlot}, {"Account", testAccount},       {"Settings", testSettings},
        {"BlocksAndDamage", testBlocksAndDamage}, {"OldDesign", testOldDesignLeftovers}, {"ReadOnlyCalls", testReadOnlyCalls},
        {"SettingsWriters", testSettingsWriters}, {"WiringAndQuit", testWiringAndQuit},
        {"AccountNames", testAccountNames}, {"BeforeCopies", testBeforeCopies}, {"LiveStructure", testLiveStructure}, {"FailedListing", testFailedListing},
        {"ConfigRestore", testConfigRestore}, {"StampParse", testStampParse}, {"AccountReads", testAccountReads}, {"Identity", testIdentity},
        {"CutSave", testCutSave}, {"PinnedCount", testPinnedCount}, {"Ordering", testOrdering}, {"StrayFiles", testStrayFiles}, {"SlotOnly", testSlotOnly},
        {"AccountGuards", testAccountGuards}, {"AccountLock", testMakeCopyAccountLock}, {"WritersSync", testWritersSync},
    };
    const char* only = getenv("KS_ONLY");
    for (const T& t : tests) {
        if (only && !strstr(t.name, only)) continue;
        const int f0 = sFails, c0 = sChecks;
        fprintf(stderr, "== %s\n", t.name);
        t.fn();
        sChecks++;
        sFails += checkSpPrints() ? 1 : 0;   // the single-player folders are untouched, whatever the test did
        fprintf(stderr, "   %s: %d checks, %d failed\n", t.name, sChecks - c0, sFails - f0);
    }
    rmrf(gRoot);
    fprintf(stderr, "keepsafe_test: %d checks, %d failed\n", sChecks, sFails);
    return sFails ? 1 : 0;
}
