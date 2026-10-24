// Front-end texture manager: decides which back-end texture a texmap resolves
// to and queues uploads when the guest bytes are new or have changed.
#include "vgx_guestmem.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <unordered_map>
#include <vector>

namespace vgx {

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
// Uploads by what the back end is handed (totals over the run, for the report).
uint64_t g_dxtUploads = 0, g_dxtBytes = 0, g_rgbaUploads = 0, g_rgbaBytes = 0, g_cmprDecoded = 0, g_cmprRgbaBytes = 0;
uint64_t g_laUploads = 0, g_laBytes = 0;
// The textures of the HD pack's list, by texture id (IsCharaTexture; ids are handed out in order and never reused).
std::vector<uint8_t> g_charaTex;
inline void SetCharaTexture(uint32_t texId, bool on) {
    if (!on && texId >= g_charaTex.size()) return;
    if (texId >= g_charaTex.size()) g_charaTex.resize(static_cast<size_t>(texId) + 256, 0);
    g_charaTex[texId] = on ? 1 : 0;
}
// Texture replacement (vgx_hdtex.cpp): the hook, the filter its binds get, the bytes alive.
bool (*g_replaceHook)(const TexReplaceQuery&, TexUpload&) = nullptr;
uint8_t g_replaceMipFilter = FILT_LIN_MIP_NEAR;
uint64_t g_hdLive = 0, g_hdUploads = 0, g_hdBytes = 0;
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
void SetTextureReplaceHook(bool (*fn)(const TexReplaceQuery&, TexUpload&), uint8_t mipFilter) {
    g_replaceHook = fn;
    g_replaceMipFilter = mipFilter;
}
uint64_t ReplacedTextureBytes() { return g_hdLive; }
bool IsCharaTexture(uint32_t texId) { return texId < g_charaTex.size() && g_charaTex[texId] != 0; }

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
        for (auto& e : g_cache) { e.second.generation = ~0ull; e.second.checkedEpoch = ~0ull; }
    }
    for (uint32_t& s : g_tlutSeen) s = 0xFFFFFFFFu;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
}

uint32_t AllocTextureId() { return g_nextTexId++; }

uint32_t AcquireCopyTexture(const void* dest, uint16_t w, uint16_t h, uint32_t fmt) {
    CopyEntry& c = g_copies[dest];
    if (!c.texId) { c.texId = g_nextTexId++; ++g_copiesGen; }
    c.w = w; c.h = h; c.fmt = fmt;
    return c.texId;
}

void DestroyCopyTexture(const void* dest) {
    auto it = g_copies.find(dest);
    if (it == g_copies.end()) return;
    Pending().cmds.push_back(Cmd{CMD_TEX_FREE, it->second.texId});
    g_copies.erase(it);
    ++g_copiesGen;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
}

static bool ResolveTextureFull(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
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
    static const bool noMip = EnvInt("GXV_NOMIP", 0) != 0;
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
        e.texId = g_nextTexId++;
        e.generation = Generation(o.data, size);
        std::atomic_thread_fence(std::memory_order_acquire);
        e.contentHash = XXH3_64bits(o.data, size);
    }
    if (upload) {
        const backend::Caps& caps = backend::GetCaps();
        TexUpload up;
        up.texId = e.texId;
        up.width = o.width;
        up.height = o.height;
        const uint8_t* const data = static_cast<const uint8_t*>(o.data);
        // The levels to send. Unless the back end takes a short chain (mipClamp), a
        // mipmapped texture is completed down to 1x1 below (no GL_TEXTURE_MAX_LEVEL in GLES2).
        uint32_t authored = levels;
        // A replacement image (vgx_hdtex.cpp: HD textures), asked only here, at upload.
        if (e.hd) { g_hdLive -= e.bytes; e.hd = false; }
        bool hd = false, inPack = false;
        if (g_replaceHook && !(g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height)))) {
            TexReplaceQuery q{data, fmt, o.width, o.height, tlut, tlut ? (tlutEntries == 0xFFFF ? 32768u : tlutEntries * 2u) : 0u, caps.mipClamp, g_hdLive};
            q.inPack = &inPack;
            hd = g_replaceHook(q, up);
            if (hd) authored = static_cast<uint32_t>(up.levels.size());
        }
        SetCharaTexture(e.texId, inPack); // (GXV_CHARAA: a character's or key item's texture, by the pack's list)
        // CMPR goes to the back end as DXT1 (an eighth of the memory, no decode) unless
        // a level uses the one thing DXT1 renders differently (see CmprToDxt1).
        //   GXV_DXT1=0  always decode;  =2  DXT1 for those textures too (their transparent
        //   texels become transparent black: dark fringes where they are filtered)
        static const bool dxtAll = EnvInt("GXV_DXT1", 1) == 2;
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
                if (!CmprToDxt1(src, w, h, up.levels.back().data(), dxtAll)) { dxt = false; break; }
                src += TexLevelSize(w, h, fmt);
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            if (!dxt) {
                up.levels.clear();
                authored = levels;
                ++g_cmprDecoded;
            }
        }
        if (hd) {
            // (up was filled by the hook: its own mip chain, sampled with a mip filter below)
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
        } else if (caps.la88 && !o.mipmap && fmt <= 3 &&
                   !(g_texPatchHook && (!g_texPatchNeedsRgba || g_texPatchNeedsRgba(e.contentHash, o.width, o.height)))) {
            up.format = TF_LA88;
            up.levels.emplace_back(static_cast<size_t>(o.width) * o.height * 2);
            DecodeTextureLA88(data, o.width, o.height, fmt, up.levels.back().data());
        } else {
            // RGBA8. A back end that makes its own mip levels (mipGen) only gets level 0.
            up.genMips = wantMips && caps.mipGen;
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
        }
        up.authored = static_cast<uint8_t>(authored < up.levels.size() ? authored : up.levels.size());
        e.mipmapped = wantMips || hd;
        {
            // Bytes the back end will hold for this texture.
            uint32_t bytes = 0;
            for (const auto& lv : up.levels) bytes += static_cast<uint32_t>(lv.size());
            if (up.genMips) bytes += bytes / 3;
            if (hd) { ++g_hdUploads; g_hdBytes += bytes; g_hdLive += bytes; e.hd = true; }
            else if (up.format == TF_DXT1) { ++g_dxtUploads; g_dxtBytes += bytes; }
            else if (up.format == TF_LA88) { ++g_laUploads; g_laBytes += bytes; }
            else { ++g_rgbaUploads; g_rgbaBytes += bytes; if (fmt == 0xE) g_cmprRgbaBytes += bytes; }
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
    static const int m = [] {
        int v = static_cast<int>(EnvInt("GXV_TEXMEMO", 1));
        if (v == 2 && !diag::kEnabled) v = 1;
        return v;
    }();
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

void TexCacheEndFrame() {
    static const uint64_t envBudget = static_cast<uint64_t>(EnvInt("GXV_TEXBUDGET", 0)) << 20;
    const uint64_t budget = g_budgetSet ? g_budget : envBudget;
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
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.lastUsed != b.lastUsed ? a.lastUsed < b.lastUsed : a.texId < b.texId; });
    CmdBuf& buf = Pending();
    for (const Cand& c : cand) {
        if (g_liveBytes <= budget) break;
        auto it = g_cache.find(c.key);
        const uint32_t bytes = it->second.bytes;
        if (it->second.hd) g_hdLive -= bytes;
        buf.cmds.push_back(Cmd{CMD_TEX_FREE, c.texId});
        stats::TexLive(-1, -static_cast<int64_t>(bytes));
        g_liveBytes -= bytes;
        ++g_evicted;
        g_evictedBytes += bytes;
        ++stats::Cur().texEvicted;
        stats::Cur().texEvictBytes += bytes;
        g_cache.erase(it);
        CacheFrontClear();
        ++g_cacheEraseGen;
    }
    if (g_liveBytes > budget) ++g_overBudgetFrames;
}

void TexCacheReport(FILE* f) {
    std::fprintf(f, "texture cache: %u entries, %.1f MB live; %llu changed without a write notification (GXV_TEXTRUST=verify)\n", static_cast<unsigned>(g_cache.size()),
                 g_liveBytes / 1048576.0, static_cast<unsigned long long>(g_texStale));
    std::fprintf(f, "texture uploads: %llu as DXT1 (%.1f MB), %llu as RGBA8 (%.1f MB); of the RGBA8 ones %llu are CMPR textures with transparent "
                    "texels (%.1f MB), which DXT1 would render with dark fringes\n",
                 static_cast<unsigned long long>(g_dxtUploads), g_dxtBytes / 1048576.0, static_cast<unsigned long long>(g_rgbaUploads),
                 g_rgbaBytes / 1048576.0, static_cast<unsigned long long>(g_cmprDecoded), g_cmprRgbaBytes / 1048576.0);
    if (g_laUploads) std::fprintf(f, "texture uploads: %llu as LA88 (%.1f MB)\n", static_cast<unsigned long long>(g_laUploads), g_laBytes / 1048576.0);
    if (g_hdUploads)
        std::fprintf(f, "texture uploads: %llu replaced by HD textures (%.1f MB; %.1f MB alive now)\n", static_cast<unsigned long long>(g_hdUploads),
                     g_hdBytes / 1048576.0, g_hdLive / 1048576.0);
    const uint64_t budget = g_budgetSet ? g_budget : (static_cast<uint64_t>(EnvInt("GXV_TEXBUDGET", 0)) << 20);
    if (budget)
        std::fprintf(f, "texture budget %.0f MB: %llu textures freed (%.1f MB), %llu frames ended over budget (working set larger than the budget)\n",
                     budget / 1048576.0, static_cast<unsigned long long>(g_evicted), g_evictedBytes / 1048576.0,
                     static_cast<unsigned long long>(g_overBudgetFrames));
}

} // namespace vgx
