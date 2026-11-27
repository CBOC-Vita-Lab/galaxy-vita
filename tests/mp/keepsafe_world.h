// Galaxy 3.0 multiplayer (keepsafe): worlds and Team Files for the host tests (keepsafe_test.cpp, keepsafe_cut_test.cpp). Only ever READS
// the real GameData.bin (KS_GAMEDATA, default /data/saves0/...). A world is <root>/wN_<name>/data with mp/, online/ and a single-player
// saves/ folder whose fingerprint (bytes, inode, mtime) is recorded when the world is made and checked by the tests.
#ifndef KEEPSAFE_WORLD_H
#define KEEPSAFE_WORLD_H
#pragma GCC diagnostic ignored "-Wunused-function"   // (a helper set shared by two test programs)
#include "port/mp_keepsafe.h"

#include "../../platform/src/mp/mp_savecheck.h"
#include "../../platform/src/mp/mp_savefile.h"

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// (mp_ui_flow.cpp, linked into both keepsafe tests, asks the core for a friend code; there is no core here)
extern "C" bool mp_core_slot_friend_code(int, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    return false;
}

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>


typedef std::vector<uint8_t> Bytes;
typedef std::map<std::string, Bytes> Tree;

static int sChecks = 0, sFails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        sChecks++;                                                        \
        if (!(c)) {                                                       \
            sFails++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
        }                                                                 \
    } while (0)
#define CHECKM(c, ...)                                                    \
    do {                                                                  \
        sChecks++;                                                        \
        if (!(c)) {                                                       \
            sFails++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);  \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
        }                                                                 \
    } while (0)

// ---- small file helpers --------------------------------------------------------------------------------------------------------
static std::string gRoot;
static MpKsPaths gP;
static int gWorldN = 0;

static bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static int rmOne(const char* p, const struct stat*, int, struct FTW*) { return remove(p) == 0 ? 0 : 0; }
static void rmrf(const std::string& d) { nftw(d.c_str(), rmOne, 16, FTW_DEPTH | FTW_PHYS); }   // (keepfix: no shell per world: the quick run)
static void mkdirs(const std::string& d) { mp_save_mkdirs(d.c_str()); }
static Bytes readFile(const std::string& p) {
    Bytes b(MP_SAVE_MAX_FILE + 16);
    uint32_t n = 0;
    if (mp_save_read_file(p.c_str(), b.data(), (uint32_t)b.size(), &n) != 0) return Bytes();
    b.resize(n);
    return b;
}
static void writeFile(const std::string& p, const Bytes& b) {
    std::string d = p.substr(0, p.find_last_of('/'));
    mkdirs(d);
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) { perror(p.c_str()); exit(2); }
    if (!b.empty()) fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}
static Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
static std::string S_(const Bytes& b) { return std::string(b.begin(), b.end()); }

static void walk(const std::string& root, const std::string& rel, Tree* out) {
    DIR* d = opendir((root + (rel.empty() ? "" : "/" + rel)).c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    for (const std::string& n : names) {
        const std::string r = rel.empty() ? n : rel + "/" + n;
        if (isDir(root + "/" + r)) walk(root, r, out);
        else (*out)[r] = readFile(root + "/" + r);
    }
}
static Tree treeOf(const std::string& root) { Tree t; walk(root, "", &t); return t; }
static Tree dataSet() {   // "mp/..." and "online/..." (what a snapshot covers)
    Tree t, a = treeOf(gP.mpDir), b = treeOf(gP.onlineDir);
    for (auto& kv : a) t["mp/" + kv.first] = kv.second;
    for (auto& kv : b) t["online/" + kv.first] = kv.second;
    return t;
}
static Tree spTree() { return treeOf(std::string(gP.dataDir) + "/saves"); }
static bool sameTree(const Tree& a, const Tree& b) { return a == b; }

static Bytes gReal;
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Clear Power Star bits of the mario block of `slot` until `target` remain (never adds).
static void reduceStars(Bytes* img, int slot, int target) {
    char n[32];
    snprintf(n, sizeof n, "mario%d", slot);
    uint32_t off, size;
    if (!mp_save_find(img->data(), n, &off, &size)) return;
    uint8_t* block = img->data() + off;
    uint32_t p = 4;
    for (uint32_t c = 0; c < block[1]; c++) {
        const uint32_t sig = rd32(block + p), cs = rd32(block + p + 8);
        if (sig == 0x47414C41u) {
            uint8_t* d = block + p + 12;
            const uint32_t galaxies = rd16(d), attrs = rd16(d + 2), rec = rd16(d + 4);
            uint32_t h = 0;
            for (const char* s = "mPowerStarFlag"; *s; s++) h = (uint32_t)(signed char)*s + h * 31u;
            uint32_t flagOff = 0;
            for (uint32_t a = 0; a < attrs; a++)
                if (rd16(d + 6 + a * 4) == (uint16_t)h) flagOff = rd16(d + 8 + a * 4);
            uint8_t* recs = d + 2 + 4 + attrs * 4;
            int total = 0;
            for (uint32_t g = 0; g < galaxies; g++)
                for (uint8_t f = recs[g * rec + flagOff]; f; f &= f - 1) total++;
            for (uint32_t g = 0; g < galaxies && total > target; g++) {
                uint8_t& f = recs[g * rec + flagOff];
                while (f && total > target) {
                    f &= (uint8_t)(f - 1);
                    total--;
                }
            }
            return;
        }
        p += cs;
    }
}

// A valid Team Files image: slot s has stars[s-1] Power Stars (0..50), made from the real save's slot 2 (50 stars) and slot 1 (blank).
static Bytes makeTeam(const int stars[6], int variant = 0) {
    Bytes img(MP_SAVE_MAX_FILE, 0);
    memcpy(img.data(), gReal.data(), gReal.size());
    for (int s = 1; s <= 6; s++) {
        mp_save_copy_slot(gReal.data(), stars[s - 1] > 0 ? 2 : 1, img.data(), s);
        if (stars[s - 1] > 0 && stars[s - 1] < 50) reduceStars(&img, s, stars[s - 1]);
        if (variant && stars[s - 1] > 0) {   // a different file with the same stars: change a harmless byte in the luigi block
            char n[32];
            snprintf(n, sizeof n, "luigi%d", s);
            uint32_t off, size;
            if (mp_save_find(img.data(), n, &off, &size) && size > 100) img[off + size - 1] ^= (uint8_t)variant;
        }
    }
    mp_save_finalize(img.data(), MP_SAVE_MAX_FILE);
    img.resize(mp_save_disk_length(img.data()));
    return img;
}
static int starsOf(const Bytes& img, int slot) { return mp_save_slot_stars(img.data(), slot, nullptr, nullptr); }

static Bytes makeRecovery(int session, int stars) {
    int st[6] = {0, stars, 0, 0, 0, 0};
    Bytes img = makeTeam(st);
    MpSaveSnapshotView v;
    mp_save_view_slot(img.data(), 2, &v);
    MpSaveRecoveryInfo info;
    memset(&info, 0, sizeof info);
    info.sessionId = (uint32_t)session;
    info.seq = 1;
    info.unixTime = 1700000000;
    info.flags = MP_SAVE_REC_FLAG_ORIGINAL_HOST | MP_SAVE_REC_FLAG_MARIO;
    info.slot = 2;
    info.srcKind = MP_SAVE_SRC_COOP;
    info.srcSlot = 2;
    info.stars = (uint16_t)stars;
    snprintf(info.galaxy, sizeof info.galaxy, "HeavenlyBeachGalaxy");
    Bytes out(MP_SAVE_REC_MAX);
    const uint32_t n = mp_save_recovery_pack(&info, &v, out.data(), (uint32_t)out.size());
    out.resize(n);
    return out;
}

static void put32(Bytes* b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) (*b)[o + i] = (uint8_t)(v >> (8 * i)); }
static uint32_t crc(const Bytes& b, size_t n) { return mp_save_crc32(b.data(), n); }
static Bytes makeAccount(int pid, char tokenFill, int version = 1) {
    Bytes b(112, 0);
    memcpy(b.data(), "SMGO", 4);
    put32(&b, 4, (uint32_t)version);
    put32(&b, 8, (uint32_t)pid);
    for (int i = 0; i < 43; i++) b[12 + i] = (uint8_t)(tokenFill + (i % 5));
    memcpy(&b[12 + 64], "Player", 6);
    memcpy(&b[12 + 64 + 20], "ABCD-EFGH", 9);
    put32(&b, 108, crc(b, 108));
    return b;
}

static std::string cfgText(const char* name, const char* key, int extra = 0) {
    char b[800];
    snprintf(b, sizeof b,
             "# Super Mario Galaxy Vita: multiplayer settings (written by the game)\nversion = 1\nname = %s\nlook = 1\nconnection = 1\nmode = 0\n"
             "max_players = 4\ninteraction = 1\nstun = 1\nupnp_permanent = 40123\nconsole_key = %s\nrules_hide = 0\nrules_seen = 1,2,0,0\n"
             "future_option = %d\n",
             name, key, extra);
    return b;
}

// ---- worlds ------------------------------------------------------------------------------------------------------------------------
struct World {
    int stars[6] = {0, 50, 10, 0, 0, 0};
    bool team = true, recovery = true, account = true, cfg = true, flow = true, coop = true, conntest = true, backups = true, moreRecovery = false;
    const char* keyHex = "00112233445566778899aabbccddeeff";
    int variant = 0;
};

// keepfix 2: the single-player save folder of every world, as it was made: bytes, size, inode and modification time of each file (a
// write that put the same bytes back still changes the time). Checked after every test (main) for every world still on disk.
static std::map<std::string, std::string> gSpPrint;
static std::string spPrint(const std::string& savesDir) {
    std::string out;
    Tree t = treeOf(savesDir);
    for (auto& kv : t) {
        struct stat st;
        stat((savesDir + "/" + kv.first).c_str(), &st);
        char b[200];
        snprintf(b, sizeof b, "%s %zu %llu %lld.%09ld %08x\n", kv.first.c_str(), kv.second.size(), (unsigned long long)st.st_ino, (long long)st.st_mtim.tv_sec,
                 st.st_mtim.tv_nsec, (unsigned)mp_save_crc32(kv.second.data(), kv.second.size()));
        out += b;
    }
    return out;
}
static int checkSpPrints() {
    int bad = 0;
    for (auto& kv : gSpPrint) {
        if (!isDir(kv.first)) continue;   // (a world removed by the random sequences)
        if (spPrint(kv.first) != kv.second) {
            bad++;
            fprintf(stderr, "FAIL single-player folder changed: %s\n", kv.first.c_str());
        }
    }
    return bad;
}

static void newWorld(const char* name) {
    gWorldN++;
    const std::string root = gRoot + "/w" + std::to_string(gWorldN) + "_" + name;
    rmrf(root);
    mkdirs(root + "/data");
    memset(&gP, 0, sizeof gP);
    snprintf(gP.dataDir, sizeof gP.dataDir, "%s/data", root.c_str());
    snprintf(gP.mpDir, sizeof gP.mpDir, "%s/data/mp", root.c_str());
    snprintf(gP.onlineDir, sizeof gP.onlineDir, "%s/data/online", root.c_str());
    snprintf(gP.teamFile, sizeof gP.teamFile, "%s/data/mp/saves/title/00010000/524d4750/data/GameData.bin", root.c_str());
    snprintf(gP.recoveryDir, sizeof gP.recoveryDir, "%s/data/mp/recovery", root.c_str());
    mp_keepsafe_set_paths(&gP);
    mp_keepsafe_test_reset();
    mp_keepsafe_test_set_free_bytes(-1);
    mp_keepsafe_test_set_time(1700000000);
    // the single-player save folder and a sentinel: never written by anything of ours
    writeFile(std::string(gP.dataDir) + "/saves/title/00010000/524d4750/data/GameData.bin", gReal);
    writeFile(std::string(gP.dataDir) + "/saves/title/00010000/524d4750/data/banner.bin", B("banner-bytes"));
    writeFile(std::string(gP.dataDir) + "/env.txt", B("GXV_X=1\n"));
    gSpPrint[std::string(gP.dataDir) + "/saves"] = spPrint(std::string(gP.dataDir) + "/saves");
}

static void fillWorld(const World& w) {
    const std::string mp = gP.mpDir;
    if (w.cfg) {
        writeFile(mp + "/mp.cfg", B(cfgText("Alex", w.keyHex, w.variant)));
        writeFile(mp + "/mp.cfg.bak", B(cfgText("Old", w.keyHex)));
    }
    if (w.flow) writeFile(mp + "/flow.cfg", B("# menus\nchar_picked = 1\nreach = 2\nlast_coop = 2\nplayed = Luigi\nplayed = Peach\n"));
    if (w.coop) writeFile(mp + "/coop.cfg", B("# who\nfile = 2\ngalaxy = 4\nplayer = 0 Alex\nplayer = 1 Luigi\n"));
    if (w.conntest) writeFile(mp + "/conntest.txt", B("connection test (last run)\nwifi = wifi_good (80%)\nverdict = all_good\n"));
    if (w.team) {
        const Bytes t = makeTeam(w.stars, w.variant);
        writeFile(gP.teamFile, t);
        writeFile(std::string(gP.teamFile) + ".mpbak", makeTeam(w.stars, 7));
    }
    if (w.recovery) {
        writeFile(mp + "/recovery/session-00000003.bin", makeRecovery(3, 20));
        if (w.moreRecovery) {
            writeFile(mp + "/recovery/session-00000004.bin", makeRecovery(4, 21));
            writeFile(mp + "/recovery/session-00000005.bin", makeRecovery(5, 22));
        }
    }
    if (w.backups) writeFile(mp + "/backups/GameData-20261001-120000-00.bin", gReal);
    if (w.account) writeFile(std::string(gP.onlineDir) + "/online.dat", makeAccount(77, 'a'));
}

static void bootAs(const char* version) {
    mp_keepsafe_test_reset();
    mp_keepsafe_boot(version, "t");
}
static void saveAtomic(const std::string& path, const Bytes& b) {
    mkdirs(path.substr(0, path.find_last_of('/')));
    mp_save_write_atomic(path.c_str(), b.data(), (uint32_t)b.size());
}
// Team File image where slot `slot` has `stars` Power Stars, cleared from the first galaxies (fromEnd false) or from the last ones
static void clearStarsFromEnd(Bytes* img, int slot, int target) {
    char n[32];
    snprintf(n, sizeof n, "mario%d", slot);
    uint32_t off, size;
    if (!mp_save_find(img->data(), n, &off, &size)) return;
    uint8_t* block = img->data() + off;
    uint32_t p = 4;
    for (uint32_t c = 0; c < block[1]; c++) {
        const uint32_t sig = rd32(block + p), cs = rd32(block + p + 8);
        if (sig == 0x47414C41u) {
            uint8_t* d = block + p + 12;
            const uint32_t galaxies = rd16(d), attrs = rd16(d + 2), rec = rd16(d + 4);
            uint32_t h = 0;
            for (const char* s = "mPowerStarFlag"; *s; s++) h = (uint32_t)(signed char)*s + h * 31u;
            uint32_t flagOff = 0;
            for (uint32_t a = 0; a < attrs; a++)
                if (rd16(d + 6 + a * 4) == (uint16_t)h) flagOff = rd16(d + 8 + a * 4);
            uint8_t* recs = d + 2 + 4 + attrs * 4;
            int total = 0;
            for (uint32_t g = 0; g < galaxies; g++)
                for (uint8_t f = recs[g * rec + flagOff]; f; f &= f - 1) total++;
            for (int g = (int)galaxies - 1; g >= 0 && total > target; g--) {
                uint8_t& f = recs[g * rec + flagOff];
                while (f && total > target) { f &= (uint8_t)(f - 1); total--; }
            }
            return;
        }
        p += cs;
    }
}
// (every other slot never created: the real save's file 3)
static Bytes teamSlot(int slot, int stars, bool fromEnd = false, int variant = 0) {
    int st[6] = {0, 0, 0, 0, 0, 0};
    st[slot - 1] = 50;
    Bytes img = makeTeam(st, variant);
    img.resize(MP_SAVE_MAX_FILE, 0);
    for (int s = 1; s <= 6; s++)
        if (s != slot) mp_save_copy_slot(gReal.data(), 3, img.data(), s);
    if (fromEnd) clearStarsFromEnd(&img, slot, stars);
    else reduceStars(&img, slot, stars);
    mp_save_finalize(img.data(), MP_SAVE_MAX_FILE);
    img.resize(mp_save_disk_length(img.data()));
    return img;
}
// A copy file by hand (the format of docs/MP-DATA-FORMATS.md): entries (canonical name, bytes), header 64 bytes, CRC-32 of everything before it.
static Bytes buildCopyFile(const std::vector<std::pair<std::string, Bytes> >& items, uint32_t created = 1700000000) {
    Bytes b = B("SMGMPEXP");
    auto p16 = [&](uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); };
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); };
    p16(1);
    p16(64);
    p32((uint32_t)items.size());
    p32(created);
    p32(0);
    p32(0);
    p32(0);
    b.resize(64, 0);
    for (auto& it : items) {
        p16((uint32_t)it.first.size());
        p16(0);
        p32((uint32_t)it.second.size());
        p32(mp_save_crc32(it.second.data(), it.second.size()));
        b.insert(b.end(), it.first.begin(), it.first.end());
        b.insert(b.end(), it.second.begin(), it.second.end());
    }
    p32(mp_save_crc32(b.data(), b.size()));
    return b;
}
static bool loadGameData() {
    const char* real = getenv("KS_GAMEDATA") ? getenv("KS_GAMEDATA") : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    gReal = readFile(real);
    return gReal.size() >= 0x1000 && mp_save_validate(gReal.data(), (uint32_t)gReal.size(), true, true) == MP_SAVE_OK;
}
#endif  // KEEPSAFE_WORLD_H
