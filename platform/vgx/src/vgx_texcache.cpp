// Front-end texture manager: decides which back-end texture a texmap resolves
// to and queues uploads when the guest bytes are new or have changed.
#include "vgx_automip.h"
#include "vgx_guestmem.h"
#include "vgx_state.h"
#include "vgx_j3n2.h"
#include "vgx_texcomp.h"
#include "vgx_texwarm.h"
#include "vgx_texwarm_id.h" // (texwarm30: GXV_TEXWARM_KEY)
#include "port/smoothstart.h" // (openstall26: GXV_SMOOTHSTART, the replay governor)
#include "vgx_texdecode.h"
#include "vgx_twocore.h"
#include "vgx_arena.h" // (heap36: arena::HostAllocScope)

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
extern "C" int port_card_write_async(const char* path, int mode, const void* data, unsigned bytes) __attribute__((weak));  // (hitch30: platform/src/os/os_alarm.cpp, the card-write queue)

// (GXV_STUTTER_LOG in vgx_backend_gl.cpp) the front end's time spent decoding and converting textures for upload, in microseconds,
// read by the back end's 5-s "stutter-5s:" line (any thread; relaxed).
static std::atomic<uint64_t> g_texDecodeUs{0};
extern "C" uint64_t vgx_tex_decode_us(void) { return g_texDecodeUs.load(std::memory_order_relaxed); }
// (endwait26, GXV_ENDWAIT_LOG) how many textures that was and the KiB of source texels (cumulative; the front end's per-picture record takes differences)
static std::atomic<uint64_t> g_texDecodeN{0}, g_texDecodeBytes{0};
extern "C" void vgx_tex_decode_counts(uint64_t* n, uint64_t* bytes) {
    *n = g_texDecodeN.load(std::memory_order_relaxed);
    *bytes = g_texDecodeBytes.load(std::memory_order_relaxed);
}
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2))); // the basic log, when the platform layer has one
// (handover26, GXV_PREWARM_RES) each texture the front end decodes for a first upload, with the time it took: the back end's census of what a stage's first seconds make (any thread, relaxed)
extern "C" void vgx_res_tex_note(uint32_t fmt, uint32_t w, uint32_t h, uint32_t bytes, uint32_t us) __attribute__((weak));
// (texage24, GXV_TEX_SCENEDROP=3) whether a stage is being played and no fade or pause menu hides it: the back end's ScreenLive() without the splash test.
// Any thread; weak, so that a host test without the game counts every picture as in play.
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_serial(void) __attribute__((weak)); // (texwarm30: counts the scenes the game has begun to build; the candidates of the game's walk carry it)
// (openstall26, GXV_SMOOTHSTART) the stage being loaded / the load itself (platform/src/port/perf.cpp), whether the closed screen is held for the warm-up and the back end's cumulative
// texture uploads (vgx_backend_gl.cpp); weak so that a host test without them replays as before.
extern "C" uint32_t port_scene_loading_key(void) __attribute__((weak));
extern "C" int port_scene_loading_active(void) __attribute__((weak));
extern "C" int vgx_smooth_hold_active(void) __attribute__((weak));
extern "C" int vgx_be_tex_totals(uint64_t* kib, double* ms) __attribute__((weak));

#include "vgx_la88_gate.h"

namespace vgx {

// GXV_LA88_MIPS (revisit23, 8 Nov 2026; gxm23's "LA88 with mips" next step): MIPMAPPED I4 / I8 / IA4 / IA8 textures go to the back end
// as LA88 (2 bytes a texel) with the levels made by the back end (genMips), as their non-mipmapped kind already does (GXV_LA88),
// instead of RGBA8 (4 bytes): half the memory and half the texture-read traffic for those textures. Today such a texture is decoded
// to RGBA8 = (I, I, I, A) and its levels are made by vitaGL's transfer-unit downscale of level 0 (glGenerateMipmap: U8U8U8U8_ABGR, the
// GX levels are not used: GXV_MIPGEN); as LA88 = (I, A) its levels are made by the same downscale in U8U8_GR. The sampler returns
// (L, L, L, A) for A8L8, so the pictures are identical exactly when the transfer unit averages each 8-bit channel of U8U8 as it does
// in U8U8U8U8. That is CHECKED ON THE CONSOLE, once, on the back end's thread before the first such texture is used
// (vgx_host_galaxy_vita.cpp vgxvita_la88MipsExact: 64x64 random (L, A) and its (L, L, L, A) twin downscaled 3 levels both ways, every
// byte compared). If it is not exact (or the probe cannot run), the back end expands each LA88 upload back to the RGBA8 bytes the old
// path made (exact: tests/vgx_la88_mips.cpp), logs it once, and the front end stops making them.
//   GXV_LA88_MIPS=0 off; 1 (Vita default) gated eligible chains; 2 bypasses the probe (host tests only).
// Log (basic log, with the texture-format line, GXV_TEXFMT_LOG): "texture uploads: N as LA88 (M MB)" counts them with the others;
// the back end says once "GXV_LA88_MIPS: the transfer unit's U8U8 downscale is exact ..." or "... NOT exact ...: RGBA8 as before".
std::atomic<int> g_la88MipsState{0}; // 0 not probed yet, 1 exact (probe passed or =2), -1 not exact / no probe: RGBA8 as before
void La88MipsVerdict(int state) { g_la88MipsState.store(state, std::memory_order_relaxed); } // (the back end, after its probe)
int La88MipsMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_LA88_MIPS", 0);
        return v == 1 || v == 2 ? v : 0L;
    }()));
    return m;
}


// ---- pix23 (GXV_PIX_ZERO / GXV_PIX_ATPASS): the channel ranges of uploaded textures (TexChannelRange, vgx_state.h)
bool PixTexRangesOn() {
    // (heavyopt25: also for GXV_PIX_CENSUS and GXV_PIX_BLENDOFF. Without ranges every textured draw's alpha is 0..1, so the c57 census runs, which set neither
    // ZERO nor ATPASS, showed "provably always pass 0" and "alpha exactly 1: 0" for every stage: no proof was possible, not "none exists".)
    // (defaults48: GXV_PIX_ZERO=3, the lean way, records none: its proof needs no texture range)
    const bool on = VGX_KNOB((EnvInt("GXV_PIX_ZERO", 0) != 0 && EnvInt("GXV_PIX_ZERO", 0) != 3) || EnvInt("GXV_PIX_ATPASS", 0) != 0 || EnvInt("GXV_PIX_CENSUS", 0) != 0 || EnvInt("GXV_PIX_BLENDOFF", 0) != 0) != 0;
    return on;
}
namespace {
struct TexRange {
    uint8_t lo[4], hi[4];
    bool known;
};
std::vector<TexRange> g_texRange; // by back-end texture id (ids are never reused); front end thread only
constexpr uint32_t kTexRangeMaxId = 1u << 20;
void TexRangeSet(uint32_t texId, bool known, const uint8_t lo[4], const uint8_t hi[4]) {
    if (texId >= kTexRangeMaxId) return;
    if (g_texRange.size() <= texId) {
        if (!known) return;
        g_texRange.resize(static_cast<size_t>(texId) + 1, TexRange{{0, 0, 0, 0}, {255, 255, 255, 255}, false});
    }
    TexRange& r = g_texRange[texId];
    r.known = known;
    for (int c = 0; c < 4; ++c) { r.lo[c] = known ? lo[c] : 0; r.hi[c] = known ? hi[c] : 255; }
}
// RGBA8 levels (R, G, B, A bytes): per channel smallest / largest
void RangeRgba8(const std::vector<std::vector<uint8_t>>& levels, uint8_t lo[4], uint8_t hi[4]) {
    uint8_t l[4] = {255, 255, 255, 255}, h[4] = {0, 0, 0, 0};
    for (const std::vector<uint8_t>& lv : levels) {
        const uint8_t* p = lv.data();
        const size_t n = lv.size() / 4;
        for (size_t i = 0; i < n; ++i, p += 4) {
            for (int c = 0; c < 4; ++c) {
                const uint8_t v = p[c];
                if (v < l[c]) l[c] = v;
                if (v > h[c]) h[c] = v;
            }
        }
    }
    for (int c = 0; c < 4; ++c) { lo[c] = l[c]; hi[c] = h[c]; }
}
// LA88 (L, A bytes; sampled as L, L, L, A)
void RangeLa88(const std::vector<uint8_t>& lv, uint8_t lo[4], uint8_t hi[4]) {
    uint8_t l[2] = {255, 255}, h[2] = {0, 0};
    const size_t n = lv.size() / 2;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c) {
            const uint8_t v = lv[i * 2 + c];
            if (v < l[c]) l[c] = v;
            if (v > h[c]) h[c] = v;
        }
    lo[0] = lo[1] = lo[2] = l[0]; hi[0] = hi[1] = hi[2] = h[0];
    lo[3] = l[1]; hi[3] = h[1];
}
// I8 (I bytes; sampled as I, I, I, I)
void RangeI8(const std::vector<uint8_t>& lv, uint8_t lo[4], uint8_t hi[4]) {
    uint8_t l = 255, h = 0;
    for (uint8_t v : lv) {
        if (v < l) l = v;
        if (v > h) h = v;
    }
    lo[0] = lo[1] = lo[2] = lo[3] = l;
    hi[0] = hi[1] = hi[2] = hi[3] = h;
}
// DXT1 blocks: the alpha is 0 for entry 3 of a three-colour block (colour 0 <= colour 1, little-endian), 255 for every other texel; the
// colours are the GPU's own decode: not known (0..255).
void RangeDxt1(const std::vector<std::vector<uint8_t>>& levels, uint8_t lo[4], uint8_t hi[4]) {
    bool transparent = false, opaque = false;
    for (const std::vector<uint8_t>& lv : levels) {
        for (size_t b = 0; b + 8 <= lv.size(); b += 8) {
            const uint8_t* d = lv.data() + b;
            const uint32_t c0 = d[0] | (d[1] << 8), c1 = d[2] | (d[3] << 8);
            bool t = false;
            if (c0 <= c1)
                for (int y = 0; y < 4; ++y) {
                    const uint32_t r = d[4 + y];
                    if ((r & (r >> 1)) & 0x55u) t = true;
                }
            if (t) transparent = true;
            // (a block has an opaque texel unless all 16 are entry 3)
            if (!t || !(c0 <= c1 && d[4] == 0xFF && d[5] == 0xFF && d[6] == 0xFF && d[7] == 0xFF)) opaque = true;
        }
    }
    for (int c = 0; c < 3; ++c) { lo[c] = 0; hi[c] = 255; }
    lo[3] = transparent ? 0 : 255;
    hi[3] = opaque ? 255 : 0;
}
} // namespace
bool TexChannelRange(uint32_t texId, uint8_t lo[4], uint8_t hi[4]) {
    if (texId == 0 || texId >= g_texRange.size() || !g_texRange[texId].known) return false;
    const TexRange& r = g_texRange[texId];
    for (int c = 0; c < 4; ++c) { lo[c] = r.lo[c]; hi[c] = r.hi[c]; }
    return true;
}


namespace {

struct Key {
    const void* data;
    uint32_t dims; // w | h << 16
    uint32_t fmt;  // format | levels << 8
    uint64_t tlutHash;
    bool operator==(const Key& o) const { return data == o.data && dims == o.dims && fmt == o.fmt && tlutHash == o.tlutHash; }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return static_cast<size_t>(reinterpret_cast<uintptr_t>(k.data) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(k.dims) << 20) ^
                                   k.fmt ^ k.tlutHash);
    }
};
struct Entry {
    uint32_t texId = 0;
    uint64_t contentHash = 0;
    uint64_t generation = ~0ull;
    bool mipmapped = false;
    uint32_t bytes = 0; // bytes held by the back-end texture (RGBA8, DXT1 or LA88, all levels)
    uint64_t lastUsed = 0; // frame of the last bind
    uint32_t size = 0;     // guest bytes of all levels
    uint64_t checkedEpoch = ~0ull; // GuestWriteEpoch() when the contents were last found unchanged
    bool hd = false;       // the back end holds a replacement (SetTextureReplaceHook), `bytes` of it
    uint8_t uploads = 0;   // (GXV_TEX_COMPRESS) uploads of this entry so far, up to 255: a texture whose bytes change is never converted
    uint16_t idleScenes = 0; // (texage24, GXV_TEX_SCENEDROP=3) scene destructions in a row that no frame of the scene bound it through
    uint32_t boundScene = 0; // (texage24) the scene destruction (count) at which it was last found bound
    bool amApplied = false;  // (automip25, GXV_AUTOMIP) the texture holds a chain the port made for it; amExtra bytes of `bytes` are its levels
    uint32_t amExtra = 0;
    uint32_t patchGen = 0;   // (settingsnative) g_texPatchGen when this entry was last uploaded or checked
    bool everMirror = false; // (g30 Wave 7) once uploaded with mirrorWrap=true, guaranteed swizzled on Vita SGX543
};
struct CopyEntry {
    uint32_t texId = 0;
    uint16_t w = 0, h = 0;
    uint32_t fmt = 0;
};

std::unordered_map<Key, Entry, KeyHash> g_cache;
// In front of g_cache: the entry last looked up for each of 1024 address classes (128 until 23 Oct: the Observatory binds more textures than that and missed) (a lookup
// in the hash table costs a division on the console's processor, and a frame binds the same
// textures hundreds of times). Entries of an unordered_map do not move; the front is
// emptied whenever one is erased.
struct CacheFront { Key key; Entry* e; };
CacheFront g_cacheFront[1024] = {};
inline Entry& CacheEntry(const Key& key) {
    CacheFront& f = g_cacheFront[((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key.data)) >> 5) * 2654435761u) >> 22];
    if (f.e && f.key == key) return *f.e;
    Entry& e = g_cache[key];
    f.key = key;
    f.e = &e;
    return e;
}
inline void CacheFrontClear() {
    for (CacheFront& f : g_cacheFront) f.e = nullptr;
}
bool g_texVerify = false;
void (*g_texDumpHook)(const TexUpload&, uint32_t, uint64_t) = nullptr;
uint64_t g_texStale = 0;
uint64_t g_liveBytes = 0;          // bytes of all guest textures held by the back end
uint64_t g_budget = 0;             // 0 = keep everything
bool g_budgetSet = false;
uint64_t g_evicted = 0, g_evictedBytes = 0, g_overBudgetFrames = 0;
// popin24 (GXV_POPIN_LOG=1): the budget's evictions and the textures decoded and uploaded again after one ("popin-tex:" basic-log line about
// every 10 s). The set holds the keys of evicted entries (at most 16384), a new entry whose key is in it counts as uploaded again.
bool g_popinTex = false;
std::unordered_set<Key, KeyHash> g_popinEvictedKeys;
uint64_t g_popinEv = 0, g_popinEvBytes = 0, g_popinRe = 0, g_popinReBytes = 0, g_popinOver = 0, g_popinLastFrame = 0;
std::chrono::steady_clock::time_point g_popinLastTime;
// Uploads by what the back end is handed (totals over the run, for the report).
uint64_t g_dxtUploads = 0, g_dxtBytes = 0, g_rgbaUploads = 0, g_rgbaBytes = 0, g_cmprDecoded = 0, g_cmprRgbaBytes = 0;
uint64_t g_laUploads = 0, g_laBytes = 0, g_i8Uploads = 0, g_i8Bytes = 0;
uint64_t g_dxt5Uploads = 0, g_dxt5Bytes = 0, g_t16Uploads = 0, g_t16Bytes = 0; // GXV_CMPRA, GXV_TEX16
uint64_t g_fmtUploads[16] = {}, g_fmtBytes[16] = {};                           // by GX format: uploads and the bytes the back end was given
// GXV_TEXFMT_LOG=<seconds> (default 0 = off; GXV_TEX16 or GXV_CMPRA alone make it 30): at most that often, and only after an upload, one basic-log
// line "texture uploads by GX format:" with the totals so far per GX format (uploads, MB handed to the back end) and per upload format, and the MB
// the back end holds now. For telling how much of the texture memory (and texture-read traffic) each format and the two switches account for.
inline void TexFormatLogMaybe() {
    static const long every = [] {
        const long v = EnvInt("GXV_TEXFMT_LOG", 0);
        return v > 0 ? v : ((EnvInt("GXV_TEX16", 0) != 0 || EnvInt("GXV_CMPRA", 0) != 0) ? 30L : 0L);
    }();
    if (every <= 0 || !port_log_summary) return;
    static auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(every)) return;
    last = now;
    static const char* const kFmt[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
    char line[640];
    int n = std::snprintf(line, sizeof line, "texture uploads by GX format:");
    for (int i = 0; i < 16 && n > 0 && n < static_cast<int>(sizeof line); ++i)
        if (g_fmtUploads[i])
            n += std::snprintf(line + n, sizeof line - n, " %s %llu (%.1f MB)", kFmt[i], static_cast<unsigned long long>(g_fmtUploads[i]), g_fmtBytes[i] / 1048576.0);
    if (n > 0 && n < static_cast<int>(sizeof line))
        std::snprintf(line + n, sizeof line - n,
                      "; as DXT1 %llu (%.1f MB), RGBA8 %llu (%.1f MB, of them CMPR %.1f MB), LA88 %llu (%.1f MB), I8 %llu (%.1f MB), DXT5 from CMPR %llu (%.1f MB, GXV_CMPRA), "
                      "16-bit %llu (%.1f MB, GXV_TEX16); %.1f MB held now",
                      static_cast<unsigned long long>(g_dxtUploads), g_dxtBytes / 1048576.0, static_cast<unsigned long long>(g_rgbaUploads), g_rgbaBytes / 1048576.0,
                      g_cmprRgbaBytes / 1048576.0, static_cast<unsigned long long>(g_laUploads), g_laBytes / 1048576.0,
                      static_cast<unsigned long long>(g_i8Uploads), g_i8Bytes / 1048576.0,
                      static_cast<unsigned long long>(g_dxt5Uploads), g_dxt5Bytes / 1048576.0, static_cast<unsigned long long>(g_t16Uploads),
                      g_t16Bytes / 1048576.0, g_liveBytes / 1048576.0);
    port_log_summary("%s", line);
}
// The textures of the HD pack's list, by texture id (IsCharaTexture; ids are handed out in order and never reused).
std::vector<uint8_t> g_charaTex;
inline void SetCharaTexture(uint32_t texId, bool on, bool player = false) {
    if (!on && texId >= g_charaTex.size()) return;
    // (revA) the back end reads this table from the render thread while the front end grows it: room for 128 K texture ids up front (128 KiB), so that a growth (a reallocation under a reader) is rare
    if (g_charaTex.capacity() < (size_t{1} << 17)) g_charaTex.reserve(size_t{1} << 17);
    if (texId >= g_charaTex.size()) g_charaTex.resize(static_cast<size_t>(texId) + 256, 0);
    g_charaTex[texId] = on ? (player ? 3 : 1) : 0; // bit 0: the pack's list, bit 1: Mario's or Luigi's
}
// Texture replacement (vgx_hdtex.cpp): the hook, the filter its binds get, the bytes alive.
bool (*g_replaceHook)(const TexReplaceQuery&, TexUpload&) = nullptr;
uint8_t g_replaceMipFilter = FILT_LIN_MIP_NEAR;
uint64_t g_hdLive = 0, g_hdUploads = 0, g_hdBytes = 0;
uint64_t g_hdUploadsPlay = 0, g_hdBytesPlay = 0, g_hdFreed = 0, g_hdFreedBytes = 0; // (hdtex24) in play / freed again
std::map<const void*, CopyEntry> g_copies;
// In front of g_copies: every texture bind asks whether its address is an EFB copy's (a walk
// down a tree of some 300 nodes, 800 times a frame), and almost always the answer is the
// one given last time. An answer is good while g_copies has not gained or lost an entry.
struct CopyFront { const void* p; const CopyEntry* e; uint32_t gen; };
CopyFront g_copyFront[256] = {};
uint32_t g_copiesGen = 1;
inline const CopyEntry* FindCopy(const void* p) {
    CopyFront& f = g_copyFront[(reinterpret_cast<uintptr_t>(p) >> 5) & 255u];
    if (f.gen == g_copiesGen && f.p == p) return f.e;
    auto it = g_copies.find(p);
    f.p = p;
    f.e = it == g_copies.end() ? nullptr : &it->second;
    f.gen = g_copiesGen;
    return f.e;
}
uint32_t g_nextTexId = 1;
uint32_t g_cacheEraseGen = 1; // bumped when an entry of g_cache goes (the binding memo holds pointers to entries)
uint64_t (*g_genHook)(const void*, size_t) = nullptr;
uint64_t g_tlutHash[kTlutSlots] = {};
uint32_t g_tlutSeen[kTlutSlots] = {};

inline bool IsPow2(uint32_t v) { return v && !(v & (v - 1)); }

// "Has this guest range been written since?" counter; ~0 = unknown (always
// compare the bytes). vgx's own table (4 KiB granules, fed by the HLE layer in
// MSC_VGX builds) when the runtime registered guest RAM, else the runtime's
// hook (64 KiB granules: a per-frame flush anywhere near a texture forces a
// hash of the whole texture every frame).
inline uint64_t Generation(const void* p, uint32_t n) {
    if (HaveGuestRam()) return GuestWriteGeneration(p, n);
    return g_genHook ? g_genHook(p, n) : ~0ull;
}

uint64_t TlutHash(int idx) {
    if (idx < 0 || idx >= kTlutSlots || !g.tlut[idx].valid || !g.tlut[idx].obj.data) return 0;
    const TlutObj& t = g.tlut[idx].obj;
    if (g_tlutSeen[idx] != t.version) {
        g_tlutSeen[idx] = t.version;
        // (entries == 0xFFFF stands for the 16384 of a 14-bit index: see BpTexSlotSync)
        const size_t tlutBytes = t.entries == 0xFFFF ? 32768u : static_cast<size_t>(t.entries) * 2;
        g_tlutHash[idx] = XXH3_64bits(t.data, tlutBytes) ^ (static_cast<uint64_t>(t.format) << 60) ^ 1;
    }
    return g_tlutHash[idx];
}

} // namespace

void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t)) { g_genHook = fn; }
void SetTextureDumpHook(void (*fn)(const TexUpload&, uint32_t, uint64_t)) { g_texDumpHook = fn; }
static uint64_t g_lastUploadHash = 0;
uint64_t LastUploadHash() { return g_lastUploadHash; }
// The host may change a texture's pixels after it is decoded (the handheld button pictures):
// called for RGBA8 uploads with level 0, top row first, and the hash of the guest's texels.
static void (*g_texPatchHook)(uint64_t, uint32_t, uint32_t, uint8_t*) = nullptr;
static bool (*g_texPatchNeedsRgba)(uint64_t, uint32_t, uint32_t) = nullptr;
void SetTexturePatchHook(void (*fn)(uint64_t hash, uint32_t width, uint32_t height, uint8_t* rgba),
                         bool (*needsRgba)(uint64_t hash, uint32_t width, uint32_t height)) {
    g_texPatchHook = fn;
    g_texPatchNeedsRgba = fn ? needsRgba : nullptr;
}
// settingsnative: the count of changes of what the patch hook makes (the button pictures follow a swapped mapping): an entry whose count is older and
// that the hook patches is uploaded again at its next use (see the `patchGen` test in the upload decision).
static std::atomic<uint32_t> g_texPatchGen{0};
void TexturePatchChanged() { g_texPatchGen.fetch_add(1, std::memory_order_relaxed); }
void SetTextureReplaceHook(bool (*fn)(const TexReplaceQuery&, TexUpload&), uint8_t mipFilter) {
    g_replaceHook = fn;
    g_replaceMipFilter = mipFilter;
}
uint64_t ReplacedTextureBytes() { return g_hdLive; }
void HdTextureStats(HdTexStats& s) {
    s.uploads = g_hdUploads;
    s.bytes = g_hdBytes;
    s.uploadsPlay = g_hdUploadsPlay;
    s.bytesPlay = g_hdBytesPlay;
    s.freed = g_hdFreed;
    s.freedBytes = g_hdFreedBytes;
    s.alive = 0;
    for (const auto& kv : g_cache)
        if (kv.second.hd && kv.second.texId) ++s.alive;
    s.aliveBytes = g_hdLive;
}
bool IsCharaTexture(uint32_t texId) { return texId < g_charaTex.size() && g_charaTex[texId] != 0; }
bool IsPlayerTexture(uint32_t texId) { return texId < g_charaTex.size() && (g_charaTex[texId] & 2) != 0; }

// ---- mario24: the player's textures as uploaded (PlayerTexFacts), and the mip-level visualisation (GXV_PLAYER_MIPVIS)
namespace {
constexpr int kPlayerFacts = 48;
std::mutex g_plFactMutex;
PlayerTexFact g_plFacts[kPlayerFacts];
int g_plFactN = 0;
void PlayerFactNote(const PlayerTexFact& f) {
    std::lock_guard<std::mutex> lock(g_plFactMutex);
    for (int i = 0; i < g_plFactN; ++i)
        if (g_plFacts[i].texId == f.texId) { // (uploaded again: the newest values, in place)
            g_plFacts[i] = f;
            return;
        }
    if (g_plFactN == kPlayerFacts) { // (full: the oldest goes; ids are never reused, so a stale fact only costs a slot)
        for (int i = 1; i < kPlayerFacts; ++i) g_plFacts[i - 1] = g_plFacts[i];
        --g_plFactN;
    }
    g_plFacts[g_plFactN++] = f;
}
// A flat colour for mip level l (0 red, 1 green, 2 blue, 3 yellow, 4 magenta, 5 cyan, 6+ white).
void MipVisColour(unsigned l, uint8_t& r, uint8_t& g_, uint8_t& b) {
    static const uint8_t kC[7][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    const uint8_t* c = kC[l < 6 ? l : 6];
    r = c[0]; g_ = c[1]; b = c[2];
}
// RGB565 of a colour (for a DXT block's two end points)
inline uint16_t Rgb565Of(uint8_t r, uint8_t g_, uint8_t b) { return static_cast<uint16_t>(((r >> 3) << 11) | ((g_ >> 2) << 5) | (b >> 3)); }
// Every level of an RGBA8 / DXT1 / DXT5 upload becomes its colour (DXT: both end points of every block, all indices 0; DXT5 keeps its alpha half),
// a texture with the back end's own mip generation gets a chain of flat levels in its place. Other formats are left as they are.
void PlayerMipVis(TexUpload& up) {
    if (up.format == TF_RGBA8) {
        if (up.genMips) { // (level 0 only was sent: make the whole chain, flat)
            uint32_t w = up.width, h = up.height;
            std::vector<std::vector<uint8_t>> lv;
            for (;;) {
                lv.emplace_back(static_cast<size_t>(w) * h * 4);
                if (w == 1 && h == 1) break;
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            up.levels = std::move(lv);
            up.genMips = false;
            up.authored = static_cast<uint8_t>(up.levels.size());
        }
        for (size_t l = 0; l < up.levels.size(); ++l) {
            uint8_t r, g_, b;
            MipVisColour(static_cast<unsigned>(l), r, g_, b);
            std::vector<uint8_t>& d = up.levels[l];
            for (size_t i = 0; i + 3 < d.size(); i += 4) { d[i] = r; d[i + 1] = g_; d[i + 2] = b; } // (alpha kept)
        }
    } else if (up.format == TF_DXT1 || up.format == TF_DXT5) {
        const size_t blockBytes = up.format == TF_DXT1 ? 8 : 16, colourAt = up.format == TF_DXT1 ? 0 : 8;
        for (size_t l = 0; l < up.levels.size(); ++l) {
            uint8_t r, g_, b;
            MipVisColour(static_cast<unsigned>(l), r, g_, b);
            const uint16_t c = Rgb565Of(r, g_, b);
            std::vector<uint8_t>& d = up.levels[l];
            for (size_t i = 0; i + blockBytes <= d.size(); i += blockBytes) {
                uint8_t* p = d.data() + i + colourAt;
                p[0] = p[2] = static_cast<uint8_t>(c & 0xFF);
                p[1] = p[3] = static_cast<uint8_t>(c >> 8);
                p[4] = p[5] = p[6] = p[7] = 0; // (index 0 everywhere: colour 0; with c0 == c1 the 3-colour mode's transparent index is never used)
            }
        }
    }
}
} // namespace
int PlayerTexFacts(PlayerTexFact* out, int max) {
    std::lock_guard<std::mutex> lock(g_plFactMutex);
    const int n = g_plFactN < max ? g_plFactN : max;
    for (int i = 0; i < n; ++i) out[i] = g_plFacts[g_plFactN - n + i];
    return n;
}
bool PlayerMipVisOn() {
    static const bool on = EnvInt("GXV_PLAYER_MIPVIS", 0) != 0;
    return on;
}

// GXInvalidateTexAll. On the console it empties the texture cache in front of
// the texture units, and the game calls it every frame. It carries no
// information the write generations do not already give: texture data only
// reached the hardware through RAM, so a changed texture was always flushed
// from the data cache as well, and the runtime reports those flushes. With
// tracked guest RAM the call is therefore not a reason to hash every bound
// texture again (108 textures / 2.8 MB per frame in a match).
//   GXV_TEXTRUST=0       re-hash after every GXInvalidateTexAll (the old behaviour)
//   GXV_TEXTRUST=verify  hash anyway and report a texture that changed without a write notification
void InvalidateAllTextures() {
    static const int trust = [] {
        const char* m = std::getenv("GXV_TEXTRUST");
        return (m && !std::strcmp(m, "verify")) ? 2 : ((m && *m == '0') ? 0 : 1);
    }();
    g_texVerify = trust == 2;
    if (trust == 0 || !(HaveGuestRam() || g_genHook)) {
        // Contents are re-validated (generation counter / hash) on the next bind.
        two::AGuard l; // (GXV_FE_TWOCORE)
        two::Bump(VGX2_EP_TEX);
        for (auto& e : g_cache) { e.second.generation = ~0ull; e.second.checkedEpoch = ~0ull; }
    }
    for (uint32_t& s : g_tlutSeen) s = 0xFFFFFFFFu;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
}

uint32_t AllocTextureId() { return g_nextTexId++; }

// ---- grow24 (GXV_TEX_SCENEDROP, GXV_TEX_IDREUSE): a destroyed scene's guest textures go with it, and their ids are handed out again
// The session slowdown (c31, 16 minutes alternating two galaxies): every visit loaded its images at new guest addresses, the texture cache
// (keyed by address) kept the old scene's textures until the 32 MiB budget pushed them out, the texture ids were never reused, and the back
// end's id-indexed table (g_tex) and every scan of it grew with the session (370 -> 1,600 live textures, 376 -> 1,720 ids after 15 visits).
//   GXV_TEX_SCENEDROP=0 (default here; 1 from vita/vgx-game/entry.cpp) nothing is dropped (the census in the "vgx: scene destroyed" line is
//     still made); 1 the cache entries whose guest address lies in the destroyed scene's heaps are freed with it (CMD_TEX_FREE, the back end's
//     glDeleteTextures: vitaGL's collector keeps the storage until the GPU is past it) and erased; 2 also every entry not bound since the
//     previous scene was destroyed (a texture no frame of the whole scene needed; the next scene that needs it decodes it again);
//     3 (texage24) by age: a texture no frame of the scene bound has its idle count raised and is freed when it has been idle through
//     GXV_TEX_SCENEDROP_AGE (default 2) scene destructions in a row, or at once when it lies in the destroyed heaps; then, if the cache holds
//     more than GXV_TEX_SCENEDROP_MAX entries (700) or GXV_TEX_SCENEDROP_MAXKB KiB of texels (14336), idle textures are freed oldest first down
//     to the caps (never one the destroyed scene bound). A scene that drew fewer than GXV_TEX_SCENEDROP_MINFRAMES pictures in play (30) does
//     not count as an idle scene (a loading or hop scene must not age the stages' textures).
//   GXV_TEX_IDREUSE (default: follows GXV_TEX_SCENEDROP, on when it is not 0) the ids of freed guest textures are handed out again, to guest
//     textures only (copy and atlas ids keep coming from the counter: the pass analyses keep per-id memories of those), after
//     GXV_TEX_IDQUAR frames (default 16) in quarantine. Everything keyed by id is reset when the id is freed (see ForgetGuestTexId).
// A dropped texture that is needed again is decoded and uploaded again as a new entry, as a texture the budget freed is: the picture is the
// same (host test tests/vgx_bowser_loop_gl.cpp, BOWSER_DROP / BOWSER_HASH).
int TexSceneDropMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_TEX_SCENEDROP", 0);
        return v == 1 || v == 2 || v == 3 ? v : 0L;
    }()));
    return m;
}
namespace {
bool TexIdReuseOn() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_TEX_IDREUSE", -1);
        if (v >= 0) return v != 0 ? 1L : 0L;
        return TexSceneDropMode() != 0 ? 1L : 0L;
    }()));
    return m != 0;
}
uint64_t TexIdQuarantine() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_TEX_IDQUAR", 16);
        return v < 0 ? 0L : (v > 100000 ? 100000L : v);
    }()));
    return static_cast<uint64_t>(m);
}
struct QuarId { uint32_t id; uint64_t frame; };
std::vector<uint32_t> g_freeIds;     // guest-texture ids free to hand out (the most recently released last)
std::vector<QuarId> g_quarIds;       // released, waiting out the quarantine (oldest first)
size_t g_quarHead = 0;
uint64_t g_sceneGoneFrame = 0;       // the frame at which the previous scene was destroyed (GXV_TEX_SCENEDROP=2: "bound since")
uint64_t g_idsReused = 0, g_dropFreed = 0, g_dropFreedBytes = 0, g_dropScenes = 0;

void QuarantineSweep() {
    const uint64_t now = CurrentFrame(), q = TexIdQuarantine();
    while (g_quarHead < g_quarIds.size() && g_quarIds[g_quarHead].frame + q <= now) g_freeIds.push_back(g_quarIds[g_quarHead++].id);
    if (g_quarHead && g_quarHead == g_quarIds.size()) { g_quarIds.clear(); g_quarHead = 0; }
    else if (g_quarHead > 1024) { g_quarIds.erase(g_quarIds.begin(), g_quarIds.begin() + static_cast<std::ptrdiff_t>(g_quarHead)); g_quarHead = 0; }
}
// A new guest texture's id: a released one when reuse is on, else the next never used.
uint32_t NewGuestTexId() {
    if (TexIdReuseOn()) {
        if (g_quarHead < g_quarIds.size()) QuarantineSweep();
        if (!g_freeIds.empty()) {
            const uint32_t id = g_freeIds.back();
            g_freeIds.pop_back();
            ++g_idsReused;
            return id;
        }
    }
    return g_nextTexId++;
}
// Everything the front end keeps by guest-texture id, reset for a texture that is freed (and the id given back). The other tables keyed by
// id (the pass analyses' copy memories, g_constTex, the atlases) concern copy textures and atlas pages, whose ids are never reused.
void ForgetGuestTexId(uint32_t id) {
    if (id < g_texRange.size()) g_texRange[id] = TexRange{{0, 0, 0, 0}, {255, 255, 255, 255}, false};
    SetCharaTexture(id, false);
    texcomp::UsageReset(id);
}
void ReleaseGuestTexId(uint32_t id) {
    if (!TexIdReuseOn()) return; // (off: the id is simply never used again, as before)
    ForgetGuestTexId(id);
    g_quarIds.push_back(QuarId{id, CurrentFrame()});
}
} // namespace
unsigned TextureIdsFree() { return static_cast<unsigned>(g_freeIds.size() + (g_quarIds.size() - g_quarHead)); }
unsigned TextureIdsReused() { return static_cast<unsigned>(g_idsReused); }

// ---- texage24 (GXV_TEX_SCENEDROP=3, GXV_TEX_ADOPT): the age rule's knobs, the refetch census, and adopting a live texture of the same content
// GXV_TEX_SCENEDROP=2 cured the slowdown but freed textures the next visit needed again (c35: in-play uploads 647 -> 1,986 over 18 minutes).
// Mode 3 frees by age and by a cap instead; the census counts what freeing cost, in modes 2 and 3 alike (and 1, and the budget's evictions):
// a freed texture's cache key (address, size, format with levels, palette hash) and its content identity (size, format with levels, palette
// hash and the hash of the guest bytes, which the cache computes for every new entry anyway) go into a bounded table; a new entry that matches
// one counts as a refetch (same address, same bytes) or as the same content decoded again at another address, and again when the screen is
// live (a texture decoded and uploaded in play). Nothing here changes what is drawn.
namespace {
long TexDropKnob(const char* name, long def, long lo, long hi) {
    const long v = EnvInt(name, def);
    return v < lo ? lo : (v > hi ? hi : v);
}
int TexDropAge() { return static_cast<int>(VGX_KNOB(TexDropKnob("GXV_TEX_SCENEDROP_AGE", 2, 1, 1000))); }
long TexDropMaxEntries() { return VGX_KNOB(TexDropKnob("GXV_TEX_SCENEDROP_MAX", 700, 0, 1 << 20)); }      // 0: no cap
long TexDropMaxKiB() { return VGX_KNOB(TexDropKnob("GXV_TEX_SCENEDROP_MAXKB", 14336, 0, 1 << 22)); }     // 0: no cap
long TexDropMinFrames() { return VGX_KNOB(TexDropKnob("GXV_TEX_SCENEDROP_MINFRAMES", 30, 0, 1 << 20)); }
bool TexAdoptOn() { return VGX_KNOB(EnvInt("GXV_TEX_ADOPT", 0) != 0) != 0; }

// A picture in play: a stage is being played and no fade or pause menu hides it. Without the game's hooks (host tests) every picture is.
bool PlayNow() {
    if (!port_scene_key || !port_screen_quiet) return true;
    return port_scene_key() != 0 && port_screen_quiet() == 0;
}
bool CompactMipsReady() {
    const int mode = La88MipsMode();
    const int verdict = g_la88MipsState.load(std::memory_order_relaxed);
    return mode && (verdict > 0 || (verdict == 0 && (mode == 2 || !PlayNow())));
}
uint64_t g_picPlay = 0, g_picPlayAtGone = 0; // pictures in play so far (TexCacheEndFrame), and at the previous scene destruction
uint32_t g_sceneSeq = 0;                     // scene destructions so far

struct CKey { // the content identity
    uint64_t hash;
    uint64_t tlutHash;
    uint32_t dims, fmt;
    bool operator==(const CKey& o) const { return hash == o.hash && tlutHash == o.tlutHash && dims == o.dims && fmt == o.fmt; }
};
struct CKeyHash {
    size_t operator()(const CKey& k) const { return static_cast<size_t>(k.hash ^ (k.tlutHash * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(k.dims) << 20) ^ k.fmt); }
};
struct FreedRec { Key key; CKey ck; uint32_t seq; };
constexpr size_t kFreedMax = 4096;                                          // the table's bound (records; the oldest are forgotten first)
std::deque<FreedRec> g_freedFifo;
std::unordered_map<Key, std::pair<uint32_t, uint64_t>, KeyHash> g_freedByKey; // key -> (record seq, content hash)
std::unordered_map<CKey, uint32_t, CKeyHash> g_freedByContent;                 // content identity -> record seq
uint32_t g_freedSeq = 0;
struct Census { uint64_t freed = 0, refAddr = 0, refAddrLive = 0, refContent = 0, refContentLive = 0, adopted = 0, adoptedBytes = 0; };
Census g_census, g_censusAtGone;

void CensusFreed(const Key& key, const Entry& e) {
    if (!e.texId || !e.contentHash) return;
    ++g_census.freed;
    while (g_freedFifo.size() >= kFreedMax) {
        const FreedRec& old = g_freedFifo.front();
        auto k = g_freedByKey.find(old.key);
        if (k != g_freedByKey.end() && k->second.first == old.seq) g_freedByKey.erase(k);
        auto c = g_freedByContent.find(old.ck);
        if (c != g_freedByContent.end() && c->second == old.seq) g_freedByContent.erase(c);
        g_freedFifo.pop_front();
    }
    FreedRec r{key, CKey{e.contentHash, key.tlutHash, key.dims, key.fmt}, ++g_freedSeq};
    g_freedFifo.push_back(r);
    g_freedByKey[key] = {r.seq, e.contentHash};
    g_freedByContent[r.ck] = r.seq;
}
// A new entry's bytes were hashed (it is about to be decoded and uploaded, or adopted): is it a texture that was freed?
void CensusNewEntry(const Key& key, uint64_t contentHash) {
    if (g_freedFifo.empty()) return;
    const bool live = PlayNow();
    const CKey ck{contentHash, key.tlutHash, key.dims, key.fmt};
    auto k = g_freedByKey.find(key);
    bool byAddr = false;
    if (k != g_freedByKey.end()) {
        const uint32_t seq = k->second.first;
        const uint64_t oldHash = k->second.second;
        byAddr = oldHash == contentHash; // (the same address with other bytes is a new texture, not a refetch)
        g_freedByKey.erase(k);
        auto c = g_freedByContent.find(CKey{oldHash, key.tlutHash, key.dims, key.fmt});
        if (c != g_freedByContent.end() && c->second == seq) g_freedByContent.erase(c);
    }
    if (byAddr) {
        ++g_census.refAddr;
        if (live) ++g_census.refAddrLive;
        return;
    }
    auto c = g_freedByContent.find(ck);
    if (c != g_freedByContent.end()) { // (same bytes, size, format and palette freed at another address)
        g_freedByContent.erase(c);
        ++g_census.refContent;
        if (live) ++g_census.refContentLive;
    }
}
// GXV_TEX_ADOPT=1 (default 0): a new entry whose bytes are the bytes of a live entry at another address (same size, format with levels,
// palette hash and XXH3-64 of the guest bytes: what a stage's archive reloaded at a new address gives) takes that entry's back-end texture
// instead of decoding and uploading the same image again. The texture is MOVED, not shared: the donor entry is left empty (texture 0), so no
// back-end texture has two owners and every free stays single; the donor decodes afresh if its address is ever bound again. Only a donor that
// no frame has bound for 30 pictures is taken (its draws are long recorded; two live addresses never swap a texture back and forth). What
// the upload would have made is a function of those bytes, the size, the format with levels and the palette (the key) and of the back end's
// fixed capabilities, so the texture is the one a new upload would make, up to a collision of the 64-bit hash (2^-64 a pair).
bool TexAdopt(Entry& e, uint64_t contentHash, const Key& key) {
    constexpr uint64_t kMinIdle = 30;
    const uint64_t now = CurrentFrame();
    Entry* best = nullptr;
    for (auto& kv : g_cache) {
        Entry& d = kv.second;
        if (&d == &e || !d.texId || d.contentHash != contentHash || d.lastUsed + kMinIdle >= now) continue;
        const Key& k = kv.first;
        if (k.dims != key.dims || k.fmt != key.fmt || k.tlutHash != key.tlutHash || k.data == key.data) continue;
        // (the most recently bound one; the texture id breaks ties, so that the choice does not depend on the hash map's order)
        if (!best || d.lastUsed > best->lastUsed || (d.lastUsed == best->lastUsed && d.texId > best->texId)) best = &d;
    }
    if (!best) return false;
    e.texId = best->texId;
    e.bytes = best->bytes;
    e.hd = best->hd;
    e.mipmapped = best->mipmapped;
    e.uploads = best->uploads;
    e.everMirror = best->everMirror;
    best->texId = 0;
    best->bytes = 0;
    best->hd = false;
    best->contentHash = 0;
    best->generation = ~0ull;
    best->checkedEpoch = ~0ull;
    best->uploads = 0;
    best->everMirror = false;
    two::Bump(VGX2_EP_TEX); // (GXV_FE_TWOCORE: the caller holds the lock)
    ++g_cacheEraseGen;      // (the binding memo points at entries: it asks again)
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
    ++g_census.adopted;
    g_census.adoptedBytes += e.bytes;
    return true;
}
} // namespace

uint32_t AcquireCopyTexture(const void* dest, uint16_t w, uint16_t h, uint32_t fmt) {
    if (!g_copies.count(dest)) { two::AGuard l; two::Bump(VGX2_EP_TEX); g_copies[dest]; } // (GXV_FE_TWOCORE: a new EFB-copy texture)
    CopyEntry& c = g_copies[dest];
    if (!c.texId) { c.texId = g_nextTexId++; ++g_copiesGen; }
    c.w = w; c.h = h; c.fmt = fmt;
    return c.texId;
}

// polish23 (8 Nov; GXV_COPY_DROP, vgx_galaxy.cpp port_vgx_scene_heaps_gone): the EFB-copy textures whose destination lies in
// [lo, hi), a destroyed scene's heap. Nothing calls DestroyCopyTexture: an entry and its back-end render texture (video memory, the copy's
// size at the render scale) stay for the rest of the run, and since every scene allocates its copy buffers anew (bloom, blur, depth of
// field, the special-draw raster image ...), at addresses of its own, they pile up scene after scene (2.2 player log: 2, then 5, then 7
// render textures resampled at each picture-size change, 4-30 ms each). An entry also wins over the guest bytes at its address (see
// ResolveTextureFull), so an old scene's copy shadowed a texture the next scene loaded at that address. Front-end thread, before the
// first command published after the scene's destruction. Returns how many lie in the range; drops them when `drop`.
unsigned CopyTexturesInRange(const void* lo, const void* hi, bool drop) {
    if (!lo || !hi || !(lo < hi)) return 0;
    unsigned n = 0;
    two::AGuard l; // (GXV_FE_TWOCORE)
    for (auto it = g_copies.lower_bound(lo); it != g_copies.end() && it->first < hi;) {
        ++n;
        if (!drop) { ++it; continue; }
        if (it->second.texId) Pending().cmds.push_back(Cmd{CMD_TEX_FREE, it->second.texId});
        it = g_copies.erase(it);
    }
    if (drop && n) {
        two::Bump(VGX2_EP_TEX);
        ++g_copiesGen;
        g.texDirty = true;
        g.texSlotDirty = 0xFF;
    }
    return n;
}
unsigned CopyTextureCount() { return static_cast<unsigned>(g_copies.size()); }
uint32_t TextureIdsMade() { return g_nextTexId - 1; }
size_t TextureCacheEntries() { return g_cache.size(); } // (heap23b: the scene-destroy census line; front-end thread)

void DestroyCopyTexture(const void* dest) {
    auto it = g_copies.find(dest);
    if (it == g_copies.end()) return;
    Pending().cmds.push_back(Cmd{CMD_TEX_FREE, it->second.texId});
    two::AGuard l; // (GXV_FE_TWOCORE)
    two::Bump(VGX2_EP_TEX);
    g_copies.erase(it);
    ++g_copiesGen;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
}

namespace { void AmUpload(const Key& key, Entry& e, const TexObj& o, uint32_t fmt, TexUpload& up, uint8_t minF, bool hd, bool player); void AmFrameEnd(); } // (GXV_AUTOMIP, below)

// ---- endwait26 (GXV_TEXWARM): textures first used in the first 15 s of a stage are recorded per stage and made behind the closed screen the next time the stage loads.
// Mode 0 = off; 1 = record + replay in the closed frames; 2 = also replay in the first 3 s of the open screen (budgeted per picture). Nothing is deferred in play (the memo, the
// per-draw slot cache and the retained templates would keep a "no texture" answer; see REPORT.txt): the census counts what a per-picture cap WOULD have deferred.
namespace tw {
int Mode() {
    static const int m = [] { const long v = EnvInt("GXV_TEXWARM", 0); return v < 0 ? 0 : (v > 2 ? 2 : static_cast<int>(v)); }();
    return m;
}
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
texwarm::Book g_book;
texwarm::Replay g_replay;
texwarm::Census g_cen;
std::vector<texwarm::Item> g_plan;
texwarm::Budget g_bClosed = [] { texwarm::Budget b; b.maxMs = 10.0; b.maxKiB = 1536; return b; }(), g_bOpen;
uint32_t g_stage = 0;
double g_openMs = 0;
bool g_wasPlay = false, g_loaded = false, g_logOpen = false, g_logLate = false, g_inReplay = false;
// ---- openstall26 (GXV_SMOOTHSTART): the replay starts when the stage's load is over (before: the textures are not in guest memory yet and every try is a miss that counts toward the
// give-up: kMaxMisses 6 tries at 24 pictures = about 2.4 s of a load that takes 8), it is keyed by the stage being loaded (the scene key changes only when the scene starts), it is
// begun again for every load (the scene drop frees the textures, a restart needs them again), and the closed-screen spending is governed (port/smoothstart.h).
int SmoothLevel() { static const int m = [] { const long v = EnvInt("GXV_SMOOTHSTART", 0); return v < 0 ? 0 : (v > 2 ? 2 : static_cast<int>(v)); }(); return m; }
port::smoothstart::Governor g_gov;
bool g_wasLoading = false;
unsigned long long g_closedKiB = 0;
double g_closedMs = 0, g_openReplayMs = 0;
std::atomic<unsigned> g_pendingA{0};
bool LoadingNow() { return port_scene_loading_active && port_scene_loading_active() != 0; }
void GovReset() {
    static const double cap = static_cast<double>(std::max(0L, EnvInt("GXV_SMOOTHSTART_MS", 400)));
    g_gov.Reset(SmoothLevel() > 0 ? cap : 1e12);  // (off: no cap, as before)
    g_closedKiB = 0;
    g_closedMs = g_openReplayMs = 0;
}
uint64_t g_pic = 0;
double g_picMs = 0;   // first-use decode+upload ms in the current picture (in play)
// ---- texwarm30 (GXV_TEXWARM_KEY, default 1; 0 = the address-keyed book above): the book keys a texture by what it is (vgx_texwarm_id.h), the stage's candidates come from the game's walk of its
// loaded models (decomp/src/Game/System/PortTexEnum.cpp), one file a stage (<GXV_TEXWARM_FILE>.<stage>, "texwarm 2"), the old single file and GXV_TEXWARM_SEED are still read.
// GXV_TEXWARM_ENUM (default 0): also upload the candidates no book item named, behind the closed screen only, GXV_TEXWARM_ENUM_MAX (160) textures and GXV_TEXWARM_ENUM_MIB (8) a stage.
// GXV_TEXWARM_OPEN_S (3) seconds, _OPEN_US (1500) microseconds, _OPEN_KIB (256), _OPEN_ITEMS (3): what the open screen's share of the replay may spend a picture (GXV_TEXWARM=2).
int KeyMode() { static const int m = EnvInt("GXV_TEXWARM_KEY", 1) != 0 ? 1 : 0; return m; }
int EnumMode() { static const int m = EnvInt("GXV_TEXWARM_ENUM", 0) != 0 ? 1 : 0; return m; }
texid::Book g_ib;
texid::CandIndex g_ci;
texid::Sched g_sch;
texid::Census g_icen;
std::vector<texid::Item> g_iplan;
std::vector<uint32_t> g_dirtyStages;       // stages whose file is out of date
uint32_t g_ciSerial = 0xFFFFFFFFu;         // the scene serial the candidates belong to (0xFFFFFFFF: not known yet)
unsigned g_stagePics = 0;                  // pictures since the stage's plan began
bool g_idLoaded = false, g_enumBuilt = false;
std::vector<uint32_t> g_enumList;          // candidates (indices) to upload with no book item, in walk order
size_t g_enumPos = 0;
struct EnumKey { Key key; uint64_t frame; };
std::vector<EnumKey> g_enumKeys;           // what the enumeration made, to count at +20 s which the game bound
texid::Budget g_ibClosed = [] { texid::Budget b; b.maxMs = 10.0; b.maxKiB = 1536; return b; }();
texid::Budget& OpenBudget() {   // (read at first use, after entry.cpp's setenv: not at static-init time)
    static texid::Budget b = [] {
        texid::Budget x;
        x.maxMs = static_cast<double>(std::max(100L, EnvInt("GXV_TEXWARM_OPEN_US", 1500))) / 1000.0;
        x.maxKiB = static_cast<uint32_t>(std::max(16L, EnvInt("GXV_TEXWARM_OPEN_KIB", 256)));
        x.maxItems = static_cast<unsigned>(std::max(1L, EnvInt("GXV_TEXWARM_OPEN_ITEMS", 3)));
        return x;
    }();
    return b;
}
double OpenWindowMs() { static const double v = static_cast<double>(std::max(0L, EnvInt("GXV_TEXWARM_OPEN_S", 3))) * 1000.0; return v; }

// The game thread's reports (port_texwarm_cand, below) wait here for the front end.
struct RawCand { uint32_t key, serial, addr; uint16_t w, h; uint8_t fmt, mips, minF, maxLod8; uint32_t order; };
std::mutex g_inMu;
std::vector<RawCand> g_inbox;
bool g_inDone = false;
uint32_t g_inDoneKey = 0, g_inDoneSerial = 0;
unsigned g_inDoneCount = 0;
unsigned long long g_inDropped = 0;

std::string StagePath(uint32_t stage) {
    const char* p = getenv("GXV_TEXWARM_FILE");
    if (!p || !*p) return std::string();
    char suf[16];
    std::snprintf(suf, sizeof suf, ".%08x", static_cast<unsigned>(stage));
    return std::string(p) + suf;
}
void LoadOneId(const std::string& path) {
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (4u << 20)) text.append(buf, n);
    std::fclose(f);
    g_ib.Parse(text.data(), text.size());
}
void IdLoad() {  // the old single file and the shipped seed (either format), once
    const char* seed = getenv("GXV_TEXWARM_SEED");
    if (seed && *seed) LoadOneId(seed);
    const char* file = getenv("GXV_TEXWARM_FILE");
    if (file && *file) LoadOneId(file);
}
void IdLoadStage(uint32_t stage) {
    static std::unordered_set<uint32_t> loaded;
    if (!loaded.insert(stage).second) return;
    LoadOneId(StagePath(stage));
}
void IdSave() {  // one file a stage, each <= ~18 KiB: the card queue's 20 KiB slot; behind the cover or in the closed frames, never in play
    static const bool queueOn = EnvInt("GXV_CARD_ASYNC", 1) != 0 && port_card_write_async != nullptr;
    for (size_t i = 0; i < g_dirtyStages.size();) {
        const uint32_t st = g_dirtyStages[i];
        if (!g_ib.StageDirty(st)) { g_dirtyStages.erase(g_dirtyStages.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        const std::string path = StagePath(st);
        if (path.empty()) { g_ib.CleanStage(st); g_dirtyStages.erase(g_dirtyStages.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        const std::string text = g_ib.SerializeStage(st);
        bool done = false;
        if (queueOn && text.size() <= (64u << 10)) {
            done = port_card_write_async(path.c_str(), 0, text.data(), static_cast<unsigned>(text.size())) != 0;  // (a full queue: still dirty, asked again at the next picture)
        } else {
            const std::string tmp = path + ".tmp";
            FILE* f = std::fopen(tmp.c_str(), "wb");
            if (f) {
                const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
                std::fclose(f);
                if (ok) std::rename(tmp.c_str(), path.c_str());
            }
            done = true;
        }
        if (!done) return;
        g_ib.CleanStage(st);
        g_dirtyStages.erase(g_dirtyStages.begin() + static_cast<std::ptrdiff_t>(i));
    }
}
void NoteDirty(uint32_t stage) {
    if (std::find(g_dirtyStages.begin(), g_dirtyStages.end(), stage) == g_dirtyStages.end()) g_dirtyStages.push_back(stage);
}
// A texture is about to be uploaded for the first time in play (after its bytes changed, or after its texture was freed): was a loaded model's texture at that address (the replay
// could have known it), and, in the window, noted in the stage's book by what it is.
void RecordId(const TexObj& o, uint32_t fmt, uint32_t levels, uint32_t size, uint64_t hash) {
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o.data));
    if (g_ci.Key() == g_stage && g_ci.HasAddr(addr)) ++g_icen.firstEnumerable; else ++g_icen.firstNotEnumerable;
    if (!texwarm::InWindow(NowMs(), g_openMs)) return;
    texid::Item it;
    it.hash = hash;
    it.size = size;
    it.w = o.width;
    it.h = o.height;
    it.fmt = static_cast<uint8_t>(fmt);
    it.levels = static_cast<uint8_t>(levels);
    it.tMs = texid::MsSinceOpen(NowMs(), g_openMs);
    if (!texid::Valid(it)) return;
    const uint32_t l0 = TexDataSize(o.width, o.height, fmt, 1);
    if (l0 >= 8 && l0 <= size) it.qh = texid::QuickKey(XXH3_64bits(o.data, texid::QuickLen(l0)));
    if (g_ib.Note(g_stage, it)) NoteDirty(g_stage);
}
void NoteCI() { if (Mode() != 0 && KeyMode() && !g_inReplay && g_stage && PlayNow()) ++g_icen.firstCI; }
static void LoadOne(const char* path) {
    if (!path || !*path) return;
    FILE* f = std::fopen(path, "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (4u << 20)) text.append(buf, n);
    std::fclose(f);
    g_book.Parse(text.data(), text.size());
}
void Load() {
    // texup27: GXV_TEXWARM_SEED = a book shipped with the package (same text format, made by a recorded run: see new/texup27/REPORT.txt), read first; the learned file adds to it
    // (an item at the same place with other bytes takes the learned hash). A first visit then has a plan.
    LoadOne(getenv("GXV_TEXWARM_SEED"));
    LoadOne(getenv("GXV_TEXWARM_FILE"));
    g_book.Clean();
}
// hitch30: the book goes to the card-write queue (os_alarm.cpp, port/card_queue.h) when it fits a slot (64 KiB: 1000 items; the book's limit is 4096, about 250 KiB, then the
// old write below runs, behind the stage's cover). It was a create + rename on the thread that ends a stage's first seconds (0.3-0.5 s). GXV_CARD_ASYNC=0: as before.
void Save() {
    const char* path = getenv("GXV_TEXWARM_FILE");
    if (!path || !*path) { g_book.Clean(); return; }
    const std::string text = g_book.Serialize();
    static const bool queueOn = EnvInt("GXV_CARD_ASYNC", 1) != 0 && port_card_write_async != nullptr;
    if (queueOn && text.size() <= (64u << 10)) {
        if (port_card_write_async(path, 0, text.data(), static_cast<unsigned>(text.size()))) g_book.Clean();  // (a full queue: still dirty, asked again at the next picture)
        return;
    }
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { g_book.Clean(); return; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (ok) std::rename(tmp.c_str(), path);
    g_book.Clean();
}
void LogCensus(const char* when) {
    if (!port_log_summary) return;
    const std::vector<texwarm::Item>* it = g_book.Items(g_stage);
    char line[1024];
    g_cen.gaveUp = g_replay.GaveUp();
    texwarm::Format(line, sizeof line, g_cen, when, Mode(), it ? it->size() : 0);
    port_log_summary("%s", line);
}
// A texture is about to be uploaded for the first time (or after its bytes changed), in the front end.
void RecordId(const TexObj& o, uint32_t fmt, uint32_t levels, uint32_t size, uint64_t hash);
void Record(const TexObj& o, uint32_t fmt, uint32_t levels, uint32_t size, uint64_t hash) {
    if (Mode() == 0 || g_inReplay || !g_stage || !PlayNow()) return;
    if (KeyMode()) { RecordId(o, fmt, levels, size, hash); return; }  // (texwarm30)
    if (!texwarm::InWindow(NowMs(), g_openMs)) return;
    texwarm::Item it;
    it.hash = hash;
    it.addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o.data));
    it.size = size;
    it.w = o.width;
    it.h = o.height;
    it.fmt = static_cast<uint8_t>(fmt);
    it.levels = static_cast<uint8_t>(levels);
    if (!texwarm::Valid(it)) return;
    if (g_book.Note(g_stage, it)) ++g_cen.recorded;
}
// The end of an upload's decode+convert+send (DecodeTimer).
void Decoded(uint32_t bytes, double ms, bool chara) {
    if (Mode() == 0) return;
    if (g_inReplay) return;
    if (!PlayNow()) return;
    if (KeyMode()) { ++g_icen.firstP; g_icen.firstKiB += bytes >> 10; g_icen.firstMs += ms; }  // (texwarm30: the same count the old census keeps)
    bool newOver = false;
    g_cen.FirstUse(bytes >> 10, ms, chara, g_picMs, g_bOpen.maxMs, newOver);
    g_picMs += ms;
}
}  // namespace tw

static bool ResolveTextureFull(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: it reads the cache's entries; vgx2_tex_peek)
    out = TexBind{};
    if (g.bpTexStale & (1u << (texMap & 7))) BpTexSlotSync(texMap & 7); // Galaxy: the binding is what the registers say
    const TexSlot& slot = g.tex[texMap & 7];
    if (!slot.valid || !slot.obj.data || slot.obj.width == 0 || slot.obj.height == 0) return false;
    const TexObj& o = slot.obj;
    width = o.width;
    height = o.height;
    out.wrapS = o.wrapS > 2 ? WRAP_CLAMP : o.wrapS;
    out.wrapT = o.wrapT > 2 ? WRAP_CLAMP : o.wrapT;
    out.magFilter = o.magFilter ? FILT_LINEAR : FILT_NEAR;
    out.minFilter = o.minFilter > 5 ? FILT_LINEAR : o.minFilter;

    // An EFB copy made to this address wins over the bytes in guest RAM.
    if (const CopyEntry* ce = FindCopy(o.data)) {
        out.texId = ce->texId;
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
        // Render-target textures are never power-of-two sized in general: clamp only.
        out.wrapS = out.wrapT = WRAP_CLAMP;
        return true;
    }

    const uint32_t fmt = o.format;
    // Mip levels present in guest memory. GLES2 can only mipmap power-of-two
    // textures, and needs the full chain (completed in software below).
    uint32_t levels = 1;
    if (o.mipmap && IsPow2(o.width) && IsPow2(o.height)) {
        const uint32_t full = 32u - static_cast<uint32_t>(__builtin_clz(o.width > o.height ? o.width : o.height)); // 1 + log2 of the longer side
        const float ml = o.maxLod < 0 ? 0 : o.maxLod;
        levels = static_cast<uint32_t>(ml) + 1u;
        if (levels > full) levels = full;
    }
    // GXV_NOMIP=1 (diagnostic): never use mip levels.
    const bool noMip = VGX_KNOB(EnvInt("GXV_NOMIP", 0) != 0) != 0;
    bool wantMips = levels > 1 && out.minFilter >= FILT_NEAR_MIP_NEAR && !noMip;
    if (!wantMips) {
        levels = 1;
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    }
    if (!IsPow2(o.width) || !IsPow2(o.height)) {
        // GLES2 / the Vita GPU: non-power-of-two textures only clamp.
        if (out.wrapS != WRAP_CLAMP || out.wrapT != WRAP_CLAMP) stats::Note("non-power-of-two texture with repeat/mirror wrap (clamped)");
        out.wrapS = out.wrapT = WRAP_CLAMP;
    }

    Key key{o.data, static_cast<uint32_t>(o.width) | (static_cast<uint32_t>(o.height) << 16), fmt | (levels << 8), 0};
    const uint8_t* tlut = nullptr;
    uint32_t tlutFmt = 0, tlutEntries = 0;
    if (TexFormatIsCI(fmt)) {
        const uint32_t ti = o.tlut;
        if (ti < static_cast<uint32_t>(kTlutSlots) && g.tlut[ti].valid) {
            tlut = static_cast<const uint8_t*>(g.tlut[ti].obj.data);
            tlutFmt = g.tlut[ti].obj.format;
            tlutEntries = g.tlut[ti].obj.entries;
            key.tlutHash = TlutHash(static_cast<int>(ti));
        } else {
            VGX_LOG_N(8, "CI texture bound without a loaded TLUT (slot %u)\n", ti);
        }
    }
    if (TexDataSize(o.width, o.height, fmt, levels) > slot.avail) {
        // The mip chain the registers describe runs out of the memory the image lies in.
        if (TexDataSize(o.width, o.height, fmt, 1) > slot.avail) return false;
        levels = 1;
        wantMips = false;
        key.fmt = fmt | (levels << 8);
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    }
    Entry& e = CacheEntry(key);
    if (!e.size) e.size = TexDataSize(o.width, o.height, fmt, levels);
    const uint32_t size = e.size;
    e.lastUsed = CurrentFrame();
    bool upload = e.texId == 0;
    if (upload && g_popinTex && g_popinEvictedKeys.erase(key) != 0) {
        ++g_popinRe;  // (popin24: a texture the budget freed is decoded and uploaded again)
        g_popinReBytes += size;
    }
    // Cheapest path: no write notification at all has arrived since the contents were last
    // checked (read before the generation, so a write that lands in between is seen next time).
    const uint64_t epoch = HaveGuestRam() ? GuestWriteEpoch() : ~0ull;
    if (!upload && !(epoch != ~0ull && e.checkedEpoch == epoch && !g_texVerify)) {
        // Cheap path: the runtime's write-generation counters say nothing in the range changed.
        const uint64_t gen = Generation(o.data, size);
        std::atomic_thread_fence(std::memory_order_acquire);
        e.checkedEpoch = gen != ~0ull ? epoch : ~0ull;
        if (g_texVerify && gen != ~0ull && gen == e.generation && XXH3_64bits(o.data, size) != e.contentHash) {
            ++g_texStale;
            VGX_LOG_N(32, "texture cache: %ux%u fmt 0x%X at %p changed without a write notification\n", o.width, o.height, fmt, o.data);
            e.generation = ~0ull;
        }
        if (gen == ~0ull || gen != e.generation) {
            const uint64_t h = XXH3_64bits(o.data, size);
            ++stats::Cur().texHashes;
            stats::Cur().texHashBytes += size;
            if (gen == ~0ull) ++stats::Cur().texUntracked;
            if (h != e.contentHash) { e.contentHash = h; upload = true; }
            e.generation = gen;
        }
    } else if (upload) {
        e.generation = Generation(o.data, size);
        std::atomic_thread_fence(std::memory_order_acquire);
        e.contentHash = XXH3_64bits(o.data, size);
        CensusNewEntry(key, e.contentHash); // (texage24: was a texture like this freed before?)
        if (TexAdoptOn() && TexAdopt(e, e.contentHash, key)) upload = false; // (GXV_TEX_ADOPT: a live texture of the same bytes)
        else e.texId = NewGuestTexId();
    }
    // settingsnative: the button pictures were remade (the mapping was swapped): a texture the patch hook patches goes up again, once.
    {
        const uint32_t pg = g_texPatchGen.load(std::memory_order_relaxed);
        if (e.patchGen != pg) {
            e.patchGen = pg;
            if (!upload && g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height))) upload = true;
        }
    }
    // g30 Wave 7: a tiny single-level POT texture first uploaded with CLAMP/REPEAT (or pre-warmed/adopted)
    // may have been left linear by GXV_TEXUP_TINY_BYTES (<= 1024 B); re-upload once with mirrorWrap=true so SGX543 swizzles it.
    const bool wantMirror = (out.wrapS == WRAP_MIRROR || out.wrapT == WRAP_MIRROR);
    if (!upload && wantMirror && !e.everMirror && !o.mipmap && IsPow2(o.width) && IsPow2(o.height) &&
        o.width >= 8 && o.height >= 8 && static_cast<uint64_t>(o.width) * o.height <= 512u) {
        upload = true;
    }
    if (upload) {
        if (wantMirror) e.everMirror = true;
        if (e.uploads < 255) ++e.uploads;
        if (!TexFormatIsCI(fmt)) tw::Record(o, fmt, levels, size, e.contentHash); // (endwait26)
        else tw::NoteCI();                                                         // (texwarm30: the census counts them apart)
        // (GXV_TEX_COMPRESS / GXV_TEXCOMP_CENSUS: what vgx_texcomp.cpp made of this upload; kClsNone with both off)
        const bool tcMode = texcomp::Mode() != 0, tcCensus = texcomp::CensusOn();
        uint8_t tcCls = texcomp::kClsNone;
        // (GXV_STUTTER_LOG) the decode and conversion below, timed
        struct DecodeTimer {
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            uint32_t fmt = 0, w = 0, h = 0, bytes = 0; // (handover26)
            uint32_t texId = 0;                         // (endwait26)
            ~DecodeTimer() {
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                if (us > 0) g_texDecodeUs.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
                g_texDecodeN.fetch_add(1, std::memory_order_relaxed);        // (endwait26)
                g_texDecodeBytes.fetch_add(bytes, std::memory_order_relaxed);
                if (vgx_res_tex_note) vgx_res_tex_note(fmt, w, h, bytes, us > 0 ? static_cast<uint32_t>(us) : 0u);
                tw::Decoded(bytes, us > 0 ? us / 1000.0 : 0.0, IsCharaTexture(texId)); // (endwait26)
            }
        } decodeTimer;
        decodeTimer.texId = e.texId;
        decodeTimer.fmt = fmt;
        decodeTimer.w = o.width;
        decodeTimer.h = o.height;
        decodeTimer.bytes = size;
        const backend::Caps& caps = backend::GetCaps();
        TexUpload up;
        up.texId = e.texId;
        // (pix23: the sampled channels' ranges, TexChannelRange; set below for the formats whose bytes are known here)
        const bool wantRange = PixTexRangesOn();
        uint8_t rLo[4] = {0, 0, 0, 0}, rHi[4] = {255, 255, 255, 255};
        bool rKnown = false;
        up.width = o.width;
        up.height = o.height;
        up.mirrorWrap = wantMirror || e.everMirror;
        const uint8_t* const data = static_cast<const uint8_t*>(o.data);
        // The levels to send. Unless the back end takes a short chain (mipClamp), a
        // mipmapped texture is completed down to 1x1 below (no GL_TEXTURE_MAX_LEVEL in GLES2).
        uint32_t authored = levels;
        // A replacement image (vgx_hdtex.cpp: HD textures), asked only here, at upload.
        if (e.hd) { g_hdLive -= e.bytes; ++g_hdFreed; g_hdFreedBytes += e.bytes; e.hd = false; } // (rewritten: its replacement goes)
        bool hd = false, inPack = false, player = false;
        if (g_replaceHook && !(g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height)))) {
            TexReplaceQuery q{data, fmt, o.width, o.height, tlut, tlut ? (tlutEntries == 0xFFFF ? 32768u : tlutEntries * 2u) : 0u, caps.mipClamp, g_hdLive};
            q.inPack = &inPack;
            q.player = &player;
            hd = g_replaceHook(q, up);
            if (hd) authored = static_cast<uint32_t>(up.levels.size());
        }
        SetCharaTexture(e.texId, inPack, player); // (GXV_CHARAA: a character's or key item's texture, by the pack's list)
        if (TexMagEnabled()) { // (GXV_TEXMAG_CENSUS: the HD pack's key of this texture, so that the census joins the builder's list)
            uint64_t hk = XXH3_64bits_withSeed(data, TexLevelSize(o.width, o.height, fmt), static_cast<uint64_t>(fmt) | (static_cast<uint64_t>(o.width) << 8) | (static_cast<uint64_t>(o.height) << 20));
            const uint32_t tb = tlut ? (tlutEntries == 0xFFFF ? 32768u : tlutEntries * 2u) : 0u;
            if (tlut && tb) hk = XXH3_64bits_withSeed(tlut, tb, hk);
            TexMagNoteTexture(e.texId, hk, o.width, o.height, fmt);
        }
        // CMPR goes to the back end as DXT1 (an eighth of the memory, no decode) unless
        // a level uses the one thing DXT1 renders differently (see CmprToDxt1).
        //   GXV_DXT1=0  always decode;  =2  DXT1 for those textures too (their transparent
        //   texels become transparent black: dark fringes where they are filtered)
        const bool dxtAll = VGX_KNOB(EnvInt("GXV_DXT1", 1) == 2) != 0;
        // GXV_CMPRA=1 (default 0; the Strikers port's 1.3 MSC_VGX_CMPRA: colours can move a little): those textures go as DXT5 (CmprToDxt5)
        // instead of being decoded: 8 bits per texel instead of 32, and a quarter of the texture reads' traffic. Only where the chain
        // needs no completing (Caps::mipClamp: the console) or there is no chain; non-power-of-two sizes are still decoded (see below).
        const bool cmprA = VGX_KNOB(EnvInt("GXV_CMPRA", 0) != 0) != 0;
        bool dxt5 = false;
        bool dxt = !hd && fmt == 0xE && caps.dxt1;
        // (vitaGL stores compressed textures swizzled; for sizes that are not powers of two it
        // takes another path that has not been read through: those are decoded on the console)
        if (dxt && caps.mipClamp && !(IsPow2(o.width) && IsPow2(o.height))) dxt = false;
        if (dxt) {
            if (caps.mipClamp) { // (a back end with short chains cannot take compressed levels below one block)
                uint32_t lw = o.width, lh = o.height, n = 1;
                while (n < authored && (lw >> 1) >= 4 && (lh >> 1) >= 4) { lw >>= 1; lh >>= 1; ++n; }
                authored = n;
            }
            const uint8_t* src = data;
            uint32_t w = o.width, h = o.height;
            for (uint32_t l = 0; l < authored; ++l) {
                up.levels.emplace_back(Dxt1Size(w, h));
                if (!CmprToDxt1(src, w, h, up.levels.back().data(), dxtAll)) {
                    dxt = false;
                    dxt5 = cmprA && caps.dxt5 && (caps.mipClamp || !wantMips);
                    break;
                }
                src += TexLevelSize(w, h, fmt);
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            if (dxt5) { // (GXV_CMPRA: the same levels as DXT5)
                up.levels.clear();
                src = data;
                w = o.width;
                h = o.height;
                for (uint32_t l = 0; l < authored; ++l) {
                    up.levels.emplace_back(Dxt5Size(w, h));
                    CmprToDxt5(src, w, h, up.levels.back().data());
                    src += TexLevelSize(w, h, fmt);
                    w = w > 1 ? w / 2 : 1;
                    h = h > 1 ? h / 2 : 1;
                }
            } else if (!dxt) {
                up.levels.clear();
                authored = levels;
                ++g_cmprDecoded;
            }
        }
        if (hd) {
            // (up was filled by the hook: its own mip chain, sampled with a mip filter below)
        } else if (dxt5) {
            up.format = TF_DXT5;
        } else if (dxt) {
            up.format = TF_DXT1;
            if (wantMips && !caps.mipClamp) {
                // Complete the chain: decode the last authored level, halve, encode.
                uint32_t lw = o.width, lh = o.height;
                const uint8_t* src = data;
                for (uint32_t l = 1; l < authored; ++l) { src += TexLevelSize(lw, lh, fmt); lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
                std::vector<uint8_t> cur(static_cast<size_t>(lw) * lh * 4), next;
                DecodeTexture(src, lw, lh, fmt, nullptr, 0, 0, cur.data());
                while (lw > 1 || lh > 1) {
                    uint32_t nw, nh;
                    Downsample(cur.data(), lw, lh, next, nw, nh);
                    up.levels.emplace_back(Dxt1Size(nw, nh));
                    EncodeDxt1(next.data(), nw, nh, up.levels.back().data());
                    cur.swap(next);
                    lw = nw; lh = nh;
                }
            }
        } else if (caps.la88 && fmt <= 3 &&
                   (!o.mipmap || (la88::Candidate(fmt, o.width, o.height, wantMips && caps.mipGen,
                                                  VGX_KNOB(EnvInt("GXV_TEX16", 0)) >= 2) && CompactMipsReady())) &&
                   !(g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height)))) {
            const bool useI8 = caps.i8 && fmt <= 1 && !o.mipmap && !wantMips;
            if (useI8) {
                // g30 Wave 5 (GXV_LA88=2): non-mipmapped GX I4/I8 have L == A, so 1-byte TF_I8 (Vita GL_INTENSITY / U8_RRRR)
                // samples identically as (I,I,I,I) at half the VRAM, decode writes, and fetch bandwidth of TF_LA88.
                up.format = TF_I8;
                up.levels.emplace_back(static_cast<size_t>(o.width) * o.height);
                DecodeTextureI8(data, o.width, o.height, fmt, up.levels.back().data());
                if (wantRange) { RangeI8(up.levels.back(), rLo, rHi); rKnown = true; }
            } else {
                up.format = TF_LA88;
                up.levels.emplace_back(static_cast<size_t>(o.width) * o.height * 2);
                DecodeTextureLA88(data, o.width, o.height, fmt, up.levels.back().data());
                if (wantRange) { RangeLa88(up.levels.back(), rLo, rHi); rKnown = true; }
            }
            if (tcMode || tcCensus) {
                // (GXV_TEX_COMPRESS_IA: an I/IA texture is offered as its (L, L, L, A); without that switch it is only classed)
                texcomp::OfferIn in;
                in.texId = e.texId;
                in.ref = texcomp::EntryRef{key.data, key.dims, key.fmt, key.tlutHash};
                in.contentHash = e.contentHash;
                in.gxFmt = fmt;
                in.w = o.width;
                in.h = o.height;
                in.mips = o.mipmap && wantMips;
                in.reuploaded = e.uploads > 1;
                in.patched = IsCharaTexture(e.texId);
                if (useI8) in.i8 = up.levels.back().data();
                else in.la88 = up.levels.back().data();
                tcCls = tcMode ? texcomp::Offer(in) : texcomp::Classify(in);
            }
            // (GXV_LA88_MIPS: a mipmapped one sends level 0 only and the back end makes the levels, as the RGBA8 path does with GXV_MIPGEN;
            // without wantMips it is one level, as there; authored levels are never sent on this path, as there)
            up.genMips = o.mipmap && wantMips;
        } else {
            // RGBA8. A back end that makes its own mip levels (mipGen) only gets level 0.
            // Authored I4 alpha can differ completely from a downsample of level zero.
            // Keep this opt-in square/POT path separate from compact, HD and patched textures.
            up.preserveI4Mips = caps.authoredI4Mips && wantMips && fmt == 0 && o.width == o.height && o.width <= 4096
                && !(g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height)));
            up.genMips = wantMips && caps.mipGen && !up.preserveI4Mips;
            if (up.genMips) authored = 1;
            const uint8_t* src = data;
            uint32_t w = o.width, h = o.height;
            for (uint32_t l = 0; l < authored; ++l) {
                up.levels.emplace_back(static_cast<size_t>(w) * h * 4);
                DecodeTexture(src, w, h, fmt, tlut, tlutFmt, tlutEntries, up.levels.back().data());
                if (l == 0 && g_texPatchHook) g_texPatchHook(e.contentHash, w, h, up.levels.back().data());
                src += TexLevelSize(w, h, fmt);
                if (w == 1 && h == 1) break;
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            if ((tcMode || tcCensus) && !up.levels.empty() && up.levels[0].size() == static_cast<size_t>(o.width) * o.height * 4) {
                // GXV_TEX_COMPRESS (vgx_texcomp.cpp): offered for conversion before GXV_TEX16 packs the level (the copy is of the RGBA8 level 0;
                // nothing of this upload changes). With the switch off and the census on: only the class, for the "tex-census:" line.
                texcomp::OfferIn in;
                in.texId = e.texId;
                in.ref = texcomp::EntryRef{key.data, key.dims, key.fmt, key.tlutHash};
                in.contentHash = e.contentHash;
                in.gxFmt = fmt;
                in.w = o.width;
                in.h = o.height;
                in.mips = up.genMips;
                in.patched = (g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height))) ||
                             IsCharaTexture(e.texId); // (and the characters' textures of the HD pack's list: Mario, Luigi, Rosalina ...)
                in.ownChain = up.preserveI4Mips || up.levels.size() != 1 || (wantMips && !up.genMips);
                in.reuploaded = e.uploads > 1;
                in.rgba = up.levels[0].data();
                tcCls = tcMode ? texcomp::Offer(in) : texcomp::Classify(in);
            }
            if (wantMips && !up.genMips && !caps.mipClamp) {
                // Complete the chain down to 1x1.
                uint32_t lw = o.width, lh = o.height;
                for (size_t l = 1; l < up.levels.size(); ++l) { lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
                while (lw > 1 || lh > 1) {
                    std::vector<uint8_t> next;
                    uint32_t nw, nh;
                    Downsample(up.levels.back().data(), lw, lh, next, nw, nh);
                    up.levels.push_back(std::move(next));
                    lw = nw; lh = nh;
                }
            }
            // GXV_TEX16=1|2 (default 0; the Strikers port's 1.3 MSC_VGX_TEX16): a decoded texture whose texels all fit a 16-bit format goes in
            // that format: half the memory and half the texture reads' traffic (RGB565 sources always; RGB5A3, palettes and I4 when they fit:
            // decided from the texels, whatever the GX format). 1 = textures without mipmaps; 2 = mipmapped too (565 and 4444 only when vitaGL
            // makes the levels; its generated levels then round to the same bits). The GPU expands 5 bits to c / 31, not to the 8-bit replica
            // over 255: under half a step of 255, so an output can differ by one level at most. Not for the authored I4 chain (its own upload).
            if (wantRange) { RangeRgba8(up.levels, rLo, rHi); rKnown = true; }
            const long tex16 = static_cast<long>(VGX_KNOB(EnvInt("GXV_TEX16", 0)));
            if (tex16 && (tex16 >= 2 || !wantMips) && !up.preserveI4Mips && caps.tex16) {
                const uint8_t f16 = Rgba16Format(up.levels, !up.genMips);
                if (f16) {
                    for (std::vector<uint8_t>& lv : up.levels) PackRgba16(lv, f16);
                    up.format = f16;
                    // (pix23: the GPU expands 5- and 6-bit fields to within half a level of the bytes: one level either way, 0 and 255 stay)
                    for (int c = 0; c < 4; ++c) {
                        if (rLo[c] != 0 && rLo[c] != 255) --rLo[c];
                        if (rHi[c] != 0 && rHi[c] != 255) ++rHi[c];
                    }
                }
            }
        }
        if (wantRange && !hd && !dxt5 && dxt && up.format == TF_DXT1) { RangeDxt1(up.levels, rLo, rHi); rKnown = true; }
        if (wantRange) TexRangeSet(e.texId, rKnown && !hd, rLo, rHi);
        up.authored = static_cast<uint8_t>(authored < up.levels.size() ? authored : up.levels.size());
        if (player) { // (mario24: the back end binds this texture with the player's own bias / anisotropy, GXV_PLAYER_LODBIAS / _ANISO)
            up.player = true;
            uint8_t fm = o.minFilter > 5 ? static_cast<uint8_t>(FILT_LINEAR) : static_cast<uint8_t>(o.minFilter);
            const uint8_t origMin = fm;
            if (!wantMips && fm >= FILT_NEAR_MIP_NEAR) fm = (fm == FILT_NEAR_MIP_NEAR || fm == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
            if (hd && fm < FILT_NEAR_MIP_NEAR) fm = fm == FILT_NEAR ? FILT_NEAR_MIP_NEAR : g_replaceMipFilter; // (as the bind below)
            PlayerTexFact f;
            f.texId = e.texId;
            f.w = up.width; f.h = up.height; f.origW = o.width; f.origH = o.height;
            f.levels = static_cast<uint8_t>(up.genMips ? 0 : (up.levels.size() > 255 ? 255 : up.levels.size())); // (0: the back end makes the chain)
            f.gxFmt = static_cast<uint8_t>(fmt); f.origMin = origMin; f.finalMin = fm; f.hd = hd;
            PlayerFactNote(f);
            if (PlayerMipVisOn()) PlayerMipVis(up);
        }
        AmUpload(key, e, o, fmt, up, out.minFilter, hd, player); // (GXV_AUTOMIP: the census and, with =1, the chain; nothing with the switch off)
        e.mipmapped = wantMips || hd;
        {
            // Bytes the back end will hold for this texture.
            uint32_t bytes = 0;
            for (const auto& lv : up.levels) bytes += static_cast<uint32_t>(lv.size());
            if (up.genMips) bytes += bytes / 3;
            if (hd) {
                ++g_hdUploads; g_hdBytes += bytes; g_hdLive += bytes; e.hd = true;
                if (PlayNow()) { ++g_hdUploadsPlay; g_hdBytesPlay += bytes; } // (hdtex24: a first use in play)
            }
            else if (up.format == TF_DXT1) { ++g_dxtUploads; g_dxtBytes += bytes; }
            else if (up.format == TF_DXT5) { ++g_dxt5Uploads; g_dxt5Bytes += bytes; }
            else if (up.format == TF_I8) { ++g_i8Uploads; g_i8Bytes += bytes; }
            else if (up.format >= TF_RGB565) { ++g_t16Uploads; g_t16Bytes += bytes; }
            else if (up.format == TF_LA88) { ++g_laUploads; g_laBytes += bytes; }
            else { ++g_rgbaUploads; g_rgbaBytes += bytes; if (fmt == 0xE) g_cmprRgbaBytes += bytes; }
            ++g_fmtUploads[fmt & 15u];
            g_fmtBytes[fmt & 15u] += bytes;
            if (tcCensus) {
                if (hd) tcCls = texcomp::kClsPatched;
                else if (up.format == TF_DXT1 || up.format == TF_DXT5) tcCls = texcomp::kClsDxt;
                else if (up.format == TF_LA88 || up.format == TF_I8) tcCls = texcomp::kClsLa88;
                texcomp::CensusInfo(e.texId, fmt, o.width, o.height, up.format, bytes, wantMips || hd, tcCls);
            }
            TexFormatLogMaybe(); // (GXV_TEXFMT_LOG: the basic log's "texture uploads by GX format" line)
            ++stats::Cur().texUploads;
            stats::Cur().texUploadBytes += bytes;
            stats::TexLive(e.bytes ? 0 : 1, static_cast<int64_t>(bytes) - e.bytes);
            g_liveBytes += bytes;
            g_liveBytes -= e.bytes;
            e.bytes = bytes;
        }
        // Diagnostic hook of the host layer (GXV_TEXDUMP on the desktop harness): sees every uploaded texture.
        g_lastUploadHash = e.contentHash;
        if (g_texDumpHook) g_texDumpHook(up, fmt, CurrentFrame());
        CmdBuf& buf = Pending();
        buf.uploads.push_back(std::move(up));
        buf.cmds.push_back(Cmd{CMD_TEX_UPLOAD, static_cast<uint32_t>(buf.uploads.size() - 1)});
        stats::TexFormat(fmt, o.width, o.height, wantMips);
    }
    out.texId = e.texId;
    // A replacement is larger than the original by its scale and has a mip chain whose second
    // level is the original's size: sampled with mips, so that wherever the game draws the
    // texture at or below its original resolution (most of the time) no more texels are fetched
    // than before, and only close-ups read the larger level.
    if (e.hd && out.minFilter < FILT_NEAR_MIP_NEAR) out.minFilter = out.minFilter == FILT_NEAR ? FILT_NEAR_MIP_NEAR : g_replaceMipFilter;
    return true;
}

// ------------------------------------------------------------------ binding memo (GXV_TEXMEMO)
// A material switch binds its textures again: ResolveTexture rebuilt the texture object from the five registers of the texmap
// (BpTexSlotSync), worked out wrap, filters and mip levels (a division or two), asked whether the address is an EFB copy and
// looked the texture up in the cache, ~600 instructions, 300 times a picture in the Comet Observatory, for bindings that
// repeat every picture. The memo keeps, by the registers' values, what that produced: the binding, the size and the cache
// entry. It is good while palette memory, the set of EFB copies and the set of cache entries are unchanged (generation
// counters); the contents of the texture are checked on every use as the full path checks them (the write epoch, else the
// write generation), and anything else (changed contents, an upload, the verification modes of the cache) takes the full path.
//   GXV_TEXMEMO=0  off;  =2  the full path runs as well and must give the same binding (logged)
namespace {
struct TexMemo {
    uint32_t regs[5] = {};    // mode0, mode1, image0, image3, tlut
    uint32_t tlutGen = 0, copiesGen = 0, eraseGen = 0;
    TexBind out;
    uint16_t width = 0, height = 0;
    bool valid = false, ret = false;
    bool ci = false;          // a colour-indexed format (its palette slot is synchronised on every use)
    bool hd = false;          // the entry held an HD replacement when this was taken (its filter depends on that)
    Entry* e = nullptr;       // (nullptr: an EFB copy, or no texture)
    const void* data = nullptr;
    uint32_t size = 0;
};
TexMemo g_texMemo[256];
uint64_t g_texMemoHits = 0, g_texMemoChecked = 0, g_texMemoBad = 0;
int TexMemoMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        int v = static_cast<int>(EnvInt("GXV_TEXMEMO", 1));
        if (v == 2 && !diag::kEnabled) v = 1;
        return v;
    }()));
    return m;
}
inline void TexRegs(int m, uint32_t r[5]) {
    const int b = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
    r[0] = g.bp[b]; r[1] = g.bp[b + 4]; r[2] = g.bp[b + 8]; r[3] = g.bp[b + 0x14]; r[4] = g.bp[b + 0x18];
}
inline uint32_t TexMemoSlot(const uint32_t r[5]) {
    const uint32_t x = (r[3] * 0x9E3779B1u) ^ (r[2] * 0x85EBCA77u) ^ (r[0] * 0xC2B2AE3Du) ^ r[4];
    return (x ^ (x >> 16)) & 255u;
}
// The memo's answer, or false when the full path has to run (nothing has been changed then).
inline bool TexMemoHit(const TexMemo& t, int m, const uint32_t r[5], TexBind& out, uint16_t& width, uint16_t& height, bool& ret) {
    if (!t.valid || t.copiesGen != g_copiesGen || t.eraseGen != g_cacheEraseGen || t.tlutGen != g_tlutLoadGen) return false;
    if (t.regs[0] != r[0] || t.regs[1] != r[1] || t.regs[2] != r[2] || t.regs[3] != r[3] || t.regs[4] != r[4]) return false;
    if (t.e) {
        // ResolveTextureFull's check of the contents, without an upload
        Entry& e = *t.e;
        const uint64_t epoch = HaveGuestRam() ? GuestWriteEpoch() : ~0ull;
        if (!(epoch != ~0ull && e.checkedEpoch == epoch)) {
            const uint64_t gen = Generation(t.data, t.size);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (gen == ~0ull || gen != e.generation) return false; // (hashed and maybe uploaded by the full path)
            e.checkedEpoch = epoch;
        }
        if (e.hd != t.hd) return false; // (re-uploaded with or without its replacement since: the filter differs)
        e.lastUsed = CurrentFrame();
    }
    // (the registers' texture object and palette as the full path leaves them. A palette slot is shared state that the
    // sync rewrites, so for a colour-indexed texture the sync is done; any other slot may stay stale: whoever reads
    // g.tex[] brings it up to date first, as ResolveTextureFull and BpZTexConstant do)
    if (t.ci && (g.bpTexStale & (1u << m))) BpTexSlotSync(m);
    out = t.out;
    width = t.width; // (the callers pass zeros, and t holds what the full path left in them)
    height = t.height;
    ret = t.ret;
    return true;
}
} // namespace

// GXV_FE_PREFETCH (vgx_draw.cpp ResolvePrefetch): the memo slots of `texMaps` for their registers as they are now (step 1), then (2)
// the cache entry and the generation counter a hit checks. Only reads.
void TexMemoPrefetch(uint8_t texMaps, int step) {
    if (TexMemoMode() != 1 || g_texVerify) return;
    for (uint32_t mm = texMaps; mm; mm &= mm - 1) {
        const int m = __builtin_ctz(mm);
        uint32_t r[5];
        TexRegs(m, r);
        const TexMemo& t = g_texMemo[TexMemoSlot(r)];
        if (step == 1) {
            FePrefetchLines(&t, sizeof t, 3);
            continue;
        }
        if (!t.valid || !t.e || std::memcmp(t.regs, r, sizeof r) != 0) continue;
        FePrefetchLines(t.e, sizeof(Entry), 2);
        if (t.size) GuestWriteGenerationPrefetch(t.data, t.size);
    }
}
void TexMemoCounters(uint64_t out[3]) { out[0] = g_texMemoHits; out[1] = g_texMemoChecked; out[2] = g_texMemoBad; }
bool ResolveTexture(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    const int mode = TexMemoMode();
    if (mode == 0 || g_texVerify) return ResolveTextureFull(texMap, out, width, height);
    uint32_t r[5];
    TexRegs(texMap & 7, r);
    TexMemo& t = g_texMemo[TexMemoSlot(r)];
    bool ret = false;
    const uint16_t w0 = width, h0 = height;
    if (TexMemoHit(t, texMap & 7, r, out, width, height, ret)) {
        ++g_texMemoHits;
        if (diag::kEnabled && mode == 2) {
            TexBind full;
            uint16_t fw = w0, fh = h0;
            const bool fret = ResolveTextureFull(texMap, full, fw, fh);
            ++g_texMemoChecked;
            if (fret != ret || fw != width || fh != height || std::memcmp(&full, &out, sizeof full) != 0) {
                ++g_texMemoBad;
                VGX_LOG_N(16, "texmemo: MISMATCH (texmap %d, image %08X, texture %u against %u)\n", texMap, r[3], out.texId, full.texId);
            }
            if ((g_texMemoChecked & 0x3FFFu) == 0)
                VGX_LOG_N(200, "texmemo: %llu bindings checked, %llu mismatching\n", static_cast<unsigned long long>(g_texMemoChecked),
                          static_cast<unsigned long long>(g_texMemoBad));
            out = full; width = fw; height = fh;
            return fret;
        }
        return ret;
    }
    ret = ResolveTextureFull(texMap, out, width, height);
    // Remember it (the registers are what BpTexSlotSync read: the slot is in step with them now).
    const TexSlot& slot = g.tex[texMap & 7];
    t.valid = false;
    if (g.bpTexStale & (1u << (texMap & 7))) return ret;
    t.e = nullptr;
    if (ret && slot.valid && slot.obj.data && !FindCopy(slot.obj.data)) {
        // the cache entry the full path used (it exists now)
        const TexObj& o = slot.obj;
        // (the front of the cache holds what CacheEntry looked up last for this address class: the entry just used)
        const CacheFront& f = g_cacheFront[((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o.data)) >> 5) * 2654435761u) >> 22];
        if (!f.e || f.key.data != o.data || f.e->texId != out.texId) return ret;
        t.e = f.e;
        t.data = o.data;
        t.size = t.e->size;
        if (!t.size) return ret;
    } else if (ret && !(slot.valid && slot.obj.data && FindCopy(slot.obj.data))) {
        return ret; // (should not happen: a binding that is neither)
    }
    std::memcpy(t.regs, r, sizeof t.regs);
    t.ci = TexFormatIsCI((r[2] >> 20) & 0xF);
    t.tlutGen = g_tlutLoadGen;
    t.copiesGen = g_copiesGen;
    t.eraseGen = g_cacheEraseGen;
    t.out = out;
    t.hd = t.e && t.e->hd;
    t.width = width;
    t.height = height;
    t.ret = ret;
    t.valid = true;
    return ret;
}

// GXV_J3DNATIVE=4 / 5 (vgx_j3n2.h): a template's texture bindings. The registers are the template's (the register file may not hold
// them while a chain's lists are not applied); the memo slot is the one ResolveTexture would read for them. TexTouch is ResolveTexture's
// memo hit with its side effects (the entry's last use and content check); true only when it gives exactly the template's binding.
// TexKnown (no side effects) says whether a binding ResolveTexture just made is one the memo gives again (not a colour-indexed one: its
// hit synchronises the slot from the registers).
namespace j3n2 {
void TexRegsOf(int m, uint32_t r[5]) { TexRegs(m & 7, r); }
bool TexTouch(int m, const uint32_t r[5], const TexBind& expect, uint16_t w, uint16_t h) {
    if (TexMemoMode() != 1 || g_texVerify) return false;
    TexMemo& t = g_texMemo[TexMemoSlot(r)];
    if (!t.valid || t.ci) return false;
    TexBind out;
    uint16_t ww = 0, hh = 0;
    bool ret = false;
    if (!TexMemoHit(t, m & 7, r, out, ww, hh, ret)) return false;
    ++g_texMemoHits;
    TexBind e = expect; // (GXV_LODBIAS_SAMPLER: the draw's own sampler bias and census class are not part of what the memo resolves)
    e.lodSteps = 0; e.lodWhy = 0;
    return std::memcmp(&out, &e, sizeof out) == 0 && ww == w && hh == h;
}
bool TexKnown(int m, const uint32_t r[5], const TexBind& b, uint16_t w, uint16_t h) {
    (void)m;
    if (TexMemoMode() != 1 || g_texVerify) return false;
    const TexMemo& t = g_texMemo[TexMemoSlot(r)];
    if (!t.valid || t.ci || t.copiesGen != g_copiesGen || t.eraseGen != g_cacheEraseGen || t.tlutGen != g_tlutLoadGen) return false;
    if (std::memcmp(t.regs, r, sizeof t.regs) != 0) return false;
    TexBind e = b;
    e.lodSteps = 0; e.lodWhy = 0;
    return std::memcmp(&t.out, &e, sizeof b) == 0 && t.width == w && t.height == h;
}
} // namespace j3n2

// ================================================================ GXV_AUTOMIP (automip25; the switches: vgx_automip.h)
// WHAT: a texture the game declares without mip levels (GXInitTexObj without mipmap, or LOD range 0, or a plain linear filter) is sampled
// from its level 0 alone, however small it is drawn: far surfaces sparkle in motion and the GPU reads texels a pixel apart. With the
// switch on, such a texture is given a chain AT ITS FIRST UPLOAD, where it is decoded anyway (at the loading screen or the first pictures
// of a stage: no extra decode, no second upload), and the back end samples it with a mip filter (Tex::autoMip: LINEAR_MIPMAP_NEAREST or
// trilinear, and a bias half a level the other way). Opaque RGB565 / RGB5A3 / RGBA8 textures send level 0 only and the back end makes the
// levels by the transfer unit, as for every other mipmapped RGBA8 texture (GXV_MIPGEN); a CMPR texture costs the chain's encode here.
// WHICH (all of it must hold; the census, GXV_AUTOMIP=2, counts each reason):
//   - the game's min filter is linear (not NEAREST: a nearest texture is a mask, a font, a lookup: never touched), no chain (single level);
//   - RGB565 / RGB5A3 / RGBA8 (decoded to RGBA8) or CMPR (DXT1, opaque, at most GXV_AUTOMIP_DXT_TEXELS = 32768, and only while the picture is not live: a
//     loading screen or a fade; GXV_AUTOMIP_DXT_PLAY=1 allows it in play); not I4/I8/IA (masks, ramps, glows),
//     not colour-indexed (the palette changes under it); EFB copies never reach this function;
//   - both sides powers of two (the GPU only mips those), the larger >= GXV_AUTOMIP_MIN (64), the smaller >= 16 (a ramp or a strip is not);
//   - not an HD-pack texture, not Mario's or Luigi's or any texture of the HD pack's list (their sampling stays as it is), not patched by
//     the host (button pictures), not a texture whose bytes changed since its first upload (animated, or a re-used buffer);
//   - opaque (no texel under 255), unless GXV_AUTOMIP_ALPHA=1: then its own chain whose alpha keeps the coverage of the cut-out (automip::Coverage);
//   - the live extra memory stays under GXV_AUTOMIP_KB.
// THEN, for GXV_AUTOMIP_VERIFY pictures, the back end's usage marks (texcomp: perspective / orthographic / indirect) are watched: a texture
// that is drawn with an orthographic projection (layouts, the HUD, fonts, screen effects) or read as an indirect map is TAKEN BACK: its
// entry is freed, the next bind decodes it again the plain way, and it is banned from the switch. Until then a 2D draw of it samples level 0
// anyway wherever it is magnified or 1:1 (negative LOD): the picture does not change; a minified 2D draw is smoother.
// Memory: the levels are held by the texture and freed with it (CMD_TEX_FREE); the live extra bytes are counted again at every decision
// from the entries that still exist. A texture whose guest bytes change later is uploaded again by the ordinary path: no levels (Tex::autoMip false).
namespace {
enum AmReason : uint8_t { kAmCandidate = 0, kAmChain, kAmNearest, kAmSize, kAmFormat, kAmHd, kAmPlayer, kAmDynamic, kAmAlpha, kAmPatched, kAmCaps, kAmBanned, kAmBudget, kAmTooBig, kAmInPlay, kAmReasons };
const char* const kAmReasonName[kAmReasons] = {"candidate", "has levels", "nearest", "small/npot", "format", "hd", "player", "dynamic", "alpha", "patched", "no support", "banned", "budget", "cmpr too big", "cmpr in play"};
struct AmInfo {
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0, chain = 0, reason = 0; // chain: 0 single level, 1 full chain, 2 short chain
    uint64_t hash = 0;
    uint32_t bytes = 0;
};
struct AmWatch { Key key; uint32_t texId; uint64_t born; };
struct AmApplied { Key key; uint32_t texId, extra; };
std::mutex g_amMtx;                         // (g_amInfo: the front end writes at uploads, the back end reads at its census)
std::unordered_map<uint32_t, AmInfo> g_amInfo;
std::vector<AmWatch> g_amWatch;             // (front end thread) textures given a chain whose use is still being watched
std::vector<AmApplied> g_amApplied;
std::unordered_set<Key, KeyHash> g_amBan;   // textures taken back: plain from now on
struct AmCounters {
    uint64_t applied = 0, extraBytes = 0, confirmed = 0, taken2D = 0, takenInd = 0, expired = 0, us = 0, usMax = 0;
    uint64_t left[kAmReasons] = {};
    uint64_t lastLog = ~0ull;
} g_am;
std::chrono::steady_clock::time_point g_amLogT = std::chrono::steady_clock::now();

uint64_t AmLiveExtra() { // (the live applied textures; a freed or replaced one leaves the list)
    uint64_t live = 0;
    size_t keep = 0;
    for (size_t i = 0; i < g_amApplied.size(); ++i) {
        auto it = g_cache.find(g_amApplied[i].key);
        if (it == g_cache.end() || it->second.texId != g_amApplied[i].texId || !it->second.amApplied) continue;
        live += g_amApplied[i].extra;
        g_amApplied[keep++] = g_amApplied[i];
    }
    g_amApplied.resize(keep);
    return live;
}

// An ordinary upload (decoded, not yet pushed): the census and, in mode 1, the chain.
void AmUpload(const Key& key, Entry& e, const TexObj& o, uint32_t fmt, TexUpload& up, uint8_t minF, bool hd, bool player) {
    const automip::Cfg& c = automip::Config();
    if (c.mode == 0) return;
    e.amApplied = false; // (an upload by the ordinary path: whatever chain the texture had is gone)
    AmInfo in;
    in.w = o.width; in.h = o.height; in.fmt = static_cast<uint8_t>(fmt); in.hash = e.contentHash;
    for (const auto& lv : up.levels) in.bytes += static_cast<uint32_t>(lv.size());
    uint32_t full = 1;
    if (up.format == TF_DXT1 || up.format == TF_DXT5) { for (uint32_t w = o.width, h = o.height; (w >> 1) >= 4 && (h >> 1) >= 4; w >>= 1, h >>= 1) ++full; }
    else full = 32u - static_cast<uint32_t>(__builtin_clz(o.width > o.height ? o.width : o.height));
    in.chain = up.genMips ? 1 : up.levels.size() <= 1 ? 0 : up.levels.size() >= full ? 1 : 2;
    const backend::Caps& caps = backend::GetCaps();
    AmReason r = kAmCandidate;
    const bool rgba = up.format == TF_RGBA8 && up.levels.size() == 1 && !up.genMips;
    const bool dxt = up.format == TF_DXT1 && up.levels.size() == 1;
    const bool pow2 = IsPow2(o.width) && IsPow2(o.height);
    const uint32_t big = o.width > o.height ? o.width : o.height, small = o.width > o.height ? o.height : o.width;
    bool opaque = true;
    if (in.chain != 0) r = kAmChain;
    else if (hd) r = kAmHd;
    else if (player || IsCharaTexture(e.texId)) r = kAmPlayer;
    else if (minF != FILT_LINEAR) r = kAmNearest;
    else if (!pow2 || big < c.minSide || small < c.minSmall) r = kAmSize;
    else if (!(fmt == 4 || fmt == 5 || fmt == 6 || fmt == 0xE) || !(rgba || dxt)) r = kAmFormat;
    else if (e.uploads > 1) r = kAmDynamic;
    else if (g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height))) r = kAmPatched;
    else if (dxt && !(caps.dxt1 && caps.mipClamp)) r = kAmCaps; // (an RGBA8 texture always can: the back end's levels, or ours)
    else if (dxt && static_cast<uint64_t>(o.width) * o.height > c.dxtTexels) r = kAmTooBig;
    else if (dxt && !c.dxtPlay && PlayNow()) r = kAmInPlay; // (its levels are encoded on the CPU: not while the picture is live)
    else if (rgba && !(opaque = automip::Opaque(up.levels[0])) && (!c.alpha || PixTexRangesOn())) r = kAmAlpha; // (pix23's alpha range would have to be re-proved)
    else if (g_amBan.count(key)) r = kAmBanned;
    uint32_t extraBytes = 0;
    if (c.mode == 1 && r == kAmCandidate) {
        // the chain
        std::vector<std::vector<uint8_t>> chain; // (levels 1 ..: our own chain, when the back end does not make it)
        if (dxt) {
            std::vector<uint8_t> cur(static_cast<size_t>(o.width) * o.height * 4), next;
            DecodeTexture(static_cast<const uint8_t*>(o.data), o.width, o.height, fmt, nullptr, 0, 0, cur.data());
            uint32_t lw = o.width, lh = o.height;
            while ((lw >> 1) >= 4 && (lh >> 1) >= 4) { // (the rule of the authored chains with mipClamp: down to 4 x 4, which compressed blocks need)
                uint32_t nw, nh;
                Downsample(cur.data(), lw, lh, next, nw, nh);
                chain.emplace_back(Dxt1Size(nw, nh));
                EncodeDxt1(next.data(), nw, nh, chain.back().data());
                extraBytes += static_cast<uint32_t>(chain.back().size());
                cur.swap(next);
                lw = nw; lh = nh;
            }
        } else if (opaque && caps.mipGen) {
            extraBytes = static_cast<uint32_t>(up.levels[0].size() / 3); // (what the back end's levels will hold)
        } else {
            std::vector<std::vector<uint8_t>> all(1, up.levels[0]);
            uint32_t lw = o.width, lh = o.height;
            while (lw > 1 || lh > 1) {
                std::vector<uint8_t> next;
                uint32_t nw, nh;
                Downsample(all.back().data(), lw, lh, next, nw, nh);
                all.push_back(std::move(next));
                lw = nw; lh = nh;
            }
            if (!opaque && c.alphaMode != 2) automip::Coverage(all); // (GXV_AUTOMIP_ALPHA=2: the plain box average of the alpha, to show what the coverage rule is for)
            for (size_t k = 1; k < all.size(); ++k) { extraBytes += static_cast<uint32_t>(all[k].size()); chain.push_back(std::move(all[k])); }
        }
        if (AmLiveExtra() + extraBytes > c.budget) r = kAmBudget;
        else {
            if (!chain.empty()) for (auto& lv : chain) up.levels.push_back(std::move(lv));
            else up.genMips = true;
            up.authored = static_cast<uint8_t>(up.levels.size());
            up.autoMip = true;
            e.amApplied = true;
            e.amExtra = extraBytes;
            in.chain = 1;
            in.bytes += extraBytes;
            g_amApplied.push_back(AmApplied{key, e.texId, extraBytes});
            texcomp::UsageReset(e.texId); // (a new life of the id: the marks of an earlier one are not this texture's)
            g_amWatch.push_back(AmWatch{key, e.texId, CurrentFrame()});
            ++g_am.applied;
            g_am.extraBytes += extraBytes;
        }
    }
    in.reason = r;
    if (c.mode == 1 && r != kAmCandidate) ++g_am.left[r];
    std::lock_guard<std::mutex> l(g_amMtx);
    g_amInfo[e.texId] = in;
}

// Front-end frame end (mode 1): textures given a chain are watched; one drawn in 2D or read as an indirect map is taken back. And the log line.
void AmFrameEnd() {
    const automip::Cfg& c = automip::Config();
    if (c.mode != 1) return;
    if (!g_amWatch.empty()) {
        two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: the entries' fields)
        const uint64_t now = CurrentFrame();
        size_t keep = 0;
        for (size_t i = 0; i < g_amWatch.size(); ++i) {
            const AmWatch w = g_amWatch[i];
            auto it = g_cache.find(w.key);
            if (it == g_cache.end() || it->second.texId != w.texId || !it->second.amApplied) continue; // (gone, or uploaded again the plain way)
            const uint8_t use = texcomp::Usage(w.texId);
            if (use & (texcomp::kUse2D | texcomp::kUseInd)) {
                // taken back: the entry is freed as the budget's eviction frees it; the next bind decodes the texture the plain way
                (use & texcomp::kUse2D ? g_am.taken2D : g_am.takenInd) += 1;
                g_amBan.insert(w.key);
                Entry& e = it->second;
                CmdBuf& buf = Pending();
                two::Bump(VGX2_EP_TEX);
                buf.cmds.push_back(Cmd{CMD_TEX_FREE, w.texId});
                stats::TexLive(-1, -static_cast<int64_t>(e.bytes));
                g_liveBytes -= e.bytes;
                CensusFreed(it->first, it->second);
                g_cache.erase(it);
                ReleaseGuestTexId(w.texId);
                CacheFrontClear();
                ++g_cacheEraseGen;
                g.texDirty = true;
                g.texSlotDirty = 0xFF;
                continue;
            }
            if (now - w.born >= c.verify) { ++(use & texcomp::kUse3D ? g_am.confirmed : g_am.expired); continue; } // (watched long enough)
            g_amWatch[keep++] = w;
        }
        g_amWatch.resize(keep);
    }
    const auto t = std::chrono::steady_clock::now();
    const uint64_t sig = g_am.applied * 1000003ull + g_am.taken2D * 31ull + g_am.takenInd;
    if (port_log_summary && t - g_amLogT >= std::chrono::seconds(c.sec) && sig != g_am.lastLog) {
        g_amLogT = t;
        g_am.lastLog = sig;
        const uint64_t live = AmLiveExtra();
        char left[300];
        int n = 0;
        left[0] = 0;
        for (int k = 1; k < kAmReasons && n < static_cast<int>(sizeof left) - 40; ++k)
            if (g_am.left[k]) n += std::snprintf(left + n, sizeof left - n, "%s %u, ", kAmReasonName[k], static_cast<unsigned>(g_am.left[k]));
        port_log_summary("automip: %u textures given a chain (%u KiB extra in all, %u KiB live of %u KiB); watched %u: %u confirmed 3D, %u never drawn, taken back %u (2D) + %u (indirect); "
                         "left plain at upload: %s",
                         static_cast<unsigned>(g_am.applied), static_cast<unsigned>(g_am.extraBytes >> 10), static_cast<unsigned>(live >> 10), static_cast<unsigned>(c.budget >> 10),
                         static_cast<unsigned>(g_amWatch.size()), static_cast<unsigned>(g_am.confirmed), static_cast<unsigned>(g_am.expired), static_cast<unsigned>(g_am.taken2D),
                         static_cast<unsigned>(g_am.takenInd), left);
    }
}
} // namespace

// ---- GXV_AUTOMIP=2 (census only): the back end's draws (render thread) against what the front end knows of each texture
namespace automip {
namespace {
struct Acc { double px = 0; uint32_t draws = 0; uint8_t use = 0; bool rt = false; };
std::unordered_map<uint32_t, Acc> g_acc; // (render thread)
uint32_t g_pics = 0;
std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
} // namespace
void NoteDraw(uint32_t texId, float px, uint8_t use, bool renderTarget) {
    if (!texId) return;
    Acc& a = g_acc[texId];
    if (px > 0.0f) a.px += px;
    ++a.draws;
    a.use |= use;
    a.rt = a.rt || renderTarget;
}
void Picture() {
    ++g_pics;
    const automip::Cfg& c = automip::Config();
    const auto now = std::chrono::steady_clock::now();
    if (now - g_t0 < std::chrono::seconds(c.sec) || !port_log_summary) return;
    const double secs = std::chrono::duration<double>(now - g_t0).count();
    g_t0 = now;
    struct Row { uint32_t id; AmInfo in; Acc a; double ratio; };
    std::vector<Row> rows;
    uint32_t nSingle = 0, nFull = 0, nShort = 0, nOther = 0, single3D = 0, single2D = 0, singleInd = 0, cand = 0;
    uint32_t reasonN[kAmReasons] = {};
    uint32_t prox[3] = {};
    uint64_t candBytes = 0, candExtra = 0;
    {
        std::lock_guard<std::mutex> l(g_amMtx);
        for (const auto& kv : g_acc) {
            auto it = g_amInfo.find(kv.first);
            if (kv.second.rt || it == g_amInfo.end()) { ++nOther; continue; }
            const AmInfo& in = it->second;
            const Acc& a = kv.second;
            if (in.chain == 0) ++nSingle; else if (in.chain == 1) ++nFull; else ++nShort;
            if (in.chain != 0) continue;
            if (a.use & texcomp::kUse2D) ++single2D;
            if (a.use & texcomp::kUseInd) ++singleInd;
            const bool only3D = (a.use & texcomp::kUse3D) && !(a.use & (texcomp::kUse2D | texcomp::kUseInd));
            if (only3D) ++single3D;
            ++reasonN[in.reason];
            if (in.reason == kAmCandidate && only3D) {
                ++cand;
                const uint64_t lvl0 = static_cast<uint64_t>(in.w) * in.h * (in.fmt == 0xE ? 0.5 : 4.0);
                candBytes += lvl0;
                candExtra += lvl0 / 3;
                // proxy: the pixels all the draws of it cover in a picture against its texels (the UV range is not known here: a tiled
                // texture reads as magnified, a stretched one as minified)
                const double perPic = a.px / static_cast<double>(g_pics ? g_pics : 1);
                const double ratio = perPic / (static_cast<double>(in.w) * in.h);
                ++prox[ratio < 0.5 ? 0 : ratio < 2.0 ? 1 : 2];
                rows.push_back(Row{kv.first, in, a, ratio});
            }
        }
    }
    port_log_summary("automip-census %.0f s (%u pictures): %u textures drawn: single level %u, full chain %u, short chain %u, other (render targets, unknown) %u; of the single-level: "
                     "3D only %u, also 2D %u, also indirect %u; qualifying by the static rules and 3D only: %u (level 0 %u KiB, a chain adds %u KiB), by the px/texel proxy "
                     "(<0.5 likely minified) %u / (0.5-2) %u / (>2 magnified or tiled) %u",
                     secs, static_cast<unsigned>(g_pics), static_cast<unsigned>(g_acc.size()), static_cast<unsigned>(nSingle), static_cast<unsigned>(nFull), static_cast<unsigned>(nShort),
                     static_cast<unsigned>(nOther), static_cast<unsigned>(single3D), static_cast<unsigned>(single2D), static_cast<unsigned>(singleInd), static_cast<unsigned>(cand),
                     static_cast<unsigned>(candBytes >> 10), static_cast<unsigned>(candExtra >> 10), static_cast<unsigned>(prox[0]), static_cast<unsigned>(prox[1]), static_cast<unsigned>(prox[2]));
    {
        char line[700];
        int n = std::snprintf(line, sizeof line, "automip-census single-level textures by rule: ");
        for (int k = 0; k < kAmReasons && n > 0 && n < static_cast<int>(sizeof line) - 40; ++k)
            if (reasonN[k]) n += std::snprintf(line + n, sizeof line - n, "%s %u; ", kAmReasonName[k], static_cast<unsigned>(reasonN[k]));
        port_log_summary("%s", line);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return static_cast<uint64_t>(a.in.w) * a.in.h > static_cast<uint64_t>(b.in.w) * b.in.h; });
    static const char* const kGx[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
        const Row& r = rows[i];
        port_log_summary("automip-census candidate %u: hash %016llx %s %ux%u, %.0f px a picture in %u draws, %.2f px/texel, a chain adds %u KiB", static_cast<unsigned>(i + 1),
                         static_cast<unsigned long long>(r.in.hash), kGx[r.in.fmt & 15u], static_cast<unsigned>(r.in.w), static_cast<unsigned>(r.in.h),
                         r.a.px / static_cast<double>(g_pics ? g_pics : 1), static_cast<unsigned>(r.a.draws), r.ratio,
                         static_cast<unsigned>(static_cast<uint64_t>(r.in.w) * r.in.h * (r.in.fmt == 0xE ? 0.5 : 4.0) / 3 / 1024));
    }
    g_acc.clear();
    g_pics = 0;
}
} // namespace automip

// GXV_TEX_COMPRESS (vgx_texcomp.cpp): a finished conversion replaces its texture when the entry still holds the same bytes under the same
// id and the back end has drawn it in 3D only (never 2D, never as an indirect map). The replacement is one more upload under the same id:
// DXT1 / DXT5 levels from the worker; the binding, the filters and the entry stay. With pix23's ranges on, only when the compressed texels
// stay inside the range already published for the id (then the narrower truth is published).
namespace {
texcomp::Verdict TexCompApply(texcomp::Result& r, void*) {
    two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: the entry's fields)
    auto it = g_cache.find(Key{r.ref.data, r.ref.dims, r.ref.fmt, r.ref.tlutHash});
    if (it == g_cache.end()) return texcomp::kStale;
    Entry& e = it->second;
    if (e.texId != r.texId || e.contentHash != r.contentHash || e.hd || r.levels.empty()) return texcomp::kStale;
    if (e.amApplied) return texcomp::kStale; // (GXV_AUTOMIP gave it a chain: the single-level conversion would take it away)
    const uint8_t use = texcomp::Usage(r.texId);
    if (use & texcomp::kUse2D) return texcomp::kDrop2D;
    if (use & texcomp::kUseInd) return texcomp::kDropInd;
    if (!(use & texcomp::kUse3D)) return texcomp::kKeep;
    if (PixTexRangesOn()) {
        uint8_t lo[4], hi[4];
        if (TexChannelRange(e.texId, lo, hi))
            for (int c = 0; c < 4; ++c)
                if (r.lo[c] < lo[c] || r.hi[c] > hi[c]) return texcomp::kDropRange;
        TexRangeSet(e.texId, true, r.lo, r.hi);
    }
    TexUpload up;
    up.texId = e.texId;
    up.width = r.w;
    up.height = r.h;
    up.format = r.format;
    up.levels = std::move(r.levels);
    up.authored = static_cast<uint8_t>(up.levels.size());
    uint32_t bytes = 0;
    for (const auto& lv : up.levels) bytes += static_cast<uint32_t>(lv.size());
    texcomp::NoteApplied(e.bytes, bytes);
    if (texcomp::CensusOn()) texcomp::CensusInfo(e.texId, r.ref.fmt & 0xFFu, r.w, r.h, up.format, bytes, e.mipmapped, 0);
    ++stats::Cur().texUploads;
    stats::Cur().texUploadBytes += bytes;
    stats::TexLive(0, static_cast<int64_t>(bytes) - e.bytes);
    g_liveBytes += bytes;
    g_liveBytes -= e.bytes;
    e.bytes = bytes;
    CmdBuf& buf = Pending();
    buf.uploads.push_back(std::move(up));
    buf.cmds.push_back(Cmd{CMD_TEX_UPLOAD, static_cast<uint32_t>(buf.uploads.size() - 1)});
    return texcomp::kApplied;
}
} // namespace

// A memory budget for guest textures. The cache otherwise only grows: every
// texture the game has ever bound stays in the back end (menus, both teams'
// models, every stadium visited), which a 128 MB graphics memory cannot
// afford over a session. When the live total exceeds the budget at the end of
// a frame, the least recently bound textures are freed, oldest first, until
// it fits; a texture bound in the last `kMinIdle` frames is never freed (if
// the working set itself exceeds the budget, the budget loses: thrashing
// uploads would be worse). A freed texture that is bound again is decoded
// from guest RAM again like a new one.
//   GXV_TEXBUDGET=<MiB>   (default 0: keep everything)   or SetTextureBudget() from the host layer
void SetTextureBudget(uint64_t bytes) {
    g_budget = bytes;
    g_budgetSet = true;
}

namespace tw {
// texwarm30: the upload both replays make. 0 = uploaded, 1 = the texture was there already, 2 = dropped (the back end gave no texture). The bytes at `p` are decoded and sent exactly as the
// first bind of the same texture would do it (the cache's own path: the same key, the same hash, the same conversion): whatever the caller believed about them, what the game then
// binds from that address is this texture or, if the bytes changed meanwhile, the cache's ordinary check uploads them again.
static int ReplayUpload(const void* p, uint32_t w, uint32_t h, uint32_t fmtv, uint32_t levels, uint32_t size, double& ms) {
    ms = 0;
    {
        two::AGuard lk;
        const Key key{p, w | (h << 16), fmtv | (levels << 8), 0};
        const auto f = g_cache.find(key);
        if (f != g_cache.end() && f->second.texId != 0) return 1;
    }
    const TexSlot saved = g.tex[7];
    const uint8_t staleBit = g.bpTexStale & 0x80u;
    TexSlot& s = g.tex[7];
    s = TexSlot{};
    s.valid = true;
    s.avail = size;
    TexObj& o = s.obj;
    std::memset(&o, 0, sizeof o);
    o.data = p;
    o.width = static_cast<uint16_t>(w);
    o.height = static_cast<uint16_t>(h);
    o.format = fmtv;
    o.wrapS = o.wrapT = WRAP_CLAMP;
    o.magFilter = 1;
    o.mipmap = levels > 1 ? 1 : 0;
    o.minFilter = levels > 1 ? FILT_LIN_MIP_LIN : FILT_LINEAR;
    o.maxLod = static_cast<float>(levels - 1);
    g.bpTexStale = static_cast<uint8_t>(g.bpTexStale & ~0x80u);
    TexBind out{};
    uint16_t rw = 0, rh = 0;
    const double t0 = NowMs();
    g_inReplay = true;
    const bool ok = ResolveTextureFull(7, out, rw, rh);
    g_inReplay = false;
    ms = NowMs() - t0;
    g.tex[7] = saved;
    g.bpTexStale = static_cast<uint8_t>((g.bpTexStale & ~0x80u) | staleBit);
    return (!ok || out.texId == 0) ? 2 : 0;
}
static texwarm::Outcome TryItem(const texwarm::Item& it, double& ms, uint32_t& kib) {
    ms = 0; kib = 0;
    const void* p = reinterpret_cast<const void*>(static_cast<uintptr_t>(it.addr));
    if (!IsGuestRam(p, it.size)) return texwarm::kMissMemory;
    if (XXH3_64bits(p, it.size) != it.hash) return texwarm::kMissBytes;
    const int r = ReplayUpload(p, it.w, it.h, it.fmt, it.levels, it.size, ms);
    if (r == 1) return texwarm::kAlready;
    if (r == 2) return texwarm::kDropped;
    kib = it.size >> 10;
    return texwarm::kHit;
}

// ---- texwarm30: the identity-keyed replay. Front end thread, once a picture (StepId).
static inline bool Pow2u(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }
static void AddCand(const RawCand& r) {
    if (r.fmt > 14 || r.fmt == texid::kFmtC4 || r.fmt == texid::kFmtC8 || r.fmt == texid::kFmtC14X2) return;
    if (r.w < 1 || r.h < 1 || r.w > 1024 || r.h > 1024) return;
    const uint32_t l0 = TexDataSize(r.w, r.h, r.fmt, 1);
    if (l0 < 8) return;
    const void* p = reinterpret_cast<const void*>(static_cast<uintptr_t>(r.addr));
    if (!IsGuestRam(p, l0)) return;
    // the levels the game's bind will key (vgx_bpsync.cpp / ResolveTextureFull): a mip filter, a max lod of 1 or more, power-of-two sides; at most the levels the model holds
    uint32_t levels = 1;
    const float maxLod = static_cast<float>(static_cast<int8_t>(r.maxLod8)) * 0.125f;
    if (r.minF >= 2 && r.minF <= 5 && maxLod >= 1.0f && Pow2u(r.w) && Pow2u(r.h)) {
        const uint32_t full = 32u - static_cast<uint32_t>(__builtin_clz(r.w > r.h ? static_cast<uint32_t>(r.w) : static_cast<uint32_t>(r.h)));
        levels = static_cast<uint32_t>(maxLod) + 1u;
        if (levels > full) levels = full;
        if (r.mips >= 1 && levels > r.mips) levels = r.mips;
    }
    texid::Cand c;
    c.addr = r.addr;
    c.w = r.w;
    c.h = r.h;
    c.fmt = r.fmt;
    c.levels = static_cast<uint8_t>(levels);
    c.seq = r.order;
    c.maxBytes = TexDataSize(r.w, r.h, r.fmt, std::max<uint32_t>(levels, r.mips ? r.mips : 1u));
    c.qh = texid::QuickKey(XXH3_64bits(p, texid::QuickLen(l0)));
    g_ci.Add(c);
}
static void DrainInbox() {
    std::vector<RawCand> take;
    bool done = false;
    uint32_t dk = 0, ds = 0;
    {
        std::lock_guard<std::mutex> lk(g_inMu);
        take.swap(g_inbox);
        done = g_inDone;
        dk = g_inDoneKey;
        ds = g_inDoneSerial;
        g_inDone = false;
    }
    for (const RawCand& r : take) {
        if (r.key != g_ci.Key() || (g_ciSerial != 0xFFFFFFFFu && r.serial != g_ciSerial)) {
            if (r.key == g_ci.Key() && g_ciSerial != 0xFFFFFFFFu && r.serial < g_ciSerial) continue;  // (a straggler of an older load of the same stage)
            g_ci.Begin(r.key);   // (another stage, or the same stage loaded again: its textures lie elsewhere now)
            g_ciSerial = r.serial;
        } else if (g_ciSerial == 0xFFFFFFFFu) {
            g_ciSerial = r.serial;
        }
        AddCand(r);
    }
    if (done) {
        if (dk != g_ci.Key() || g_ciSerial == 0xFFFFFFFFu) { g_ci.Begin(dk); g_ciSerial = ds; }   // (a walk that found no texture at all, or the first word of a new load)
        if (dk == g_ci.Key() && ds == g_ciSerial) g_ci.SetWalkDone(true);                         // (a straggling "done" of an older load does not count)
    }
    g_icen.cands = static_cast<unsigned>(g_ci.Size());
}
// One book item: find its texture among the candidates, confirm the bytes, upload. `ms` is the whole attempt.
static texid::Outcome TryId(const texid::Item& it, double& ms, uint32_t& kib, bool& byScan, unsigned& dups) {
    ms = 0; kib = 0; dups = 0;
    byScan = it.qh == 0;
    const double t0 = NowMs();
    uint32_t idx[texid::kMaxScan];
    const unsigned n = g_ci.Key() == g_stage ? g_ci.Shape(it, idx, texid::kMaxScan) : 0u;
    const bool walkDone = g_ci.Key() == g_stage && g_ci.WalkDone();
    if (n == 0) {
        if (!walkDone) return texid::kWait;
        return g_ci.ShapeCount(it) ? texid::kMissBytes : texid::kMissNoCand;
    }
    uint32_t match[3];
    unsigned matched = 0;
    bool anyMem = false, anyDiffer = false;
    for (unsigned k = 0; k < n; ++k) {
        texid::Cand& c = g_ci.At(idx[k]);
        const void* p = reinterpret_cast<const void*>(static_cast<uintptr_t>(c.addr));
        if (!IsGuestRam(p, it.size)) { anyMem = true; continue; }
        if (c.hashLen != it.size) { c.hashAt = XXH3_64bits(p, it.size); c.hashLen = it.size; }   // (the full hash, once for a size)
        if (c.hashAt != it.hash) { anyDiffer = true; continue; }
        if (matched < 3) match[matched++] = idx[k];
    }
    if (!matched) {
        if (!walkDone) return texid::kWait;
        return (anyMem && !anyDiffer) ? texid::kMissMemory : texid::kMissBytes;
    }
    bool hit = false, there = false;
    for (unsigned j = 0; j < matched; ++j) {
        texid::Cand& c = g_ci.At(match[j]);
        c.uploaded = true;
        double one = 0;
        const int r = ReplayUpload(reinterpret_cast<const void*>(static_cast<uintptr_t>(c.addr)), it.w, it.h, it.fmt, it.levels, it.size, one);
        if (r == 0) { if (hit) ++dups; hit = true; kib += it.size >> 10; } else if (r == 1) there = true;
    }
    ms = NowMs() - t0;
    return hit ? texid::kHit : (there ? texid::kAlready : texid::kDropped);
}
// First visit (GXV_TEXWARM_ENUM): the candidates no book item named, in walk order, within a count and a size.
static void BuildEnum() {
    g_enumBuilt = true;
    g_enumList.clear();
    g_enumPos = 0;
    static const unsigned maxN = static_cast<unsigned>(std::max(0L, EnvInt("GXV_TEXWARM_ENUM_MAX", 160)));
    static const uint64_t maxBytes = static_cast<uint64_t>(std::max(0L, EnvInt("GXV_TEXWARM_ENUM_MIB", 8))) << 20;
    uint64_t bytes = 0;
    std::vector<uint32_t> all;
    for (size_t i = 0; i < g_ci.Size(); ++i) if (!g_ci.At(i).uploaded) all.push_back(static_cast<uint32_t>(i));
    std::sort(all.begin(), all.end(), [](uint32_t a, uint32_t b) { return g_ci.At(a).seq < g_ci.At(b).seq; });
    for (uint32_t i : all) {
        const texid::Cand& c = g_ci.At(i);
        const uint32_t size = TexDataSize(c.w, c.h, c.fmt, c.levels);
        if (size < 64 || size > (1u << 20)) continue;
        if (g_enumList.size() >= maxN || bytes + size > maxBytes) break;
        bytes += size;
        g_enumList.push_back(i);
    }
}
static void LogCensusId(const char* when) {
    if (!port_log_summary) return;
    char line[1600];
    g_icen.gaveUp = g_sch.GaveUp();
    if (!std::strcmp(when, "+20 s")) {   // (which of the unbooked uploads the game bound in the window)
        unsigned bound = 0;
        two::AGuard lk;
        for (const EnumKey& k : g_enumKeys) { const auto f = g_cache.find(k.key); if (f != g_cache.end() && f->second.lastUsed > k.frame) ++bound; }
        g_icen.enumBound = bound;
    }
    texid::Format(line, sizeof line, g_icen, when, Mode(), static_cast<unsigned>(g_sch.Left()), g_ci.Key() == g_stage && g_ci.WalkDone());
    port_log_summary("%s", line);
}
static void StepId() {
    const bool hold = vgx_smooth_hold_active && vgx_smooth_hold_active() != 0;
    const bool play = PlayNow() && !hold;
    const double now = NowMs();
    const bool loading = LoadingNow();
    uint32_t key = port_scene_key ? port_scene_key() : 0;
    if (!play && port_scene_loading_key) { const uint32_t lk = port_scene_loading_key(); if (lk) key = lk; }
    if (!g_idLoaded) { g_idLoaded = true; IdLoad(); GovReset(); }
    if (loading) g_wasLoading = true;
    const bool rearm = !loading && g_wasLoading && key && key == g_stage;
    if (!loading) g_wasLoading = false;
    if (key && (key != g_stage || rearm)) {
        g_stage = key;
        GovReset();
        g_icen.Reset(key);
        g_openMs = play ? now : 0;
        g_logOpen = g_logLate = false;
        IdLoadStage(key);
        const std::vector<texid::Item>* items = g_ib.Items(key);
        g_iplan = items ? *items : std::vector<texid::Item>();
        texid::Book::SortForReplay(g_iplan);   // (most likely first: what was first used earliest last visit)
        g_sch.Begin(g_iplan.size());
        g_icen.bookN = static_cast<unsigned>(g_iplan.size());
        g_stagePics = 0;
        g_enumBuilt = false;
        g_enumList.clear();
        g_enumPos = 0;
        g_enumKeys.clear();
        // The candidates of another stage, or of an earlier load of this one (its archives lay elsewhere), are not this load's; the game's walk of this load, if it has begun, tagged its
        // candidates with the scene serial (port_scene_serial) and they are kept.
        const uint32_t serialNow = port_scene_serial ? port_scene_serial() : 0;
        if (g_ci.Key() != key || (g_ciSerial != 0xFFFFFFFFu && g_ciSerial != serialNow)) { g_ci.Begin(key); g_ciSerial = 0xFFFFFFFFu; }
    }
    DrainInbox();
    ++g_stagePics;
    if (play && g_openMs == 0) g_openMs = now;
    if (!play && g_wasPlay) g_openMs = 0;
    g_wasPlay = play;
    g_picMs = 0;
    if (!g_stage) return;
    if (play && !g_logOpen) { g_logOpen = true; g_icen.leftAtOpen = static_cast<unsigned>(g_sch.Left()); g_icen.openSeen = true; LogCensusId("screen open"); }
    if (play && !g_logLate && now - g_openMs >= 20000.0) { g_logLate = true; LogCensusId("+20 s"); }
    const bool openWindow = play && Mode() >= 2 && now - g_openMs <= OpenWindowMs();
    if (vgx_be_tex_totals) { uint64_t kib = 0; double ms = 0; if (vgx_be_tex_totals(&kib, &ms)) g_gov.Observe(kib, ms); }
    const bool governed = SmoothLevel() > 0 && !play;
    const bool canWork = !loading && (!play || openWindow) && !(governed && g_gov.Exhausted());
    const bool enumOn = EnumMode() && !play;
    if (canWork && (g_sch.Active() || (enumOn && g_ci.Key() == g_stage && g_ci.WalkDone()))) {
        texid::Budget& b = play ? OpenBudget() : g_ibClosed;
        b.NewPicture();
        const uint32_t kibCap = b.maxKiB;
        if (governed) b.maxKiB = std::min(b.maxKiB, g_gov.KiBLeft());
        size_t idx = 0;
        bool worked = false;
        while (g_sch.Next(g_pic, idx)) {
            const texid::Item& it = g_iplan[idx];
            if (!b.Allow(it.size >> 10)) break;
            double ms = 0;
            uint32_t kib = 0;
            bool scan = false;
            unsigned dups = 0;
            const texid::Outcome oc = g_sch.Result(idx, TryId(it, ms, kib, scan, dups), g_pic);
            switch (oc) {
                case texid::kHit:
                    b.Spend(ms, kib);
                    worked = true;
                    if (scan) ++g_icen.foundScan; else ++g_icen.foundQuick;
                    g_icen.dupUploaded += dups;
                    g_icen.upKiB += kib;
                    g_icen.upMs += ms;
                    if (play) { ++g_icen.upOpen; g_openReplayMs += ms; } else { ++g_icen.upClosed; g_closedKiB += kib; g_closedMs += ms; }
                    if (governed) g_gov.Spend(kib, ms);
                    if (g_ib.Note(g_stage, it, true)) NoteDirty(g_stage);
                    break;
                case texid::kAlready:
                    ++g_icen.already;
                    if (scan) ++g_icen.foundScan; else ++g_icen.foundQuick;
                    if (g_ib.Note(g_stage, it, true)) NoteDirty(g_stage);
                    break;
                case texid::kMissNoCand: ++g_icen.missNoCand; break;
                case texid::kMissBytes: ++g_icen.missBytes; break;
                case texid::kMissMemory: ++g_icen.missMem; break;
                case texid::kDropped: ++g_icen.dropped; break;
                default: break;   // (kWait: the walk has not finished: no miss)
            }
        }
        if (enumOn && g_ci.Key() == g_stage && g_ci.WalkDone() && g_sch.Due(g_pic) == 0) {
            if (!g_enumBuilt) BuildEnum();
            while (g_enumPos < g_enumList.size()) {
                const texid::Cand& c = g_ci.At(g_enumList[g_enumPos]);
                const uint32_t size = TexDataSize(c.w, c.h, c.fmt, c.levels);
                if (!b.Allow(size >> 10)) break;
                ++g_enumPos;
                const void* p = reinterpret_cast<const void*>(static_cast<uintptr_t>(c.addr));
                if (!IsGuestRam(p, size)) continue;
                double ms = 0;
                const int r = ReplayUpload(p, c.w, c.h, c.fmt, c.levels, size, ms);
                if (r != 0) continue;
                const uint32_t kib = size >> 10;
                b.Spend(ms, kib);
                worked = true;
                ++g_icen.enumUp; g_icen.enumKiB += kib;
                g_icen.upKiB += kib; g_icen.upMs += ms;
                g_closedKiB += kib; g_closedMs += ms;
                if (governed) g_gov.Spend(kib, ms);
                if (g_enumKeys.size() < 512) g_enumKeys.push_back(EnumKey{Key{p, static_cast<uint32_t>(c.w) | (static_cast<uint32_t>(c.h) << 16), static_cast<uint32_t>(c.fmt) | (static_cast<uint32_t>(c.levels) << 8), 0}, CurrentFrame()});
            }
        }
        if (worked && !play) ++g_icen.frames;
        b.maxKiB = kibCap;
    }
    unsigned pend = 0;
    if (SmoothLevel() > 0 && !loading && !(governed && g_gov.Exhausted())) {
        const bool walkSeen = g_ci.Key() == g_stage && (g_ci.WalkDone() || g_ci.Size() > 0);
        if (g_sch.Active() && (walkSeen || g_stagePics < 120)) pend += static_cast<unsigned>(g_sch.Due(g_pic + 1));   // (no walk after 120 pictures: nothing to wait for)
        if (enumOn && g_enumBuilt && g_enumPos < g_enumList.size()) pend += static_cast<unsigned>(g_enumList.size() - g_enumPos);
    }
    g_pendingA.store(pend, std::memory_order_relaxed);
    if (!play) IdSave();
}
// Once per picture, first in TexCacheEndFrame.
static void Step() {
    ++g_pic;
    if (Mode() == 0) return;  // (openstall26: off is off; nothing below is needed, and the census lines of an unused switch are not wanted)
    if (KeyMode()) { StepId(); return; }  // (texwarm30)
    const bool hold = vgx_smooth_hold_active && vgx_smooth_hold_active() != 0;
    const bool play = PlayNow() && !hold;  // (openstall26: a screen held closed for the warm-up is not play)
    const double now = NowMs();
    const bool loading = LoadingNow();
    uint32_t key = port_scene_key ? port_scene_key() : 0;
    if (!play && port_scene_loading_key) { const uint32_t lk = port_scene_loading_key(); if (lk) key = lk; }  // (openstall26: the stage being loaded, until its scene starts)
    if (!g_loaded) { g_loaded = true; Load(); GovReset(); }
    if (loading) g_wasLoading = true;
    const bool rearm = !loading && g_wasLoading && key && key == g_stage;  // (a load ended for the stage the plan is already for: the scene drop freed its textures)
    if (!loading) g_wasLoading = false;
    if (key && (key != g_stage || rearm)) {
        g_stage = key;
        GovReset();
        g_cen.Reset(key);
        g_openMs = play ? now : 0;
        g_logOpen = g_logLate = false;
        const std::vector<texwarm::Item>* items = g_book.Items(key);
        g_plan = items ? *items : std::vector<texwarm::Item>();
        g_replay.Begin(&g_plan);
        g_cen.planned = static_cast<unsigned>(g_plan.size());
    }
    if (play && g_openMs == 0) g_openMs = now;
    if (!play && g_wasPlay) g_openMs = 0;
    g_wasPlay = play;
    g_picMs = 0;
    if (!g_stage) return;
    if (play && !g_logOpen) { g_logOpen = true; LogCensus("screen open"); }
    if (play && !g_logLate && now - g_openMs >= 20000.0) { g_logLate = true; LogCensus("+20 s"); }
    const bool openWindow = play && Mode() >= 2 && now - g_openMs <= 3000.0;
    if (vgx_be_tex_totals) { uint64_t kib = 0; double ms = 0; if (vgx_be_tex_totals(&kib, &ms)) g_gov.Observe(kib, ms); }
    const bool governed = SmoothLevel() > 0 && !play;
    if (g_replay.Active() && !loading && (!play || openWindow) && !(governed && g_gov.Exhausted())) {
        texwarm::Budget& b = play ? g_bOpen : g_bClosed;
        b.NewPicture();
        const uint32_t kibCap = b.maxKiB;
        if (governed) b.maxKiB = std::min(b.maxKiB, g_gov.KiBLeft());
        size_t idx = 0;
        while (g_replay.Next(g_pic, idx)) {
            const texwarm::Item& it = g_plan[idx];
            if (!b.Allow(it.size >> 10)) break;
            double ms = 0;
            uint32_t kib = 0;
            const texwarm::Outcome oc = TryItem(it, ms, kib);
            g_replay.Result(idx, oc, g_pic);
            switch (oc) {
                case texwarm::kHit:
                    b.Spend(ms, kib);
                    ++g_cen.hit; g_cen.hitKiB += kib; g_cen.hitMs += ms;
                    if (play) { ++g_cen.replayOpen; g_openReplayMs += ms; } else { ++g_cen.replayClosed; g_closedKiB += kib; g_closedMs += ms; }
                    if (governed) g_gov.Spend(kib, ms);
                    break;
                case texwarm::kAlready: ++g_cen.already; break;
                case texwarm::kMissBytes: ++g_cen.missBytes; break;
                case texwarm::kMissMemory: ++g_cen.missMemory; break;
                default: break;
            }
        }
        b.maxKiB = kibCap;
    }
    g_pendingA.store(SmoothLevel() > 0 && !loading && g_replay.Active() && !(governed && g_gov.Exhausted()) ? static_cast<unsigned>(g_replay.Due(g_pic + 1)) : 0u, std::memory_order_relaxed);
    if (!play && g_book.Dirty()) Save();
}
}  // namespace tw

// ---- openstall26 (GXV_SMOOTHSTART): what the back end's closed-screen hold and its census ask of the replay. Any thread (plain reads of counters the front end writes).
unsigned TexWarmPending() { return tw::g_pendingA.load(std::memory_order_relaxed); }
void TexWarmSnapshot(unsigned* planned, unsigned* closedN, unsigned long long* closedKiB, double* closedMs, unsigned* openReplay, unsigned* firstN, unsigned long long* firstKiB,
                     double* firstMs, unsigned* missed, unsigned* gaveUp, double* msPerKiB) {
    if (tw::Mode() != 0 && tw::KeyMode()) {   // (texwarm30)
        *planned = tw::g_icen.bookN;
        *closedN = tw::g_icen.upClosed;
        *closedKiB = tw::g_closedKiB;
        *closedMs = tw::g_closedMs;
        *openReplay = tw::g_icen.upOpen;
        *firstN = tw::g_icen.firstP;
        *firstKiB = tw::g_icen.firstKiB;
        *firstMs = tw::g_icen.firstMs;
        *missed = tw::g_icen.missNoCand + tw::g_icen.missBytes + tw::g_icen.missMem;
        *gaveUp = tw::g_sch.GaveUp();
        *msPerKiB = tw::g_gov.msPerKiB;
        return;
    }
    *planned = tw::g_cen.planned;
    *closedN = tw::g_cen.replayClosed;
    *closedKiB = tw::g_closedKiB;
    *closedMs = tw::g_closedMs;
    *openReplay = tw::g_cen.replayOpen;
    *firstN = tw::g_cen.firstUseN;
    *firstKiB = tw::g_cen.firstUseKiB;
    *firstMs = tw::g_cen.firstUseMs;
    *missed = tw::g_cen.missBytes + tw::g_cen.missMemory;
    *gaveUp = tw::g_replay.GaveUp();
    *msPerKiB = tw::g_gov.msPerKiB;
}

void TexCacheEndFrame() {
    tw::Step(); // (endwait26: GXV_TEXWARM)
    if (PlayNow()) ++g_picPlay; // (texage24: the pictures in play of a scene, for GXV_TEX_SCENEDROP=3's short-scene rule)
    if (texcomp::Mode()) texcomp::FrameEnd(&TexCompApply, nullptr); // (GXV_TEX_COMPRESS: finished conversions replace their textures)
    AmFrameEnd(); // (GXV_AUTOMIP=1: textures the game declared without levels, drawn in 3D only, get their chain)
    static const uint64_t envBudget = static_cast<uint64_t>(EnvInt("GXV_TEXBUDGET", 0)) << 20;
    const uint64_t budget = g_budgetSet ? g_budget : envBudget;
    static const bool popinTex = EnvInt("GXV_POPIN_LOG", 0) != 0;
    g_popinTex = popinTex;
    if (popinTex && port_log_summary && (CurrentFrame() & 127u) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (g_popinLastFrame == 0) {
            g_popinLastTime = now;
            g_popinLastFrame = CurrentFrame();
        } else if (now - g_popinLastTime >= std::chrono::seconds(10)) {
            port_log_summary("popin-tex: %.0f s: %llu textures freed for the texture budget (%.1f MiB), %llu of them uploaded again since (%.1f MiB re-decoded on first use), "
                             "%llu frames ended over budget; %.1f MiB held of the budget %.0f MiB (0 = none)",
                             std::chrono::duration<double>(now - g_popinLastTime).count(), static_cast<unsigned long long>(g_popinEv), g_popinEvBytes / 1048576.0,
                             static_cast<unsigned long long>(g_popinRe), g_popinReBytes / 1048576.0, static_cast<unsigned long long>(g_popinOver),
                             g_liveBytes / 1048576.0, budget / 1048576.0);
            g_popinEv = g_popinEvBytes = g_popinRe = g_popinReBytes = g_popinOver = 0;
            g_popinLastTime = now;
        }
    }
    if (!budget || g_liveBytes <= budget) return;
    constexpr uint64_t kMinIdle = 30;
    const uint64_t now = CurrentFrame();
    struct Cand {
        uint64_t lastUsed;
        uint32_t texId;
        Key key;
    };
    static std::vector<Cand> cand;
    cand.clear();
    for (const auto& kv : g_cache)
        if (kv.second.texId && kv.second.lastUsed + kMinIdle < now) cand.push_back(Cand{kv.second.lastUsed, kv.second.texId, kv.first});
    // Oldest first; the texture id breaks ties so that the order (and with it the
    // command stream) does not depend on the hash map's iteration order.
    // (grow24, GXV_TEX_EVICTHEAP, default 1) a heap instead of a full sort: the same order (a strict total order: ids are unique), but a frame that
    // frees a few textures of a thousand pays for the few, not for sorting all
    static const bool useHeap = EnvInt("GXV_TEX_EVICTHEAP", 1) != 0;
    const auto older = [](const Cand& a, const Cand& b) { return a.lastUsed != b.lastUsed ? a.lastUsed < b.lastUsed : a.texId < b.texId; };
    const auto newer = [&older](const Cand& a, const Cand& b) { return older(b, a); };
    if (useHeap) std::make_heap(cand.begin(), cand.end(), newer); // (front: the oldest)
    else std::sort(cand.begin(), cand.end(), older);
    CmdBuf& buf = Pending();
    two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: erasures)
    two::Bump(VGX2_EP_TEX);
    auto last = cand.end();
    for (size_t ci = 0; ci < cand.size(); ++ci) {
        if (useHeap) {
            std::pop_heap(cand.begin(), last, newer);
            --last;
        }
        const Cand& c = useHeap ? *last : cand[ci];
        if (g_liveBytes <= budget) break;
        auto it = g_cache.find(c.key);
        const uint32_t bytes = it->second.bytes;
        if (it->second.hd) { g_hdLive -= bytes; ++g_hdFreed; g_hdFreedBytes += bytes; }
        buf.cmds.push_back(Cmd{CMD_TEX_FREE, c.texId});
        stats::TexLive(-1, -static_cast<int64_t>(bytes));
        g_liveBytes -= bytes;
        ++g_evicted;
        g_evictedBytes += bytes;
        if (popinTex) {
            ++g_popinEv;
            g_popinEvBytes += bytes;
            if (g_popinEvictedKeys.size() >= 16384) g_popinEvictedKeys.clear();
            g_popinEvictedKeys.insert(c.key);
        }
        ++stats::Cur().texEvicted;
        stats::Cur().texEvictBytes += bytes;
        CensusFreed(it->first, it->second); // (texage24: the refetch census)
        g_cache.erase(it);
        ReleaseGuestTexId(c.texId); // (grow24: the id is handed out again after its quarantine, with GXV_TEX_IDREUSE)
        CacheFrontClear();
        ++g_cacheEraseGen;
    }
    if (g_liveBytes > budget) { ++g_overBudgetFrames; ++g_popinOver; }
    // (grow24, GXV_TEX_IDREUSE) a texmap slot that kept its binding (g.tex / slotBind) must not point at an id that now means another texture:
    // every slot is looked up again at the next draw. (Without reuse a stale id only ever named nothing.)
    if (TexIdReuseOn()) { g.texDirty = true; g.texSlotDirty = 0xFF; }
}

// grow24 (GXV_TEX_SCENEDROP, see AllocTextureId's section): a scene was destroyed. Front-end thread, before the first command published after
// its destruction (vgx_galaxy.cpp CopyDropService, with GXV_COPY_DROP's and GXV_SL_SCENEDROP's). Counts the cache's entries by where the
// image lies and, with `mode` 1 / 2, frees those of the destroyed heaps [lo[i], hi[i]) (and, 2, those not bound since the previous scene
// was destroyed). Each freed texture: CMD_TEX_FREE in the command stream (ordered after every draw already recorded), its entry erased, the
// byte counters and the id table updated. The next bind of the same image makes a new entry and uploads it again.
TexSceneDrop TextureCacheSceneGone(const void* const lo[], const void* const hi[], int n, int mode) {
    TexSceneDrop r;
    r.mode = mode;
    r.cacheBefore = static_cast<unsigned>(g_cache.size());
    r.idsMade = TextureIdsMade();
    // (texage24) one record per entry: whether the destroyed scene bound it, and what the rules made of it (0 keep, 1 free, 2 free for the cap)
    struct Cand { uint32_t texId; Key key; uint32_t bytes; uint32_t boundScene; uint64_t lastUsed; uint8_t take; bool bound; };
    static std::vector<Cand> cand;
    cand.clear();
    const uint64_t prevGone = g_sceneGoneFrame;
    ++g_sceneSeq;
    const uint64_t playPics = g_picPlay - g_picPlayAtGone;
    const long minFrames = TexDropMinFrames();
    const bool shortScene = mode >= 3 && static_cast<long>(playPics < (1ull << 31) ? playPics : (1ull << 31)) < minFrames;
    const int age = TexDropAge();
    r.playPics = static_cast<unsigned>(playPics);
    r.shortScene = shortScene;
    r.age = age;
    for (auto& kv : g_cache) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(kv.first.data);
        bool in = false;
        for (int i = 0; i < n && !in; ++i)
            in = lo[i] && hi[i] && a >= reinterpret_cast<uintptr_t>(lo[i]) && a < reinterpret_cast<uintptr_t>(hi[i]);
        Entry& en = kv.second;
        const bool bound = en.lastUsed >= prevGone; // (some frame since the previous scene was destroyed bound it)
        r.inRange += in ? 1u : 0u;
        r.idleOutside += (!in && !bound) ? 1u : 0u;
        uint8_t take = 0;
        if (mode >= 1 && (in || (mode == 2 && !bound))) take = 1;
        if (mode >= 3) {
            if (bound) { en.idleScenes = 0; en.boundScene = g_sceneSeq; }
            else if (!shortScene) {
                if (en.idleScenes < 0xFFFF) ++en.idleScenes;
                if (!take && en.idleScenes >= age) { take = 1; ++r.aged; }
            }
        }
        cand.push_back(Cand{en.texId, kv.first, en.bytes, en.boundScene, en.lastUsed, take, bound});
    }
    if (mode >= 3) {
        // The caps: entries and texel bytes kept after the age rule; idle ones go oldest first (by the scene they were last bound in, then the
        // frame; the texture id breaks ties) until both fit. A texture the destroyed scene bound is never one of them.
        const long maxEntries = TexDropMaxEntries(), maxKiB = TexDropMaxKiB();
        size_t entries = 0;
        uint64_t bytes = 0;
        for (const Cand& c : cand)
            if (!c.take) { ++entries; bytes += c.bytes; }
        const auto over = [&] { return (maxEntries > 0 && entries > static_cast<size_t>(maxEntries)) || (maxKiB > 0 && bytes > static_cast<uint64_t>(maxKiB) * 1024u); };
        if (over()) {
            std::vector<size_t> idle;
            for (size_t i = 0; i < cand.size(); ++i)
                if (!cand[i].take && !cand[i].bound) idle.push_back(i);
            std::sort(idle.begin(), idle.end(), [&](size_t x, size_t y) {
                const Cand& p = cand[x]; const Cand& q = cand[y];
                if (p.boundScene != q.boundScene) return p.boundScene < q.boundScene;
                if (p.lastUsed != q.lastUsed) return p.lastUsed < q.lastUsed;
                return p.texId != q.texId ? p.texId < q.texId : reinterpret_cast<uintptr_t>(p.key.data) < reinterpret_cast<uintptr_t>(q.key.data);
            });
            for (size_t i : idle) {
                if (!over()) break;
                cand[i].take = 2;
                --entries;
                bytes -= cand[i].bytes;
                ++r.freedCap;
            }
        }
    }
    g_sceneGoneFrame = CurrentFrame();
    g_picPlayAtGone = g_picPlay;
    ++g_dropScenes;
    // (by id, so that the command stream does not depend on the hash map's order; entries that never got a texture last)
    size_t keep = 0;
    for (size_t i = 0; i < cand.size(); ++i)
        if (cand[i].take) cand[keep++] = cand[i];
    cand.resize(keep);
    if (!cand.empty()) {
        std::sort(cand.begin(), cand.end(), [](const Cand& x, const Cand& y) { return x.texId != y.texId ? x.texId < y.texId : reinterpret_cast<uintptr_t>(x.key.data) < reinterpret_cast<uintptr_t>(y.key.data); });
        CmdBuf& buf = Pending();
        two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: erasures)
        two::Bump(VGX2_EP_TEX);
        for (const Cand& c : cand) {
            auto it = g_cache.find(c.key);
            if (it == g_cache.end()) continue;
            const uint32_t bytes = it->second.bytes;
            if (c.texId) {
                if (it->second.hd) { g_hdLive -= bytes; ++g_hdFreed; g_hdFreedBytes += bytes; }
                buf.cmds.push_back(Cmd{CMD_TEX_FREE, c.texId});
                stats::TexLive(-1, -static_cast<int64_t>(bytes));
                g_liveBytes -= bytes;
                ++r.freed;
                r.freedBytes += bytes;
                CensusFreed(it->first, it->second); // (texage24: the refetch census)
            } else {
                ++r.freedNoTexture;
            }
            g_cache.erase(it);
            if (c.texId) ReleaseGuestTexId(c.texId);
        }
        CacheFrontClear();
        ++g_cacheEraseGen;
        g.texDirty = true;
        g.texSlotDirty = 0xFF;
        g_dropFreed += r.freed;
        g_dropFreedBytes += r.freedBytes;
    }
    r.cacheAfter = static_cast<unsigned>(g_cache.size());
    r.liveBytes = g_liveBytes;
    r.idsFree = TextureIdsFree();
    r.idsReused = static_cast<unsigned>(g_idsReused);
    r.refAddr = static_cast<unsigned>(g_census.refAddr);
    r.refAddrLive = static_cast<unsigned>(g_census.refAddrLive);
    r.refContent = static_cast<unsigned>(g_census.refContent);
    r.refContentLive = static_cast<unsigned>(g_census.refContentLive);
    r.refAddrScene = static_cast<unsigned>(g_census.refAddr - g_censusAtGone.refAddr);
    r.refContentScene = static_cast<unsigned>(g_census.refContent - g_censusAtGone.refContent);
    r.adopted = static_cast<unsigned>(g_census.adopted);
    g_censusAtGone = g_census;
    return r;
}
// (texage24) the census for the back end's 5-s line: freed, refetched at the same address (and in play), same content at another address
// (and in play), adopted. Front-end writes, any thread reads (relaxed counters are not needed: a torn read of a log figure is harmless).
extern "C" void vgx_tex_census(uint64_t out[6]) {
    out[0] = g_census.freed; out[1] = g_census.refAddr; out[2] = g_census.refAddrLive;
    out[3] = g_census.refContent; out[4] = g_census.refContentLive; out[5] = g_census.adopted;
}

void TexCacheReport(FILE* f) {
    std::fprintf(f, "texture cache: %u entries, %.1f MB live; %llu changed without a write notification (GXV_TEXTRUST=verify)\n", static_cast<unsigned>(g_cache.size()),
                 g_liveBytes / 1048576.0, static_cast<unsigned long long>(g_texStale));
    std::fprintf(f, "texture uploads: %llu as DXT1 (%.1f MB), %llu as RGBA8 (%.1f MB); of the RGBA8 ones %llu are CMPR textures with transparent "
                    "texels (%.1f MB), which DXT1 would render with dark fringes\n",
                 static_cast<unsigned long long>(g_dxtUploads), g_dxtBytes / 1048576.0, static_cast<unsigned long long>(g_rgbaUploads),
                 g_rgbaBytes / 1048576.0, static_cast<unsigned long long>(g_cmprDecoded), g_cmprRgbaBytes / 1048576.0);
    if (g_laUploads) std::fprintf(f, "texture uploads: %llu as LA88 (%.1f MB)\n", static_cast<unsigned long long>(g_laUploads), g_laBytes / 1048576.0);
    if (g_i8Uploads) std::fprintf(f, "texture uploads: %llu as I8 (%.1f MB)\n", static_cast<unsigned long long>(g_i8Uploads), g_i8Bytes / 1048576.0);
    if (g_dxt5Uploads) std::fprintf(f, "texture uploads: %llu CMPR as DXT5 (%.1f MB, GXV_CMPRA)\n", static_cast<unsigned long long>(g_dxt5Uploads), g_dxt5Bytes / 1048576.0);
    if (g_t16Uploads) std::fprintf(f, "texture uploads: %llu in 16 bits (%.1f MB, GXV_TEX16)\n", static_cast<unsigned long long>(g_t16Uploads), g_t16Bytes / 1048576.0);
    {
        static const char* const kFmt[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
        for (int i = 0; i < 16; ++i)
            if (g_fmtUploads[i]) std::fprintf(f, "texture uploads by GX format: %s %llu (%.1f MB uploaded)\n", kFmt[i], static_cast<unsigned long long>(g_fmtUploads[i]), g_fmtBytes[i] / 1048576.0);
    }
    if (g_hdUploads)
        std::fprintf(f, "texture uploads: %llu replaced by HD textures (%.1f MB; %.1f MB alive now)\n", static_cast<unsigned long long>(g_hdUploads),
                     g_hdBytes / 1048576.0, g_hdLive / 1048576.0);
    const uint64_t budget = g_budgetSet ? g_budget : (static_cast<uint64_t>(EnvInt("GXV_TEXBUDGET", 0)) << 20);
    if (budget)
        std::fprintf(f, "texture budget %.0f MB: %llu textures freed (%.1f MB), %llu frames ended over budget (working set larger than the budget)\n",
                     budget / 1048576.0, static_cast<unsigned long long>(g_evicted), g_evictedBytes / 1048576.0,
                     static_cast<unsigned long long>(g_overBudgetFrames));
}

// ---- GXV_FE_TWOCORE (vgx_twocore.h): the second worker's texture bindings, read only, under its lock
// ResolveTextureFull for the texture object of `slotP` (the worker's TexSlot, synchronised from its registers) and the palette hash it
// worked out (TlutHash's), without changing anything: 0 when the full path would change the cache (a new entry, an upload, a hash of
// changed contents) or the verify mode is on; the worker's segment then ends. The contents' test is the generation's (the full path's
// write-epoch shortcut only skips that test when nothing was written since it last passed: the same answer).
extern "C" int vgx2_tex_peek(const void* slotP, uint64_t tlutHash, void* outBind, uint16_t* w, uint16_t* h, int* ret, void** entry) {
    const TexSlot& slot = *static_cast<const TexSlot*>(slotP);
    TexBind& out = *static_cast<TexBind*>(outBind);
    uint16_t& width = *w;
    uint16_t& height = *h;
    *entry = nullptr;
    *ret = 0;
    if (g_texVerify) return 0;
    out = TexBind{};
    if (!slot.valid || !slot.obj.data || slot.obj.width == 0 || slot.obj.height == 0) return 1;
    const TexObj& o = slot.obj;
    width = o.width;
    height = o.height;
    out.wrapS = o.wrapS > 2 ? WRAP_CLAMP : o.wrapS;
    out.wrapT = o.wrapT > 2 ? WRAP_CLAMP : o.wrapT;
    out.magFilter = o.magFilter ? FILT_LINEAR : FILT_NEAR;
    out.minFilter = o.minFilter > 5 ? FILT_LINEAR : o.minFilter;
    {
        auto it = g_copies.find(o.data);
        if (it != g_copies.end()) {
            out.texId = it->second.texId;
            if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
            out.wrapS = out.wrapT = WRAP_CLAMP;
            *ret = 1;
            return 1;
        }
    }
    const uint32_t fmt = o.format;
    uint32_t levels = 1;
    if (o.mipmap && IsPow2(o.width) && IsPow2(o.height)) {
        const uint32_t full = 32u - static_cast<uint32_t>(__builtin_clz(o.width > o.height ? o.width : o.height));
        const float ml = o.maxLod < 0 ? 0 : o.maxLod;
        levels = static_cast<uint32_t>(ml) + 1u;
        if (levels > full) levels = full;
    }
    const bool noMip = VGX_KNOB(EnvInt("GXV_NOMIP", 0) != 0) != 0;
    bool wantMips = levels > 1 && out.minFilter >= FILT_NEAR_MIP_NEAR && !noMip;
    if (!wantMips) {
        levels = 1;
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    }
    if (!IsPow2(o.width) || !IsPow2(o.height)) {
        out.wrapS = out.wrapT = WRAP_CLAMP;
    }
    Key key{o.data, static_cast<uint32_t>(o.width) | (static_cast<uint32_t>(o.height) << 16), fmt | (levels << 8), 0};
    if (TexFormatIsCI(fmt)) {
        if (!tlutHash) return 0; // (no palette: the full path logs it)
        key.tlutHash = tlutHash;
    }
    if (TexDataSize(o.width, o.height, fmt, levels) > slot.avail) {
        if (TexDataSize(o.width, o.height, fmt, 1) > slot.avail) return 1; // (false, unbound: as the full path)
        levels = 1;
        wantMips = false;
        key.fmt = fmt | (levels << 8);
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    }
    auto it = g_cache.find(key);
    if (it == g_cache.end()) return 0;
    Entry& e = it->second;
    if (!e.texId || !e.size) return 0;
    const uint64_t gen = Generation(o.data, e.size);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (gen == ~0ull || gen != e.generation) return 0;
    out.texId = e.texId;
    if (e.hd && out.minFilter < FILT_NEAR_MIP_NEAR) out.minFilter = out.minFilter == FILT_NEAR ? FILT_NEAR_MIP_NEAR : g_replaceMipFilter;
    *entry = &e;
    *ret = 1;
    return 1;
}
extern "C" void vgx2_a_tex_mark(void* e, uint64_t frame) { static_cast<Entry*>(e)->lastUsed = frame; }
extern "C" int vgx2_is_chara_tex(uint32_t id, int playerOnly) { return (playerOnly ? IsPlayerTexture(id) : IsCharaTexture(id)) ? 1 : 0; }
// (the Vita link of the second instance, c42: B's PixGather asks for a texture's channel ranges; the table is the player's)
extern "C" int vgx2_tex_channel_range(uint32_t id, uint8_t* lo, uint8_t* hi) { return TexChannelRange(id, lo, hi) ? 1 : 0; }
} // namespace vgx

// texwarm30: the game's walk of the stage's loaded models reports its textures here (decomp/src/Game/System/PortTexEnum.cpp). Game thread; the front end takes them at its next picture.
extern "C" int port_texwarm_cand_mode(void) { return vgx::tw::Mode() != 0 && vgx::tw::KeyMode() ? 1 : 0; }
extern "C" void port_texwarm_cand(uint32_t key, uint32_t serial, const void* image, unsigned w, unsigned h, unsigned fmt, unsigned mips, unsigned minFilter, unsigned maxLod8, unsigned order) {
    vgx::tw::RawCand r;
    r.key = key;
    r.serial = serial;
    r.addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(image));
    r.w = static_cast<uint16_t>(w > 65535 ? 65535 : w);
    r.h = static_cast<uint16_t>(h > 65535 ? 65535 : h);
    r.fmt = static_cast<uint8_t>(fmt > 255 ? 255 : fmt);
    r.mips = static_cast<uint8_t>(mips > 255 ? 255 : mips);
    r.minF = static_cast<uint8_t>(minFilter > 255 ? 255 : minFilter);
    r.maxLod8 = static_cast<uint8_t>(maxLod8 & 0xFF);
    r.order = order;
    vgx::arena::HostAllocScope host;  // (heap36: the game thread grows g_inbox here (GXV_TEXWARM): without this its storage came from the scene's heap, and the front end's swap kept it)
    std::lock_guard<std::mutex> lk(vgx::tw::g_inMu);
    if (vgx::tw::g_inbox.size() >= 32768) { ++vgx::tw::g_inDropped; return; }
    vgx::tw::g_inbox.push_back(r);
}
extern "C" void port_texwarm_cand_done(uint32_t key, uint32_t serial, unsigned count) {
    std::lock_guard<std::mutex> lk(vgx::tw::g_inMu);
    vgx::tw::g_inDone = true;
    vgx::tw::g_inDoneKey = key;
    vgx::tw::g_inDoneSerial = serial;
    vgx::tw::g_inDoneCount = count;
}
