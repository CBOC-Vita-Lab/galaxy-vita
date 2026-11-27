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
#include <psp2/kernel/sysmem.h>  // (ram33: GXV_IO_PREFETCH_OUT)
#include <unordered_map>
#endif
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <array>
#include <chrono>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"
#include "port/diagnostics.h"
#include "port/bootio.h"
#include "port/load_split.h"
#include "port/io_policy.h"
extern "C" void port_gpu_memory_written(void* addr, uint32_t size);
#include "port/qemu_hooks.h"
#include "port/resume.h"
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
    uint8_t cls = PORT_DISC_OTHER;  // (cook24) which disc folder: the load-split census's class (port/load_split.h)
    uint8_t sub = 0;                // (disc26) inside AudioRes: 1 = Stream (music), 2 = Waves (sound banks); the "io:" census splits them
};

// (cook24, port/load_split.h) the always-on disc counters of the "load-split:" lines: relaxed atomics, bumped once per read.
namespace loadsplit {
struct Counters {
    std::atomic<uint64_t> reads[PORT_DISC_CLASSES], bytes[PORT_DISC_CLASSES], workerNs[PORT_DISC_CLASSES], blockedNs[PORT_DISC_CLASSES];
    std::atomic<uint64_t> probes{0}, asyncReads{0}, queueNs{0}, opens{0}, openNs{0}, stats{0}, statNs{0}, readAheadHits{0};
    std::atomic<uint64_t> decodes{0}, decodeNs{0}, decodeBytes{0};
    std::atomic<uint64_t> slowest{0};  // (the slowest read since the last port_load_split_slowest: ns << 24 | entry)
};
static Counters g;
static uint8_t classOf(const std::string& path) {
    const auto starts = [&](const char* p) { return strncasecmp(path.c_str(), p, strlen(p)) == 0; };
    if (starts("ObjectData/")) return PORT_DISC_OBJECT;
    if (starts("StageData/")) return PORT_DISC_STAGE;
    if (starts("LayoutData/")) return PORT_DISC_LAYOUT;
    if (starts("AudioRes/")) return PORT_DISC_AUDIO;
    return PORT_DISC_OTHER;
}
static uint8_t subOf(const std::string& path) {  // (disc26)
    if (strncasecmp(path.c_str(), "AudioRes/Stream/", 16) == 0) return 1;
    if (strncasecmp(path.c_str(), "AudioRes/Waves/", 15) == 0) return 2;
    return 0;
}
}  // namespace loadsplit

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

// Start-up watchdog inputs (vita/vgx loading screen): bytes the drive worker has read for the game, the number of operations
// that failed, and the first failing path. Plain counters and one write-once string; no behaviour depends on them here.
static std::atomic<uint64_t> sDvdBytesRead{0};
static std::atomic<unsigned> sDvdFailures{0};
static std::atomic<int> sDvdFirstFailureState{0};  // 0 none, 1 being written, 2 readable
static char sDvdFirstFailure[200];
static void noteDvdFailure(const char* operation, const char* path, int error) {
    sDvdFailures.fetch_add(1, std::memory_order_relaxed);
    int expected = 0;
    if (!sDvdFirstFailureState.compare_exchange_strong(expected, 1)) return;
    snprintf(sDvdFirstFailure, sizeof(sDvdFirstFailure), "%s %s (errno %d)", operation, path ? path : "?", error);
    sDvdFirstFailureState.store(2, std::memory_order_release);
    port_log_summary("dvd: first failure: %s", sDvdFirstFailure);
}
extern "C" uint64_t port_dvd_bytes_read(void) { return sDvdBytesRead.load(std::memory_order_relaxed); }
extern "C" unsigned port_dvd_failures(void) { return sDvdFailures.load(std::memory_order_relaxed); }
// Copies "<operation> <path> (errno n)" of the first failed operation; 0 when nothing has failed.
extern "C" int port_dvd_first_failure(char* out, size_t size) {
    if (!out || !size || sDvdFirstFailureState.load(std::memory_order_acquire) != 2) return 0;
    snprintf(out, size, "%s", sDvdFirstFailure);
    return 1;
}

// Opt-in startup guard, not a replacement for the game's audio preparation. The known
// title basename is shared by the supported data layout; regional sound-table mapping
// remains a validation prerequisite before enabling this by default.
static constexpr const char* kBootTitle = "AudioRes/Stream/SMG_title_strm.ast";
static u32 sBootTitleEntry = ~0u, sBootTitleBytes = 0;
static std::atomic<bool> sFstReady{false};
namespace AudParams { extern const int streamBlockSize, numStreamChannels; }
extern "C" int port_boot_data_error(const char*) __attribute__((weak));
extern "C" int port_vgx_vita_fail(const char*) __attribute__((weak));  // (vgx_host_galaxy_vita.cpp: a message on the loading screen; 1 when it can be shown)

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

// g30 Wave 7: cache sys/fst.bin (81 KiB) after its first valid read so boot does not open/read it 4 times
// (disc probe, language check, DVD init, and mp_boot StageDataHash).
static std::mutex sFstCacheMu;
static std::string sFstCacheRoot;
static std::vector<u8> sFstCacheData;

extern "C" size_t port_dvd_cached_fst_copy(const char* root, uint8_t* out, size_t cap) {
    if (!root || !*root) return 0;
    std::lock_guard<std::mutex> lk(sFstCacheMu);
    if (sFstCacheData.empty() || sFstCacheRoot != root) return 0;
    if (!out) return sFstCacheData.size();
    if (cap < sFstCacheData.size()) return 0;
    memcpy(out, sFstCacheData.data(), sFstCacheData.size());
    return sFstCacheData.size();
}

// The disc's file table (sys/fst.bin under `root`): 12-byte entries, the
// root's holding their count, then the names.  Empty when missing or cut.
static std::vector<u8> readFst(const std::string& root, bool patient = false) {
    {
        std::lock_guard<std::mutex> lk(sFstCacheMu);
        if (!sFstCacheData.empty() && sFstCacheRoot == root) return sFstCacheData;
    }
    std::vector<u8> data;
    // (bootio: open(2) and read(2) on the C library's descriptor table, not a stdio FILE: the FILE slot search of this library's fopen has no lock, and a
    // failed open on the card for a moment is repeated for up to 3 s, logged with its SCE code, instead of ending the start-up)
    const std::string path = root + "/sys/fst.bin";
    // (patient: the start-up's own reads wait up to 3 s for a file that is there; a probe of a folder that may not be a copy (vr_setup) tries once)
    bootio::Options patience;
    if (!patient) patience.budgetMs = patience.missingMs = 0;
    const int fd = bootio::OpenRetry(path.c_str(), O_RDONLY | O_CLOEXEC, "fst.bin", patience);
    if (fd < 0) {
        return data;
    }
    const off_t len = lseek(fd, 0, SEEK_END);
    if (len >= 12 && lseek(fd, 0, SEEK_SET) == 0) {
        data.resize((size_t)len);
        size_t got = 0;
        while (got < data.size()) {
            const ssize_t n = read(fd, data.data() + got, data.size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got != data.size() || (uint64_t)be32(&data[8]) * 12 >= data.size()) {
            data.clear();
        }
    }
    close(fd);
    if (!data.empty()) {
        std::lock_guard<std::mutex> lk(sFstCacheMu);
        sFstCacheRoot = root;
        sFstCacheData = data;
    }
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

static const PortDisc* identifyDisc(const char* root, bool patient) {
    PortHostAllocScope scope;
    std::vector<u8> fst = readFst(root, patient);
    if (fst.empty()) {
        return nullptr;
    }
    // (the start-up's own call retries the stat of the texts for up to 3 s as well: a stat that fails for a moment would otherwise leave no disc)
    for (unsigned tries = 0;; ++tries) {
        for (const PortDisc& disc : kDiscs) {
            // The disc has the folder of its first language, and the copy the
            // texts in it.
            if (hasTexts(root, fst, disc.languages[0])) {
                if (tries) port_log_summary("io: disc texts of %s found after %u tries", root, tries + 1);
                return &disc;
            }
        }
        if (!patient || tries >= 60) return nullptr;
        if (!tries) port_log_summary("io: no disc texts found under %s yet (a stat of Message.arc failed); trying again every 50 ms for up to 3 s", root);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

extern "C" const PortDisc* port_dvd_identify(const char* root) { return identifyDisc(root, false); }

extern "C" const PortDisc* port_dvd_disc(void) { return sDisc; }

extern "C" void port_dvd_set_root(const char* root) {
    PortHostAllocScope scope;
    sDataRoot = root;
    sDisc = identifyDisc(root, true);
    sLanguageCount = 0;
    if (sDisc) {
        std::vector<u8> fst = readFst(root, true);
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

// settingsnative: the console's language, a fallback for when no language is wanted (the "System Language" choice); the disc is not read when
// Settings start-up runs, so the choice is made here, once the disc's languages are known. A wanted language always wins.
static char sLanguageFallback[32];
extern "C" void port_language_set_fallback(const char* name) {
    size_t n = 0;
    for (; name && name[n] && n + 1 < sizeof(sLanguageFallback); n++) {
        sLanguageFallback[n] = (char)tolower((unsigned char)name[n]);
    }
    sLanguageFallback[n] = 0;
}

extern "C" int port_language_wanted(void) {
    for (int i = 0; i < sLanguageCount; i++) {
        if (strcmp(sLanguageWanted, sLanguages[i]->name) == 0) {
            return i;
        }
    }
    if (!sLanguageWanted[0] && sLanguageFallback[0]) {
        for (int i = 0; i < sLanguageCount; i++) {
            if (strcmp(sLanguageFallback, sLanguages[i]->name) == 0) {
                return i;
            }
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
    std::vector<u8> data = readFst(sDataRoot, true);
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
            fe.cls = loadsplit::classOf(full);
            fe.sub = loadsplit::subOf(full);
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
    // movie33, TEST ONLY: PETARI_MOVIE_DIR=<host folder> (default unset: nothing changes). A movie of the same name found in that folder
    // is played from there instead of from files/MovieData (its size taken from the file, not from the sizes cache), so one full-size
    // original can be tried on a card whose movies were shrunk, without touching the shrunk set. The sizes cache is not written for
    // these files' places (its signature changes with the paths, so the next start without the key lists the folders again).
    // (movie44: both lines in the basic log, port_log_summary: a player build's log.txt has no port_log lines)
    if (const char* movieDir = getenv("PETARI_MOVIE_DIR"); movieDir && *movieDir) {
        unsigned swapped = 0;
        for (FstEntry& fe : sFst) {
            if (fe.isDir || fe.name.size() < 5 || strcasecmp(fe.name.c_str() + fe.name.size() - 4, ".thp") != 0) continue;
            if (fe.hostPath.find("/MovieData/") == std::string::npos) continue;
            const std::string alt = std::string(movieDir) + "/" + fe.name;
            struct stat st;
            if (stat(alt.c_str(), &st) != 0 || st.st_size <= 96) continue;
            fe.hostPath = alt;
            fe.size = (s64)st.st_size;
            fe.cached = false;
            ++swapped;
            port_log_summary("dvd: PETARI_MOVIE_DIR: %s is played from %s (%lld bytes)", fe.name.c_str(), alt.c_str(), (long long)st.st_size);
        }
        if (!swapped) port_log_summary("dvd: PETARI_MOVIE_DIR=%s holds no movie of the game's names (PrologueB.thp, ...): the card's movies play", movieDir);
    }
    sFstReady.store(true, std::memory_order_release);  // (host paths are final: the stall report may read them)
    return true;
}

// `force`: check although GXV_BOOTDATA is off (the loading screen's stall report asks, once, after a minute without progress).
static const char* validateBootTitleImpl(bool force) {
    if (!force && !port_boot_data_enabled()) return nullptr;
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

static const char* validateBootTitle() { return validateBootTitleImpl(false); }

// Stall report: "" when the title audio file looks fine or the file table is not loaded yet, else the reason (a static string).
// Reads 64 bytes of one file; the file table is not changed after loadFst, which sets sFstReady last.
extern "C" const char* port_boot_title_audio_problem(void) {
    if (!sFstReady.load(std::memory_order_acquire)) return "";
    const char* reason = validateBootTitleImpl(true);
    return reason ? reason : "";
}

static s64 entrySize(u32 entry) {
    FstEntry& fe = sFst[entry];
    if (fe.size < 0) {
        struct stat st;
        const int64_t statStart = port_host_time_ns();
        const bool found = stat(fe.hostPath.c_str(), &st) == 0;
        if (!found) reportDvdError(DvdError::Stat, entry, -1, errno);
        const int64_t statNs = port_host_time_ns() - statStart;
        loadsplit::g.stats.fetch_add(1, std::memory_order_relaxed);
        loadsplit::g.statNs.fetch_add(static_cast<uint64_t>(statNs), std::memory_order_relaxed);
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
    s32 priority;
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

static int sLastOpenErrno;  // errno of the last failed openEntry (the logging in between may change errno)

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
            sLastOpenErrno = errno;
            port_log("dvd: close cached entry %u failed: %s (errno=%d)", slot->entry, strerror(errno), errno);
            return -1;
        }
    }
    const auto& path = sFst[entry].hostPath;
    const int64_t openStart = port_host_time_ns();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    loadsplit::g.opens.fetch_add(1, std::memory_order_relaxed);
    loadsplit::g.openNs.fetch_add(static_cast<uint64_t>(port_host_time_ns() - openStart), std::memory_order_relaxed);
    if (fd < 0) {
        sLastOpenErrno = errno;
        reportDvdError(DvdError::Open, entry, fd, sLastOpenErrno);
        noteDvdFailure("open", path.c_str(), sLastOpenErrno);
        errno = sLastOpenErrno;
        port_log("dvd: open %s failed: %s (errno=%d)", path.c_str(), strerror(errno), errno);
        errno = sLastOpenErrno;
        return -1;
    }
    slot->fd = fd;
    slot->entry = entry;
    slot->used = ++sOpenFileUse;
    return fd;
}

// Recovery from a file the system took away.  After the console slept (or the player went to the Home menu and back) the
// memory card can answer an open descriptor with an I/O error, or refuse an open for a moment while it comes back.  JAudio2's
// stream reader treats ONE failed read as fatal for every stream until the game restarts (JASAramStream: UNK_BOOL_B), which is
// the music stopping for good.  So a read or open that fails with an error a missing file does not give is retried: the cached
// descriptor is closed, the file opened again and the read repeated, a few times with short waits.  Only error paths run it;
// a read that works never gets here.  GXV_DVD_REOPEN=0 turns it off (read once).
static bool reopenOn() {
    static const bool on = vgx::EnvInt("GXV_DVD_REOPEN", 1) != 0;
    return on;
}
// GXV_RESUME_RESET (default on since 3.0, 0 = off): also close every cached descriptor and the read-ahead windows when the system
// reports a resume, before the first read after it (needs the resume watcher of vita/runtime/audio.cpp; read once).
static bool resumeResetOn() {
    static const bool on = vgx::EnvInt("GXV_RESUME_RESET", 1) != 0;
    return on;
}
// Waits (ms) before each retry; the first retry is at once.  A test sets them to 0.  The last three are used only within
// kResumeWindowNs of a reported resume (the card can take seconds to come back; elsewhere a failing card must not stall the worker).
static unsigned sReopenDelayMs[] = {0, 40, 120, 300, 700, 1500, 3000};
constexpr unsigned kReopenShortSteps = 4;
constexpr int64_t kResumeWindowNs = 20000000000ll;
static int64_t sResumeNs;  // when noteResume() last saw a resume (worker thread only; fix33 A1: stamped there, not at the first query)
static bool sLazyDropPending;   // fix33 (A2): a PS-button return was seen; the descriptors are dropped when the first failure comes
static bool recentResume() {
    return resumeResetOn() && sResumeNs != 0 && port_host_time_ns() - sResumeNs < kResumeWindowNs;
}
static unsigned sReopenRecovered, sReopenGaveUp;

// Errors that a retry cannot change: the file is not there, is not a file, may not be read, or the request is wrong.
static bool retriableErrno(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: case EISDIR: case EACCES: case EINVAL: case ENAMETOOLONG: case EFBIG: case EROFS: case ELOOP:
        return false;
    default:
        return true;
    }
}

static void dropOpenFile(u32 entry) {
    for (auto& slot : sOpenFiles) {
        if (slot.fd >= 0 && slot.entry == entry) {
            close(slot.fd);
            slot.fd = -1;
        }
    }
}

static void dropReadCaches();

static void dropAllOpenFiles() {
    for (auto& slot : sOpenFiles) {
        if (slot.fd >= 0) {
            close(slot.fd);
            slot.fd = -1;
        }
    }
    dropReadCaches();
}

static void waitMs(unsigned ms) {
    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// openEntry, repeated after a failure that may pass.  (errno is the last failure's when it gives up.)
static void lazyResumeDrop();
static int openEntryRetry(u32 entry) {
    int fd = openEntry(entry);
    if (fd >= 0 || !reopenOn() || !retriableErrno(sLastOpenErrno)) return fd;
    lazyResumeDrop();
    unsigned attempt = 0;
    for (unsigned ms : sReopenDelayMs) {
        if (attempt >= kReopenShortSteps && !recentResume()) break;
        ++attempt;
        waitMs(ms);
        fd = openEntry(entry);
        if (fd >= 0) {
            ++sReopenRecovered;
            port_log("dvd: entry %u opened again after %u tries", entry, attempt);
            return fd;
        }
        if (!retriableErrno(sLastOpenErrno)) break;
    }
    ++sReopenGaveUp;
    errno = sLastOpenErrno;
    return -1;
}

// A read failed with errno `error`: close the descriptor, open the file again and repeat the read.  `fd` is the new descriptor.
static ssize_t rereadAfterFailure(u32 entry, int error, int& fd, void* dst, size_t want, u64 offset) {
    if (!reopenOn()) {
        errno = error;
        return -1;
    }
    // (3.0) The first reopen is made whatever the error was: a descriptor the system took away can answer with any code (EINVAL,
    // EISDIR, ...), and a fresh descriptor's own failure decides whether to go on.
    lazyResumeDrop();
    unsigned attempt = 0;
    int last = retriableErrno(error) ? error : EBADF;
    for (unsigned ms : sReopenDelayMs) {
        if (attempt >= kReopenShortSteps && !recentResume()) break;
        ++attempt;
        dropOpenFile(entry);
        fd = -1;
        waitMs(ms);
        const int fresh = openEntry(entry);
        if (fresh < 0) {
            last = sLastOpenErrno;
            if (!retriableErrno(last)) break;
            continue;
        }
        fd = fresh;
        const ssize_t got = pread(fd, dst, want, (off_t)offset);
        if (got >= 0) {
            ++sReopenRecovered;
            port_log("dvd: read of entry %u failed (errno=%d), worked after reopening (try %u)", entry, error, attempt);
            return got;
        }
        last = errno;
        if (!retriableErrno(last)) break;
    }
    ++sReopenGaveUp;
    port_log("dvd: read of entry %u failed (errno=%d) and again after %u reopen tries (errno=%d)", entry, error, attempt, last);
    errno = last;
    return -1;
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

extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" int64_t port_player_input_age_ms(void) __attribute__((weak));
extern "C" int port_scene_loading_active(void) __attribute__((weak));  // (perf.cpp: the game's loading thread is building a scene)
extern "C" int port_is_default_thread(void) __attribute__((weak));     // (os_thread.cpp: the caller is the Wii default thread = the game thread)

// (firstuse24, GXV_FIRSTUSE_LOG, default 1; GXV_FIRSTUSE_DISC_MS, default 5) the first-use ledger's disc side: a read that took this long
// while the screen was open (a stage playing, no fade or pause menu) is data the game fetched lazily in play: an archive, a wave bank, an
// animation, a stream. One line each, at most 24 a minute; the totals in play are in the 5 s "dvd:" line only with diagnostics.
static void firstUseDiscNote(u32 entry, size_t bytes, u32 offset, int64_t t0, int64_t t1) {
    static const bool on = !getenv("GXV_FIRSTUSE_LOG") || atoi(getenv("GXV_FIRSTUSE_LOG")) != 0;
    static const int64_t minNs = (getenv("GXV_FIRSTUSE_DISC_MS") ? (int64_t)atoi(getenv("GXV_FIRSTUSE_DISC_MS")) : 5) * 1000000ll;
    if (!on || t1 - t0 < minNs || !port_scene_key || !port_screen_quiet || port_scene_key() == 0 || port_screen_quiet() != 0) return;
    // (disc26) A stage's own load is not "in play": its archives are read on the loading thread while the screen counts as open (the star select,
    // the fade). 707 of the 781 lines of five console logs were that; GXV_FIRSTUSE_DISC_LOAD=1 brings them back.
    static const bool withLoad = vgx::EnvInt("GXV_FIRSTUSE_DISC_LOAD", 0) != 0;
    if (!withLoad && port_scene_loading_active && port_scene_loading_active() != 0) return;
    static int64_t windowStart = 0;
    static unsigned lines = 0;
    if (t1 - windowStart > 60000000000ll) { windowStart = t1; lines = 0; }
    if (lines >= 24) return;
    ++lines;
    const int64_t age = port_player_input_age_ms ? port_player_input_age_ms() : -1;
    char since[40] = "no pad input yet";
    if (age >= 0) snprintf(since, sizeof since, "pad input %lld ms ago", (long long)age);
    port_log_summary("first-use: disc read in play: %.1f ms, %u KiB at %u of %s (%s)", (t1 - t0) / 1e6, (unsigned)(bytes / 1024), offset,
                     sFst[entry].hostPath.c_str() + std::min(sDataRoot.size(), sFst[entry].hostPath.size()), since);
}

static void noteRead(u32 entry, size_t bytes, u32 offset, int64_t t0, int64_t t1) {
    firstUseDiscNote(entry, bytes, offset, t0, t1);
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
// (disc26) four windows, one per recently read file (GXV_DVD_RA_SLOTS, 1..4, default 4; 1 = the old single window): the music stream and a
// wave bank used to evict each other's window and both read every piece from the card. A window is filled for a file only when its
// previous read ended exactly where this one began.
constexpr int kRaSlots = 4;
struct RaBuf {
    alignas(64) u8 data[kReadAheadMax];
};
static RaBuf sRaBuf[kRaSlots];
static port::iopol::RaTable<kRaSlots> sRa;  // (worker thread only)
static u32 sSeqEntry = ~0u;  // the MOVIE player's mark: the file of the last read and where it ended
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

static void pfDropAll();  // (disc26, below)
static void dropReadCaches() {
    sRa.ClearAll();
    sMovieWin.entry = ~0u;
    sSeqEntry = ~0u;
    pfDropAll();  // (the prefetch cache holds file contents read before the sleep; a card that changed under the console is read afresh)
}

// Worker thread, before each request: when the system reported a resume since the last one, forget every descriptor and
// window so the next read opens its file afresh.
static unsigned sResumeSeen, sAppResumeSeen;
static void noteResume() {
    if (!resumeResetOn()) return;
    const unsigned generation = port::ResumeGeneration();
    if (generation != sResumeSeen) {   // the console slept: drop everything before the first read
        sResumeSeen = generation;
        sResumeNs = port_host_time_ns();
        sLazyDropPending = false;
        dropAllOpenFiles();
        port_log("dvd: resume %u: cached files closed, read-ahead dropped", generation);
    }
    const unsigned app = port::ResumeAppGeneration();
    if (app != sAppResumeSeen) {   // a plain PS-button return: keep the caches (no loss of the learned prefetch); fix them only if something fails
        sAppResumeSeen = app;
        sResumeNs = port_host_time_ns();
        sLazyDropPending = true;
        port_log("dvd: app resume %u: caches kept, dropped only if a read fails in the next 20 s", app);
    }
}
// A read or open failed: when a PS-button return was just seen, close every cached descriptor and window once (the failing file's own
// descriptor is reopened by the caller anyway; the others may be as stale).
static void lazyResumeDrop() {
    if (!sLazyDropPending || !recentResume()) return;
    sLazyDropPending = false;
    dropAllOpenFiles();
    port_log("dvd: a read failed after an app resume: cached files closed, read-ahead dropped");
}

// ===========================================================================================================================================
// (disc26) Play-time disc policy: switches, long reads in pieces, the learned prefetch cache, and the "io:" census.
//
//   GXV_DVD_CHUNK_KIB      (default 512; 0 = off) a read longer than twice this is done in pieces of this size, and between two pieces a request
//                          of a more urgent priority (the music streams ask at priority 1, archives at 2) is served first. Before, the one
//                          drive worker was busy for the whole of a 2.6 MiB archive (305 ms measured, OceanRingPlanet.arc) and a music-stream
//                          read queued behind it. Costs one extra pread per piece (~1 ms on the card); nothing else changes.
//   GXV_DVD_RA_SLOTS      (default 4; 1 = one window) read-ahead windows, one per recently read file (see RaTable in port/io_policy.h).
//   GXV_IO_PREFETCH_MB    (default 0 = off) the learned prefetch cache: whole files the drive worker reads while it is idle, from the list
//                          GXV_IO_PREFETCH_LIST (lines "<stage key hex or *> <path>", e.g. "a8a904f7 /ObjectData/OceanRingPlanet.arc") when
//                          the stage's play window begins; a later read of the file is answered from memory. Needs the RAM: off until a
//                          console run shows the heap has it.
//   GXV_IO_PREFETCH_OUT   (default 0, ram33) 1: the prefetch buffers are their own main-memory blocks outside the C heap, taken only while
//                          GXV_IO_PREFETCH_FLOOR_MIB (48) of main memory stays free; a file that does not fit is passed over.
//   GXV_IO_LEDGER_FILE    (default none) the files a stage read while it PLAYED are appended here as prefetch-list lines (written when the
//                          window closes), so one play session writes the list the next one prefetches from.
//   GXV_IO_LINE           (default 1) the "io:" line; GXV_IO_LINE_S (default 60) the running line's period; GXV_IO_STREAM_LATE_MS (250).
static bool readAheadEnabled() {
    static const bool on = !getenv("PETARI_READAHEAD") || getenv("PETARI_READAHEAD")[0] != '0';
    return on;
}
static bool movieWindowEnabled() {
    static const bool on = !getenv("PETARI_MOVIEWIN") || getenv("PETARI_MOVIEWIN")[0] != '0';
    return on;
}
static size_t chunkBytes() {
    static const size_t c = (size_t)std::max(0, (int)vgx::EnvInt("GXV_DVD_CHUNK_KIB", 512)) * 1024u;
    return c;
}
static int raSlotsOn() {
    static const int n = std::min(kRaSlots, std::max(1, (int)vgx::EnvInt("GXV_DVD_RA_SLOTS", kRaSlots)));
    return n;
}
static size_t pfCapBytes() {
    static const size_t c = (size_t)std::max(0, (int)vgx::EnvInt("GXV_IO_PREFETCH_MB", 0)) * 1048576u;
    return c;
}

namespace ioc {
enum { kObj, kStage, kLayout, kStream, kWave, kOther, kN };
struct Cls {
    std::atomic<uint64_t> n{0}, ns{0}, bytes{0}, worstNs{0}, worstLatNs{0}, s20{0}, s50{0};
};
struct Block {
    std::atomic<uint64_t> n{0}, ns{0}, worstNs{0};
};
static std::atomic<int> active{0};  // a stage's play window is open
static Cls cls[kN];
static Block gameSync, otherSync, gameWait, hostJob, hostJobGame, openB;
static std::atomic<uint64_t> raHits{0}, yields{0}, cuts{0}, pfHits{0}, pfBytes{0}, pfFiles{0}, pfSkipped{0}, streamLate{0}, worstPacked{0};
static std::mutex lineMutex;  // the window's identity and the line
static char stageName[48];
static uint32_t stageKey;
static int64_t t0Ns, lastLineNs;
static uint64_t shaderRam0, shaderUser0, shaderMiss0;
static unsigned lines;
static uint32_t ledger[64];  // entries read in play (ArchiveData etc.), first read only
static unsigned ledgerN;
static std::mutex ledgerMutex;

static inline void maxU(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}
static inline void addBlock(Block& b, int64_t ns) {
    if (ns < 0) ns = 0;
    b.n.fetch_add(1, std::memory_order_relaxed);
    b.ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
    maxU(b.worstNs, static_cast<uint64_t>(ns));
}
static inline bool on() { return active.load(std::memory_order_relaxed) != 0; }
static int classOf(const FstEntry& fe) {
    if (fe.sub == 1) return kStream;
    if (fe.sub == 2) return kWave;
    switch (fe.cls) {
    case PORT_DISC_OBJECT: return kObj;
    case PORT_DISC_STAGE: return kStage;
    case PORT_DISC_LAYOUT: return kLayout;
    default: return kOther;
    }
}
// The drive worker, after a read that really touched the card (or memory): work = the worker's own time, queue = submit to start.
static void noteRead(u32 entry, size_t bytes, int64_t workNs, int64_t queueNs) {
    if (!on()) return;
    Cls& c = cls[classOf(sFst[entry])];
    if (workNs < 0) workNs = 0;
    if (queueNs < 0) queueNs = 0;
    c.n.fetch_add(1, std::memory_order_relaxed);
    c.ns.fetch_add(static_cast<uint64_t>(workNs), std::memory_order_relaxed);
    c.bytes.fetch_add(bytes, std::memory_order_relaxed);
    maxU(c.worstNs, static_cast<uint64_t>(workNs));
    maxU(c.worstLatNs, static_cast<uint64_t>(workNs + queueNs));
    if (workNs >= 20000000) c.s20.fetch_add(1, std::memory_order_relaxed);
    if (workNs >= 50000000) c.s50.fetch_add(1, std::memory_order_relaxed);
    const bool stream = sFst[entry].sub == 1;
    static const int64_t lateNs = static_cast<int64_t>(std::max(1, (int)vgx::EnvInt("GXV_IO_STREAM_LATE_MS", 250))) * 1000000ll;
    if (stream && workNs + queueNs >= lateNs) streamLate.fetch_add(1, std::memory_order_relaxed);
    const uint64_t packed = (static_cast<uint64_t>(workNs) << 24) | (entry & 0xFFFFFFu);
    if (packed > worstPacked.load(std::memory_order_relaxed) && sFst[entry].sub != 1) worstPacked.store(packed, std::memory_order_relaxed);
    if (sFst[entry].sub == 0 && sFst[entry].cls != PORT_DISC_OTHER) {  // a game file read in play: the ledger (first read of each)
        std::lock_guard<std::mutex> lk(ledgerMutex);
        bool seen = false;
        for (unsigned i = 0; i < ledgerN; ++i) seen = seen || ledger[i] == entry;
        if (!seen && ledgerN < sizeof ledger / sizeof ledger[0]) ledger[ledgerN++] = entry;
    }
}
static void reset() {
    for (Cls& c : cls) {
        c.n = 0; c.ns = 0; c.bytes = 0; c.worstNs = 0; c.worstLatNs = 0; c.s20 = 0; c.s50 = 0;
    }
    for (Block* b : {&gameSync, &otherSync, &gameWait, &hostJob, &hostJobGame, &openB}) {
        b->n = 0; b->ns = 0; b->worstNs = 0;
    }
    raHits = 0; yields = 0; cuts = 0; pfHits = 0; pfBytes = 0; pfFiles = 0; pfSkipped = 0; streamLate = 0; worstPacked = 0;
    std::lock_guard<std::mutex> lk(ledgerMutex);
    ledgerN = 0;
}
}  // namespace ioc

static std::string relName(u32 entry) {
    if (entry >= sFst.size()) return "?";
    const std::string& p = sFst[entry].hostPath;
    return p.substr(std::min(sDataRoot.size() + 7, p.size()));  // (past "<root>/files/")
}

// The prefetch list file's lines for this stage -> the entries to read ahead (worker thread).
static std::deque<u32> sPfQueue;        // wanted entries (sQueueMutex); ~0u = "read the list for the stage now"
static std::atomic<uint32_t> sPfStage{0};
static constexpr int kPfItems = 8;
static port::iopol::PfBudget<kPfItems> sPf;  // worker thread only
static u8* sPfData[kPfItems];
// ram33: GXV_IO_PREFETCH_OUT=1 (default 0): the prefetch cache's file buffers come from their own main-memory blocks outside the 96 MiB C heap
// (the console's extended memory is otherwise unused by it), and only while the system keeps GXV_IO_PREFETCH_FLOOR_MIB (default 48, 16..200)
// of free main memory after the block; below that the file is passed over (counted as "passed over"), never read into the C heap instead.
// Off: malloc as before. On a host build the switch changes nothing (malloc).
struct PfBuf { u8* p = nullptr; int uid = -1; };
static int sPfUid[kPfItems];
static bool pfOutside() {
    static const bool on = vgx::EnvInt("GXV_IO_PREFETCH_OUT", 0) != 0;
    return on;
}
static PfBuf pfAlloc(size_t size) {
    PfBuf r;
#if defined(__vita__)
    if (pfOutside()) {
        static const long floorMib = std::min(200L, std::max(16L, (long)vgx::EnvInt("GXV_IO_PREFETCH_FLOOR_MIB", 48)));
        SceKernelFreeMemorySizeInfo info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        const size_t bytes = (size + 4095u) & ~(size_t)4095u;
        if (sceKernelGetFreeMemorySize(&info) < 0 || (long long)info.size_user - (long long)bytes < floorMib * 1048576ll) return r;
        const SceUID uid = sceKernelAllocMemBlock("io_prefetch", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)bytes, nullptr);
        if (uid < 0) return r;
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) { sceKernelFreeMemBlock(uid); return r; }
        r.p = static_cast<u8*>(base);
        r.uid = uid;
        return r;
    }
#endif
    r.p = static_cast<u8*>(malloc(size));
    return r;
}
static void pfFreeBuf(u8* p, int uid) {
    if (!p) return;
#if defined(__vita__)
    if (uid >= 0) { sceKernelFreeMemBlock(uid); return; }
#endif
    (void)uid;
    free(p);
}
static bool pfWorkWaiting() { return pfCapBytes() > 0 && !sPfQueue.empty(); }  // (sQueueMutex held by the caller)
static int pfFind(u32 entry, u64 offset, size_t want) {
    if (!pfCapBytes()) return -1;
    const int i = sPf.Find(entry);
    if (i < 0 || offset + want > sPf.item[i].size) return -1;
    sPf.Touch(i);
    return i;
}
static void pfDropAll() {
    for (int i = 0; i < kPfItems; ++i) {
        if (sPfData[i]) { pfFreeBuf(sPfData[i], sPfUid[i]); sPfData[i] = nullptr; sPfUid[i] = -1; }
        sPf.Drop(i);
    }
}
static void serveUrgent(int runningPrio, DVDCommandBlock* outer);
// Read the list file's lines for the stage (key hex or "*"), queue the files (worker thread; a few lines of text).
static void pfReadList() {
    const char* path = getenv("GXV_IO_PREFETCH_LIST");
    if (!path || !*path) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    const uint32_t key = sPfStage.load(std::memory_order_relaxed);
    char line[300];
    unsigned queued = 0;
    while (fgets(line, sizeof line, f)) {
        char k[24], p[260];
        if (sscanf(line, "%23s %259s", k, p) != 2) continue;
        if (k[0] != '*' && (uint32_t)strtoul(k, nullptr, 16) != key) continue;
        const s32 e = DVDConvertPathToEntrynum(p);
        if (e < 0) continue;
        std::lock_guard<std::mutex> lk(sQueueMutex);
        sPfQueue.push_back((u32)e);
        ++queued;
    }
    fclose(f);
    if (queued) port_log_summary("io: prefetch: %u files listed for stage %08x in %s", queued, key, path);
}
// One piece of work for an idle drive: the next wanted file, read whole in pieces; abandoned (and asked for again later) the moment
// a request arrives.
static void prefetchStep() {
    u32 entry;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        if (sPfQueue.empty() || !sPending.empty()) return;
        entry = sPfQueue.front();
        sPfQueue.pop_front();
    }
    if (entry == ~0u) { pfReadList(); return; }
    const size_t cap = pfCapBytes();
    const s64 size = entry < sFst.size() && !sFst[entry].isDir ? entrySize(entry) : -1;
    if (!cap || size <= 0 || (size_t)size > cap || sPf.Find(entry) >= 0) { ioc::pfSkipped.fetch_add(1, std::memory_order_relaxed); return; }
    const int fd = openEntry(entry);  // (this is also the open-ahead: the descriptor stays in the cache)
    if (fd < 0) return;
    const PfBuf pb = pfAlloc((size_t)size);
    u8* const buf = pb.p;
    if (!buf) { ioc::pfSkipped.fetch_add(1, std::memory_order_relaxed); return; }
    const int64_t t0 = port_host_time_ns();
    size_t done = 0;
    while (done < (size_t)size) {
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            if (!sPending.empty()) { sPfQueue.push_front(entry); pfFreeBuf(buf, pb.uid); return; }  // a request: the card is not idle
        }
        const size_t n = port::iopol::NextPiece((size_t)size - done, 256 * 1024);
        const ssize_t got = pread(fd, buf + done, n, (off_t)done);
        if (got <= 0) { pfFreeBuf(buf, pb.uid); return; }
        done += (size_t)got;
    }
    uint32_t dropped[kPfItems];
    int nDropped = 0;
    const int slot = sPf.Admit(entry, (size_t)size, cap, dropped, &nDropped);
    (void)nDropped;
    for (int k = 0; k < kPfItems; ++k)  // (the files that made room: their buffers go)
        if (sPfData[k] && sPf.item[k].entry == ~0u) { pfFreeBuf(sPfData[k], sPfUid[k]); sPfData[k] = nullptr; sPfUid[k] = -1; }
    if (slot < 0) { pfFreeBuf(buf, pb.uid); return; }
    if (sPfData[slot]) pfFreeBuf(sPfData[slot], sPfUid[slot]);
    sPfData[slot] = buf;
    sPfUid[slot] = pb.uid;
    ioc::pfFiles.fetch_add(1, std::memory_order_relaxed);
    static unsigned logged;
    if (logged++ < 24)
        port_log_summary("io: prefetched %s (%u KiB) in %.0f ms while the card was idle", relName(entry).c_str(), (unsigned)(size / 1024), (port_host_time_ns() - t0) / 1e6);
}
extern "C" int port_dvd_prefetch(const char* path) {
    if (!pfCapBytes() || !path) return 0;
    const s32 e = DVDConvertPathToEntrynum(path);
    if (e < 0) return 0;
    PortHostAllocScope scope;  // (heap36: a game thread may call this: the deque's nodes are host memory, as in submit)
    std::lock_guard<std::mutex> lk(sQueueMutex);
    sPfQueue.push_back((u32)e);
    sQueueCv.notify_one();
    return 1;
}

// Between two pieces of a long read: serve what is more urgent. Worker thread only.
static void serveRequest(const ReadRequest& req, bool nested);
static void (*sTestPieceHook)(size_t done) = nullptr;  // host tests only
static void serveUrgent(int runningPrio, DVDCommandBlock* outer) {
    for (;;) {
        ReadRequest u;
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            if (sPending.empty() || !port::iopol::MayInterleave(sPending.front().priority, runningPrio)) return;
            u = sPending.front();
            sPending.pop_front();
            sActive = u.block;
        }
        ioc::yields.fetch_add(1, std::memory_order_relaxed);
        serveRequest(u, true);
        std::lock_guard<std::mutex> lk(sQueueMutex);
        sActive = outer;
    }
}

// One request, start to completion (the drive worker's loop calls it; so does serveUrgent() between the pieces of a long read: nested).
static void serveRequest(const ReadRequest& req, bool nested) {
    const bool readAheadOn = readAheadEnabled();
    const bool movieWindowOn = readAheadOn && movieWindowEnabled();
    DVDCommandBlock* b = req.block;
    s32 result;
    noteResume();
    int64_t workStart = port_host_time_ns();
    sTraceWaitNs = workStart - req.submitNs;
    loadsplit::g.queueNs.fetch_add(static_cast<uint64_t>(sTraceWaitNs > 0 ? sTraceWaitNs : 0), std::memory_order_relaxed);
    const size_t want = b->length;
    const u64 offset = b->offset;
    const bool movie = readAheadOn && isMovieEntry(req.entry);
    const bool fromMovieWin = movie && movieWindowOn && sMovieWin.entry == req.entry && offset >= sMovieWin.start &&
                              offset + want <= sMovieWin.start + sMovieWin.len;
    const int raSlot = (!movie && readAheadOn) ? sRa.Find(req.entry, offset, want) : -1;
    const bool fromReadAhead = raSlot >= 0;
    const int pfSlot = (!movie && !fromReadAhead) ? pfFind(req.entry, offset, want) : -1;  // (disc26) the learned prefetch cache
    const bool fromPrefetch = pfSlot >= 0;
    const int64_t queuedNs = workStart - req.submitNs;
    int64_t nestedNs = 0;  // time spent serving urgent requests between the pieces of this read (not this read's own)
    int fd = (fromReadAhead || fromMovieWin || fromPrefetch) ? -1 : openEntryRetry(req.entry);
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
    if (ioc::on() && sTraceOpenNs >= 1000000 && fd >= 0 && !fromReadAhead && !fromMovieWin && !fromPrefetch) ioc::addBlock(ioc::openB, sTraceOpenNs);
    if (fd < 0 && !fromReadAhead && !fromMovieWin && !fromPrefetch) {
        result = -1;
    } else {
        u8* dst = (u8*)b->addr;
        int64_t readStart = port_host_time_ns();
        ssize_t got;
        if (fromMovieWin) {
            memcpy(dst, sMovieWin.data + (offset - sMovieWin.start), want);
            got = (ssize_t)want;
            ++sMovieHits;
            loadsplit::g.readAheadHits.fetch_add(1, std::memory_order_relaxed);
        } else if (fromPrefetch) {
            memcpy(dst, sPfData[pfSlot] + offset, want);
            got = (ssize_t)want;
            ioc::pfHits.fetch_add(1, std::memory_order_relaxed);
            ioc::pfBytes.fetch_add(want, std::memory_order_relaxed);
        } else if (fromReadAhead) {
            memcpy(dst, sRaBuf[raSlot].data, want);
            got = (ssize_t)want;
            loadsplit::g.readAheadHits.fetch_add(1, std::memory_order_relaxed);
            ioc::raHits.fetch_add(1, std::memory_order_relaxed);
            if (++sReadAheadHits % 100 == 0) {
                port_log("dvd: read-ahead served %u reads from memory (%u sequential reads were not ready)", sReadAheadHits, sReadAheadMisses);
            }
        } else {
            const size_t chunk = (nested || movie) ? 0 : chunkBytes();
            if (chunk && want > 2 * chunk) {
                // (disc26) a long read in pieces: between two of them the more urgent requests (the music streams) go first
                size_t done = 0;
                got = 0;
                ioc::cuts.fetch_add(1, std::memory_order_relaxed);
                while (done < want) {
                    const size_t n = port::iopol::NextPiece(want - done, chunk);
                    ssize_t g = pread(fd, dst + done, n, (off_t)(offset + done));
                    if (g < 0) g = rereadAfterFailure(req.entry, errno, fd, dst + done, n, offset + done);
                    if (g < 0) { got = -1; break; }
                    done += (size_t)g;
                    got = (ssize_t)done;
                    if ((size_t)g < n) break;  // end of file
                    if (done < want) {
                        if (sTestPieceHook) sTestPieceHook(done);  // (host tests: a request arrives between two pieces)
                        const int64_t y0 = port_host_time_ns();
                        serveUrgent(req.priority, b);
                        const int64_t y1 = port_host_time_ns();
                        if (y1 - y0 > 100000) {  // something was served
                            nestedNs += y1 - y0;
                            fd = openEntry(req.entry);  // (cached; the urgent request may have used the descriptor table)
                            if (fd < 0) { got = -1; break; }
                        }
                    }
                }
                workStart += nestedNs;
                readStart += nestedNs;
            } else {
                got = pread(fd, dst, want, (off_t)offset);
                if (got < 0) got = rereadAfterFailure(req.entry, errno, fd, dst, want, offset);
            }
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
            noteDvdFailure("read", sFst[req.entry].hostPath.c_str(), error);
        } else {
            readEnd = port_host_time_ns();
            if ((size_t)got < want) {
                memset(dst + got, 0, want - (size_t)got);  // past EOF the drive returns adjacent data
            }
            b->transferredSize = b->length;
            result = (s32)b->length;
            sDvdBytesRead.fetch_add((uint64_t)want, std::memory_order_relaxed);
            const int64_t hookStart = port_host_time_ns();
            port_gpu_memory_written(dst, (uint32_t)want);  // models, textures, display lists: see vgx_galaxy.h
            sTraceGpuNs = port_host_time_ns() - hookStart;
        }
        {   // (cook24) the load-split census: this read's folder, bytes and the worker's time (open + read)
            const uint8_t cls = sFst[req.entry].cls;
            const int64_t spent = readEnd - workStart;
            loadsplit::g.reads[cls].fetch_add(1, std::memory_order_relaxed);
            if (result >= 0) loadsplit::g.bytes[cls].fetch_add(static_cast<uint64_t>(want), std::memory_order_relaxed);
            loadsplit::g.workerNs[cls].fetch_add(static_cast<uint64_t>(spent > 0 ? spent : 0), std::memory_order_relaxed);
            const uint64_t packed = (static_cast<uint64_t>(spent > 0 ? spent : 0) << 24) | (req.entry & 0xFFFFFFu);
            if (packed > loadsplit::g.slowest.load(std::memory_order_relaxed)) loadsplit::g.slowest.store(packed, std::memory_order_relaxed);
        }
        noteRead(req.entry, want, (u32)offset, readStart, readEnd);
        ioc::noteRead(req.entry, want, readEnd - workStart, queuedNs);
    }
    bool pendingNow;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        b->currTransferSize = (u32)result;
        if (!nested) sActive = nullptr;
        sDone.push_back(b);
        pendingNow = !sPending.empty();
    }
    port_irq_raise(PORT_IRQ_DVD);
    port_vclock_external_end();
    if (nested) return;  // (an urgent request between the pieces of a long read: no read-ahead of its own now, the long read goes on)

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
        const bool sequence = sRa.NoteRead(req.entry, offset, want);
        if (sequence && !pendingNow) {
            const s64 size = sFst[req.entry].size;
            const int pfd = (size > 0 && (s64)(offset + want) < size) ? (fd >= 0 ? fd : openEntry(req.entry)) : -1;
            if (pfd >= 0) {
                const int slot = raSlotsOn() == 1 ? 0 : sRa.SlotFor(req.entry);
                ssize_t got = pread(pfd, sRaBuf[slot].data, want, (off_t)(offset + want));
                if (got > 0) {
                    if ((size_t)got < want) {
                        memset(sRaBuf[slot].data + got, 0, want - (size_t)got);
                    }
                    sRa.Set(slot, req.entry, offset + want, want);
                } else {
                    sRa.Clear(slot);
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

static void workerMain() {
    PORT_QEMU_THREAD("dvd-worker");
    port_thread_class(2);
    port_thread_io(0, 0);  // GXV_CORE3_IO=1: the read-ahead worker tries the system core (port.h); off by default
    for (;;) {
        ReadRequest req;
        {
            std::unique_lock<std::mutex> lk(sQueueMutex);
            sQueueCv.wait(lk, [] { return !sPending.empty() || pfWorkWaiting(); });
            if (sPending.empty()) {
                lk.unlock();
                prefetchStep();  // (disc26) the card is idle: one piece of the learned prefetch
                continue;
            }
            req = sPending.front();
            sPending.pop_front();
            sActive = req.block;
        }
        serveRequest(req, false);
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

// (cook24, port/load_split.h) the census's readers. Counters only; nothing here touches the card.
extern "C" void port_load_split_disc(PortDiscSplit* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    for (int c = 0; c < PORT_DISC_CLASSES; ++c) {
        out->reads[c] = loadsplit::g.reads[c].load(std::memory_order_relaxed);
        out->bytes[c] = loadsplit::g.bytes[c].load(std::memory_order_relaxed);
        out->workerNs[c] = loadsplit::g.workerNs[c].load(std::memory_order_relaxed);
        out->blockedNs[c] = loadsplit::g.blockedNs[c].load(std::memory_order_relaxed);
    }
    out->probes = loadsplit::g.probes.load(std::memory_order_relaxed);
    out->asyncReads = loadsplit::g.asyncReads.load(std::memory_order_relaxed);
    out->queueNs = loadsplit::g.queueNs.load(std::memory_order_relaxed);
    out->opens = loadsplit::g.opens.load(std::memory_order_relaxed);
    out->openNs = loadsplit::g.openNs.load(std::memory_order_relaxed);
    out->stats = loadsplit::g.stats.load(std::memory_order_relaxed);
    out->statNs = loadsplit::g.statNs.load(std::memory_order_relaxed);
    out->readAheadHits = loadsplit::g.readAheadHits.load(std::memory_order_relaxed);
    out->decodes = loadsplit::g.decodes.load(std::memory_order_relaxed);
    out->decodeNs = loadsplit::g.decodeNs.load(std::memory_order_relaxed);
    out->decodeBytes = loadsplit::g.decodeBytes.load(std::memory_order_relaxed);
}
extern "C" int port_load_split_slowest(char* name, size_t size, double* ms) {
    const uint64_t packed = loadsplit::g.slowest.exchange(0, std::memory_order_relaxed);
    if (!packed) return 0;
    const u32 entry = static_cast<u32>(packed & 0xFFFFFFu);
    if (ms) *ms = static_cast<double>(packed >> 24) / 1e6;
    if (name && size) {
        name[0] = 0;
        if (entry < sFst.size()) {
            const std::string& p = sFst[entry].hostPath;
            const size_t cut = std::min(sDataRoot.size() + 7, p.size());  // (past "<root>/files/")
            snprintf(name, size, "%s", p.c_str() + cut);
        }
    }
    return 1;
}
extern "C" void port_load_note_decomp(uint32_t bytes, int64_t ns) {
    loadsplit::g.decodes.fetch_add(1, std::memory_order_relaxed);
    loadsplit::g.decodeBytes.fetch_add(bytes, std::memory_order_relaxed);
    loadsplit::g.decodeNs.fetch_add(static_cast<uint64_t>(ns > 0 ? ns : 0), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// (disc26) the "io:" census. The play window of a stage opens when its scene build ends (perf.cpp: port_scene_load_end) and closes when the
// next scene build begins; the counters are bumped by the drive worker, DVDReadPrio, the host-job caller and the game thread's file waits.
//   io: stage X (key K) play 83.2 s [final|so far]: <verdict>; disc reads 12 (1.2 MiB, drive 80 ms, worst 31 ms: ObjectData/Foo.arc);
//   by class (reads / MiB / drive ms / worst ms / >=20 ms / >=50 ms): ...; game thread waited for the card: sync reads 0 (0 ms, worst 0),
//   requested archives/files 0 (0 ms, worst 0); other guest threads in sync reads 3 (...); audio streams: 70 reads, worst latency 16 ms
//   (queue + card), 0 at >= 250 ms; read-ahead hits 70, long reads cut 2 (urgent requests served between pieces 5), prefetch 0 hits (...);
//   opens of 1 ms or more 3 (...); host jobs (saves, NAND) 0 (...), 0 on the game thread; shader files in play: 0 pack hits, 0 misses; ...
// This is the regression check: the verdict must be CLEAN and "game thread waited" 0 in a normal play window.
extern "C" void port_load_split_shader_files(uint64_t* ramHits, uint64_t* userHits, uint64_t* misses) __attribute__((weak));
static void ioLine(const char* tag) {
    static const bool on = vgx::EnvInt("GXV_IO_LINE", 1) != 0;
    std::lock_guard<std::mutex> lk(ioc::lineMutex);
    const int64_t now = port_host_time_ns();
    ioc::lastLineNs = now;
    if (!on) return;
    uint64_t n = 0, ns = 0, bytes = 0, worst = 0, s20 = 0, s50 = 0;
    for (const auto& c : ioc::cls) {
        n += c.n; ns += c.ns; bytes += c.bytes; worst = std::max<uint64_t>(worst, c.worstNs); s20 += c.s20; s50 += c.s50;
    }
    // (the log keeps about 1000 characters of a line on the console: the first line is the check, the second the detail)
    static const char* const kShort[ioc::kN] = {"Obj", "Stage", "Layout", "Strm", "Wave", "other"};
    char byClass[360];
    int len = 0;
    byClass[0] = 0;
    for (int i = 0; i < ioc::kN && len >= 0 && (size_t)len < sizeof byClass - 70; ++i) {
        const auto& c = ioc::cls[i];
        if (!c.n) continue;
        len += snprintf(byClass + len, sizeof byClass - (size_t)len, " %s %ur/%.0fms/worst %.0f%s;", kShort[i], (unsigned)c.n.load(), c.ns / 1e6, c.worstNs / 1e6,
                        c.s20 ? (c.s50 ? " (>=50: yes)" : " (>=20: yes)") : "");
    }
    const uint64_t wp = ioc::worstPacked.load();
    char worstName[80] = "-";
    if (wp) snprintf(worstName, sizeof worstName, "%s", relName((u32)(wp & 0xFFFFFFu)).c_str());
    uint64_t ram = 0, user = 0, miss = 0;
    if (port_load_split_shader_files) port_load_split_shader_files(&ram, &user, &miss);
    const double gameBlockedMs = (ioc::gameSync.ns + ioc::gameWait.ns) / 1e6;
    const double worstGameMs = std::max(ioc::gameSync.worstNs.load(), ioc::gameWait.worstNs.load()) / 1e6;
    const char* verdict = port::iopol::Verdict(gameBlockedMs, worstGameMs, ioc::cls[ioc::kStream].worstLatNs / 1e6, (unsigned)s50, (unsigned)s20);
    port_log_summary("io: stage %s (%08x) play %.1f s [%s]: %s; reads in play %u (%.1f MiB, %.0f ms, worst %.0f ms %s); game thread waited for the card: sync %u (%.0f ms, worst %.0f), "
                     "files %u (%.0f ms, worst %.0f); by class:%s stream latency worst %.0f ms, %u late; host jobs %u (%.0f ms, worst %.0f, %u on the game thread); "
                     "shader files in play %u hits, %u misses",
                     ioc::stageName, ioc::stageKey, (now - ioc::t0Ns) / 1e9, tag, verdict, (unsigned)n, bytes / 1048576.0, ns / 1e6, worst / 1e6, worstName,
                     (unsigned)ioc::gameSync.n.load(), ioc::gameSync.ns / 1e6, ioc::gameSync.worstNs / 1e6, (unsigned)ioc::gameWait.n.load(), ioc::gameWait.ns / 1e6,
                     ioc::gameWait.worstNs / 1e6, byClass[0] ? byClass : " none;", ioc::cls[ioc::kStream].worstLatNs / 1e6, (unsigned)ioc::streamLate.load(),
                     (unsigned)ioc::hostJob.n.load(), ioc::hostJob.ns / 1e6, ioc::hostJob.worstNs / 1e6, (unsigned)ioc::hostJobGame.n.load(),
                     (unsigned)(ram + user - ioc::shaderRam0 - ioc::shaderUser0), (unsigned)(miss - ioc::shaderMiss0));
    char ledgerText[300] = "";
    {
        std::lock_guard<std::mutex> l2(ioc::ledgerMutex);
        int k = 0;
        for (unsigned i = 0; i < ioc::ledgerN && k >= 0 && (size_t)k < sizeof ledgerText - 70; ++i)
            k += snprintf(ledgerText + k, sizeof ledgerText - (size_t)k, "%s%s", i ? ", " : "", relName(ioc::ledger[i]).c_str());
    }
    port_log_summary("io-more: stage %s: other guest threads in sync reads %u (%.0f ms, worst %.0f); stream late limit %d ms; read-ahead hits %u; long reads cut %u (urgent requests "
                     "served between pieces %u); prefetch %u hits (%.1f MiB), %u files read ahead, %u passed over; opens of 1 ms or more %u (%.0f ms, worst %.0f); game files read in play: %s",
                     ioc::stageName, (unsigned)ioc::otherSync.n.load(), ioc::otherSync.ns / 1e6, ioc::otherSync.worstNs / 1e6,
                     (int)vgx::EnvInt("GXV_IO_STREAM_LATE_MS", 250), (unsigned)ioc::raHits.load(), (unsigned)ioc::cuts.load(), (unsigned)ioc::yields.load(),
                     (unsigned)ioc::pfHits.load(), ioc::pfBytes / 1048576.0, (unsigned)ioc::pfFiles.load(), (unsigned)ioc::pfSkipped.load(),
                     (unsigned)ioc::openB.n.load(), ioc::openB.ns / 1e6, ioc::openB.worstNs / 1e6, ledgerText[0] ? ledgerText : "none");
    ++ioc::lines;
}
extern "C" void port_io_play_begin(const char* stage, uint32_t key) {
    {
        std::lock_guard<std::mutex> lk(ioc::lineMutex);
        ioc::reset();
        snprintf(ioc::stageName, sizeof ioc::stageName, "%s", stage && *stage ? stage : "?");
        ioc::stageKey = key;
        ioc::t0Ns = ioc::lastLineNs = port_host_time_ns();
        ioc::lines = 0;
        uint64_t ram = 0, user = 0, miss = 0;
        if (port_load_split_shader_files) port_load_split_shader_files(&ram, &user, &miss);
        ioc::shaderRam0 = ram; ioc::shaderUser0 = user; ioc::shaderMiss0 = miss;
    }
    ioc::active.store(1, std::memory_order_release);
    if (pfCapBytes()) {  // the learned prefetch: the worker reads the list for this stage when the card is idle
        sPfStage.store(key, std::memory_order_relaxed);
        PortHostAllocScope scope;  // (heap36: the game thread, at the end of a stage's load (GXV_IO_PREFETCH_MB): a new deque node here came from the scene's heap)
        std::lock_guard<std::mutex> lk(sQueueMutex);
        sPfQueue.push_back(~0u);
        sQueueCv.notify_one();
    }
}
extern "C" void port_io_play_end(void) {
    if (!ioc::on()) return;
    ioc::active.store(0, std::memory_order_release);
    ioLine("final");
    static const char* ledgerFile = getenv("GXV_IO_LEDGER_FILE");
    if (ledgerFile && *ledgerFile) {  // the files this stage read while it played, as lines of the prefetch list
        std::lock_guard<std::mutex> lk(ioc::ledgerMutex);
        if (ioc::ledgerN) {
            if (FILE* f = fopen(ledgerFile, "a")) {
                if (ftell(f) < 65536)  // (capped: a list for one card's play, not a log)
                    for (unsigned i = 0; i < ioc::ledgerN; ++i) fprintf(f, "%08x /%s\n", ioc::stageKey, relName(ioc::ledger[i]).c_str());
                fclose(f);
            }
        }
    }
}
extern "C" void port_io_tick(void) {
    if (!ioc::on()) return;
    static unsigned calls;
    if ((++calls & 63u) != 0) return;
    static const int periodS = vgx::EnvInt("GXV_IO_LINE_S", 60);
    if (periodS <= 0 || port_host_time_ns() - ioc::lastLineNs < static_cast<int64_t>(periodS) * 1000000000ll) return;
    ioLine("so far");
}
extern "C" void port_io_note_wait(int64_t ns) {
    if (ioc::on() && port_is_default_thread && port_is_default_thread()) ioc::addBlock(ioc::gameWait, ns);
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

static void submit(DVDCommandBlock* b, u32 entry, s32 priority) {
    b->state = kStateBusy;
    b->transferredSize = 0;
    // The host drive worker frees deque nodes while it does not own the guest CPU.
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> lk(sQueueMutex);
    // The SDK serves priorities 0..3, FIFO within each. Invalid priorities were
    // ignored by this port; keep accepting them as ordinary archive priority 2.
    if (priority < 0 || priority > 3) priority = 2;
    auto next = sPending.end();
    while (next != sPending.begin() && std::prev(next)->priority > priority) --next;
    sPending.insert(next, {b, entry, port_host_time_ns(), priority});
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
    if (ioc::on()) {  // (disc26) the "io:" census: a save / NAND call in play, and whether the game thread was the one that sat in it
        ioc::addBlock(ioc::hostJob, port_host_time_ns() - t0);
        if (port_is_default_thread && port_is_default_thread()) ioc::addBlock(ioc::hostJobGame, port_host_time_ns() - t0);
    }
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
        // (bootio: readFst has already tried for 3 s and logged the SCE code. The message goes on the loading screen whether or not GXV_BOOTDATA is on.)
        static const char kMissing[] = "Game file list is unreadable. Restore converted game data and relaunch; saves unchanged.";
        if (port_boot_data_report(kMissing) || (port_vgx_vita_fail && port_vgx_vita_fail(kMissing)))
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
        static std::atomic<unsigned> sNotFound{0};  // (luigi24: the first few in the basic log; a missing archive must name itself there)
        if (sNotFound.fetch_add(1, std::memory_order_relaxed) < 16) port_log_summary("file-missing: DVDOpen: no such file in the file table: %s", fileName);
        return FALSE;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    (void)fileInfo;
    return TRUE;
}

static std::atomic<uint64_t> sXsGameSyncN{0}, sXsGameSyncNs{0};  // (xskip27)
BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    loadsplit::g.asyncReads.fetch_add(1, std::memory_order_relaxed);
    fileInfo->callback = callback;
    DVDCommandBlock* b = &fileInfo->cb;
    b->addr = addr;
    b->length = (u32)length;
    b->offset = (u32)offset;
    b->callback = fileCallback;
    submit(b, fileInfo->startAddr, prio);
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
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
    const int64_t lsT0 = port_host_time_ns();  // (cook24) how long this caller sits in the read, for the load-split census
    if (length <= 32 && offset == 0) loadsplit::g.probes.fetch_add(1, std::memory_order_relaxed);
    submit(b, fileInfo->startAddr, prio);
    if (gapThr > 0) {
        const int64_t gapSubmit = port_host_time_ns() - gapT0;
        static unsigned sLogged;
        if ((gapPre >= gapThr || gapSubmit >= gapThr / 2) && sLogged < 200) {
            ++sLogged;
            port_log("dvdread: %.1f ms since the last safe point when called, submit took %.1f ms; %d bytes at %d of %s; callers %p %p %p (port_irq_poll %p)",
                     gapPre / 1e6, gapSubmit / 1e6, (int)length, (int)offset, sFst[fileInfo->startAddr].hostPath.c_str(), __builtin_return_address(0),
                     (void*)0, (void*)0 /* (pgo27: return_address(1..2) walk the frame-pointer chain, which -fomit-frame-pointer builds do not keep) */, (void*)&port_irq_poll);
        }
    }
    BOOL en = OSDisableInterrupts();
    while (b->state == kStateBusy || b->state == kStateWaiting) {
        OSSleepThread(&sSyncQueue);
    }
    OSRestoreInterrupts(en);
    {
        const int64_t waited = port_host_time_ns() - lsT0;
        loadsplit::g.blockedNs[sFst[fileInfo->startAddr].cls].fetch_add(static_cast<uint64_t>(waited > 0 ? waited : 0), std::memory_order_relaxed);
        if (ioc::on()) ioc::addBlock(port_is_default_thread && port_is_default_thread() ? ioc::gameSync : ioc::otherSync, waited);  // (disc26)
        if (port_is_default_thread && port_is_default_thread()) {  // (xskip27: always on, two relaxed adds: the game thread's synchronous reads, for the "xskip:" lines)
            sXsGameSyncN.fetch_add(1, std::memory_order_relaxed);
            sXsGameSyncNs.fetch_add(static_cast<uint64_t>(waited > 0 ? waited : 0), std::memory_order_relaxed);
        }
    }
    return b->state == kStateEnd ? (s32)b->transferredSize : -1;
}
// (xskip27) cumulative: asynchronous reads asked for, and the game thread's synchronous reads (count, ms waited), any thread
extern "C" void port_dvd_xskip_counters(unsigned long long* asyncReads, unsigned long long* gameSyncN, double* gameSyncMs) {
    if (asyncReads) *asyncReads = loadsplit::g.asyncReads.load(std::memory_order_relaxed);
    if (gameSyncN) *gameSyncN = sXsGameSyncN.load(std::memory_order_relaxed);
    if (gameSyncMs) *gameSyncMs = static_cast<double>(sXsGameSyncNs.load(std::memory_order_relaxed)) / 1e6;
}

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        for (auto it = sPending.begin(); it != sPending.end(); ++it) {
            if (it->block == block) {
                sPending.erase(it);
                removed = true;
                // (armhf/qemu harness: the drive worker will not complete it, so submit's external_begin ends here; compiled to
                // nothing on the Vita. From Producdevity's fork, 2a1d164.)
                port_vclock_external_end();
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
                // (armhf/qemu harness: the drive worker will not complete it, so submit's external_begin ends here; compiled to
                // nothing on the Vita. From Producdevity's fork, 2a1d164.)
                port_vclock_external_end();
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
