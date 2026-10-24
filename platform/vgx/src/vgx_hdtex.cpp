// HD texture replacement (see vgx_hdtex.h; pack format: tools/hdtex/README.md).
//
// The pack's index is read once at start-up. When the texture cache uploads a guest texture
// (first bind, or its bytes changed), Replace() is asked: if the texture's format and size occur
// in the pack, its level-0 texels (and palette) are hashed and looked up; a hit hands the back
// end the pack's image (DXT1/DXT5 with its own mip chain) in place of the decoded original.
// Never consulted for EFB copies (the cache answers those before), and never per frame.
// The guest-visible dimensions (TEV texel sizes, texture matrices) stay the original ones: only
// the back-end texture is larger.
#include "vgx_hdtex.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx::hdtex {

namespace {

constexpr uint8_t kPackDxt1 = 1, kPackDxt5 = 3;

#pragma pack(push, 1)
struct PackHeader {
    char magic[4];       // "HDTX"
    uint32_t version;    // 1
    uint32_t count;      // index entries
    uint32_t indexOff;   // PackEntry[count], sorted by key
    uint32_t namesOff;   // NUL-terminated names (for the log)
    uint32_t dataOff;
    uint32_t fileSize;
    uint32_t reserved;
};
struct PackEntry {
    uint64_t key;        // HdKey() of the original
    uint32_t off, size;  // all levels, level 0 first, each a whole number of 4x4 blocks
    uint16_t w, h;       // level 0 of the replacement
    uint16_t origW, origH;
    uint8_t format;      // kPackDxt1 / kPackDxt5
    uint8_t levels;      // down to 1x1
    uint8_t origFmt;     // GX texture format of the original
    uint8_t flags;       // bit 0: has alpha
    uint32_t nameOff;    // into the names
};
#pragma pack(pop)
static_assert(sizeof(PackHeader) == 32 && sizeof(PackEntry) == 32, "pack layout");

void (*g_log)(const char*, ...) = nullptr;
int g_mode = 0; // 0 off, 1 replace, 2 dry run, 3 identification only (GXV_HDTEX=0 with GXV_CHARAA on)
FILE* g_file = nullptr;
std::vector<PackEntry> g_index;
std::vector<uint64_t> g_shapes; // sorted ShapeKey()s of the originals: the filter in front of the hash
std::vector<char> g_names;
std::vector<uint8_t> g_preload; // the whole pack, when GXV_HDTEX_PRELOAD allows
uint64_t g_budget = 16ull << 20;
uint8_t g_mipFilter = FILT_LIN_MIP_NEAR;
long g_logLeft = 48;
std::atomic<uint64_t> g_hashed{0}, g_hits{0}, g_refused{0}, g_bytesRead{0}, g_lookupNs{0}, g_readNs{0}, g_readMaxNs{0}, g_peakLive{0};

inline uint64_t ShapeKey(uint32_t fmt, uint32_t w, uint32_t h) { return (static_cast<uint64_t>(fmt) << 32) | (w << 16) | h; }

// Must match tex_key() in tools/hdtex/build_pack.py.
inline uint64_t HdKey(const uint8_t* level0, uint32_t bytes, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut, uint32_t tlutBytes) {
    uint64_t k = XXH3_64bits_withSeed(level0, bytes, static_cast<uint64_t>(fmt) | (static_cast<uint64_t>(w) << 8) | (static_cast<uint64_t>(h) << 20));
    if (tlut && tlutBytes) k = XXH3_64bits_withSeed(tlut, tlutBytes, k);
    return k;
}

inline uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline uint32_t BlockBytes(uint8_t format) { return format == kPackDxt5 ? 16u : 8u; }
inline uint32_t LevelBytes(uint8_t format, uint32_t w, uint32_t h) { return ((w + 3) / 4) * ((h + 3) / 4) * BlockBytes(format); }

const char* NameOf(const PackEntry& e) {
    return e.nameOff < g_names.size() ? &g_names[e.nameOff] : "?";
}

void Note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Note(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log("%s", buf);
    else std::fprintf(stderr, "%s\n", buf);
}

bool ReadAt(uint32_t off, void* dst, uint32_t n) {
    if (!g_preload.empty()) {
        if (static_cast<uint64_t>(off) + n > g_preload.size()) return false;
        std::memcpy(dst, g_preload.data() + off, n);
        return true;
    }
    if (!g_file || std::fseek(g_file, static_cast<long>(off), SEEK_SET) != 0) return false;
    return std::fread(dst, 1, n, g_file) == n;
}

// The texture cache's hook (player thread).
bool Replace(const TexReplaceQuery& q, TexUpload& up) {
    // (a dozen shapes: a few compares for the uploads that cannot be in the pack, not timed)
    if (!std::binary_search(g_shapes.begin(), g_shapes.end(), ShapeKey(q.fmt, q.width, q.height))) return false;
    const uint64_t t0 = NowNs();
    g_hashed.fetch_add(1, std::memory_order_relaxed);
    const uint64_t key = HdKey(q.data, TexLevelSize(q.width, q.height, q.fmt), q.fmt, q.width, q.height, q.tlut, q.tlutBytes);
    auto it = std::lower_bound(g_index.begin(), g_index.end(), key, [](const PackEntry& e, uint64_t k) { return e.key < k; });
    const uint64_t t1 = NowNs();
    g_lookupNs.fetch_add(t1 - t0, std::memory_order_relaxed);
    if (it == g_index.end() || it->key != key || it->origW != q.width || it->origH != q.height || it->origFmt != q.fmt) return false;
    if (q.inPack) *q.inPack = true; // (GXV_CHARAA: one of the characters' and key items' textures, replaced or not)
    if (g_mode == 3) return false;   // (identification only)
    const PackEntry& e = *it;
    // The levels to hand over: all of them, or (a back end that cannot take compressed levels
    // below one block: Caps::mipClamp, the Vita) down to the last level of at least 4x4.
    uint32_t levels = 1, bytes = LevelBytes(e.format, e.w, e.h);
    for (uint32_t lw = e.w, lh = e.h; levels < e.levels; ++levels) {
        if (q.mipClamp && ((lw >> 1) < 4 || (lh >> 1) < 4)) break;
        lw = lw > 1 ? lw >> 1 : 1;
        lh = lh > 1 ? lh >> 1 : 1;
        bytes += LevelBytes(e.format, lw, lh);
    }
    if (bytes > e.size) {
        Note("hdtex: %s: entry smaller than its levels (%u < %u bytes): pack damaged, not used", NameOf(e), e.size, bytes);
        return false;
    }
    if (q.liveBytes + bytes > g_budget) {
        if (g_refused.fetch_add(1, std::memory_order_relaxed) < 8)
            Note("hdtex: %s not replaced: %llu KiB of HD textures alive, budget %llu KiB (GXV_HDTEX_BUDGET)", NameOf(e),
                 static_cast<unsigned long long>(q.liveBytes >> 10), static_cast<unsigned long long>(g_budget >> 10));
        return false;
    }
    g_hits.fetch_add(1, std::memory_order_relaxed);
    if (g_logLeft > 0) {
        --g_logLeft;
        Note("hdtex: %s %ux%u -> %ux%u %s, %u levels, %u KiB%s", NameOf(e), q.width, q.height, e.w, e.h, e.format == kPackDxt5 ? "DXT5" : "DXT1", levels,
             bytes >> 10, g_mode == 2 ? " (dry run)" : "");
    }
    if (g_mode == 2) return false;
    std::vector<uint8_t> blob(bytes);
    const bool ok = ReadAt(e.off, blob.data(), bytes);
    const uint64_t rd = NowNs() - t1;
    g_readNs.fetch_add(rd, std::memory_order_relaxed);
    if (rd > g_readMaxNs.load(std::memory_order_relaxed)) g_readMaxNs.store(rd, std::memory_order_relaxed);
    if (!ok) {
        Note("hdtex: %s: read failed (offset %u, %u bytes)", NameOf(e), e.off, bytes);
        return false;
    }
    g_bytesRead.fetch_add(bytes, std::memory_order_relaxed);
    up.width = e.w;
    up.height = e.h;
    up.format = e.format == kPackDxt5 ? TF_DXT5 : TF_DXT1;
    up.genMips = false;
    up.levels.clear();
    up.levels.reserve(levels);
    const uint8_t* src = blob.data();
    for (uint32_t l = 0, lw = e.w, lh = e.h; l < levels; ++l) {
        const uint32_t n = LevelBytes(e.format, lw, lh);
        up.levels.emplace_back(src, src + n);
        src += n;
        lw = lw > 1 ? lw >> 1 : 1;
        lh = lh > 1 ? lh >> 1 : 1;
    }
    up.authored = static_cast<uint8_t>(levels);
    const uint64_t live = q.liveBytes + bytes;
    if (live > g_peakLive.load(std::memory_order_relaxed)) g_peakLive.store(live, std::memory_order_relaxed);
    return true;
}

} // namespace

void Install(void (*log)(const char* fmt, ...)) {
    g_log = log;
    g_mode = static_cast<int>(EnvInt("GXV_HDTEX", 1));
    if (g_mode <= 0 && CharaAAMode() != 0) {
        // GXV_CHARAA needs the pack's list of character and key-item textures, not its images: the index is read, the
        // originals are uploaded (mode 3, identification only; nothing is counted as replaced).
        g_mode = 3;
    } else if (g_mode <= 0) {
        Note("hdtex: off (GXV_HDTEX=0)");
        return;
    } else if (g_mode != 2) {
        g_mode = 1;
    }
    const char* path = std::getenv("GXV_HDTEX_PATH");
#if defined(__vita__) || defined(__VGX_VITA__)
    if (!path || !*path) path = "ux0:data/smg/hdtex.pack";
#else
    if (!path || !*path) path = "hdtex.pack";
#endif
    const uint64_t t0 = NowNs();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        Note("hdtex: no pack at %s: textures as the game has them", path);
        g_mode = 0;
        return;
    }
    PackHeader h{};
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && !std::memcmp(h.magic, "HDTX", 4) && h.version == 1 && h.count < 65536 &&
              h.indexOff >= sizeof h && h.namesOff >= h.indexOff + h.count * sizeof(PackEntry) && h.dataOff >= h.namesOff;
    if (ok) {
        g_index.resize(h.count);
        g_names.resize(h.dataOff - h.namesOff + 1, 0);
        ok = std::fseek(f, static_cast<long>(h.indexOff), SEEK_SET) == 0 && std::fread(g_index.data(), sizeof(PackEntry), h.count, f) == h.count &&
             std::fseek(f, static_cast<long>(h.namesOff), SEEK_SET) == 0 && std::fread(g_names.data(), 1, h.dataOff - h.namesOff, f) == h.dataOff - h.namesOff;
        for (size_t i = 0; ok && i < g_index.size(); ++i) {
            const PackEntry& e = g_index[i];
            ok = (i == 0 || g_index[i - 1].key < e.key) && (e.format == kPackDxt1 || e.format == kPackDxt5) && e.levels >= 1 && e.w && e.h &&
                 !(e.w & (e.w - 1)) && !(e.h & (e.h - 1)) && static_cast<uint64_t>(e.off) + e.size <= h.fileSize;
        }
    }
    if (!ok) {
        Note("hdtex: %s is not a usable pack (version 1 expected): textures as the game has them", path);
        std::fclose(f);
        g_index.clear();
        g_mode = 0;
        return;
    }
    for (const PackEntry& e : g_index) g_shapes.push_back(ShapeKey(e.origFmt, e.origW, e.origH));
    std::sort(g_shapes.begin(), g_shapes.end());
    g_shapes.erase(std::unique(g_shapes.begin(), g_shapes.end()), g_shapes.end());
    g_budget = static_cast<uint64_t>(EnvInt("GXV_HDTEX_BUDGET", 16)) << 20;
    g_mipFilter = EnvInt("GXV_HDTEX_TRILINEAR", 0) ? FILT_LIN_MIP_LIN : FILT_LIN_MIP_NEAR;
    g_logLeft = EnvInt("GXV_HDTEX_LOG", 48);
    // (default: the whole pack, 4.5 MiB today, is read here, so that no texture's first use
    // during play waits on the memory card: a first Bee Mushroom would otherwise read 100 KiB)
    const uint64_t preload = g_mode == 3 ? 0 : static_cast<uint64_t>(EnvInt("GXV_HDTEX_PRELOAD", 8)) << 20;
    if (preload && h.fileSize <= preload) {
        g_preload.resize(h.fileSize);
        if (std::fseek(f, 0, SEEK_SET) != 0 || std::fread(g_preload.data(), 1, h.fileSize, f) != h.fileSize) g_preload.clear();
    }
    if (!g_preload.empty()) std::fclose(f);
    else g_file = f;
    SetTextureReplaceHook(&Replace, g_mipFilter);
    Note("hdtex: %s: %u textures in %u shapes, %u KiB; opened in %.1f ms%s; budget %llu MiB%s", path, h.count, static_cast<unsigned>(g_shapes.size()),
         h.fileSize >> 10, (NowNs() - t0) / 1e6, g_preload.empty() ? ", data read on first use" : ", whole pack preloaded",
         static_cast<unsigned long long>(g_budget >> 20), g_mode == 2 ? "; DRY RUN (GXV_HDTEX=2): originals uploaded"
                                                         : (g_mode == 3 ? "; IDENTIFICATION ONLY (GXV_HDTEX=0, GXV_CHARAA on): originals uploaded" : ""));
}

void GetCounters(Counters& c) {
    c.hashed = g_hashed.load(std::memory_order_relaxed);
    c.hits = g_hits.load(std::memory_order_relaxed);
    c.refused = g_refused.load(std::memory_order_relaxed);
    c.bytesRead = g_bytesRead.load(std::memory_order_relaxed);
    c.lookupNs = g_lookupNs.load(std::memory_order_relaxed);
    c.readNs = g_readNs.load(std::memory_order_relaxed);
    c.readMaxNs = g_readMaxNs.load(std::memory_order_relaxed);
    c.peakLive = g_peakLive.load(std::memory_order_relaxed);
    c.entries = static_cast<uint32_t>(g_index.size());
}

} // namespace vgx::hdtex
