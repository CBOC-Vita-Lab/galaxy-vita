// DVD file system over the cooked game data directory.
//
// Entry numbers come from the disc's own fst.bin, so they match the original
// game exactly; file sizes come from the cooked (decompressed, byte-swapped)
// files on the host.  Reads run on a worker thread and complete through the
// DVD interrupt, like the real drive.
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__vita__)
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <unordered_map>
#endif
#include <algorithm>
#include <condition_variable>
#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"
#include "port/diagnostics.h"
extern "C" void port_gpu_memory_written(void* addr, uint32_t size);
#include "port/qemu_hooks.h"
#include "port/vclock.h"
#include "../../vgx/src/vgx_common.h"
#include "revolution/dvd.h"
#include "revolution/os.h"
#include "revolution/sc.h"

enum {
    kStateEnd = 0,
    kStateBusy = 1,
    kStateWaiting = 2,
    kStateCanceled = 10,
    kStateFatal = -1,
};

struct FstEntry {
    bool isDir;
    std::string name;
    u32 parent;  // dirs: parent entry
    u32 next;    // dirs: index after the last child
    std::string hostPath;
    s64 size = -1;
    bool cached = false;   // size came from the sizes cache (checked against the file at its first open)
    bool checked = false;
};

static std::vector<FstEntry> sFst;
static std::string sDataRoot;
static u32 sCurrentDir = 0;

// GXV_DVDERRORS=1 opts into basic-log file errors (read once; default off). Error paths only:
// no repair, altered DVD result or successful-read observer. Each failure reports cumulative attempts
// 1, 8 and 64, with a process-wide 24-line ceiling even when the failing path keeps changing.
enum class DvdError { Stat, Open, Read };
static void reportDvdError(DvdError operation, u32 entry, int result, int error) {
    const int savedErrno = errno;
    static const bool enabled = vgx::EnvInt("GXV_DVDERRORS", 0) != 0;
    if (!enabled) { errno = savedErrno; return; }
    struct Record { DvdError operation{}; u32 entry = 0, count = 0; int error = 0; };
    constexpr unsigned maxReports = 24;
    static Record records[maxReports];
    static unsigned reports = 0;
    static std::mutex lock;
    {
        // The guest can fail stat while the host DVD worker fails open/read. Do not take the DVD queue lock.
        std::lock_guard<std::mutex> guard(lock);
        if (reports < maxReports) {
            Record* record = nullptr;
            for (auto& r : records) {
                if (!r.count || (r.entry == entry && r.operation == operation && r.error == error)) { record = &r; break; }
            }
            u32 repeat = 1;
            if (record) {
                if (!record->count) *record = {operation, entry, 0, error};
                if (record->count != UINT32_MAX) ++record->count;
                repeat = record->count;
            }
            if (repeat == 1 || repeat == 8 || repeat == 64) {
                const char* name = operation == DvdError::Stat ? "stat" : operation == DvdError::Open ? "open" : "read";
                ++reports;
                port_log_summary("dvd-error: operation=%s entry=%u result=%d errno=%d repeat=%u path=\"%s\"%s", name, entry, result,
                                 error, repeat, sFst[entry].hostPath.c_str(), reports == maxReports ? "; further errors suppressed" : "");
            }
        }
    }
    errno = savedErrno;
}

// Opt-in startup guard, not a replacement for the game's audio preparation. The known
// title basename is shared by the supported data layout; regional sound-table mapping
// remains a validation prerequisite before enabling this by default.
static constexpr const char* kBootTitle = "AudioRes/Stream/SMG_title_strm.ast";
static u32 sBootTitleEntry = ~0u, sBootTitleBytes = 0;
namespace AudParams { extern const int streamBlockSize, numStreamChannels; }
extern "C" int port_boot_data_error(const char*) __attribute__((weak));

extern "C" int port_boot_data_enabled(void) {
    // GXV_BOOTDATA=1 checks only critical title audio before the game starts. Read once; default off.
    static const bool enabled = vgx::EnvInt("GXV_BOOTDATA", 0) != 0;
    return enabled;
}

extern "C" int port_boot_data_report(const char* message) {
    if (!port_boot_data_enabled()) return 0;
    port_log_summary("boot-data: %s", message);
    return port_boot_data_error && port_boot_data_error(message);
}

extern "C" [[noreturn]] void port_boot_data_park(void) {
    // Only the first DVDInit boot guest calls this, after visible error publication and
    // after validation releases its file. The host/render threads remain independent.
    // An unexpected resume must not let the game continue with invalid title data.
    for (;;) OSSuspendThread(OSGetCurrentThread());
}

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// The disc's file table (sys/fst.bin under `root`): 12-byte entries, the
// root's holding their count, then the names.  Empty when missing or cut.
static std::vector<u8> readFst(const std::string& root) {
    std::vector<u8> data;
    FILE* f = fopen((root + "/sys/fst.bin").c_str(), "rb");
    if (!f) {
        return data;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len >= 12) {
        data.resize((size_t)len);
        if (fread(data.data(), 1, data.size(), f) != data.size() || (uint64_t)be32(&data[8]) * 12 >= data.size()) {
            data.clear();
        }
    }
    fclose(f);
    return data;
}

// Whether the disc's root directory has a folder `name`.
static bool fstHasRootDir(const std::vector<u8>& fst, const char* name) {
    u32 count = be32(&fst[8]);
    const char* strings = (const char*)&fst[count * 12];
    size_t stringsSize = fst.size() - count * 12;
    size_t len = strlen(name);
    for (u32 i = 1; i < count;) {
        const u8* e = &fst[i * 12];
        bool isDir = e[0] != 0;
        u32 nameOff = be32(e) & 0x00FFFFFF;
        if (isDir && nameOff + len < stringsSize && strncasecmp(strings + nameOff, name, len + 1) == 0) {
            return true;
        }
        u32 next = be32(e + 8);
        i = (isDir && next > i) ? next : i + 1;  // past a folder's contents
    }
    return false;
}

// Each region's disc keeps its texts and translated layouts in language
// folders named after it (EuEnglish, UsEnglish...), and the game picks the
// folder from the disc's game code and the console's language
// (Language.cpp).  The port tells the disc by those folders in its file
// table, which every conversion has, and runs the game in the language of
// the `language` setting, or the disc's first where it has no such texts.
// (The European disc also has an EuDutch folder: its texts are the English
// ones.)
static const PortDisc kDiscs[] = {
    {"RMGP01",
     "Europe",
     {{"english", "EuEnglish", SC_LANG_ENGLISH},
      {"french", "EuFrench", SC_LANG_FRENCH},
      {"german", "EuGerman", SC_LANG_GERMAN},
      {"spanish", "EuSpanish", SC_LANG_SPANISH},
      {"italian", "EuItalian", SC_LANG_ITALIAN}},
     5},
    {"RMGE01",
     "North America",
     {{"english", "UsEnglish", SC_LANG_ENGLISH}, {"french", "UsFrench", SC_LANG_FRENCH}, {"spanish", "UsSpanish", SC_LANG_SPANISH}},
     3},
    // Not tried: no such disc at hand.
    {"RMGJ01", "Japan", {{"japanese", "JpJapanese", SC_LANG_JAPANESE}}, 1},
    {"RMGK01", "Korea", {{"korean", "KrKorean", SC_LANG_KOREAN}}, 1},
};
static const PortDisc* sDisc = nullptr;
// The running disc's languages whose texts are in the copy (the first is the
// disc's own, which port_dvd_identify checked), the one asked for
// (port_language_set) and the one the game started in.
static const PortLanguage* sLanguages[PORT_DISC_MAX_LANGUAGES];
static int sLanguageCount = 0;
static char sLanguageWanted[16];
static const PortLanguage* sLanguageRunning = nullptr;

// Whether the copy in `root` has the texts of a language of its disc.
static bool hasTexts(const std::string& root, const std::vector<u8>& fst, const PortLanguage& language) {
    std::string texts = root + "/files/" + language.folder + "/MessageData/Message.arc";
    struct stat st;
    return fstHasRootDir(fst, language.folder) && stat(texts.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

extern "C" const PortDisc* port_dvd_identify(const char* root) {
    PortHostAllocScope scope;
    std::vector<u8> fst = readFst(root);
    if (fst.empty()) {
        return nullptr;
    }
    for (const PortDisc& disc : kDiscs) {
        // The disc has the folder of its first language, and the copy the
        // texts in it.
        if (hasTexts(root, fst, disc.languages[0])) {
            return &disc;
        }
    }
    return nullptr;
}

extern "C" const PortDisc* port_dvd_disc(void) { return sDisc; }

extern "C" void port_dvd_set_root(const char* root) {
    PortHostAllocScope scope;
    sDataRoot = root;
    sDisc = port_dvd_identify(root);
    sLanguageCount = 0;
    if (sDisc) {
        std::vector<u8> fst = readFst(root);
        for (int i = 0; i < sDisc->languageCount; i++) {
            if (i == 0 || hasTexts(root, fst, sDisc->languages[i])) {
                sLanguages[sLanguageCount++] = &sDisc->languages[i];
            }
        }
    }
    // Debug: PETARI_LANGUAGE=<name> instead of the setting.
    if (const char* name = getenv("PETARI_LANGUAGE")) {
        port_language_set(name);
    }
}

extern "C" int port_language_count(void) { return sLanguageCount; }

extern "C" const char* port_language_name(int index) { return index >= 0 && index < sLanguageCount ? sLanguages[index]->name : ""; }

extern "C" void port_language_set(const char* name) {
    size_t n = 0;
    for (; name && name[n] && n + 1 < sizeof(sLanguageWanted); n++) {
        sLanguageWanted[n] = (char)tolower((unsigned char)name[n]);
    }
    sLanguageWanted[n] = 0;
}

extern "C" int port_language_wanted(void) {
    for (int i = 0; i < sLanguageCount; i++) {
        if (strcmp(sLanguageWanted, sLanguages[i]->name) == 0) {
            return i;
        }
    }
    return 0;
}

extern "C" int port_language_running(void) {
    for (int i = 0; i < sLanguageCount; i++) {
        if (sLanguages[i] == sLanguageRunning) {
            return i;
        }
    }
    return -1;
}

extern "C" int port_language_start(void) {
    if (sLanguageCount == 0) {
        return SC_LANG_ENGLISH;
    }
    const PortLanguage* language = sLanguages[port_language_wanted()];
    if (language != sLanguageRunning) {
        sLanguageRunning = language;
        port_log("language: %s (texts from %s)", language->name, language->folder);
    }
    return language->code;
}

extern "C" const char* port_dvd_root(void) { return sDataRoot.c_str(); }

#if defined(__vita__)
// ---- Big directories are kept as bucket folders on the memory card -------------------------------------------------------------------
// Opening a file on the memory card looks its name up in the directory by reading it from the start (the 17-18 Oct console logs:
// open() in ObjectData, 1844 archives, takes 75 ms on average, 176 ms at worst; in a directory of 30-90 files 2.5-6.5 ms, and 1.1 ms
// when the same file was opened a moment before).  A stage load opens a few hundred archives, so a cold start spent 9-18 s of its 17 on
// that, with the CPU idle.  A directory with more than kBucketMinFiles files (ObjectData, StageData, LayoutData, AudioRes/Stream) is
// therefore split in folders _bk00, _bk01... of about kBucketFiles files each; the folder of a file is a hash of its lower-case name,
// so it follows from the disc's file table alone.  The first start after the update moves the files (one rename each, a few minutes;
// a start that finds files in the old place moves what is left), and a file found in either place is used.  PETARI_BUCKETS=0 keeps
// the flat layout (a card that is shared with another build).
constexpr u32 kBucketMinFiles = 64;
constexpr u32 kBucketFiles = 32;
static std::vector<u32> sBucketCount;  // per FST entry: the number of bucket folders of a directory, 0 = not bucketed

static u32 bucketOf(const std::string& name, u32 buckets) {
    u32 h = 2166136261u;
    for (char c : name) {
        h = (h ^ (u32)(unsigned char)tolower((unsigned char)c)) * 16777619u;
    }
    return h % buckets;
}

static std::string bucketDirName(u32 b) {
    char buf[16];
    snprintf(buf, sizeof buf, "_bk%02u", b);
    return buf;
}

static std::string hostDirOf(const std::vector<std::string>& dirPath, u32 d) {
    return sDataRoot + "/files" + (dirPath[d].empty() ? std::string() : "/" + dirPath[d]);
}

// Decides which directories are bucketed and points the files in them at their bucket folder.
static void assignBuckets(const std::vector<std::string>& dirPath) {
    sBucketCount.assign(sFst.size(), 0);
    const char* off = getenv("PETARI_BUCKETS");
    if (off && off[0] == '0') {
        port_log("dvd: PETARI_BUCKETS=0, the big directories stay as they are");
        return;
    }
    u32 dirs = 0, files = 0;
    for (u32 d = 0; d < sFst.size(); ++d) {
        if (!sFst[d].isDir) {
            continue;
        }
        u32 count = 0;
        for (u32 i = d + 1; i < sFst[d].next; i = sFst[i].isDir ? sFst[i].next : i + 1) {
            count += sFst[i].isDir ? 0 : 1;
        }
        if (count <= kBucketMinFiles) {
            continue;
        }
        const u32 n = (count + kBucketFiles - 1) / kBucketFiles;
        sBucketCount[d] = n;
        ++dirs;
        const std::string host = hostDirOf(dirPath, d);
        for (u32 i = d + 1; i < sFst[d].next; i = sFst[i].isDir ? sFst[i].next : i + 1) {
            if (!sFst[i].isDir) {
                sFst[i].hostPath = host + "/" + bucketDirName(bucketOf(sFst[i].name, n)) + "/" + sFst[i].name;
                ++files;
            }
        }
    }
    port_log("dvd: %u big directories in bucket folders of about %u files (%u files)", dirs, kBucketFiles, files);
}

// What the folder `path` holds: regular files by lower-case name, with their sizes.  False when it cannot be opened.
static bool listDirectory(const std::string& path, std::unordered_map<std::string, s64>& out, std::vector<std::string>* realNames = nullptr) {
    const SceUID fd = sceIoDopen(path.c_str());
    if (fd < 0) {
        return false;
    }
    SceIoDirent de;
    for (;;) {
        memset(&de, 0, sizeof(de));
        if (sceIoDread(fd, &de) <= 0) {
            break;
        }
        if (SCE_S_ISREG(de.d_stat.st_mode)) {
            std::string key(de.d_name);
            for (char& c : key) {
                c = (char)tolower((unsigned char)c);
            }
            out[key] = (s64)de.d_stat.st_size;
            if (realNames) {
                realNames->push_back(de.d_name);
            }
        }
    }
    sceIoDclose(fd);
    return true;
}

extern "C" void port_boot_progress(const char* what, int done, int total) __attribute__((weak));

// Moves the files that are still in the old place (directly in a bucketed directory) into their bucket folder.
static u32 moveToBuckets(const std::vector<std::string>& dirPath) {
    struct Move {
        std::string from, to;
        size_t dirLen;  // the destination folder is to[0, dirLen)
    };
    std::vector<Move> moves;
    std::vector<std::string> makeDirs;
    const int64_t t0 = port_host_time_ns();
    for (u32 d = 0; d < sFst.size(); ++d) {
        if (!sBucketCount[d]) {
            continue;
        }
        const std::string host = hostDirOf(dirPath, d);
        std::unordered_map<std::string, s64> flat;
        std::vector<std::string> real;
        if (!listDirectory(host, flat, &real)) {
            continue;
        }
        std::unordered_map<std::string, u32> kids;
        for (u32 i = d + 1; i < sFst[d].next; i = sFst[i].isDir ? sFst[i].next : i + 1) {
            if (!sFst[i].isDir) {
                std::string key = sFst[i].name;
                for (char& c : key) {
                    c = (char)tolower((unsigned char)c);
                }
                kids[key] = i;
            }
        }
        size_t before = moves.size();
        for (const std::string& name : real) {
            std::string key = name;
            for (char& c : key) {
                c = (char)tolower((unsigned char)c);
            }
            const auto it = kids.find(key);
            if (it == kids.end()) {
                continue;
            }
            const std::string dest = host + "/" + bucketDirName(bucketOf(sFst[it->second].name, sBucketCount[d]));
            moves.push_back({host + "/" + name, dest + "/" + name, dest.size()});
        }
        if (moves.size() > before) {
            for (u32 b = 0; b < sBucketCount[d]; ++b) {
                makeDirs.push_back(host + "/" + bucketDirName(b));
            }
        }
    }
    if (moves.empty()) {
        return 0;
    }
    // One folder after the other: the card looks the folder's name up in the big directory (the new folders are at its end), and the
    // kernel keeps the last few names it has resolved, so the 32 files of a folder cost one such lookup between them.
    std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.to.compare(0, a.dirLen, b.to, 0, b.dirLen) < 0; });
    port_log("dvd: %u files are not in their bucket folder yet (looking took %.0f ms): moving them", (unsigned)moves.size(), (port_host_time_ns() - t0) / 1e6);
    for (const std::string& dir : makeDirs) {
        sceIoMkdir(dir.c_str(), 0777);  // (it may exist from an earlier, interrupted start)
    }
    const int64_t t1 = port_host_time_ns();
    int64_t lastShown = 0, lastTick = 0;
    u32 done = 0, failed = 0;
    for (const Move& m : moves) {
        const int r = sceIoRename(m.from.c_str(), m.to.c_str());
        if (r < 0) {
            if (++failed <= 5) {
                port_log("dvd: could not move %s: 0x%08x", m.from.c_str(), (unsigned)r);
            }
            if (failed >= 20 && done == 0) {
                port_log("dvd: giving up the move: nothing could be moved");
                break;
            }
        }
        ++done;
        const int64_t now = port_host_time_ns();
        if (now - lastTick > 1000000000LL) {
            lastTick = now;
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);  // (a few minutes without input: the console must not dim or sleep)
        }
        if (now - lastShown > 100000000LL && port_boot_progress) {
            lastShown = now;
            port_boot_progress("Organising the game's files", (int)done, (int)moves.size());
        }
    }
    if (port_boot_progress) {
        port_boot_progress("Organising the game's files", 0, 0);
    }
    const double ms = (port_host_time_ns() - t1) / 1e6;
    port_log("dvd: moved %u files into bucket folders in %.1f s (%.1f ms each), %u failed", done - failed, ms / 1000.0, done ? ms / done : 0.0, failed);
    return (u32)moves.size();
}

// The sizes the listing below finds are kept in <root>/sizes.cache: the listing costs 4-6 s at every cold start (the card needs about
// 2.5 ms for a directory entry) and the game cannot begin before it is done, while the sizes of a game folder that nobody touches do
// not change.  The file is written after a listing that found every file where it should be, and used at the next start when it was
// made for the same file table and bucket layout and no file had to be moved at this start.  Whatever changes files has to delete it
// (push.sh does); as a second line the first open of a file compares the real size with the cached one, and a difference deletes the
// cache for the next start.  PETARI_SIZECACHE=0 turns it off.
struct SizeCacheHead {
    char magic[8];
    u32 signature, files, entries, pad;
};
struct SizeCacheRec {
    u32 index, pad;
    s64 size;
};

static std::string sizeCachePath() { return sDataRoot + "/sizes.cache"; }

static bool sizeCacheEnabled() {
    const char* e = getenv("PETARI_SIZECACHE");
    return !e || e[0] != '0';
}

// The file table with the place each file has on the card (bucket folders included).
static u32 sizeCacheSignature() {
    u32 h = 2166136261u;
    auto mix = [&h](unsigned char c) { h = (h ^ c) * 16777619u; };
    for (const FstEntry& fe : sFst) {
        mix(fe.isDir ? 1 : 0);
        for (char c : fe.name) {
            mix((unsigned char)c);
        }
        mix(0);
        for (char c : fe.hostPath) {
            mix((unsigned char)c);
        }
        mix(0);
    }
    return h;
}

static bool loadSizeCache() {
    if (!sizeCacheEnabled()) {
        return false;
    }
    const int64_t t0 = port_host_time_ns();
    FILE* f = fopen(sizeCachePath().c_str(), "rb");
    if (!f) {
        return false;
    }
    u32 files = 0;
    for (const FstEntry& fe : sFst) {
        files += fe.isDir ? 0 : 1;
    }
    SizeCacheHead h;
    std::vector<SizeCacheRec> recs;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && memcmp(h.magic, "SMGSIZE1", 8) == 0 && h.entries == sFst.size() && h.files == files &&
              h.signature == sizeCacheSignature();
    if (ok) {
        recs.resize(h.files);
        ok = fread(recs.data(), sizeof(SizeCacheRec), h.files, f) == h.files;
    }
    fclose(f);
    for (size_t i = 0; ok && i < recs.size(); ++i) {
        ok = recs[i].index < sFst.size() && !sFst[recs[i].index].isDir && recs[i].size >= -1;
    }
    if (!ok) {
        port_log("dvd: the sizes cache does not fit this game folder (or is damaged): listing the folders instead");
        remove(sizeCachePath().c_str());
        return false;
    }
    for (const SizeCacheRec& r : recs) {
        if (r.size >= 0) {
            sFst[r.index].size = r.size;
            sFst[r.index].cached = true;
        }
    }
    port_log("dvd: sizes of %u files from the sizes cache in %.0f ms", (unsigned)recs.size(), (port_host_time_ns() - t0) / 1e6);
    return true;
}

static void saveSizeCache() {
    if (!sizeCacheEnabled()) {
        return;
    }
    std::vector<SizeCacheRec> recs;
    for (u32 i = 0; i < sFst.size(); ++i) {
        if (sFst[i].isDir) {
            continue;
        }
        recs.push_back({i, 0, sFst[i].size});  // (-1: a file the listing did not find; entrySize() asks for it when the game does)
    }
    SizeCacheHead h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "SMGSIZE1", 8);
    h.signature = sizeCacheSignature();
    h.files = (u32)recs.size();
    h.entries = (u32)sFst.size();
    const std::string tmp = sizeCachePath() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        return;
    }
    const bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(recs.data(), sizeof(SizeCacheRec), recs.size(), f) == recs.size();
    const bool closed = fclose(f) == 0;
    if (ok && closed && rename(tmp.c_str(), sizeCachePath().c_str()) == 0) {
        port_log("dvd: sizes cache written (%u files)", (unsigned)recs.size());
    } else {
        remove(tmp.c_str());
    }
}

// The size of a file is needed the first time the game opens it (DVDFastOpen), and a stat() of one file on the memory card takes 45-160 ms
// when its directory is big (ObjectData holds 1844 archives): FAT looks the name up in the directory each time.  The file loader thread
// does that three times a second while a stage loads, holding the (single) guest CPU: the music cannot be served for those 100 ms (97 such
// stretches in the first minute of the 17 Oct console log, 9.4 s in all).  One listing per directory returns the size of every file in it in
// the same pass (the size is in the directory entry), so the sizes are all known before the game starts.  (A bucketed directory is listed
// folder by folder; a file that is still in the old place is used from there.)
static void presetSizesFromListings(const std::vector<std::string>& dirPath) {
    const int64_t t0 = port_host_time_ns();
    u32 preset = 0, dirs = 0, missing = 0, strays = 0;
    for (u32 d = 0; d < sFst.size(); ++d) {
        if (!sFst[d].isDir) {
            continue;
        }
        bool hasFile = false;
        for (u32 i = d + 1; i < sFst[d].next; i = sFst[i].isDir ? sFst[i].next : i + 1) {
            if (!sFst[i].isDir) {
                hasFile = true;
                break;
            }
        }
        if (!hasFile) {
            continue;
        }
        const std::string host = hostDirOf(dirPath, d);
        std::unordered_map<std::string, s64> sizes;
        const bool listed = listDirectory(host, sizes);
        const u32 n = sBucketCount[d];
        std::vector<std::unordered_map<std::string, s64>> inBucket(n);
        for (u32 b = 0; b < n; ++b) {
            listDirectory(host + "/" + bucketDirName(b), inBucket[b]);
            ++dirs;
        }
        if (!listed && !n) {
            continue;
        }
        ++dirs;
        for (u32 i = d + 1; i < sFst[d].next; i = sFst[i].isDir ? sFst[i].next : i + 1) {
            FstEntry& fe = sFst[i];
            if (fe.isDir) {
                continue;
            }
            std::string key(fe.name);
            for (char& c : key) {
                c = (char)tolower((unsigned char)c);
            }
            auto it = sizes.end();
            if (n) {
                auto& bucket = inBucket[bucketOf(fe.name, n)];
                it = bucket.find(key);
                if (it == bucket.end()) {
                    it = sizes.find(key);
                    if (it != sizes.end()) {
                        fe.hostPath = host + "/" + fe.name;  // (not moved: the old place)
                        ++strays;
                    }
                } else {
                    fe.size = it->second;
                    ++preset;
                    if (fe.size == 0) {
                        port_log("dvd: empty file %s (as on the disc)", fe.hostPath.c_str());
                    }
                    continue;
                }
            } else {
                it = sizes.find(key);
            }
            if (it == sizes.end()) {
                ++missing;  // (entrySize() will stat it when the game asks, and say it is missing)
                continue;
            }
            fe.size = it->second;
            ++preset;
            if (fe.size == 0) {
                // (the disc itself has one empty file, AudioRes/Waves/B109take_0.aw: the original game opens it and reads nothing)
                port_log("dvd: empty file %s (as on the disc)", fe.hostPath.c_str());
            }
        }
    }
    port_log("dvd: sizes of %u files from %u directory listings in %.0f ms (%u not listed, %u not in a bucket folder)", preset, dirs,
             (port_host_time_ns() - t0) / 1e6, missing, strays);
    if (!strays) {
        saveSizeCache();
    }
}
#endif

static bool loadFst() {
    PortHostAllocScope scope;
    std::vector<u8> data = readFst(sDataRoot);
    if (data.empty()) {
        port_log("dvd: cannot read %s/sys/fst.bin", sDataRoot.c_str());
        return false;
    }

    u32 count = be32(&data[8]);
    sBootTitleEntry = ~0u;
    sBootTitleBytes = 0;
    const u8* strings = &data[count * 12];
    sFst.resize(count);
    // Directory path of each directory entry, built as we walk.
    std::vector<std::string> dirPath(count);
    dirPath[0] = "";
    std::vector<u32> stack;  // (dir index) while walking
    stack.push_back(0);
    sFst[0].isDir = true;
    sFst[0].parent = 0;
    sFst[0].next = count;
    for (u32 i = 1; i < count; i++) {
        while (stack.size() > 1 && i >= sFst[stack.back()].next) {
            stack.pop_back();
        }
        const u8* e = &data[i * 12];
        FstEntry& fe = sFst[i];
        fe.isDir = e[0] != 0;
        u32 nameOff = be32(e) & 0x00FFFFFF;
        fe.name = (const char*)(strings + nameOff);
        u32 parentDir = stack.back();
        std::string full = dirPath[parentDir].empty() ? fe.name : dirPath[parentDir] + "/" + fe.name;
        if (fe.isDir) {
            fe.parent = be32(e + 4);
            fe.next = be32(e + 8);
            dirPath[i] = full;
            stack.push_back(i);
        } else {
            fe.hostPath = sDataRoot + "/files/" + full;
            if (strcasecmp(full.c_str(), kBootTitle) == 0) {
                sBootTitleEntry = i;
                sBootTitleBytes = be32(e + 8);  // AST cooking swaps fields without changing file length.
            }
        }
    }
    port_log("dvd: fst loaded, %u entries, root %s", count, sDataRoot.c_str());
#if defined(__vita__)
    assignBuckets(dirPath);
    const u32 moved = moveToBuckets(dirPath);
    if (moved) {
        remove(sizeCachePath().c_str());  // (files changed places: whatever it says about them is from before)
    }
    if (moved || !loadSizeCache()) {
        presetSizesFromListings(dirPath);
    }
#endif
    return true;
}

static const char* validateBootTitle() {
    if (!port_boot_data_enabled()) return nullptr;
    if (sBootTitleEntry >= sFst.size()) {
        port_log_summary("boot-data: missing title entry in sys/fst.bin: %s", kBootTitle);
        return "missing from file list";
    }
    const char* path = sFst[sBootTitleEntry].hostPath.c_str();
    const auto fail = [&](const char* reason, int error = 0, s64 actual = -1) {
        port_log_summary("boot-data: %s: path=\"%s\" errno=%d expected=%u actual=%lld", reason, path, error,
                         sBootTitleBytes, static_cast<long long>(actual));
        return reason;
    };
    // Nonblocking only affects special files; it lets fstat reject a FIFO without waiting for a writer.
    struct File { int fd; ~File() { if (fd >= 0) close(fd); } } file{open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    if (file.fd < 0) return fail(errno == ENOENT ? "missing file" : "unreadable file", errno);
    struct stat st;
    if (fstat(file.fd, &st) != 0) return fail("unreadable file", errno);
    if (!S_ISREG(st.st_mode)) return fail("not a regular file");
    if (st.st_size != sBootTitleBytes) return fail("size mismatch", 0, st.st_size);
    u8 header[64];
    const ssize_t got = pread(file.fd, header, sizeof(header), 0);
    if (got < 0) return fail("unreadable header", errno);
    if (got != sizeof(header)) return fail("short header", 0, got);
    if (memcmp(header, "STRM", 4) != 0) return fail("invalid header");
    const auto le16 = [&](unsigned at) { return static_cast<u32>(header[at]) | (static_cast<u32>(header[at + 1]) << 8); };
    const auto le32 = [&](unsigned at) { return le16(at) | (le16(at + 2) << 16); };
    if (header[9] > 1) return fail("invalid format");  // JASAramStream::channelStart supports only ADPCM and PCM16.
    if (!le16(12) || le16(12) > static_cast<u32>(AudParams::numStreamChannels)) return fail("invalid channels");
    if (le32(32) != static_cast<u32>(AudParams::streamBlockSize)) return fail("invalid block size");
    const s32 loopStart = static_cast<s32>(le32(24)), loopEnd = static_cast<s32>(le32(28));
    if (loopEnd <= 0 || (le16(14) && (loopStart < 0 || loopStart >= loopEnd))) return fail("invalid loop bounds");
    // No payload scan or stride-modulus test: the last AST block may be rounded past EOF.
    return nullptr;
}

static s64 entrySize(u32 entry) {
    FstEntry& fe = sFst[entry];
    if (fe.size < 0) {
        struct stat st;
        const int64_t statStart = port_host_time_ns();
        const bool found = stat(fe.hostPath.c_str(), &st) == 0;
        if (!found) reportDvdError(DvdError::Stat, entry, -1, errno);
        const int64_t statNs = port_host_time_ns() - statStart;
        static unsigned sSlowStats;
        if (statNs >= 5000000 && sSlowStats < 100) {  // 5 ms
            ++sSlowStats;
            port_log("dvd: stat took %.0f ms: %s", statNs / 1e6, fe.hostPath.c_str());
        }
        fe.size = found ? (s64)st.st_size : 0;
        if (!found) {
            port_log("dvd: missing file %s", fe.hostPath.c_str());
        } else if (fe.size == 0) {
            // (the disc itself has one empty file, AudioRes/Waves/B109take_0.aw: the original game opens it and reads nothing)
            port_log("dvd: empty file %s (as on the disc)", fe.hostPath.c_str());
        }
    }
    return fe.size;
}

// Case-insensitive lookup of `name` among the children of directory `dir`.
static s32 findChild(u32 dir, const char* name, size_t len) {
    for (u32 i = dir + 1; i < sFst[dir].next;) {
        const FstEntry& fe = sFst[i];
        if (fe.name.size() == len && strncasecmp(fe.name.c_str(), name, len) == 0) {
            return (s32)i;
        }
        i = fe.isDir ? fe.next : i + 1;
    }
    return -1;
}

extern "C" s32 DVDConvertPathToEntrynum(const char* path) {
    u32 dir = (path[0] == '/') ? 0 : sCurrentDir;
    const char* p = path;
    while (*p == '/') {
        p++;
    }
    while (*p) {
        const char* end = p;
        while (*end && *end != '/') {
            end++;
        }
        size_t len = (size_t)(end - p);
        s32 found;
        if (len == 1 && p[0] == '.') {
            found = (s32)dir;
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            found = (s32)sFst[dir].parent;
        } else {
            found = findChild(dir, p, len);
        }
        if (found < 0) {
            return -1;
        }
        if (*end == 0) {
            return found;
        }
        if (!sFst[found].isDir) {
            return -1;
        }
        dir = (u32)found;
        p = end + 1;
        while (*p == '/') {
            p++;
        }
    }
    return (s32)dir;
}

extern "C" int port_disc_file_present(const char* path) {
    const s32 entry = DVDConvertPathToEntrynum(path);
    if (entry < 0 || sFst[entry].isDir) {
        return 0;
    }
    return entrySize((u32)entry) > 0;
}

// ---------------------------------------------------------------------------
// Read worker
// ---------------------------------------------------------------------------
struct ReadRequest {
    DVDCommandBlock* block;
    u32 entry;
    int64_t submitNs;
};

static std::mutex sQueueMutex;
static std::condition_variable sQueueCv;
static std::deque<ReadRequest> sPending;       // not yet started
static std::deque<DVDCommandBlock*> sDone;     // finished, callbacks pending
static DVDCommandBlock* sActive = nullptr;     // being read by the worker
static OSThreadQueue sSyncQueue;

// Host jobs: slow file-system calls the game makes itself (save files: fsync, create, delete) run on this thread while the
// calling guest thread sleeps, so the guest CPU and the audio interrupts keep going (see port_host_job_wait).
struct HostJob {
    DVDCommandBlock* block;
    void (*fn)(void*);
    void* arg;
};
static std::deque<HostJob> sJobs;
static std::condition_variable sJobCv;
static bool sJobsReady = false;

// Only the read worker owns these descriptors; leave room for audio, saves and diagnostics.
struct OpenFileSlot {
    int fd = -1;
    u32 entry = 0;
    u64 used = 0;
};
static std::array<OpenFileSlot, 32> sOpenFiles;
static u64 sOpenFileUse = 0;

static int openEntry(u32 entry) {
    OpenFileSlot* slot = &sOpenFiles[0];
    for (auto& candidate : sOpenFiles) {
        if (candidate.fd >= 0 && candidate.entry == entry) {
            candidate.used = ++sOpenFileUse;
            return candidate.fd;
        }
        if (candidate.fd < 0 || (slot->fd >= 0 && candidate.used < slot->used)) {
            slot = &candidate;
        }
    }
    if (slot->fd >= 0) {
        const int fd = slot->fd;
        slot->fd = -1;
        if (close(fd) != 0) {
            port_log("dvd: close cached entry %u failed: %s (errno=%d)", slot->entry, strerror(errno), errno);
            return -1;
        }
    }
    const auto& path = sFst[entry].hostPath;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        reportDvdError(DvdError::Open, entry, fd, errno);
        port_log("dvd: open %s failed: %s (errno=%d)", path.c_str(), strerror(errno), errno);
        return -1;
    }
    slot->fd = fd;
    slot->entry = entry;
    slot->used = ++sOpenFileUse;
    return fd;
}

// What the drive worker spent reading (handhelds log it: a long read stalls the thread that waits for it).
struct DriveStats {
    int64_t windowStart = 0, ns = 0, maxNs = 0;
    u32 reads = 0;
    u64 bytes = 0;
    u32 maxEntry = 0;
};
static u32 sSlowLogged;
static DriveStats sDrive;
extern "C" int port_handheld(void);
extern "C" int64_t port_gap_age_ns(void);
extern "C" int64_t port_gap_threshold_ns(void);
extern "C" void port_irq_poll(void);

static int64_t sTraceOpenNs, sTraceGpuNs, sTraceWaitNs;  // of the read being noted (worker thread only)

static void noteRead(u32 entry, size_t bytes, u32 offset, int64_t t0, int64_t t1) {
    static const bool on = PORT_RUNTIME_DIAGNOSTICS && (port_handheld() != 0 || getenv("PETARI_DVDLOG") != nullptr);
    // PETARI_DVDTRACE=<n>: one line per read for the first n reads (what the game reads in what order and in what pieces).
    static const int traceMax = getenv("PETARI_DVDTRACE") ? atoi(getenv("PETARI_DVDTRACE")) : 0;
    static int traced = 0;
    if (traced < traceMax) {
        ++traced;
        port_log("dvdt: %u bytes at %u of %s: %.1f ms (queue %.1f, open %.1f, hook %.1f)", (unsigned)bytes, offset,
                 sFst[entry].hostPath.c_str() + sDataRoot.size(), (t1 - t0) / 1e6, sTraceWaitNs / 1e6, sTraceOpenNs / 1e6, sTraceGpuNs / 1e6);
    }
    if (!on) return;
    DriveStats& d = sDrive;
    const int64_t dt = t1 - t0;
    if (!d.windowStart) d.windowStart = t0;
    d.ns += dt;
    d.bytes += bytes;
    ++d.reads;
    if (dt > d.maxNs) {
        d.maxNs = dt;
        d.maxEntry = entry;
    }
    if (dt >= 40000000 && sSlowLogged < 300) {  // 40 ms
        ++sSlowLogged;
        port_log("dvd: slow read: %.0f ms, %u KiB at %u of %s", dt / 1e6, (unsigned)(bytes / 1024), offset, sFst[entry].hostPath.c_str());
    }
    if (t1 - d.windowStart >= 5000000000ll) {
        port_log("dvd: %u reads, %.1f MiB, %.0f ms in the drive (longest %.0f ms: %s)", d.reads, d.bytes / 1048576.0, d.ns / 1e6, d.maxNs / 1e6,
                 sFst[d.maxEntry].hostPath.c_str());
        d = DriveStats{};
    }
}

// Read-ahead.  The wave banks (AudioRes/Waves/*.aw, 12.7 MiB in a cold start) are read in 32 KiB pieces, one after the other, the next
// one asked for a couple of milliseconds after the last one arrived (419 reads, each waited for).  When a read continues the one
// before it in the same file, the worker reads the piece after it too while the game handles the one it has (the card is idle then)
// and answers the game's next request from memory when it is for exactly that piece.  Nothing but the worker touches this.
constexpr size_t kReadAheadMax = 65536;
struct ReadAhead {
    u32 entry = ~0u;
    u64 offset = 0;
    size_t len = 0;
    u8 data[kReadAheadMax];
};
static ReadAhead sReadAhead;
static u32 sSeqEntry = ~0u;  // the file of the last read and where it ended: the next read starts there when it is a sequence
static u64 sSeqEnd = 0;
static unsigned sReadAheadHits, sReadAheadMisses;

// Movies (.thp).  The player reads one frame at a time, the next only after the one before has arrived, and the frames are of
// every size (14 KB at the start of PrologueA, up to 51 KB in busy scenes), so the exact-piece read-ahead above never matches
// and only delays the next frame by a read of its own: with it the opening movie got 53 frames a second instead of 60, and the
// sound broke up.  A movie instead keeps a window of the file after the last frame: a frame inside the window is answered from
// memory, and when less than kMovieLow of the window is left the worker tops it up with ONE large read (the card is much faster
// per byte in big reads: a 15 KB read costs about 5 ms, a 256 KB one not much more than twice that).  PETARI_MOVIEWIN=0 turns the
// window off (plain reads, as before the read-ahead); PETARI_READAHEAD=0 turns both off.
constexpr size_t kMovieWindow = 256 * 1024;
constexpr size_t kMovieLow = 96 * 1024;
struct MovieWindow {
    u32 entry = ~0u;
    u64 start = 0;  // file offset of data[0]
    size_t len = 0;
    alignas(64) u8 data[kMovieWindow];
};
static MovieWindow sMovieWin;
static unsigned sMovieHits, sMovieMisses, sMovieTopUps;
static u64 sMovieTopUpBytes;
static int64_t sMovieTopUpNs, sMovieTopUpMaxNs;

static bool isMovieEntry(u32 entry) {
    const std::string& p = sFst[entry].hostPath;
    return p.size() > 4 && strcasecmp(p.c_str() + p.size() - 4, ".thp") == 0;
}

// Make the window cover the file from `end` on: keep what it already holds beyond `end`, read the rest of it from the card.
static void movieTopUp(u32 entry, u64 end, int fdHint) {
    MovieWindow& w = sMovieWin;
    const s64 size = sFst[entry].size;
    if (size <= 0 || (s64)end >= size) {
        return;
    }
    size_t keep = 0;
    if (w.entry == entry && end >= w.start && end < w.start + w.len) {
        keep = (size_t)(w.start + w.len - end);
    }
    if (keep >= kMovieLow) {
        return;
    }
    const int fd = fdHint >= 0 ? fdHint : openEntry(entry);
    if (fd < 0) {
        return;
    }
    const int64_t t0 = port_host_time_ns();
    if (keep) {
        memmove(w.data, w.data + (end - w.start), keep);
    }
    const u64 from = end + keep;
    size_t room = kMovieWindow - keep;
    if ((s64)from >= size) {
        room = 0;
    } else if ((u64)size - from < room) {
        room = (size_t)((u64)size - from);
    }
    ssize_t got = room ? pread(fd, w.data + keep, room, (off_t)from) : 0;
    if (got < 0) {
        got = 0;
    }
    w.entry = entry;
    w.start = end;
    w.len = keep + (size_t)got;
    const int64_t dt = port_host_time_ns() - t0;
    ++sMovieTopUps;
    sMovieTopUpBytes += (u64)got;
    sMovieTopUpNs += dt;
    if (dt > sMovieTopUpMaxNs) {
        sMovieTopUpMaxNs = dt;
    }
}

static void workerMain() {
    PORT_QEMU_THREAD("dvd-worker");
    port_thread_class(2);
    const bool readAheadOn = !getenv("PETARI_READAHEAD") || getenv("PETARI_READAHEAD")[0] != '0';
    const bool movieWindowOn = readAheadOn && (!getenv("PETARI_MOVIEWIN") || getenv("PETARI_MOVIEWIN")[0] != '0');
    for (;;) {
        ReadRequest req;
        {
            std::unique_lock<std::mutex> lk(sQueueMutex);
            sQueueCv.wait(lk, [] { return !sPending.empty(); });
            req = sPending.front();
            sPending.pop_front();
            sActive = req.block;
        }
        DVDCommandBlock* b = req.block;
        s32 result;
        const int64_t workStart = port_host_time_ns();
        sTraceWaitNs = workStart - req.submitNs;
        const size_t want = b->length;
        const u64 offset = b->offset;
        const bool movie = readAheadOn && isMovieEntry(req.entry);
        const bool fromMovieWin = movie && movieWindowOn && sMovieWin.entry == req.entry && offset >= sMovieWin.start &&
                                  offset + want <= sMovieWin.start + sMovieWin.len;
        const bool fromReadAhead = !movie && readAheadOn && sReadAhead.entry == req.entry && sReadAhead.offset == offset && sReadAhead.len == want;
        int fd = (fromReadAhead || fromMovieWin) ? -1 : openEntry(req.entry);
#if defined(__vita__)
        if (fd >= 0 && sFst[req.entry].cached && !sFst[req.entry].checked) {
            // A size from the sizes cache is compared with the real one at the file's first open (a cheap call on an open file).
            FstEntry& fe = sFst[req.entry];
            fe.checked = true;
            struct stat st;
            if (fstat(fd, &st) == 0 && (s64)st.st_size != fe.size) {
                port_log("dvd: %s is %lld bytes, the sizes cache said %lld: deleting the cache (the next start lists the folders again)", fe.hostPath.c_str(),
                         (long long)st.st_size, (long long)fe.size);
                fe.size = (s64)st.st_size;
                remove(sizeCachePath().c_str());
            }
        }
#endif
        sTraceOpenNs = port_host_time_ns() - workStart;
        sTraceGpuNs = 0;
        if (fd < 0 && !fromReadAhead && !fromMovieWin) {
            result = -1;
        } else {
            u8* dst = (u8*)b->addr;
            const int64_t readStart = port_host_time_ns();
            ssize_t got;
            if (fromMovieWin) {
                memcpy(dst, sMovieWin.data + (offset - sMovieWin.start), want);
                got = (ssize_t)want;
                ++sMovieHits;
            } else if (fromReadAhead) {
                memcpy(dst, sReadAhead.data, want);
                got = (ssize_t)want;
                if (++sReadAheadHits % 100 == 0) {
                    port_log("dvd: read-ahead served %u reads from memory (%u sequential reads were not ready)", sReadAheadHits, sReadAheadMisses);
                }
            } else {
                got = pread(fd, dst, want, (off_t)offset);
                if (movie) {
                    ++sMovieMisses;
                }
            }
            int64_t readEnd;
            if (got < 0) {
                const int error = errno;
                readEnd = port_host_time_ns();
                result = -1;
                reportDvdError(DvdError::Read, req.entry, result, error);
            } else {
                readEnd = port_host_time_ns();
                if ((size_t)got < want) {
                    memset(dst + got, 0, want - (size_t)got);  // past EOF the drive returns adjacent data
                }
                b->transferredSize = b->length;
                result = (s32)b->length;
                const int64_t hookStart = port_host_time_ns();
                port_gpu_memory_written(dst, (uint32_t)want);  // models, textures, display lists: see vgx_galaxy.h
                sTraceGpuNs = port_host_time_ns() - hookStart;
            }
            noteRead(req.entry, want, (u32)offset, readStart, readEnd);
        }
        bool pendingNow;
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            b->currTransferSize = (u32)result;
            sActive = nullptr;
            sDone.push_back(b);
            pendingNow = !sPending.empty();
        }
        port_irq_raise(PORT_IRQ_DVD);
        port_vclock_external_end();

        if (movie) {
            // A movie frame that continues the one before it: keep the window ahead of the player.
            if (movieWindowOn && result >= 0 && want >= 4096 && want <= kMovieWindow / 2) {
                const u64 end = offset + want;
                const bool sequence = sSeqEntry == req.entry && sSeqEnd == offset;
                sSeqEntry = req.entry;
                sSeqEnd = end;
                if (sequence && !pendingNow) {
                    movieTopUp(req.entry, end, fd);
                }
            } else {
                sSeqEntry = ~0u;
            }
            if ((sMovieHits + sMovieMisses) % 300 == 0 && sMovieHits + sMovieMisses != 0 && (fromMovieWin || movieWindowOn)) {
                static unsigned sLoggedAt = 0;
                if (sLoggedAt != sMovieHits + sMovieMisses) {
                    sLoggedAt = sMovieHits + sMovieMisses;
                    port_log("dvd: movie reads %u from memory, %u from the card; %u window refills, %.0f KiB each, %.1f ms on average (longest %.1f ms)", sMovieHits,
                             sMovieMisses, sMovieTopUps, sMovieTopUps ? sMovieTopUpBytes / 1024.0 / sMovieTopUps : 0.0,
                             sMovieTopUps ? sMovieTopUpNs / 1e6 / sMovieTopUps : 0.0, sMovieTopUpMaxNs / 1e6);
                }
            }
        } else if (readAheadOn && result >= 0 && want >= 4096 && want <= kReadAheadMax) {
            const bool sequence = sSeqEntry == req.entry && sSeqEnd == offset;
            sSeqEntry = req.entry;
            sSeqEnd = offset + want;
            if (sequence && !pendingNow) {
                const s64 size = sFst[req.entry].size;
                const int pfd = (size > 0 && (s64)(offset + want) < size) ? (fd >= 0 ? fd : openEntry(req.entry)) : -1;
                if (pfd >= 0) {
                    ssize_t got = pread(pfd, sReadAhead.data, want, (off_t)(offset + want));
                    if (got > 0) {
                        if ((size_t)got < want) {
                            memset(sReadAhead.data + got, 0, want - (size_t)got);
                        }
                        sReadAhead.entry = req.entry;
                        sReadAhead.offset = offset + want;
                        sReadAhead.len = want;
                    } else {
                        sReadAhead.entry = ~0u;
                    }
                }
            }
            if (!fromReadAhead && sequence) {
                ++sReadAheadMisses;
            }
        } else {
            sSeqEntry = ~0u;
        }
    }
}

static void jobMain() {
    PORT_QEMU_THREAD("host-job");
    port_thread_class(2);
    for (;;) {
        HostJob job;
        {
            std::unique_lock<std::mutex> lk(sQueueMutex);
            sJobCv.wait(lk, [] { return !sJobs.empty(); });
            job = sJobs.front();
            sJobs.pop_front();
        }
        job.fn(job.arg);
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            job.block->currTransferSize = 0;
            sDone.push_back(job.block);
        }
        port_irq_raise(PORT_IRQ_DVD);
        port_vclock_external_end();
    }
}

// PETARI_OPENBENCH=1: before the game starts, time open() + a 32-byte pread + close() of files in the big directory (ObjectData: 1844
// files) and of files in small ones, one thread and then two at once. The disc worker pays this for every archive the game opens for the
// first time; this says whether the cost follows the size of the directory and whether opens in parallel overlap.
static void openBench() {
    if (!getenv("PETARI_OPENBENCH")) return;
    PortHostAllocScope scope;
    std::vector<u32> big, small;
    std::string bigDir = sDataRoot + "/files/ObjectData/";
    for (u32 i = 1; i < sFst.size(); ++i) {
        if (sFst[i].isDir || sFst[i].hostPath.empty()) continue;
        if (sFst[i].hostPath.compare(0, bigDir.size(), bigDir) == 0) big.push_back(i);
        else if (sFst[i].hostPath.find("/files/StageData/") != std::string::npos && sFst[i].hostPath.find('/', sDataRoot.size() + 17) != std::string::npos) small.push_back(i);
    }
    port_log("openbench: %u files in ObjectData, %u in StageData subdirectories", (unsigned)big.size(), (unsigned)small.size());
    struct R { double open, pread, close; };
    auto one = [](const std::string& path) {
        R r{};
        const int64_t t0 = port_host_time_ns();
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        const int64_t t1 = port_host_time_ns();
        if (fd >= 0) {
            char b[32];
            (void)!pread(fd, b, sizeof b, 0);
        }
        const int64_t t2 = port_host_time_ns();
        if (fd >= 0) close(fd);
        const int64_t t3 = port_host_time_ns();
        r.open = (t1 - t0) / 1e6;
        r.pread = (t2 - t1) / 1e6;
        r.close = (t3 - t2) / 1e6;
        return r;
    };
    auto run = [&](const char* name, const std::vector<u32>& v, size_t first, size_t count, size_t step) {
        double so = 0, sp = 0, sc = 0, mo = 0;
        size_t n = 0;
        const int64_t t0 = port_host_time_ns();
        for (size_t k = 0; k < count && first + k * step < v.size(); ++k, ++n) {
            const R r = one(sFst[v[first + k * step]].hostPath);
            so += r.open; sp += r.pread; sc += r.close;
            if (r.open > mo) mo = r.open;
        }
        const double wall = (port_host_time_ns() - t0) / 1e6;
        port_log("openbench: %-34s %2u files: open avg %.2f max %.2f ms, pread avg %.2f ms, close avg %.2f ms (%.1f ms for all)", name, (unsigned)n, n ? so / n : 0.0,
                 mo, n ? sp / n : 0.0, n ? sc / n : 0.0, wall);
    };
    run("ObjectData, spread over the list", big, 3, 30, 59);
    run("ObjectData, the same again", big, 3, 30, 59);
    run("StageData subdirectories", small, 0, 30, small.size() > 30 ? small.size() / 30 : 1);
    // two threads at once, 20 different ObjectData files each
    {
        const int64_t t0 = port_host_time_ns();
        std::thread other([&] { run("  (thread B) ObjectData", big, 1000, 20, 7); });
        run("  (thread A) ObjectData", big, 1003, 20, 7);
        other.join();
        port_log("openbench: two threads, 20 files each: %.1f ms in all", (port_host_time_ns() - t0) / 1e6);
    }
    {
        const int64_t t0 = port_host_time_ns();
        run("  (one thread) ObjectData", big, 1200, 40, 7);
        port_log("openbench: one thread, 40 files: %.1f ms in all", (port_host_time_ns() - t0) / 1e6);
    }
}

// PETARI_DVDBENCH=1: before the game starts, read big archives from the data directory in several ways (one call or chunks,
// aligned or not, pread or lseek+read, one reader or two at once) and log the MB/s of each. The handhelds read the game's archives
// at 9-12 MB/s; this says whether the way the game reads is the limit or the card is. Every try reads files no other try has
// touched, so a cache cannot flatter a later one.
static void benchDrive() {
    if (!getenv("PETARI_DVDBENCH")) return;
    PortHostAllocScope scope;  // this runs on a game thread: the buffers and the helper thread belong to the host heap
    std::vector<u32> files;
    std::vector<size_t> sizes;
    for (u32 i = 1; i < sFst.size() && files.size() < 26; ++i) {
        if (sFst[i].isDir) continue;
        struct stat st;
        if (stat(sFst[i].hostPath.c_str(), &st) == 0 && st.st_size >= 1500000 && st.st_size <= 6000000) {
            files.push_back(i);
            sizes.push_back((size_t)st.st_size);
        }
    }
    port_log("dvd-bench: %u candidate files", (unsigned)files.size());
    if (files.size() < 20) return;
    const size_t cap = 6100000;
    u8* raw = (u8*)malloc(cap + 8192);
    u8* raw2 = (u8*)malloc(cap + 8192);
    if (!raw || !raw2) { port_log("dvd-bench: no memory"); free(raw); free(raw2); return; }
    u8* aligned = (u8*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    u8* aligned2 = (u8*)(((uintptr_t)raw2 + 4095) & ~(uintptr_t)4095);
    struct Try { const char* name; size_t chunk; size_t misalign; bool seq; };
    const Try tries[] = {
        {"pread whole, 4K-aligned", 0, 0, false},
        {"pread whole, +36 bytes", 0, 36, false},
        {"pread 1 MiB chunks", 1u << 20, 0, false},
        {"pread 256 KiB chunks", 256u << 10, 0, false},
        {"pread 64 KiB chunks", 64u << 10, 0, false},
        {"pread 32 KiB chunks", 32u << 10, 0, false},
        {"lseek+read whole", 0, 0, true},
    };
    unsigned next = 0;
    for (const Try& t : tries) {
        double mbs[2] = {0, 0};
        for (int rep = 0; rep < 2 && next < files.size(); ++rep, ++next) {
            const u32 e = files[next];
            const size_t n = sizes[next];
            const int fd = open(sFst[e].hostPath.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            u8* dst = aligned + t.misalign;
            const int64_t t0 = port_host_time_ns();
            size_t done = 0;
            bool ok = true;
            if (t.seq) {
                lseek(fd, 0, SEEK_SET);
                ok = read(fd, dst, n) == (ssize_t)n;
            } else {
                const size_t step = t.chunk ? t.chunk : n;
                while (done < n) {
                    const size_t want = (n - done < step) ? n - done : step;
                    const ssize_t got = pread(fd, dst + done, want, (off_t)done);
                    if (got <= 0) { ok = false; break; }
                    done += (size_t)got;
                }
            }
            const int64_t t1 = port_host_time_ns();
            close(fd);
            mbs[rep] = ok && t1 > t0 ? (double)n / 1048576.0 / ((t1 - t0) / 1e9) : 0;
        }
        port_log("dvd-bench: %-26s %5.1f and %5.1f MiB/s", t.name, mbs[0], mbs[1]);
    }
    // two readers at once (the movie decoders and the drive worker do that during a stage change)
    if (next + 2 <= files.size()) {
        const u32 ea = files[next], eb = files[next + 1];
        const size_t na = sizes[next], nb = sizes[next + 1];
        next += 2;
        int64_t tb0 = 0, tb1 = 0;
        const int64_t t0 = port_host_time_ns();
        std::thread other([&] {
            const int fd = open(sFst[eb].hostPath.c_str(), O_RDONLY | O_CLOEXEC);
            tb0 = port_host_time_ns();
            if (fd >= 0) { (void)!pread(fd, aligned2, nb, 0); close(fd); }
            tb1 = port_host_time_ns();
        });
        const int fd = open(sFst[ea].hostPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { (void)!pread(fd, aligned, na, 0); close(fd); }
        const int64_t t1 = port_host_time_ns();
        other.join();
        const int64_t tEnd = tb1 > t1 ? tb1 : t1;
        port_log("dvd-bench: two readers at once: %.1f MiB in %.0f ms = %.1f MiB/s together", (na + nb) / 1048576.0, (tEnd - t0) / 1e6,
                 (na + nb) / 1048576.0 / ((tEnd - t0) / 1e9));
    }
    // the first try again, on fresh files, to see how steady the card is
    for (int rep = 0; rep < 2 && next < files.size(); ++rep, ++next) {
        const int fd = open(sFst[files[next]].hostPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        const int64_t t0 = port_host_time_ns();
        const ssize_t got = pread(fd, aligned, sizes[next], 0);
        const int64_t t1 = port_host_time_ns();
        close(fd);
        port_log("dvd-bench: pread whole again: %.1f MiB/s", got > 0 ? (double)sizes[next] / 1048576.0 / ((t1 - t0) / 1e9) : 0.0);
    }
    free(raw);
    free(raw2);
}

static void dvdIrq() {
    for (;;) {
        DVDCommandBlock* b;
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            if (sDone.empty()) {
                break;
            }
            b = sDone.front();
            sDone.pop_front();
        }
        s32 result = (s32)b->currTransferSize;
        b->state = result < 0 ? kStateFatal : kStateEnd;
        if (b->callback) {
            b->callback(result, b);
        }
    }
    OSWakeupThread(&sSyncQueue);
}

static void submit(DVDCommandBlock* b, u32 entry) {
    b->state = kStateBusy;
    b->transferredSize = 0;
    // The host drive worker frees deque nodes while it does not own the guest CPU.
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> lk(sQueueMutex);
    sPending.push_back({b, entry, port_host_time_ns()});
    port_vclock_external_begin();  // armhf/qemu harness: virtual time waits for the drive worker (port/vclock.h)
    sQueueCv.notify_one();
}

// Runs fn(arg) on the host-job thread and puts the calling guest thread to sleep until it is done, the way DVDReadPrio waits for a read.
// A host call that blocks (an fsync or a file create on the SD card takes tens to hundreds of ms) would otherwise hold the one guest CPU
// token and starve the audio interrupts. Calls made with interrupts disabled, from a non-guest thread or before DVDInit run inline.
extern "C" void port_host_job_wait(const char* what, void (*fn)(void*), void* arg) {
    static unsigned sLogged;
    const int64_t t0 = port_host_time_ns();
    if (!sJobsReady || OSGetCurrentThread() == nullptr) {
        fn(arg);
        return;
    }
    const BOOL en = OSDisableInterrupts();
    if (!en) {
        OSRestoreInterrupts(en);
        fn(arg);
        return;
    }
    DVDCommandBlock b;
    memset(&b, 0, sizeof(b));
    b.state = kStateBusy;
    {
        PortHostAllocScope scope;
        std::lock_guard<std::mutex> lk(sQueueMutex);
        sJobs.push_back({&b, fn, arg});
        port_vclock_external_begin();
        sJobCv.notify_one();
    }
    while (b.state == kStateBusy) {
        OSSleepThread(&sSyncQueue);
    }
    OSRestoreInterrupts(en);
    const double ms = (port_host_time_ns() - t0) / 1e6;
    if (ms >= 5.0 && sLogged < 100) {
        ++sLogged;
        port_log("hostjob: %s took %.1f ms", what, ms);
    }
}

// DVDFileInfo embeds its command block first; the file callback gets the
// file info back (as on hardware).
static void fileCallback(s32 result, DVDCommandBlock* block) {
    DVDFileInfo* fi = (DVDFileInfo*)block;
    if (fi->callback) {
        fi->callback(result, fi);
    }
}

// ---------------------------------------------------------------------------
// SDK API
// ---------------------------------------------------------------------------
extern "C" {

void DVDInit(void) {
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;
    OSInitThreadQueue(&sSyncQueue);
    if (!loadFst()) {
        if (port_boot_data_report("Game file list is unreadable. Restore converted game data and relaunch; saves unchanged."))
            port_boot_data_park();
        port_fatal("dvd: game data not found under %s (expected sys/fst.bin and files/)", sDataRoot.c_str());
    }
    if (const char* error = validateBootTitle()) {
        char message[180];
        snprintf(message, sizeof(message), "SMG_title_strm.ast: %s. Restore converted game data and relaunch; saves unchanged.", error);
        if (port_boot_data_report(message)) port_boot_data_park();
        port_fatal("%s", message);  // No visible splash: exit explicitly, never park invisibly.
    }
    benchDrive();
    openBench();
    port_irq_set_handler(PORT_IRQ_DVD, dvdIrq);
    PortHostAllocScope scope;
    std::thread(workerMain).detach();
    std::thread(jobMain).detach();
    sJobsReady = true;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    if (entrynum < 0 || (u32)entrynum >= sFst.size() || sFst[entrynum].isDir) {
        return FALSE;
    }
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = (u32)entrynum;
    fileInfo->length = (u32)entrySize((u32)entrynum);
    fileInfo->callback = nullptr;
    fileInfo->cb.state = kStateEnd;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    s32 entry = DVDConvertPathToEntrynum(fileName);
    if (entry < 0) {
        port_log("dvd: file not found: %s", fileName);
        return FALSE;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    (void)fileInfo;
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    (void)prio;
    fileInfo->callback = callback;
    DVDCommandBlock* b = &fileInfo->cb;
    b->addr = addr;
    b->length = (u32)length;
    b->offset = (u32)offset;
    b->callback = fileCallback;
    submit(b, fileInfo->startAddr);
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    (void)prio;
    DVDCommandBlock* b = &fileInfo->cb;
    // PETARI_GAPLOG: where did a long stretch without a safe point before this read go, in the caller or in submit()?
    const int64_t gapThr = port_gap_threshold_ns();
    const int64_t gapPre = gapThr > 0 ? port_gap_age_ns() : 0;
    const int64_t gapT0 = gapThr > 0 ? port_host_time_ns() : 0;
    fileInfo->callback = nullptr;
    b->addr = addr;
    b->length = (u32)length;
    b->offset = (u32)offset;
    b->callback = nullptr;
    submit(b, fileInfo->startAddr);
    if (gapThr > 0) {
        const int64_t gapSubmit = port_host_time_ns() - gapT0;
        static unsigned sLogged;
        if ((gapPre >= gapThr || gapSubmit >= gapThr / 2) && sLogged < 200) {
            ++sLogged;
            port_log("dvdread: %.1f ms since the last safe point when called, submit took %.1f ms; %d bytes at %d of %s; callers %p %p %p (port_irq_poll %p)",
                     gapPre / 1e6, gapSubmit / 1e6, (int)length, (int)offset, sFst[fileInfo->startAddr].hostPath.c_str(), __builtin_return_address(0),
                     __builtin_return_address(1), __builtin_return_address(2), (void*)&port_irq_poll);
        }
    }
    BOOL en = OSDisableInterrupts();
    while (b->state == kStateBusy || b->state == kStateWaiting) {
        OSSleepThread(&sSyncQueue);
    }
    OSRestoreInterrupts(en);
    return b->state == kStateEnd ? (s32)b->transferredSize : -1;
}

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        for (auto it = sPending.begin(); it != sPending.end(); ++it) {
            if (it->block == block) {
                sPending.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (removed) {
        block->state = kStateCanceled;
    }
    if (callback) {
        callback(0, block);
    }
    return TRUE;
}

s32 DVDCancel(DVDCommandBlock* block) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        for (auto it = sPending.begin(); it != sPending.end(); ++it) {
            if (it->block == block) {
                sPending.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (removed) {
        block->state = kStateCanceled;
        return 0;
    }
    // In flight: wait for it to finish.
    BOOL en = OSDisableInterrupts();
    while (block->state == kStateBusy) {
        OSSleepThread(&sSyncQueue);
    }
    OSRestoreInterrupts(en);
    return 0;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block->state; }

s32 DVDGetDriveStatus(void) {
    std::lock_guard<std::mutex> lk(sQueueMutex);
    return (sPending.empty() && sActive == nullptr) ? 0 : 1;
}

BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    block->state = kStateEnd;
    if (callback) {
        callback(1, block);
    }
    return TRUE;
}

BOOL DVDOpenDir(const char* dirName, DVDDir* dir) {
    s32 entry = DVDConvertPathToEntrynum(dirName);
    if (entry < 0 || !sFst[entry].isDir) {
        return FALSE;
    }
    dir->entryNum = (u32)entry;
    dir->location = (u32)entry + 1;
    dir->next = sFst[entry].next;
    return TRUE;
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirent) {
    u32 loc = dir->location;
    if (loc <= dir->entryNum || loc >= dir->next) {
        return FALSE;
    }
    FstEntry& fe = sFst[loc];
    dirent->entryNum = loc;
    dirent->isDir = fe.isDir;
    dirent->name = (char*)fe.name.c_str();
    dir->location = fe.isDir ? fe.next : loc + 1;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir* dir) {
    (void)dir;
    return TRUE;
}

BOOL DVDChangeDir(const char* dirName) {
    s32 entry = DVDConvertPathToEntrynum(dirName);
    if (entry < 0 || !sFst[entry].isDir) {
        return FALSE;
    }
    sCurrentDir = (u32)entry;
    return TRUE;
}

}  // extern "C"
